#!/usr/bin/env python3
"""Read and checksum hubble1 factory calibration through fb.cal; never programs flash."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', default='s20', help='SSH alias')
p.add_argument('--output-dir', type=pathlib.Path, required=True)
a = p.parse_args()
a.output_dir.mkdir(parents=True, exist_ok=True)
for name in ('normal.bin', '120hs.bin', 'manifest.json'):
    if (a.output_dir / name).exists():
        p.error(f'{name} already exists; choose a new output directory')

def remote(command):
    return subprocess.check_output(['ssh', a.host, command], text=True, timeout=180)

ident = remote('/redwood/bin/rwsys kern.redwood.poke "fb.cal"')
if 'jedec 0 3: c2 28 14' not in ident:
    raise SystemExit('Unsupported/missing flash ID: ' + ident.strip())
commands = []
for base in (0xf0000, 0xf3000):
    for off in range(0, 0x26b7, 128):
        size = min(128, 0x26b7 - off)
        commands.append(f'/redwood/bin/rwsys kern.redwood.poke "fb.cal {base + off:#x} {size}"')
raw = remote('; '.join(commands))
chunks = {}
for address, size, data in re.findall(r'flash (0x[0-9a-f]+) (\d+): ([0-9a-f ]+)', raw):
    chunk = bytes.fromhex(data)
    if len(chunk) != int(size):
        raise SystemExit('Short read at ' + address)
    chunks[int(address, 16)] = chunk
manifest = {'jedec': 'c22814', 'partitions': {}}
validated = []
for base, name in ((0xf0000, 'normal'), (0xf3000, '120hs')):
    data = b''.join(chunks[base + off] for off in range(0, 0x26b7, 128))
    expected = int.from_bytes(data[0x26b4:0x26b6], 'big')
    actual = sum(data[:7392]) & 0xffff
    if actual != expected or data[0x26b6] != 1:
        raise SystemExit(f'{name}: invalid checksum/magic ({actual:04x}/{expected:04x})')
    manifest['partitions'][name] = {'address': hex(base), 'checksum': f'{actual:04x}',
                                   'sha256': hashlib.sha256(data).hexdigest()}
    validated.append((name, data))
for name, data in validated:
    with (a.output_dir / (name + '.bin')).open('xb') as f:
        f.write(data)
with (a.output_dir / 'manifest.json').open('x') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
print(json.dumps(manifest, indent=2))
