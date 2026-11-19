"""Bounded offline constructor inspection; never invokes native kernel APIs.

Executes ARM64 instructions in Unicorn only. Models OSMetaClass construction
as recording arguments and returning the supplied object. Stops at every other
external function. This can recover declared sizes, not prove runtime behavior.
"""
import argparse
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
from unicorn.arm64_const import UC_ARM64_REG_X3, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC
from inspect_kernel import commands, file_offset, segments, symbols

CTOR = '__ZN11OSMetaClassC2EPKcPKS_j'
BASES = ('__ZN8OSObject9MetaClassC2Ev', '__ZN9IOService9MetaClassC2Ev',
         '__ZN12IOUserClient9MetaClassC2Ev')
STACK, SENTINEL = 0x600000000, 0x610000000


def inspect_calls(path, roots, allowed, extra_images=()):
    data = path.read_bytes()
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    names = symbols(data)
    by_name = {n: a for a, ns in names.items() for n in ns}
    ranges = []
    images = [(data, segments(data))]
    for extra in extra_images:
        content = extra.read_bytes()
        images.append((content, segments(content)))
    for seg in (s for _, ss in images for s in ss):
        if not seg['size'] or seg['name'] == '__PAGEZERO':
            continue
        start = seg['address'] & ~4095
        end = (seg['address'] + seg['size'] + 4095) & ~4095
        ranges.append((start, end))
    merged = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    for start, end in merged:
        cpu.mem_map(start, end - start)
    for content, image_segments in images:
        for seg in image_segments:
            if seg['filesize']:
                cpu.mem_write(seg['address'], content[seg['fileoff']:seg['fileoff'] + seg['filesize']])
    cpu.mem_map(STACK, 0x200000)
    cpu.mem_map(SENTINEL, 4096)
    records = []
    local_classes = {}
    stopped = None
    returned = False

    def hook(machine, address, size, userdata):
        nonlocal stopped, returned
        if address == SENTINEL:
            returned = True
            machine.emu_stop()
            return
        if address == by_name[CTOR]:
            obj, name, parent, class_size = [machine.reg_read(r) for r in (
                UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3)]
            text = bytes(machine.mem_read(name, 128)).split(b'\0', 1)[0].decode('ascii')
            if not text or len(text) > 100 or not 0 < class_size <= 0x100000:
                raise ValueError('Unexpected metaclass arguments')
            records.append(dict(name=text, declared_size=class_size,
                                superclass=names.get(parent, []) or ([local_classes[parent]] if parent in local_classes else [])))
            local_classes[obj] = text
            machine.reg_write(UC_ARM64_REG_X0, obj)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return
        if not any(base <= address < base + length for base, length in allowed):
            stopped = dict(address=hex(address), symbols=names.get(address, []))
            machine.emu_stop()

    cpu.hook_add(UC_HOOK_CODE, hook)
    outcomes = []
    for root in roots:
        stopped, returned = None, False
        cpu.reg_write(UC_ARM64_REG_SP, STACK + 0x100000)
        cpu.reg_write(UC_ARM64_REG_X0, STACK + 0x180000)
        cpu.reg_write(UC_ARM64_REG_LR, SENTINEL)
        before = len(records)
        error = None
        try:
            cpu.emu_start(root, SENTINEL + 4, timeout=2000000, count=200000)
        except UcError as exc:
            error = str(exc)
        outcomes.append(dict(root=hex(root), returned=returned, stopped=stopped,
                             error=error, pc=hex(cpu.reg_read(UC_ARM64_REG_PC)),
                             classes=records[before:]))
    return outcomes


def standalone_roots(path):
    data = path.read_bytes()
    ss = segments(data)
    roots = []
    for command, pos, _ in commands(data):
        if command != 0x19:
            continue
        for index in range(struct.unpack_from('<I', data, pos + 64)[0]):
            sec = pos + 72 + 80 * index
            if data[sec:sec + 16].split(b'\0', 1)[0] != b'__mod_init_func':
                continue
            address, size = struct.unpack_from('<QQ', data, sec + 32)
            offset = file_offset(ss, address, size)
            roots.extend(r[0] for r in struct.iter_unpack('<Q', data[offset:offset + size]))
    return roots, [(s['address'], s['filesize']) for s in ss if s['name'] == '__TEXT_EXEC']


def module_roots(path, inventory):
    data = path.read_bytes()
    ss = segments(data)
    roots, allowed = [], []
    for component in inventory['components']:
        for seg in component['segments']:
            if seg['name'] == '__TEXT_EXEC':
                allowed.append((int(seg['address'], 16), seg['file_size']))
        address = next(int(s['address'], 16) for s in component['segments'] if s['name'] == '__TEXT')
        header = file_offset(ss, address)
        for command, pos, _ in commands(data, header):
            if command != 0x19:
                continue
            count = struct.unpack_from('<I', data, pos + 64)[0]
            for index in range(count):
                sec = pos + 72 + 80 * index
                name = data[sec:sec + 16].split(b'\0', 1)[0]
                if name != b'__mod_init_func':
                    continue
                pointer, size = struct.unpack_from('<QQ', data, sec + 32)
                offset = file_offset(ss, pointer, size)
                roots.extend(r[0] for r in struct.iter_unpack('<Q', data[offset:offset + size]))
    return roots, allowed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock', type=Path)
    parser.add_argument('custom', type=Path)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--staged', type=Path)
    args = parser.parse_args()
    report = dict(model='OSMetaClass constructor records arguments only; all other externals stop')
    for label, path in (('stock_bases', args.stock), ('custom_bases', args.custom)):
        by_name = {n: a for a, ns in symbols(path.read_bytes()).items() for n in ns}
        roots = [by_name[n] for n in BASES]
        report[label] = inspect_calls(path, roots, [(r, 128) for r in roots])
    roots, allowed = module_roots(args.stock, json.loads(args.inventory.read_text()))
    report['stock_module_initializers'] = inspect_calls(args.stock, roots, allowed)
    if args.staged:
        paths = [args.staged / (n + '.macho') for n in ('FairPlayIOKit', 'IOTextEncryptionFamily')]
        roots, allowed = [], []
        for path in paths:
            r, a = standalone_roots(path)
            roots.extend(r)
            allowed.extend(a)
        report['staged_module_initializers'] = inspect_calls(args.custom, roots, allowed, paths)
    outcomes = [outcome for key, values in report.items() if key != 'model' for outcome in values]
    report['all_returned_without_unknown_external'] = all(o['returned'] and not o['error'] and not o['stopped'] for o in outcomes)
    report['base_declared_sizes_match'] = ([o['classes'] for o in report['stock_bases']] ==
                                          [o['classes'] for o in report['custom_bases']])
    if args.staged:
        report['staged_class_records_match'] = ([o['classes'] for o in report['stock_module_initializers']] ==
                                                [o['classes'] for o in report['staged_module_initializers']])
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for label in report:
        if not isinstance(report[label], list):
            continue
        for result in report[label]:
            print(label, 'returned', result['returned'], 'classes', result['classes'],
                  'stopped', result['stopped'], 'error', result['error'])
    if not report['all_returned_without_unknown_external'] or not report['base_declared_sizes_match'] or not report.get('staged_class_records_match', True):
        raise RuntimeError('Initializer or declared-size comparison failed; see output report')
