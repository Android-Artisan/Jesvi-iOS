#!/usr/bin/env python3
"""Locate named blocks and inspect decoded fields in Samsung ISV8 ISP tuning.

Offline only. Requires the layout recovered from the DDK identified in the schema.
Does not convert or install tuning. Firmware compatibility is a research hypothesis,
not established by matching container/version strings. Keep output outside git.
"""
import argparse
import hashlib
import importlib.util
import json
import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('container', HERE / 'inspect-samsung-setfile.py')
container = importlib.util.module_from_spec(spec)
spec.loader.exec_module(container)


def load_schema(path=HERE / 'samsung-isv8-schema.json'):
    schema = json.loads(path.read_text())
    offset = 8
    names = set()
    for block in schema['blocks']:
        if block['key'] in names or block['offset'] != offset or block['size'] <= 0:
            raise ValueError('non-contiguous, duplicate or empty schema block')
        names.add(block['key'])
        offset += block['size']
    if offset != schema['serialized_bytes'] + 8:
        raise ValueError('schema total does not match blocks')
    return schema


def split_payload(payload, schema):
    if len(payload) < 8:
        raise ValueError('truncated tuning header')
    magic, size = struct.unpack_from('<II', payload)
    if magic != int(schema['payload_magic'], 0) or size != schema['serialized_bytes']:
        raise ValueError('unsupported tuning magic or serialized size')
    end = 8 + size
    if len(payload) != ((end + 15) & ~15):
        raise ValueError('tuning entry length is not expected 16-byte padded size')
    if any(payload[end:]):
        raise ValueError('nonzero tuning entry padding')
    return {b['key']: payload[b['offset']:b['offset'] + b['size']] for b in schema['blocks']}


CCM_NAMES = (['outdoorCcm', 'shadyCcm'] + [f'indoorCcm{i}' for i in range(6)]
             + ['lowLightCcm'] + [f'lowLightCcm{i}' for i in range(6)] + ['darknessCcm']
             + ['outdoorFaceCcm', 'shadyFaceCcm'] + [f'indoorFaceCcm{i}' for i in range(6)]
             + ['lowLightFaceCcm'] + [f'lowLightFaceCcm{i}' for i in range(6)]
             + ['flashLowLightCcm', 'flashCwfCcm', 'flashD65Ccm'])


def decode_ccm(data, values=False):
    if len(data) != 0x20f4:
        raise ValueError('unsupported CCM size')
    records = []
    for i, name in enumerate(CCM_NAMES):
        at = 0x78 + i * 0xf4
        record = dict(name=name, offset=at)
        # Signed 16-bit loads in SDK serializer 0x1ea9d4, NOT float matrices.
        for key, offset in (('PreCCM', 0), ('OuterCCM', 18)):
            v = struct.unpack_from('<9h', data, at + offset)
            record[key] = dict(row_sums=[sum(v[j:j+3]) for j in range(0, 9, 3)],
                               identity_256=v == (256, 0, 0, 0, 256, 0, 0, 0, 256))
            if values:
                record[key]['signed_coefficients'] = v
        # Additional vector transforms are material; PreCCM alone is not final colour correction.
        if values:
            for key, offset, count in (('VectorsHigh', 0x24, 27), ('VectorsLow', 0x5a, 27),
                                       ('VectorsCcm7', 0x90, 21), ('VectorsCcm9', 0xba, 27)):
                record[key] = struct.unpack_from(f'<{count}h', data, at + offset)
        records.append(record)
    return dict(isEnabled=data[0], useGMapCCM=data[1],
                enableSaturatedInputInterpolation=data[2], useSignedOutput=data[3],
                awbLowlightCcmCord=struct.unpack_from('<6H', data, 8),
                awbCcmCord=struct.unpack_from('<6H', data, 0x14),
                records=records, coefficient_scale='256 unity is inferred; runtime composition still unresolved')


def decode_gras_lut(data, values=False):
    if len(data) != 0x67a4:
        raise ValueError('unsupported GRAS LUT size')
    # Serializer 0x200a48: one u16, four 3300-element i16 arrays,
    # two bytes of struct alignment, then two 16-element u32 arrays.
    table_size, = struct.unpack_from('<H', data)
    if table_size != 6600:
        raise ValueError('unsupported GRAS tableSize')
    out = dict(tableSize=table_size, grid_class='33_25',
               warning='signed LUT representation and colour-plane order are not gain maps yet')
    for i, name in enumerate(('lut', 'lutRef', 'lutActPosDiff', 'lutColorTempDiff')):
        at = 2 + i * 6600
        v = struct.unpack_from('<3300h', data, at)
        out[name] = dict(offset=at, count=3300, minimum=min(v), maximum=max(v),
                         sha256=hashlib.sha256(data[at:at + 6600]).hexdigest())
        if values:
            out[name]['signed_values'] = v
    for name, offset in (('lutWeight', 0x6724), ('lutTh', 0x6764)):
        out[name] = struct.unpack_from('<16I', data, offset)
    return out


