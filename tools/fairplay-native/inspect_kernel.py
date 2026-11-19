"""Read-only compatibility inventory for legacy arm64 iOS kernelcaches.

Writes metadata only. Does not extract, execute, patch, or load Apple code.
Direct branch and exact symbol-pointer references are a lower bound on imports;
indirect calls, stripped symbols and ABI/layout compatibility require more work.
"""
import argparse
import bisect
import base64
import collections
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

TARGETS = ('com.apple.driver.FairPlayIOKit', 'com.apple.IOTextEncryptionFamily')


def commands(data, offset=0):
    if struct.unpack_from('<I', data, offset)[0] != 0xfeedfacf:
        raise ValueError('Expected thin little-endian Mach-O 64')
    count, length = struct.unpack_from('<II', data, offset + 16)
    pos, end = offset + 32, offset + 32 + length
    if end > len(data):
        raise ValueError('Truncated load commands')
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, pos)
        if size < 8 or pos + size > end:
            raise ValueError('Invalid load command size')
        yield command, pos, size
        pos += size


def segments(data, offset=0):
    result = []
    for command, pos, _ in commands(data, offset):
        if command != 0x19:
            continue
        name, address, size, fileoff, filesize = struct.unpack_from('<16sQQQQ', data, pos + 8)
        result.append(dict(name=name.split(b'\0')[0].decode(), address=address,
                           size=size, fileoff=fileoff, filesize=filesize))
    return result


def file_offset(segs, address, size=1):
    for seg in segs:
        delta = address - seg['address']
        if 0 <= delta and delta + size <= seg['filesize']:
            return seg['fileoff'] + delta
    raise ValueError(f'Address {address:#x} is not file-backed')


def decode_prelink(raw):
    # Apple's XML serialization permits ID/IDREF and hex integers.
    root = ET.fromstring(raw.rstrip(b'\0'))
    ids = {e.attrib['ID']: e for e in root.iter() if 'ID' in e.attrib}

    def value(element, active=frozenset()):
        if id(element) in active:
            raise ValueError('Cyclic prelink XML reference')
        active = active | {id(element)}
        if 'IDREF' in element.attrib:
            return value(ids[element.attrib['IDREF']], active)
        if element.tag == 'dict':
            children = list(element)
            if len(children) % 2 or any(e.tag != 'key' for e in children[::2]):
                raise ValueError('Malformed prelink dictionary')
            return {children[i].text: value(children[i + 1], active)
                    for i in range(0, len(children), 2)}
        if element.tag == 'array':
            return [value(child, active) for child in element]
        if element.tag == 'integer':
            return int(element.text or '0', 0)
        if element.tag == 'data':
            return base64.b64decode(element.text or '', validate=False)
        if element.tag in ('true', 'false'):
            return element.tag == 'true'
        if element.tag == 'plist':
            return value(element[0], active)
        return element.text or ''

    return value(root)


def symbols(data):
    result = collections.defaultdict(list)
    for command, pos, _ in commands(data):
        if command != 2:
            continue
        symoff, count, stroff, strsize = struct.unpack_from('<IIII', data, pos + 8)
        if symoff + count * 16 > len(data) or stroff + strsize > len(data):
            raise ValueError('Truncated symbol table')
        strings = data[stroff:stroff + strsize]
        for index in range(count):
            nameoff, kind, section, desc, address = struct.unpack_from('<IBBHQ', data, symoff + 16 * index)
            if not address or kind & 0xe0 or nameoff >= len(strings):
                continue
            name = strings[nameoff:].split(b'\0', 1)[0].decode('utf-8', 'replace')
            if name:
                result[address].append(name)
    return result


def branch_target(instruction, pc):
    if instruction & 0x7c000000 != 0x14000000:
        return None
    immediate = instruction & 0x03ffffff
    if immediate & 0x02000000:
        immediate -= 0x04000000
    return pc + immediate * 4


