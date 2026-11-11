#!/usr/bin/env python3
"""Check S20 CP reserved memory against loader/audio sources and a saved boot log.

Offline only. A clean report is necessary, not sufficient, for modem DMA safety.
Unknown dynamic allocations keep the report unverified (exit 2). Conflicts exit 1.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CP = [('cp_rmem', 0xf0000000, 0x800000),
      ('cp_msi_rmem', 0xf6200000, 0x1000),
      ('cp_rmem_2', 0xd0000000, 0x4000000)]


def overlaps(first, second):
    return first[1] < second[1] + second[2] and second[1] < first[1] + first[2]


def literal_define(source, name):
    match = re.search(r'^#define\s+' + re.escape(name) +
                      r'\s+(0[xX][0-9a-fA-F]+|[0-9]+)(?:[uUlL]*)\b', source, re.M)
    if not match:
        raise ValueError(f'cannot resolve literal {name}; audit source manually')
    return int(match[1], 0)


def audit(root, log_text):
    loader = (root / 'xnuloader/loader.c').read_text()
    sound = (root / 'xnu/files/bsd/dev/redwood_exynos_sound.c').read_text()
    allocations = [('loader DOWNLOAD', literal_define(loader, 'DOWNLOAD_BASE'),
                    literal_define(loader, 'DOWNLOAD_MAX_SIZE'))]
    for stem in ('DRAM', 'SLOG', 'DUMP', 'PCI'):
        allocations.append(('Calliope ' + stem, literal_define(sound, 'CAL_' + stem + '_PHYS'),
                            literal_define(sound, 'CAL_' + stem + '_SIZE')))
    allocations.append(('Calliope VSS spare', literal_define(sound, 'CAL_VSS_SPARE_PHYS'), 1 << 20))
    unknown = []
    # Most recent occurrence: logs may contain several boots.
    for label, pattern in (
        ('display pool', r'display pool:\s*([0-9a-fA-F]+)\s+size\s+([0-9a-fA-F]+)'),
        ('wifi dma', r'wifi dma:\s*([0-9a-fA-F]+)\s+size\s+([0-9a-fA-F]+)'),
        ('XNU window', r'phys window\s+([0-9a-fA-F]+)\s+size\s+([0-9a-fA-F]+)'),
    ):
        matches = re.findall(pattern, log_text)
        if matches:
            allocations.append((label, int(matches[-1][0], 16), int(matches[-1][1], 16)))
        else:
            unknown.append(label)
    conflicts = []
    for cp in CP:
        for allocation in allocations:
            if overlaps(cp, allocation):
                start = max(cp[1], allocation[1])
                end = min(cp[1] + cp[2], allocation[1] + allocation[2])
                conflicts.append(dict(cp=cp[0], owner=allocation[0],
                                      overlap_start=hex(start), overlap_size=hex(end - start)))
    return dict(scope='source candidates and saved-log allocations; not a hardware access grant',
                conflicts=conflicts, unknown=unknown,
                cp_regions=[dict(name=n, base=hex(b), size=hex(s)) for n, b, s in CP],
                allocations=[dict(name=n, base=hex(b), size=hex(s)) for n, b, s in allocations])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--boot-log', type=Path, help='saved loader/kernel log; never contacts phone')
    args = parser.parse_args()
    try:
        report = audit(args.root, args.boot_log.read_text(errors='replace') if args.boot_log else '')
    except (OSError, ValueError) as error:
        parser.exit(2, f'{error}\n')
    print(json.dumps(report, indent=2))
    return 1 if report['conflicts'] else 2 if report['unknown'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
