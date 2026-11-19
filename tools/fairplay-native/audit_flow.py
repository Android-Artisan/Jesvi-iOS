"""Conservative offline flow scan seeded by native KASLR code-pointer targets.

Requires capstone. Seeds may include data; computed branches are not resolved.
This scan is supporting evidence, never an admission check for live execution.
"""
import argparse
import collections
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, arm64
from inspect_kernel import file_offset, segments

MASK = (1 << 64) - 1


def audit(kernel, inventory):
    data = kernel.read_bytes()
    parent_segments = segments(data)
    copied = [(int(s['address'], 16), s['vm_size']) for c in inventory['components']
              for s in c['segments'] if s['vm_size']]
    code = [(int(s['address'], 16), s['file_size']) for c in inventory['components']
            for s in c['segments'] if s['name'] == '__TEXT_EXEC']

    def inside(address):
        return any(base <= address < base + size for base, size in code)

    roots = {int(r['target'], 16) for c in inventory['components'] for r in c['kaslr_relocations']
             if r['classification'] == 'component' and inside(int(r['target'], 16))
             and int(r['target'], 16) % 4 == 0}
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    decoder.detail = True
    todo, seen, invalid = list(roots), set(), []
    branches, pages, indirect = collections.Counter(), collections.Counter(), collections.Counter()
    while todo:
        pc = todo.pop()
        while pc not in seen and inside(pc):
            seen.add(pc)
            offset = file_offset(parent_segments, pc, 4)
            ins = next(decoder.disasm(data[offset:offset + 4], pc), None)
            if ins is None:
                invalid.append(hex(pc))
                break
            if ins.mnemonic == 'adrp':
                target = ins.operands[1].imm & MASK
                if not any(base // 4096 <= target // 4096 <= (base + size - 1) // 4096 for base, size in copied):
                    pages[hex(target)] += 1
            if ins.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.'):
                target = next(o.imm & MASK for o in reversed(ins.operands) if o.type == arm64.ARM64_OP_IMM)
                if inside(target):
                    todo.append(target)
                else:
                    branches[hex(target)] += 1
                if ins.mnemonic == 'b':
                    break
            if ins.mnemonic in ('br', 'blr'):
                indirect[ins.mnemonic] += 1
            if ins.mnemonic in ('ret', 'br', 'eret', 'brk'):
                break
            pc += 4
    return dict(status='INCOMPLETE_STATIC_EVIDENCE', roots=len(roots),
                decoded_instruction_addresses=len(seen), invalid_decodes=sorted(invalid),
                indirect_instructions=dict(indirect), external_direct_branches=dict(branches),
                external_adrp_pages=dict(pages),
                limitations=['Pointer seeds can identify data as code.',
                             'Computed branches and runtime-generated addresses are unresolved.',
                             'No instruction execution or runtime ABI validation performed.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.kernel, json.loads(args.inventory.read_text()))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('roots', result['roots'], 'instructions', result['decoded_instruction_addresses'],
          'invalid_decodes', len(result['invalid_decodes']),
          'external_direct_branches', len(result['external_direct_branches']),
          'external_adrp_pages', len(result['external_adrp_pages']))
