"""Assemble an OFFLINE prelink image for VM/layout validation.

Never deploy this prototype. The default-off admission patch must be compiled
into a freshly built kernel before a test image can be prepared. This tool
labels and rejects incomplete metadata; it does not establish live safety.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import uuid

from inspect_kernel import commands, decode_prelink, file_offset, segments, symbols
from stage_native import PAGE, align

GROUPS = {'__TEXT': '__PRELINK_TEXT', '__TEXT_EXEC': '__PLK_TEXT_EXEC',
          '__DATA_CONST': '__PLK_DATA_CONST', '__DATA': '__PRELINK_DATA'}


def metadata_xml(value):
    # OSUnserializeXML consumes the dictionary itself, not a plist wrapper.
    import xml.etree.ElementTree as ET
    root = ET.fromstring(plistlib.dumps(value, fmt=plistlib.FMT_XML))
    return ET.tostring(root[0], encoding='utf-8') + b'\0'


def bounds(parts):
    if not parts:
        raise ValueError('Missing prelink segment group')
    return min(s['address'] for s, _ in parts) & ~(PAGE - 1), align(max(s['address'] + s['size'] for s, _ in parts))


def validate_parts(parts, lower, upper):
    previous = lower
    for seg, _ in sorted(parts, key=lambda part: part[0]['address']):
        if seg['address'] < previous or seg['address'] + seg['size'] > upper:
            raise ValueError('Overlapping or out-of-range module segments')
        previous = seg['address'] + seg['size']


def pack(kernel, stage_dir, inventory_path, stock, output):
    if output.exists():
        raise ValueError('Output must not already exist')
    original = kernel.read_bytes()
    stage = json.loads((stage_dir / 'manifest.json').read_text())
    inventory = json.loads(inventory_path.read_text())
    digest = hashlib.sha256(original).hexdigest()
    if digest != stage['custom_sha256'] or digest != inventory['custom_sha256']:
        raise ValueError('Kernel differs from the kernel used for pointer rebinding')
    if stage['stock_sha256'] != inventory['stock_sha256']:
        raise ValueError('Stock metadata and bundles have different provenance')
    originals = segments(original)
    for seg in originals:
        if seg['name'] in set(GROUPS.values()) | {'__PRELINK_INFO'} and (seg['size'] or seg['filesize']):
            raise ValueError('Input already contains a populated prelink segment')
    buckets = {name: [] for name in GROUPS.values()}
    module_info = []
    for module in stage['components']:
        path = stage_dir / module['file']
        content = path.read_bytes()
        if hashlib.sha256(content).hexdigest() != module['sha256']:
            raise ValueError('Staged module hash mismatch')
        for seg in segments(content):
            if not seg['size']:
                continue
            if seg['name'] not in GROUPS:
                raise ValueError('Unsupported nonempty module segment')
            buckets[GROUPS[seg['name']]].append((seg, content))
        info = plistlib.loads(path.with_suffix('.plist').read_bytes())
        if info['CFBundleIdentifier'] != module['identifier']:
            raise ValueError('Staged module/plist identifier mismatch')
        module_info.append(info)
    intervals = {name: bounds(parts) for name, parts in buckets.items()}
    text, code, const = ('__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST')
    if not intervals[text][0] < intervals[code][0] < intervals[const][0]:
        raise ValueError('Unexpected source read-only segment ordering')
    # Preserve the uniform-slide gaps as zero-filled storage. XNU computes
    # vm_prelink_etext by summing the three read-only sizes, so they must join.
    intervals[text] = (intervals[text][0], intervals[code][0])
    intervals[code] = (intervals[code][0], intervals[const][0])
    if intervals[text][1] - intervals[text][0] >= 0x03000000:
        raise ValueError('PRELINK_TEXT exceeds XNU arm64 size constraint')
    for name, parts in buckets.items():
        low, high = intervals[name]
        validate_parts(parts, low, high)
        for seg in originals:
            if seg['size'] and max(low, seg['address']) < min(high, seg['address'] + seg['size']):
                raise ValueError('Prelink VM range overlaps the input kernel')

    # The original KPI interface Mach-Os are tied to the stock kernel. Use
    # metadata for the KPIs actually supplied by this custom kernel instead.
    # They contain no executable code and declare the kernel dependency.
    stock_data = stock.read_bytes()
    if hashlib.sha256(stock_data).hexdigest() != stage['stock_sha256']:
        raise ValueError('Stock kernel hash mismatch')
    stock_segs = segments(stock_data)
    prelink = next(s for s in stock_segs if s['name'] == '__PRELINK_INFO')
    stock_info = decode_prelink(stock_data[prelink['fileoff']:prelink['fileoff'] + prelink['filesize']])
    required = {dep for module in module_info for dep in module.get('OSBundleLibraries', {})}
    required -= {m['CFBundleIdentifier'] for m in module_info}
    aliases = []
    by_name = {n: a for a, ns in symbols(original).items() for n in ns}
    release_offset = file_offset(originals, by_name['_osrelease'])
    release = original[release_offset:release_offset + 32].split(b'\0', 1)[0].decode('ascii')
    for name in sorted(required):
        if not name.startswith('com.apple.kpi.'):
            raise ValueError('Non-KPI dependency needs separate implementation')
        source = next(b for b in stock_info['_PrelinkInfoDictionary'] if b.get('CFBundleIdentifier') == name)
        if source['CFBundleVersion'] != release:
            raise ValueError('Custom kernel release differs from the original KPI version')
        aliases.append(dict(CFBundleIdentifier=name, CFBundleVersion=release,
                            OSBundleCompatibleVersion=source['OSBundleCompatibleVersion'],
                            OSKernelResource=True, OSBundleRequired='Root'))
    slide = int(stage['slide'], 16)
    sites = [int(r['site'], 16) + slide - intervals[text][0]
             for c in inventory['components'] for r in c['kaslr_relocations']]
    if any(site < 0 or site > 0xffffffff for site in sites):
        raise ValueError('Relocation offsets exceed original packed format')
    packed_offsets = struct.pack('<I', len(sites)) + b''.join(struct.pack('<I', s) for s in sites)
    info_blob = metadata_xml(dict(_PrelinkInfoDictionary=aliases + module_info,
                                 _PrelinkKCID=uuid.uuid4().bytes,
                                 _PrelinkLinkKASLROffsets=packed_offsets))
    info_low = align(max(high for low, high in intervals.values()))
    intervals['__PRELINK_INFO'] = (info_low, info_low + align(len(info_blob)))
    blobs = {}
    for name, parts in buckets.items():
        low, high = intervals[name]
        blob = bytearray(high - low)
        for seg, content in parts:
            offset = seg['address'] - low
            blob[offset:offset + seg['filesize']] = content[seg['fileoff']:seg['fileoff'] + seg['filesize']]
        blobs[name] = bytes(blob)
    blobs['__PRELINK_INFO'] = info_blob + b'\0' * (intervals['__PRELINK_INFO'][1] - info_low - len(info_blob))
    result = bytearray(original)
    locations = {}
    for name, blob in blobs.items():
        offset = align(len(result))
        result.extend(b'\0' * (offset - len(result)))
        result.extend(blob)
        locations[name] = offset
    changed_headers = []
    updated = set()
    for command, pos, _ in commands(original):
        if command != 0x19:
            continue
        name = original[pos + 8:pos + 24].split(b'\0', 1)[0].decode()
        if name not in blobs:
            continue
        updated.add(name)
        if struct.unpack_from('<I', original, pos + 64)[0] != 1:
            raise ValueError('Expected exactly one section in each empty prelink placeholder')
        low, high = intervals[name]
        offset = locations[name]
        prot = 5 if name == '__PLK_TEXT_EXEC' else 3
        struct.pack_into('<QQQQii', result, pos + 24, low, high - low, offset, len(blobs[name]), prot, prot)
        section = pos + 72
        size = len(info_blob) if name == '__PRELINK_INFO' else high - low
        struct.pack_into('<QQI', result, section + 32, low, size, offset)
        changed_headers.append((pos + 24, pos + 64))
        changed_headers.append((section + 32, section + 52))
    if updated != set(locations):
        raise ValueError('Missing prelink placeholders')
    # Check untouched original bytes, original segment contents and packed pointer readback.
    allowed = {i for start, end in changed_headers for i in range(start, end)}
    if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(original, result))):
        raise ValueError('Unexpected modification to original kernel')
    rs = segments(result)
    for module in stage['components']:
        content = (stage_dir / module['file']).read_bytes()
        for seg in segments(content):
            if seg['filesize']:
                offset = file_offset(rs, seg['address'], seg['filesize'])
                if result[offset:offset + seg['filesize']] != content[seg['fileoff']:seg['fileoff'] + seg['filesize']]:
                    raise ValueError('Embedded module segment differs from staged bytes')
    metadata = next(s for s in rs if s['name'] == '__PRELINK_INFO')
    parsed = decode_prelink(result[metadata['fileoff']:metadata['fileoff'] + metadata['filesize']])
    if len(parsed['_PrelinkInfoDictionary']) != len(aliases) + len(module_info):
        raise ValueError('Prelink XML roundtrip lost entries')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    report = dict(status='OFFLINE_LAYOUT_PROTOTYPE_DO_NOT_DEPLOY', bytes=len(result),
                  sha256=hashlib.sha256(result).hexdigest(), source_kernel_sha256=digest,
                  admission_gate_present=b'redwood_native_fairplay_gate_v1' in original,
                  kpi_metadata_aliases=[a['CFBundleIdentifier'] for a in aliases],
                  preserved_original_bytes=True, embedded_modules_verified=True,
                  layout={n: dict(start=hex(a), end=hex(b), fileoff=locations[n]) for n, (a, b) in intervals.items()})
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Offline prelink layout assembled:', len(result), 'bytes;', len(aliases),
          'KPI metadata aliases; admission gate present:', report['admission_gate_present'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('staged', type=Path)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('stock', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pack(args.kernel, args.staged, args.inventory, args.stock, args.output)
