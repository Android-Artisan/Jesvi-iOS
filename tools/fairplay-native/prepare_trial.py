"""Prepare a layout-only recovery image from a working template, never flash it.

Preserves the template's boot components, ramdisk and boot arguments. Adds an
explicit fairplay_native=0 if it fits; otherwise relies on the verified default-off
gate with the original arguments unchanged. Never enables native execution.
Firmware-derived input/output images stay private and ignored.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from sidecar import validate

PARTITION_SIZE = 69009408
PAYLOAD_AT, PAYLOAD_MAX = 0x100870, 0x80000
SIDECAR_AT, KERNEL_AT, RAMDISK_AT = 0x1800000, 0x2000000, 0x3400000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_payload(data):
    if data[PAYLOAD_AT:PAYLOAD_AT + 4] != b'XNUP':
        raise ValueError('Template has no payload at the established offset')
    length = struct.unpack_from('<I', data, PAYLOAD_AT + 4)[0]
    if length > PAYLOAD_MAX - 8:
        raise ValueError('Template payload exceeds its fixed slot')
    pos, end, entries = PAYLOAD_AT + 8, PAYLOAD_AT + 8 + length, []
    while pos + 24 <= end:
        name, size, reserved = struct.unpack_from('<16sII', data, pos)
        name = name.split(b'\0', 1)[0]
        if not name:
            if pos + 24 != end:
                raise ValueError('Unexpected bytes after payload terminator')
            return entries
        if reserved or size > end - pos - 24:
            raise ValueError('Invalid template payload entry')
        entries.append((name.decode('ascii'), bytes(data[pos + 24:pos + 24 + size])))
        pos += 24 + ((size + 15) & ~15)
    raise ValueError('Missing payload terminator')


def write_payload(entries):
    body = bytearray()
    names = set()
    for name, content in entries:
        if name in names or len(name.encode()) > 16:
            raise ValueError('Duplicate or oversized payload name')
        names.add(name)
        body.extend(struct.pack('<16sII', name.encode(), len(content), 0))
        body.extend(content)
        body.extend(b'\0' * (-len(content) % 16))
    body.extend(b'\0' * 24)
    result = b'XNUP' + struct.pack('<I', len(body)) + body
    if len(result) > PAYLOAD_MAX:
        raise ValueError('New loader/payload exceeds the fixed payload slot')
    return result


def boot_components(data):
    if data[:8] != b'ANDROID!' or struct.unpack_from('<I', data, 40)[0] != 2:
        raise ValueError('Expected established Android v2 recovery template')
    page = struct.unpack_from('<I', data, 36)[0]
    if page not in (2048, 4096, 16384):
        raise ValueError('Unexpected boot page size')
    sizes = [struct.unpack_from('<I', data, pos)[0] for pos in (8, 16, 24, 1632, 1648)]
    offset, parts = page, []
    for size in sizes:
        parts.append((offset, size))
        offset += (size + page - 1) // page * page
    if offset > 0x1000000 or PAYLOAD_AT + PAYLOAD_MAX > parts[0][0] + parts[0][1]:
        raise ValueError('Boot components overlap native/kernel slots or omit payload region')
    return parts, (offset + 4095) & ~4095


def prepare(template, loader, sidecar_dir, avbtool, key, output_dir):
    if output_dir.exists():
        raise ValueError('Output directory must be new')
    original = template.read_bytes()
    if len(original) != PARTITION_SIZE:
        raise ValueError('Template partition size differs from the established recovery size')
    parts, boot_end = boot_components(original)
    entries = read_payload(original)
    old = dict(entries)
    required = ('loader.efi', 'cmdline', 'kpart', 'koff', 'rpart', 'roff', 'rsize')
    if len(old) != len(entries) or any(name not in old for name in required):
        raise ValueError('Incomplete or duplicated template boot parameters')
    if any(name in old for name in ('kernel', 'fpart', 'foff', 'fsize')):
        raise ValueError('Expected an ordinary working recovery template without native additions')
    text = lambda name: old[name].rstrip(b'\0').decode('ascii')
    if text('kpart') != 'recovery' or int(text('koff'), 16) != KERNEL_AT or text('rpart') != 'recovery' or int(text('roff'), 16) != RAMDISK_AT:
        raise ValueError('Template does not use the established recovery slots')
    command = old['cmdline'].rstrip(b'\0').decode('utf-8')
    if any(arg.startswith(('fairplay_native=', 'fplay_native=')) for arg in command.split()):
        raise ValueError('Template already contains a native FairPlay boot argument')
    layout_argument = next((arg for arg in ('fairplay_native=0', 'fplay_native=0')
                            if len((command + ' ' + arg).encode()) <= 550), None)
    explicit_layout_argument = layout_argument is not None
    if layout_argument:
        command += ' ' + layout_argument
    kernel = (sidecar_dir / 'kernel.macho').read_bytes()
    sidecar = (sidecar_dir / 'native-fairplay.sidecar').read_bytes()
    checks = json.loads((sidecar_dir / 'c-validator-check.json').read_text())
    if not checks.get('c_validator_pass') or checks['kernel_sha256'] != sha(kernel) or checks['sidecar_sha256'] != sha(sidecar):
        raise ValueError('Actual C validator has not accepted this exact kernel/sidecar pair')
    if b'redwood_native_fairplay_gate_v1' not in kernel or len(kernel) > 0x1000000:
        raise ValueError('Kernel lacks the default-off gate or exceeds its read window')
    validate(kernel, sidecar)
    pe = loader.read_bytes()
    if pe[:2] != b'MZ' or b'native sidecar validation failed' not in pe:
        raise ValueError('Expected the newly built sidecar-capable EFI loader')
    padded = sidecar + b'\0' * (-len(sidecar) % 0x4000)
    if SIDECAR_AT + len(padded) > KERNEL_AT or any(original[SIDECAR_AT:SIDECAR_AT + len(padded)]):
        raise ValueError('Sidecar exceeds its empty reserved recovery slot')
    ramdisk_size = int(text('rsize'), 16)
    if RAMDISK_AT + ramdisk_size > len(original) - 64:
        raise ValueError('Ramdisk extent exceeds template')
    replacement = [(name, pe if name == 'loader.efi' else (command.encode() + b'\0') if name == 'cmdline' else content)
                   for name, content in entries]
    replacement += [('fpart', b'recovery\0'), ('foff', b'1800000\0'), ('fsize', f'{len(sidecar):x}'.encode() + b'\0')]
    payload = write_payload(replacement)
    changed = bytearray(original)
    changed[PAYLOAD_AT:PAYLOAD_AT + PAYLOAD_MAX] = payload + b'\0' * (PAYLOAD_MAX - len(payload))
    h = hashlib.sha1()
    for offset, size in parts:
        h.update(changed[offset:offset + size])
        h.update(struct.pack('<I', size))
    changed[576:608] = h.digest().ljust(32, b'\0')
    output_dir.mkdir(parents=True, mode=0o700)
    signed = output_dir / 'signed-head.img'
    signed.write_bytes(changed[:boot_end])
    subprocess.run(['python3', '-I', str(avbtool), 'add_hash_footer', '--image', str(signed),
                    '--partition_size', str(PARTITION_SIZE), '--partition_name', 'recovery',
                    '--algorithm', 'SHA256_RSA4096', '--key', str(key), '--rollback_index', '1'], check=True)
    final = bytearray(signed.read_bytes())
    if len(final) != PARTITION_SIZE:
        raise ValueError('AVB output partition size mismatch')
    final[0x1000000:PARTITION_SIZE - 64] = original[0x1000000:PARTITION_SIZE - 64]
    final[SIDECAR_AT:SIDECAR_AT + len(padded)] = padded
    final[KERNEL_AT:RAMDISK_AT] = b'\0' * (RAMDISK_AT - KERNEL_AT)
    final[KERNEL_AT:KERNEL_AT + len(kernel)] = kernel
    # The ramdisk (including account/cellular/Touch ID support) is untouched.
    if final[RAMDISK_AT:RAMDISK_AT + ramdisk_size] != original[RAMDISK_AT:RAMDISK_AT + ramdisk_size]:
        raise ValueError('Ramdisk unexpectedly changed')
    if final[:576] != original[:576] or final[608:PAYLOAD_AT] != original[608:PAYLOAD_AT]:
        raise ValueError('Unexpected boot-header or pre-payload firmware change')
    if final[PAYLOAD_AT + PAYLOAD_MAX:boot_end] != original[PAYLOAD_AT + PAYLOAD_MAX:boot_end]:
        raise ValueError('Unexpected firmware/DTB change outside payload')
    reloaded = dict(read_payload(final))
    if reloaded['cmdline'] != command.encode() + b'\0':
        raise ValueError('Layout-only boot argument failed roundtrip')
    if any(reloaded[name] != content for name, content in entries if name not in ('loader.efi', 'cmdline')):
        raise ValueError('Existing boot parameters changed')
    # avbtool resolves the descriptor's partition name to recovery.img in the
    # same directory when verifying the hash, regardless of --image basename.
    image = output_dir / 'recovery.img'
    image.write_bytes(final)
    subprocess.run(['python3', '-I', str(avbtool), 'verify_image', '--image', str(image), '--key', str(key)], check=True)
    (output_dir / 'manifest.json').write_text(json.dumps(dict(
        status='PREPARED_LAYOUT_ONLY_NOT_FLASHED', fairplay_native=0,
        explicit_layout_boot_argument=explicit_layout_argument,
        layout_boot_argument=layout_argument,
        template_sha256=sha(original), image_sha256=sha(final), kernel_sha256=sha(kernel),
        loader_sha256=sha(pe), sidecar_sha256=sha(sidecar), ramdisk_sha256=sha(original[RAMDISK_AT:RAMDISK_AT + ramdisk_size]),
        validator_sha256=checks['validator_sha256'],
        partition_bytes=len(final), kernel_bytes=len(kernel), sidecar_bytes=len(sidecar), payload_bytes=len(payload),
        template_boot_arguments_preserved=True, ramdisk_preserved=True, avb_verified=True,
        requires_phone_holder_handoff=True, requires_user_reboot_approval=True), indent=2) + '\n')
    signed.unlink()
    print('Prepared layout-only recovery image; AVB and preserved ramdisk verified. NOT FLASHED.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('template', type=Path)
    parser.add_argument('loader', type=Path)
    parser.add_argument('sidecar_directory', type=Path)
    parser.add_argument('--avbtool', type=Path, required=True)
    parser.add_argument('--public-test-key', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.template, args.loader, args.sidecar_directory, args.avbtool, args.public_test_key, args.output_dir)