def compare_vtables(stock_data, custom_data, stock_symbols, custom_symbols):
    """Compare named slots up to next symbol (an upper bound on table size)."""
    report = []
    for name in ('__ZTV9IOService', '__ZTV12IOUserClient', '__ZTV11OSMetaClass'):
        tables = []
        for data, names in ((stock_data, stock_symbols), (custom_data, custom_symbols)):
            addresses = sorted(names)
            address = next((a for a, ns in names.items() if name in ns), None)
            if address is None:
                tables.append([])
                continue
            index = bisect.bisect_right(addresses, address)
            if index == len(addresses):
                tables.append([])
                continue
            size = min(addresses[index] - address, 4096)
            offset = file_offset(segments(data), address, size)
            tables.append([names.get(struct.unpack_from('<Q', data, offset + i)[0], [])
                           for i in range(0, size - 7, 8)])
        mismatches = []
        matched = unknown = 0
        for slot, (old, new) in enumerate(zip(*tables)):
            if not old or not new:
                unknown += 1
            elif set(old) & set(new):
                matched += 1
            else:
                mismatches.append(dict(slot=slot, stock=old, custom=new))
        report.append(dict(symbol=name, stock_slot_upper_bound=len(tables[0]),
                           custom_slot_upper_bound=len(tables[1]), matched_named_slots=matched,
                           unknown_slots=unknown, mismatches=mismatches))
    return report


