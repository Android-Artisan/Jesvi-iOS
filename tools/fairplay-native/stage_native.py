"""Reconstruct fixed-address analysis bundles using Apple's KASLR pointer list.

OFFLINE ONLY: these are not boot images or dynamically loadable kexts. This
prototype does not validate indirect code references, object layouts, startup
behavior, or runtime page permissions. Never deploy its output directly.
Proprietary inputs/outputs must remain in ignored local private storage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct

from inspect_kernel import commands, decode_prelink, file_offset, inspect, segments, symbols

PAGE = 16384


def align(value):
    return (value + PAGE - 1) & ~(PAGE - 1)


def resolve_target(relocation, custom_names, slide):
    target = int(relocation['target'], 16)
    if relocation['classification'] == 'component':
        return target + slide
    if relocation['classification'] == 'named_kernel_symbol':
        candidates = {custom_names[n] for n in relocation['symbols'] if n in custom_names}
        if len(candidates) == 1:
            return candidates.pop() + relocation['symbol_addend']
        raise ValueError('Missing or ambiguous custom symbol for pointer relocation')
    raise ValueError(f"Unresolved pointer target {relocation['target']}")


def reconstruct(data, stock_segments, component, bundle, custom_names, slide):
    header = file_offset(stock_segments, bundle['_PrelinkExecutableLoadAddr'])
    old_segments = segments(data, header)
    output = bytearray()
    chunks = []
    for seg in old_segments:
        if not seg['size']:
            continue
        start = align(len(output)) if chunks else 0
        output.extend(b'\0' * (start - len(output)))
        if seg['filesize']:
            source = file_offset(stock_segments, seg['address'], seg['filesize'])
            block = data[source:source + seg['filesize']]
            output.extend(block)
        else:
            block = b''
        chunks.append(dict(seg, new_fileoff=start, original=block, changed=set()))

    def patch(address, value):
        chunk = next((s for s in chunks if s['address'] <= address
                      and address + 8 <= s['address'] + s['filesize']), None)
        if chunk is None:
            raise ValueError('Relocation site not contained within one output segment')
        offset = chunk['new_fileoff'] + address - chunk['address']
        struct.pack_into('<Q', output, offset, value)
        chunk['changed'].update(range(address - chunk['address'], address - chunk['address'] + 8))

    for relocation in component['kaslr_relocations']:
        patch(int(relocation['site'], 16), resolve_target(relocation, custom_names, slide))

    # Reconstruct file offsets; preserve relative virtual layout with one
    # page-aligned slide across BOTH modules. All non-linkage code stays intact.
    for command, pos, _ in commands(data, header):
        if command != 0x19:
            continue
        old_address, old_size = struct.unpack_from('<QQ', data, pos + 24)
        chunk = next((s for s in chunks if s['address'] == old_address), None)
        command_offset = pos - header
        new_fileoff = chunk['new_fileoff'] if chunk else len(output)
        struct.pack_into('<Q', output, command_offset + 24, old_address + slide)
        struct.pack_into('<Q', output, command_offset + 40, new_fileoff)
        section_count = struct.unpack_from('<I', data, pos + 64)[0]
        for index in range(section_count):
            source_section = pos + 72 + index * 80
            output_section = source_section - header
            address, size, old_fileoff = struct.unpack_from('<QQI', data, source_section + 32)
            struct.pack_into('<Q', output, output_section + 32, address + slide)
            # S_ZEROFILL and S_GB_ZEROFILL sections have no file contents.
            kind = struct.unpack_from('<I', data, source_section + 64)[0] & 0xff
            if kind not in (1, 0xc) and size:
                if chunk is None or not (old_address <= address and address + size <= old_address + chunk['filesize']):
                    raise ValueError('Section extends outside its file-backed segment')
                struct.pack_into('<I', output, output_section + 48,
                                 new_fileoff + address - old_address)

    # Verify all bytes outside load commands and recorded pointer sites survived.
    header_length = 32 + struct.unpack_from('<I', data, header + 20)[0]
    for chunk in chunks:
        for index, value in enumerate(chunk['original']):
            if chunk['address'] == bundle['_PrelinkExecutableLoadAddr'] and index < header_length:
                continue
            if index not in chunk['changed'] and output[chunk['new_fileoff'] + index] != value:
                raise ValueError('Unexpected modification outside relocation/header fields')
    for relocation in component['kaslr_relocations']:
        address = int(relocation['site'], 16)
        chunk = next(s for s in chunks if s['address'] <= address < s['address'] + s['filesize'])
        offset = chunk['new_fileoff'] + address - chunk['address']
        # Header addresses are separately reconstructed and validated below.
        if not (chunk['address'] == bundle['_PrelinkExecutableLoadAddr'] and address - chunk['address'] < header_length):
            if struct.unpack_from('<Q', output, offset)[0] != resolve_target(relocation, custom_names, slide):
                raise ValueError('Relocated pointer failed readback')
    rebuilt = segments(output)
    for before, after in zip(old_segments, rebuilt):
        if after['address'] != before['address'] + slide or after['size'] != before['size']:
            raise ValueError('Reconstructed virtual layout differs from uniform slide')
    return bytes(output)


def stage(stock, custom, destination):
    if destination.exists():
        raise ValueError('Output directory must not already exist')
    data, custom_data = stock.read_bytes(), custom.read_bytes()
    report = inspect(stock, custom)
    if any(v['mismatches'] for v in report['base_vtable_checks']):
        raise ValueError('Named base vtable slots differ')
    ss, cs = segments(data), segments(custom_data)
    selected = report['components']
    minimum = min(int(s['address'], 16) for c in selected for s in c['segments'] if s['vm_size']) & ~(PAGE - 1)
    maximum_custom = max(s['address'] + s['size'] for s in cs)
    slide = align(maximum_custom + PAGE) - minimum
    custom_names = {}
    for address, names in symbols(custom_data).items():
        for name in names:
            if name in custom_names and custom_names[name] != address:
                raise ValueError('Duplicate custom symbol with different address')
            custom_names[name] = address
    prelink = next(s for s in ss if s['name'] == '__PRELINK_INFO')
    info = decode_prelink(data[prelink['fileoff']:prelink['fileoff'] + prelink['filesize']])
    manifest = dict(status='OFFLINE_ANALYSIS_ONLY_DO_NOT_DEPLOY', slide=f'{slide:#x}',
                    stock_sha256=report['stock_sha256'], custom_sha256=report['custom_sha256'],
                    remaining_gates=['Complete instruction/indirect-reference audit',
                                     'Validate C++ object sizes and configuration-dependent ABI',
                                     'Implement and validate prelink admission and VM protections',
                                     'Resolve stock fairplayd.H2 early return',
                                     'Native licensed download, install and launch validation'], components=[])
    pending = []
    for component in selected:
        bundle = next(b for b in info['_PrelinkInfoDictionary'] if b.get('CFBundleIdentifier') == component['identifier'])
        blob = reconstruct(data, ss, component, bundle, custom_names, slide)
        moved = dict(bundle)
        for key in ('_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkKmodInfo'):
            if moved.get(key):
                moved[key] += slide
        filename = bundle['CFBundleExecutable']
        pending.append((filename, blob, plistlib.dumps(moved)))
        manifest['components'].append(dict(identifier=component['identifier'], file=filename + '.macho',
                                           sha256=hashlib.sha256(blob).hexdigest(),
                                           pointer_relocations=len(component['kaslr_relocations']),
                                           bytes=len(blob)))
    # Only write output after all modules pass the offline reconstruction checks.
    destination.mkdir(parents=True, mode=0o700)
    for filename, blob, plist in pending:
        (destination / (filename + '.macho')).write_bytes(blob)
        (destination / (filename + '.plist')).write_bytes(plist)
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Staged', len(pending), 'offline analysis bundles; runtime compatibility NOT established.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock_kernel', type=Path)
    parser.add_argument('custom_kernel', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stage(args.stock_kernel, args.custom_kernel, args.output)
