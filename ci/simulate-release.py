#!/usr/bin/env python3
"""TEMPORARY CI simulation helper: emit placeholder release artifacts.

This is only for validating the hosted workflow plumbing. The ZIPs are marked
`simulated` and `NOT FLASHABLE`; they do not contain Jesvi iOS images.
"""
import hashlib
import io
import json
import sys
import tarfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/release'))
import release_config

out = Path(sys.argv[1])
rest = sys.argv[2:]
overrides = []
while rest:
    flag = rest.pop(0)
    if flag != '--set':
        raise SystemExit(f'unexpected argument: {flag}')
    overrides.append(rest.pop(0))
conf = release_config.load(overrides=overrides)
todo, skipped = release_config.plan(conf)
for target, reason in skipped:
    print(f'SKIP {target}: {reason}')
out.mkdir(parents=True, exist_ok=True)
for target, output, path in todo:
    action = 'restore' if output == 'restore' else 'install'
    allow_writes = conf['WRITES'] and output != 'preflight'
    config = {'schema': 2, 'simulated': True, 'action': action, 'device': target,
              'mode': conf['MODE'], 'allow_writes': allow_writes,
              'installer_validated': False}
    raw = (json.dumps(config, indent=2, sort_keys=True) + '\n').encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    if output == 'tar':
        with tarfile.open(path, 'w') as tar:
            info = tarfile.TarInfo('installer.json')
            info.size = len(raw)
            tar.addfile(info, io.BytesIO(raw))
    else:
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as z:
            z.writestr('installer.json', raw)
            z.writestr('README.txt',
                       f'SIMULATED {output} package for CI plumbing test. NOT FLASHABLE.\n'
                       f'target={target} mode={conf["MODE"]} writes={allow_writes}\n'
                       f'config_sha256={hashlib.sha256(raw).hexdigest()}\n')
            z.writestr('META-INF/com/google/android/updater-script',
                       '# simulated Jesvi iOS recovery package\n')
    Path(str(path) + '.sha256').write_text(
        f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n')
    Path(str(path) + '.build.json').write_text(json.dumps(
        {'simulated': True, 'device': target, 'output': output, 'mode': conf['MODE'],
         'writes_enabled': allow_writes, 'installer_validated': False}, indent=2) + '\n')
    print(f'SIMULATED {target} {output} -> {path.relative_to(Path.cwd())}; writes={allow_writes}')
