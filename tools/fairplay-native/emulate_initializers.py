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


def inspect_calls(path, roots, allowed, extra_images=(), service_probes=None):
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
    class_objects = {}
    mode, events = 'initializers', []
    heap_next = STACK + 0x140000
    stopped = None
    returned = False

    def hook(machine, address, size, userdata):
        nonlocal stopped, returned, heap_next
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
            class_objects[text] = obj
            machine.reg_write(UC_ARM64_REG_X0, obj)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return
        if mode != 'initializers' and address in names:
            labels = names[address]
            label = labels[0]
            args = [machine.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                                 UC_ARM64_REG_X2, UC_ARM64_REG_X3)]
            modeled = False
            result = 0
            if '_OSObject_typed_operator_new' in labels or '_IOMallocTypeImpl' in labels:
                # arm64 kalloc_type_view: zone_view32 + signature8 + flags4,
                # followed by kt_size; the high byte is the packed zone index.
                amount = args[1] if '_OSObject_typed_operator_new' in labels else struct.unpack('<I', machine.mem_read(args[0] + 44, 4))[0] & 0xffffff
                if not 0 < amount <= 65536:
                    raise ValueError('Unexpected modeled allocation size')
                result = heap_next
                heap_next += (amount + 15) & ~15
                if heap_next > STACK + 0x170000:
                    raise ValueError('Modeled heap exhausted')
                machine.mem_write(result, b'\0' * amount)
                modeled = True
            elif any(n in labels for n in ('__ZN9IOServiceC1EPK11OSMetaClass', '__ZN9IOServiceC2EPK11OSMetaClass')):
                # Ordinary base construction is omitted, not claimed correct.
                # This trace only identifies imports reached by native startup.
                result, modeled = args[0], True
            elif '__ZN9IOService5startEPS_' in labels:
                result, modeled = 1, True  # The source implementation returns true.
            elif any(n in labels for n in ('_IOLockAlloc', '_IORWLockAlloc')):
                result, modeled = heap_next, True
                heap_next += 64
                machine.mem_write(result, b'\0' * 64)
            elif any(n in labels for n in ('_memset', '_secure_memset', '_memcpy', '_memmove')):
                if args[2] > 0x100000:
                    raise ValueError('Modeled memory operation exceeds its bound')
                block = bytes([args[1] & 255]) * args[2] if any(n in labels for n in ('_memset', '_secure_memset')) else bytes(machine.mem_read(args[1], args[2]))
                machine.mem_write(args[0], block)
                result, modeled = args[0], True
            elif any(n in labels for n in ('_IOLockLock', '_IOLockUnlock', '_IORWLockRead', '_IORWLockWrite',
                                           '_IORWLockUnlock', '_IOLog', '_IOLogv',
                                           '__ZNK11OSMetaClass19instanceConstructedEv',
                                           '__ZN9IOService15registerServiceEj', '_text_crypter_create_hook_set')):
                modeled = True
            if modeled:
                events.append(dict(api=label, model='ordinary startup scaffolding only'))
                machine.reg_write(UC_ARM64_REG_X0, result)
                machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
                return
            details = dict(address=hex(address), symbols=labels)
            if any(n in labels for n in ('__ZN9IOService15serviceMatchingEPKcP12OSDictionary',
                                         '_kernel_sysctlbyname', '_sysctlbyname')):
                value = bytes(machine.mem_read(args[0], 128)).split(b'\0', 1)[0]
                if value and all(32 <= c < 127 for c in value):
                    details['query'] = value.decode('ascii')
            events.append(dict(api=label, unmodeled=True))
            stopped = details
            machine.emu_stop()
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
    if service_probes is not None:
        def call_probe(entry, object_address, provider=0):
            nonlocal stopped, returned
            stopped, returned = None, False
            cpu.reg_write(UC_ARM64_REG_SP, STACK + 0x100000)
            cpu.reg_write(UC_ARM64_REG_X0, object_address)
            cpu.reg_write(UC_ARM64_REG_X1, provider)
            cpu.reg_write(UC_ARM64_REG_LR, SENTINEL)
            error = None
            try:
                cpu.emu_start(entry, SENTINEL + 4, timeout=2000000, count=200000)
            except UcError as exc:
                error = str(exc)
            return dict(returned=returned, stopped=stopped, error=error,
                        pc=hex(cpu.reg_read(UC_ARM64_REG_PC))), cpu.reg_read(UC_ARM64_REG_X0)

        for class_name in ('com_apple_driver_FairPlayIOKit', 'AppleFairplayTextCrypter'):
            if class_name not in class_objects:
                continue
            events = []
            mode = 'allocate'
            meta = class_objects[class_name]
            vtable = struct.unpack('<Q', cpu.mem_read(meta, 8))[0]
            # ABI check: OSMetaClass alloc is slot15 in table, address point+13.
            entry = struct.unpack('<Q', cpu.mem_read(vtable + 13 * 8, 8))[0]
            allocation, object_address = call_probe(entry, meta)
            report = dict(name=class_name, allocation=allocation, events=events)
            if allocation['returned'] and object_address:
                mode = 'service_start'
                provider = STACK + 0x190000
                provider_vtable = by_name.get('__ZTV11IOResources', 0)
                cpu.mem_write(provider, struct.pack('<Q', provider_vtable + 16))
                object_vtable = struct.unpack('<Q', cpu.mem_read(object_address, 8))[0]
                # IOService start is slot88 in table, address point+86.
                start = struct.unpack('<Q', cpu.mem_read(object_vtable + 86 * 8, 8))[0]
                report['start'], result = call_probe(start, object_address, provider)
                report['start']['return_register'] = result
            report['limitation'] = 'Dependency trace with omitted base construction; not real driver initialization or licensing.'
            service_probes.append(report)
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
    parser.add_argument('--probe-service-start', action='store_true')
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
        probes = [] if args.probe_service_start else None
        report['staged_module_initializers'] = inspect_calls(args.custom, roots, allowed, paths, probes)
    outcomes = [outcome for key, values in report.items() if key != 'model' for outcome in values]
    report['all_returned_without_unknown_external'] = all(o['returned'] and not o['error'] and not o['stopped'] for o in outcomes)
    report['base_declared_sizes_match'] = ([o['classes'] for o in report['stock_bases']] ==
                                          [o['classes'] for o in report['custom_bases']])
    if args.staged:
        report['staged_class_records_match'] = ([o['classes'] for o in report['stock_module_initializers']] ==
                                                [o['classes'] for o in report['staged_module_initializers']])
        if probes is not None:
            report['service_start_dependency_probes'] = probes
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for label in report:
        if not isinstance(report[label], list):
            continue
        if label == 'service_start_dependency_probes':
            for result in report[label]:
                print(label, result['name'], 'allocation', result['allocation'], 'start', result.get('start'))
            continue
        for result in report[label]:
            print(label, 'returned', result['returned'], 'classes', result['classes'],
                  'stopped', result['stopped'], 'error', result['error'])
    if not report['all_returned_without_unknown_external'] or not report['base_declared_sizes_match'] or not report.get('staged_class_records_match', True):
        raise RuntimeError('Initializer or declared-size comparison failed; see output report')
