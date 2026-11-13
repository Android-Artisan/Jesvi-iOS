#!/usr/bin/env python3
"""Record provenance of a source-built kernel; a kernel is not a flashable image."""
import json
from pathlib import Path
import platform
import subprocess
from cache import digest

root = Path(__file__).resolve().parents[2]
out = root / 'out/kernel'
def command(*args):
    return subprocess.check_output(args, cwd=root, text=True).strip()
manifest = {
    'schema': 1, 'kind': 'development-kernel-not-flashable', 'device': 'x1slte',
    'source_commit': command('git', 'rev-parse', 'HEAD'),
    'source_dirty': bool(command('git', 'status', '--porcelain', '--untracked-files=no')),
    'platform': platform.platform(),
    'compiler': command('/usr/bin/xcrun', 'clang', '--version'),
    'dependency_lock_sha256': digest(root / 'ci/sources.lock.json'),
    'kernel_source_key': command('python3', 'tools/release/source_key.py'),
    'sha256': digest(out / 'mach.x1slte'), 'size': (out / 'mach.x1slte').stat().st_size,
}
(out / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'SHA256SUMS').write_text(f'{manifest["sha256"]}  mach.x1slte\n')
print(json.dumps(manifest, indent=2))