def inspect(stock, custom):
    data = stock.read_bytes()
    custom_data = custom.read_bytes()
    segs = segments(data)
    prelink = next(seg for seg in segs if seg['name'] == '__PRELINK_INFO')
    info = decode_prelink(data[prelink['fileoff']:prelink['fileoff'] + prelink['filesize']])
    stock_symbols = symbols(data)
    stock_symbol_addresses = sorted(stock_symbols)
    custom_symbols = symbols(custom_data)
    custom_names = {name for names in custom_symbols.values() for name in names}
    custom_segments = segments(custom_data)
    selected = []
    for bundle in info['_PrelinkInfoDictionary']:
        if bundle.get('CFBundleIdentifier') in TARGETS:
            selected.append((bundle, segments(data, file_offset(segs, bundle['_PrelinkExecutableLoadAddr']))))
    kaslr = info.get('_PrelinkLinkKASLROffsets', b'')
    if len(kaslr) < 4:
        raise ValueError('Missing original KASLR relocation list')
    kaslr_count = struct.unpack_from('<I', kaslr)[0]
    if len(kaslr) != 4 + 4 * kaslr_count:
        raise ValueError('Malformed original KASLR relocation list')
    kaslr_base = next(s['address'] for s in segs if s['name'] == '__PRELINK_TEXT')
    relocation_sites = [kaslr_base + r[0] for r in struct.iter_unpack('<I', kaslr[4:])]
    def owner(address):
        return next((b['CFBundleIdentifier'] for b, ss in selected
                     if any(s['address'] <= address < s['address'] + s['size'] for s in ss)), None)
    report = dict(stock_sha256=hashlib.sha256(data).hexdigest(),
                  custom_sha256=hashlib.sha256(custom_data).hexdigest(),
                  limitations=['References are a lower bound, not a complete import table.',
                               'Matching names do not establish ABI or object-layout compatibility.',
                               'Linear branch scans include data/obfuscation: unresolved candidates are not proven calls.',
                               'No binary loading, relocation, execution, or decryption was attempted.'],
                  base_vtable_checks=compare_vtables(data, custom_data, stock_symbols, custom_symbols),
                  stock_kaslr_relocation_count=kaslr_count,
                  components=[])
    for bundle in info['_PrelinkInfoDictionary']:
        if bundle.get('CFBundleIdentifier') not in TARGETS:
            continue
        header = file_offset(segs, bundle['_PrelinkExecutableLoadAddr'])
        component_segments = segments(data, header)
        relocations = []
        for address in relocation_sites:
            if owner(address) != bundle['CFBundleIdentifier']:
                continue
            offset = file_offset(segs, address, 8)
            target = struct.unpack_from('<Q', data, offset)[0]
            target_owner = owner(target)
            names = stock_symbols.get(target, [])
            addend = 0
            if not target_owner and not names:
                index = bisect.bisect_right(stock_symbol_addresses, target) - 1
                if index >= 0:
                    base = stock_symbol_addresses[index]
                    # Several original imports have small low-bit addends.
                    # Preserve them symbolically; do not assume a call pointer.
                    if 0 < target - base < 8:
                        names = stock_symbols[base]
                        addend = target - base
            classification = 'component' if target_owner else ('named_kernel_symbol' if names else 'unresolved')
            relocations.append(dict(site=f'{address:#x}', target=f'{target:#x}',
                                    classification=classification, component=target_owner,
                                    symbols=names, symbol_addend=addend,
                                    present_in_custom=any(n in custom_names for n in names)))
        linkage = {}
        for command, pos, size in commands(data, header):
            if command in (2, 11, 0x1e):
                linkage[hex(command)] = list(struct.unpack_from('<' + 'I' * ((size - 8) // 4), data, pos + 8))
        overlaps = []
        for seg in component_segments:
            for other in custom_segments:
                low = max(seg['address'], other['address'])
                high = min(seg['address'] + seg['size'], other['address'] + other['size'])
                if low < high:
                    overlaps.append(dict(component_segment=seg['name'], custom_segment=other['name'],
                                         start=f'{low:#x}', end=f'{high:#x}'))
        references = collections.Counter()
        unresolved = collections.Counter()
        def is_local(address):
            return any(s['address'] <= address < s['address'] + s['size'] for s in component_segments)
        for seg in component_segments:
            if not seg['filesize']:
                continue
            offset = file_offset(segs, seg['address'], seg['filesize'])
            block = data[offset:offset + seg['filesize']]
            if seg['name'] == '__TEXT_EXEC':
                for index in range(0, len(block) - 3, 4):
                    word = struct.unpack_from('<I', block, index)[0]
                    target = branch_target(word, seg['address'] + index)
                    if target is None or is_local(target):
                        continue
                    names = stock_symbols.get(target)
                    if names:
                        references.update(names)
                    else:
                        unresolved[f'{target:#x}'] += 1
            elif seg['name'] in ('__DATA', '__DATA_CONST'):
                for index in range(0, len(block) - 7, 8):
                    target = struct.unpack_from('<Q', block, index)[0]
                    if not is_local(target) and target in stock_symbols:
                        references.update(stock_symbols[target])
        report['components'].append(dict(
            identifier=bundle['CFBundleIdentifier'], version=bundle['CFBundleVersion'],
            linkage_load_commands=linkage, original_address_collisions=overlaps,
            kaslr_relocations=relocations,
            dependencies=bundle.get('OSBundleLibraries', {}),
            personalities=bundle.get('IOKitPersonalities', {}),
            segments=[dict(name=s['name'], address=f"{s['address']:#x}",
                           vm_size=s['size'], file_size=s['filesize']) for s in component_segments],
            referenced_symbols=[dict(name=n, count=c, present_in_custom=n in custom_names)
                                for n, c in sorted(references.items())],
            unresolved_external_branches=dict(sorted(unresolved.items()))))
    if {c['identifier'] for c in report['components']} != set(TARGETS):
        raise ValueError('Expected both FairPlay components in stock kernel')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock_kernel', type=Path)
    parser.add_argument('custom_kernel', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.stock_kernel, args.custom_kernel)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    for component in result['components']:
        refs = component['referenced_symbols']
        missing = [ref['name'] for ref in refs if not ref['present_in_custom']]
        print(component['identifier'], 'resolved_symbols', len(refs),
              'missing_names', len(missing),
              'unresolved_branch_targets', len(component['unresolved_external_branches']))
        for name in missing:
            print('  missing:', name)
