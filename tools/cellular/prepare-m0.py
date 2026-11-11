#!/usr/bin/env python3
"""Prepare an S20 memory-layout test from the phone's own recovery backup.

Offline only. Preserve installed BootShim, DT table, kernel, root and payload
arguments; replace the 2-MiB FD and loader, then rebuild Android/AVB hashes.
Produces a verified candidate, bounded write segments, rollback segments and
a metadata manifest. Does not contact or write to the phone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent
PART_SIZE = 69009408
FD_OFFSET = 0x870
FD_SIZE = 0x200000
SLOT = FD_OFFSET + 0x100000
SLOT_SIZE = 0x80000
HEAD_SIZE = 0x1000000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def entries(image):
    require(image[SLOT:SLOT + 4] == b'XNUP', 'missing installed payload')
    body_size, = struct.unpack_from('<I', image, SLOT + 4)
    require(body_size <= SLOT_SIZE - 8, 'payload exceeds slot')
    cursor, end = SLOT + 8, SLOT + 8 + body_size
    result, names = [], set()
    while cursor + 24 <= end:
        name, size, reserved = struct.unpack_from('<16sII', image, cursor)
        if not any(name):
            return result
        raw = name.rstrip(b'\0')
        require(b'\0' not in raw and raw not in names, 'invalid/duplicate payload name')
        require(cursor + 24 + ((size + 15) & ~15) <= end, 'truncated payload item')
        names.add(raw)
        result.append((name, reserved, image[cursor + 24:cursor + 24 + size]))
        cursor += 24 + ((size + 15) & ~15)
    raise ValueError('missing payload terminator')


def boot_layout(image):
    require(len(image) == PART_SIZE and image[:8] == b'ANDROID!', 'not a full S20 recovery backup')
    page, version = struct.unpack_from('<II', image, 36)
    require(page == 2048 and version == 2, 'expected page-2048 Android boot v2')
    sizes = [struct.unpack_from('<I', image, offset)[0] for offset in (8, 16, 24, 1632, 1648)]
    require(sizes[0] == FD_SIZE + 0x70, 'unexpected BootShim/FD kernel layout')
    spans, cursor = [], page
    for size in sizes:
        spans.append((cursor, size))
        cursor += (size + page - 1) // page * page
    require(cursor <= HEAD_SIZE and image[spans[-1][0]:spans[-1][0] + 4] == bytes.fromhex('d7b7ab1e'),
            'missing Samsung DT table or boot image exceeds head')
    return spans, (cursor + 4095) & ~4095


def verify(avb, key, image):
    # avbtool resolves the recovery hash descriptor by basename recovery.img.
    with tempfile.TemporaryDirectory(dir=image.parent) as directory:
        link = Path(directory) / 'recovery.img'
        link.symlink_to(image.resolve())
        subprocess.run(['python3', '-I', str(avb), 'verify_image', '--image', str(link),
                        '--key', str(key)], check=True, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--fd', type=Path, required=True)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--avbtool', type=Path, default=RESEARCH / 'tools/bin/avbtool.py')
    parser.add_argument('--key', type=Path, default=RESEARCH / 'tools/dl/avb/testkey_rsa4096.pem')
    args = parser.parse_args()
    original = args.backup.read_bytes()
    spans, boot_size = boot_layout(original)
    verify(args.avbtool, args.key, args.backup)
    old_entries = entries(original)
    fd, loader = args.fd.read_bytes(), args.loader.read_bytes()
    require(len(fd) == FD_SIZE and fd[0x28:0x2c] == b'_FVH', 'not the expected 2-MiB FD')
    require(fd[0x100000:0x180000] == b'\xff' * SLOT_SIZE, 'new FD occupies payload slot')
    require(loader[:2] == b'MZ' and len(loader) >= 64, 'loader is not PE/COFF')
    pe, = struct.unpack_from('<I', loader, 0x3c)
    require(loader[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', loader, pe + 4)[0] == 0xaa64,
            'loader is not ARM64 PE/COFF')
    require(struct.unpack_from('<H', loader, pe + 24 + 68)[0] == 10, 'loader is not an EFI application')
    replaced = [(name, reserved, loader if name.rstrip(b'\0') == b'loader.efi' else data)
                for name, reserved, data in old_entries]
    require(sum(name.rstrip(b'\0') == b'loader.efi' for name, _, _ in old_entries) == 1,
            'expected exactly one installed loader')
    body = b''.join(struct.pack('<16sII', name, len(data), reserved) + data + bytes(-len(data) % 16)
                    for name, reserved, data in replaced) + bytes(24)
    payload = b'XNUP' + struct.pack('<I', len(body)) + body
    require(len(payload) <= SLOT_SIZE, 'replacement payload too large')
    changed = bytearray(original)
    changed[FD_OFFSET:FD_OFFSET + FD_SIZE] = fd
    changed[SLOT:SLOT + len(payload)] = payload
    require(entries(changed) == replaced, 'replacement payload did not round-trip')
    digest = hashlib.sha1()
    for offset, size in spans:
        digest.update(changed[offset:offset + size])
        digest.update(struct.pack('<I', size))
    changed[576:608] = digest.digest().ljust(32, b'\0')
    output = args.output
    output.mkdir(parents=True, exist_ok=False)
    head = output / 'signed-head.img'
    head.write_bytes(changed[:boot_size])
    subprocess.run(['python3', '-I', str(args.avbtool), 'add_hash_footer', '--image', str(head),
                    '--partition_size', str(PART_SIZE), '--partition_name', 'recovery',
                    '--algorithm', 'SHA256_RSA4096', '--key', str(args.key), '--rollback_index', '1'],
                   check=True, timeout=30)
    signed = head.read_bytes()
    # AVB's footer must point into the writeable head; kernel/root stay intact.
    require(signed[-64:-60] == b'AVBf', 'missing new AVB footer')
    _, _, original_size, vb_offset, vb_size = struct.unpack_from('>IIQQQ', signed, PART_SIZE - 60)
    require(original_size == boot_size and vb_offset + vb_size <= HEAD_SIZE,
            'unexpected AVB metadata layout')
    changed[:HEAD_SIZE] = signed[:HEAD_SIZE]
    changed[-64:] = signed[-64:]
    require(changed[HEAD_SIZE:-64] == original[HEAD_SIZE:-64], 'kernel/root/log body changed')
    require(changed[2048:FD_OFFSET] == original[2048:FD_OFFSET], 'installed BootShim changed')
    require(entries(changed) == replaced, 'signed payload changed')
    candidate = output / 'recovery.img'
    candidate.write_bytes(changed)
    verify(args.avbtool, args.key, candidate)
    (output / 'head.bin').write_bytes(changed[:HEAD_SIZE])
    (output / 'rollback-head.bin').write_bytes(original[:HEAD_SIZE])
    footer_changed = changed[-64:] != original[-64:]
    if footer_changed:
        (output / 'footer-block.bin').write_bytes(changed[-4096:])
        (output / 'rollback-footer-block.bin').write_bytes(original[-4096:])
    manifest = dict(scope='offline candidate only; no phone writes',
                    backup_sha256=sha(original), candidate_sha256=sha(changed),
                    fd_sha256=sha(fd), loader_sha256=sha(loader),
                    partition_size=PART_SIZE, head_size=HEAD_SIZE,
                    footer_changed=footer_changed, footer_block_offset=PART_SIZE - 4096,
                    kernel_root_log_body_preserved=True, bootshim_preserved=True,
                    payload_arguments_preserved=True, avb_verified=True)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    head.unlink()
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
