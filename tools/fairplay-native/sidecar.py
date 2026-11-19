"""Compact native-module payload prototype; loader integration is not live.

Builds from the verified offline prelink image. Coalesced VM ranges remain
reserved in the small kernel; module bytes travel in a separate bounded payload.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from inspect_kernel import commands, file_offset, segments
from stage_native import align

GROUPS = {'__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST', '__PRELINK_DATA'}
HEADER = struct.Struct('<4sIII16s')
ENTRY = struct.Struct('<QQQII')


def checksum(content):
    value = 2166136261
    for byte in content:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def image_uuid(data):
    matches = [data[pos + 8:pos + 24] for cmd, pos, _ in commands(data) if cmd == 0x1b]
    if len(matches) != 1:
        raise ValueError('Expected one kernel UUID')
    return matches[0]


def validate(kernel, payload):
    if len(payload) < HEADER.size:
        raise ValueError('Truncated sidecar header')
    magic, version, count, size, identity = HEADER.unpack_from(payload)
    if magic != b'RFP1' or version != 1 or size != len(payload) or not 1 <= count <= 16:
        raise ValueError('Invalid sidecar header')
    if identity != image_uuid(kernel):
        raise ValueError('Sidecar kernel UUID mismatch')
    table_end = HEADER.size + count * ENTRY.size
    if table_end > size:
        raise ValueError('Truncated sidecar table')
    groups = [s for s in segments(kernel) if s['name'] in GROUPS and s['size']]
    ranges = []
    payload_ranges = []
    result = []
    for index in range(count):
        address, length, offset, digest, reserved = ENTRY.unpack_from(payload, HEADER.size + index * ENTRY.size)
        if reserved or not length or offset < table_end or offset > size or length > size - offset:
            raise ValueError('Invalid sidecar entry bounds')
        if not any(s['address'] <= address and address - s['address'] <= s['size']
                   and length <= s['size'] - (address - s['address']) for s in groups):
            raise ValueError('Sidecar target outside reserved native VM ranges')
        if any(max(address, low) < min(address + length, high) for low, high in ranges):
            raise ValueError('Sidecar VM targets overlap')
        if any(max(offset, low) < min(offset + length, high) for low, high in payload_ranges):
            raise ValueError('Sidecar payload ranges overlap')
        content = payload[offset:offset + length]
        if checksum(content) != digest:
            raise ValueError('Sidecar checksum mismatch')
        ranges.append((address, address + length))
        payload_ranges.append((offset, offset + length))
        result.append((address, content))
    return result


def build(source, layout, staged, output_dir):
    if output_dir.exists():
        raise ValueError('Output directory must be new')
    original, full = source.read_bytes(), layout.read_bytes()
    metadata = json.loads(layout.with_suffix(layout.suffix + '.json').read_text())
    if hashlib.sha256(original).hexdigest() != metadata['source_kernel_sha256']:
        raise ValueError('Source kernel hash mismatch')
    if hashlib.sha256(full).hexdigest() != metadata['sha256']:
        raise ValueError('Prelink layout hash mismatch')
    full_segments = segments(full)
    info = next(s for s in full_segments if s['name'] == '__PRELINK_INFO')
    compact = bytearray(original)
    info_offset = align(len(compact))
    compact.extend(b'\0' * (info_offset - len(compact)))
    compact.extend(full[info['fileoff']:info['fileoff'] + info['filesize']])
    changed = set()
    for command, pos, size in commands(original):
        if command != 0x19:
            continue
        name = original[pos + 8:pos + 24].split(b'\0', 1)[0].decode()
        if name not in GROUPS | {'__PRELINK_INFO'}:
            continue
        descriptor = next(s for s in full_segments if s['name'] == name)
        if name == '__PRELINK_INFO':
            replacement = next((full[p:p + n] for cmd, p, n in commands(full)
                                if cmd == 0x19 and full[p + 8:p + 24].split(b'\0', 1)[0].decode() == name))
            compact[pos:pos + size] = replacement
            struct.pack_into('<Q', compact, pos + 40, info_offset)
            struct.pack_into('<I', compact, pos + 72 + 48, info_offset)
        else:
            prot = 5 if name == '__PLK_TEXT_EXEC' else 3
            struct.pack_into('<QQQQiiI', compact, pos + 24, descriptor['address'], descriptor['size'], 0, 0, prot, prot, 0)
            # Coalesced descriptors reserve/protect VM; bytes arrive through
            # the sidecar. No file-backed section describes this zero-fill area.
            compact[pos + 72:pos + size] = b'\0' * (size - 72)
        changed.update(range(pos + 24, pos + size))
    manifest = json.loads((staged / 'manifest.json').read_text())
    pieces = []
    for module in manifest['components']:
        content = (staged / module['file']).read_bytes()
        if hashlib.sha256(content).hexdigest() != module['sha256']:
            raise ValueError('Module hash mismatch')
        for seg in segments(content):
            if seg['filesize']:
                offset = file_offset(full_segments, seg['address'], seg['filesize'])
                block = content[seg['fileoff']:seg['fileoff'] + seg['filesize']]
                if full[offset:offset + seg['filesize']] != block:
                    raise ValueError('Layout contains different native module bytes')
                pieces.append((seg['address'], block))
    payload = bytearray(HEADER.size + ENTRY.size * len(pieces))
    for index, (address, block) in enumerate(pieces):
        offset = (len(payload) + 15) & ~15
        payload.extend(b'\0' * (offset - len(payload)))
        payload.extend(block)
        ENTRY.pack_into(payload, HEADER.size + ENTRY.size * index, address, len(block), offset, checksum(block), 0)
    HEADER.pack_into(payload, 0, b'RFP1', 1, len(pieces), len(payload), image_uuid(compact))
    verified = validate(compact, payload)
    if verified != pieces:
        raise ValueError('Sidecar reconstruction differs from input pieces')
    if any(a != b and i not in changed for i, (a, b) in enumerate(zip(original, compact))):
        raise ValueError('Unexpected source kernel modification')
    if len(compact) > 0x1000000:
        raise ValueError('Compact kernel exceeds existing 16 MiB boot read window')
    output_dir.mkdir(parents=True, mode=0o700)
    (output_dir / 'kernel.macho').write_bytes(compact)
    (output_dir / 'native-fairplay.sidecar').write_bytes(payload)
    report = dict(status='OFFLINE_SIDECAR_PROTOTYPE_DO_NOT_DEPLOY', kernel_bytes=len(compact),
                  sidecar_bytes=len(payload), pieces=len(pieces), loader_read_window_ok=True,
                  admission_gate_present=b'redwood_native_fairplay_gate_v1' in original,
                  kernel_sha256=hashlib.sha256(compact).hexdigest(), sidecar_sha256=hashlib.sha256(payload).hexdigest(),
                  required_changes=['Compile native prelink admission gate',
                                    'Compile bounded sidecar loader and package payload entry',
                                    'Use imports rebound to that exact rebuilt kernel',
                                    'Validate layout-only boot before enabling native execution'])
    (output_dir / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Compact kernel', len(compact), 'bytes; sidecar', len(payload), 'bytes;', len(pieces), 'validated pieces.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('layout', type=Path)
    parser.add_argument('staged', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    build(args.kernel, args.layout, args.staged, args.output_dir)