def decode_blc(data, values=False):
    """Field types/offsets from SDK serializer at file offset 0x1ee55c."""
    if len(data) != 0x180:
        raise ValueError('unsupported BLC size')
    out = dict(isEnabled=data[0], applyDgain=data[1],
               pixelMaximalLimit=struct.unpack_from('<h', data, 2)[0],
               pixelMinimalLimit=struct.unpack_from('<h', data, 4)[0],
               selectOffsetType=struct.unpack_from('<I', data, 8)[0],
               channel_order='unresolved; preserve four-site order',
               units='ISP-domain units; do not subtract directly from RAW10')
    for name, offset, fmt in (('offsetsTableFixed', 0xc, 'h'),
                               ('offsetsAfterGainTableFixed', 0x14, 'h'),
                               ('gainsTableFixed', 0x1c, 'H')):
        if values:
            out[name] = struct.unpack_from('<4' + fmt, data, offset)
    for name, count_at, data_at, fmt, columns in (
            ('noiseIndex', 0x24, 0x28, 'f', 1),
            ('totalGain', 0x4c, 0x50, 'I', 1),
            ('bv', 0x74, 0x78, 'i', 1),
            ('offsetTableVariable', 0x9c, 0xa0, 'h', 4),
            ('offsetTableVariableSecond', 0xe8, 0xec, 'h', 4),
            ('gainTableVariable', 0x134, 0x138, 'H', 4)):
        count = struct.unpack_from('<I', data, count_at)[0]
        if count > 9:
            raise ValueError('BLC array count exceeds fixed capacity')
        entry = dict(count=count)
        if values:
            raw = struct.unpack_from(f'<{count * columns}{fmt}', data, data_at)
            entry['values'] = ([raw[i:i+columns] for i in range(0, len(raw), columns)]
                               if columns > 1 else raw)
        out[name] = entry
    return out


def scenario_names(source):
    """Read named enum assignments; reject expressions rather than evaluating C."""
    clean = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    match = re.search(r'enum\s+is_subscenario_id\s*\{(.*?)\}', clean, re.S)
    if match is None:
        raise ValueError('missing is_subscenario_id enum')
    symbols, names, current = {}, {}, -1
    for item in match[1].split(','):
        if not item.strip():
            continue
        parts = item.strip().split('=')
        name = parts[0].strip()
        if not re.fullmatch(r'ISS_SUB_(?:SCENARIO_[A-Z0-9_]+|END)', name):
            raise ValueError('unsupported scenario symbol')
        if len(parts) == 1:
            current += 1
        elif len(parts) == 2:
            token = parts[1].strip()
            if re.fullmatch(r'0[xX][0-9a-fA-F]+|[0-9]+', token):
                current = int(token, 0)
            elif token in symbols:
                current = symbols[token]
            else:
                raise ValueError('unsupported scenario expression')
        else:
            raise ValueError('invalid scenario assignment')
        symbols[name] = current
        if name != 'ISS_SUB_END':
            names.setdefault(current, []).append(name)
    return names


def inspect(data, schema, values=False, scenarios=None):
    metadata = container.inspect(data)
    if metadata['format'] != 3 or metadata['version'] != 'ISV8':
        raise ValueError('only ISV8 v3 containers supported')
    group = next((g for g in metadata['groups'] if g['design'] == '3AA_ISP'), None)
    if group is None:
        raise ValueError('container has no 3AA/ISP group')
    result = dict(sha256=metadata['sha256'], schema_ddk_sha256=schema['ddk_sha256'],
                  compatibility='layout validated; DDK/firmware provenance and runtime use remain unverified',
                  entries=[])
    for entry in group['entries']:
        start, size = entry['offset'], entry['size']
        blocks = split_payload(data[start:start + size], schema)
        names = []
        for block in schema['blocks']:
            name = block['key']
            names.append(dict(key=name, offset=start + block['offset'], size=block['size'],
                              sha256=hashlib.sha256(blocks[name]).hexdigest()))
        result['entries'].append(dict(index=entry['index'], scenarios=entry['scenarios'],
            scenario_names={str(s): scenarios.get(s, []) for s in entry['scenarios']} if scenarios else {}, blocks=names,
            ccm=decode_ccm(blocks['ccmTuning'], values),
            gras_lut=decode_gras_lut(blocks['grasLutTuning'], values),
            blc={key: decode_blc(blocks[key], values) for key in ('blcStrp', 'blcDns')}))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    parser.add_argument('--values', action='store_true', help='include signed tuning arrays; keep output local')
    parser.add_argument('--scenario-header', type=Path, help='local matching is-metadata-vendor.h; labels remain source-version dependent')
    args = parser.parse_args()
    try:
        source = args.scenario_header.read_text() if args.scenario_header else None
        result = inspect(args.file.read_bytes(), load_schema(), args.values,
                         scenario_names(source) if source else None)
        if source is not None:
            result['scenario_header_sha256'] = hashlib.sha256(source.encode()).hexdigest()
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'tuning: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
