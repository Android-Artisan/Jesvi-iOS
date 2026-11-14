#!/usr/bin/env python3
"""One entry point for source checks, kernel builds and private image assembly."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def profile(name):
    if not re.fullmatch('[a-z0-9]+', name):
        raise ValueError('invalid device id')
    p = ROOT / 'devices/exynos990' / name / 'device.json'
    if not p.is_file():
        raise ValueError(f'unknown device: {name}')
    d = json.loads(p.read_text())
    if d['id'] != name or d['platform'] != 'exynos990' or d['schema'] != 1:
        raise ValueError(f'invalid profile: {name}')
    if d['status'] not in ('development', 'unmaintained', 'validated'):
        raise ValueError(f'invalid status: {name}')
    if d['installer_validated']:
        # No device is qualified yet. A future qualification PR must change this
        # code and include its evidence, rather than flipping a JSON boolean.
        raise ValueError('installer qualification is not implemented; see docs/release/flashing.md')
    if d['status'] != 'unmaintained':
        if not d['models'] or not d['bootloaders'] or not d['evidence']:
            raise ValueError(f'missing identity/evidence: {name}')
        for part in ('boot', 'recovery', 'super', 'userdata'):
            if not isinstance(d['partitions'].get(part), int) or d['partitions'][part] <= 0:
                raise ValueError(f'missing partition size: {name}/{part}')
        l = d['layout']
        if not (l['system_offset'] + l['system_max_bytes'] <= l['super_log_offset'] <
                d['partitions']['super']):
            raise ValueError('system/log overlap')
        if not (l['log_offset'] + l['log_bytes'] <= l['kernel_offset'] and
                l['kernel_offset'] + l['kernel_max_bytes'] <= l['root_offset'] <
                d['partitions']['recovery']):
            raise ValueError('recovery slots overlap')
    return d


def run(*args, env=None):
    subprocess.run(args, cwd=ROOT, env=env, check=True)


def doctor(device, images=False):
    d = profile(device)
    problems = []
    if d['status'] == 'unmaintained':
        problems.append('target has no qualified board profile or build adapter')
    if platform.system() != 'Darwin':
        problems.append('the release build adapter currently requires macOS')
    for tool in ('git', 'python3', 'clang', 'make'):
        if shutil.which(tool) is None:
            problems.append(f'missing host tool: {tool}')
    if ' ' in str(ROOT):
        problems.append('XNU makefiles require a checkout path without spaces')
    if images:
        for p in ('sysimage/rootfs', 'sysimage/cryptex', 'sysimage/cryptex-app',
                  'sysimage/system.manifest', 'sysimage/20F66__iPhone10,3_6/dyld_shared_cache_arm64',
                  'inputs/x1slte/Mu-x1slte.img', 'inputs/x1slte/boot-dtb.img',
                  'inputs/tools/avbtool.py', 'inputs/tools/testkey_rsa4096.pem',
                  'inputs/toolchain.json', 'inputs/private-inputs.lock.json'):
            if not (ROOT / p).exists():
                problems.append(f'missing private/local input: {p}')
        for p in ('ssh', 'apps', 'common/devices/x1slte.local.conf', 'fw-wifi/network'):
            if (ROOT / p).exists():
                problems.append(f'personal input present; use a dedicated sanitized checkout: {p}')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('devices')
    sub.add_parser('validate')
    sub.add_parser('lock-inputs')
    p = sub.add_parser('package')
    p.add_argument('--device', default='x1slte')
    p.add_argument('--output', required=True, type=Path)
    for cmd in ('doctor', 'kernel', 'images'):
        p = sub.add_parser(cmd)
        p.add_argument('--device', default='x1slte')
        if cmd == 'doctor':
            p.add_argument('--images', action='store_true')
        elif cmd == 'kernel':
            p.add_argument('--jobs', type=int, default=4)
        elif cmd == 'images':
            p.add_argument('--development', action='store_true',
                           help='acknowledge unqualified legacy prebuilts; private candidate only')
    a = parser.parse_args()
    if a.command == 'lock-inputs':
        from private_inputs import write
        write()
        return 0
    if a.command == 'package':
        from package import package
        package(a.device, a.output)
        return 0
    if a.command in ('devices', 'validate'):
        for p in sorted((ROOT / 'devices/exynos990').glob('*/device.json')):
            d = profile(p.parent.name)
            print(f'{d["id"]:8} {d["status"]:13} {d["name"]}; installer: unqualified')
        return 0
    problems = doctor(a.device, images=a.command == 'images' or getattr(a, 'images', False))
    if problems:
        for problem in problems:
            print('BLOCKED: ' + problem, file=sys.stderr)
        return 1
    if a.command == 'doctor':
        print('Host prerequisites found. Run kernel to verify the actual compiler/toolchain.')
        return 0
    if a.command == 'kernel':
        if a.device != 'x1slte' or not 1 <= a.jobs <= 64:
            raise ValueError('only the x1slte kernel adapter is qualified; jobs must be 1..64')
        run('bash', 'ci/build-kernel.sh', str(a.jobs))
    elif a.command == 'images':
        if a.device != 'x1slte' or not a.development:
            raise ValueError('full images are private development candidates only; pass --development')
        from private_inputs import verify
        from images import hfs_kit
        verify()
        hfs_kit(ROOT / 'userland/hfs-kit.bin', profile(a.device)['partitions']['userdata'])
        env = dict(os.environ, SHARE='1', REST='1', IOS_INPUT_DIR='../sysimage', REQUIRE_SOURCE='1')
        env['HACKIOS_IMAGE_CACHE'] = str(Path(env.get('HACKIOS_IMAGE_CACHE', ROOT / '.cache/private-images')).resolve())
        # Build locally, never export xnu-src over the source under review.
        run('bash', 'ci/build-kernel.sh', '4')
        shutil.copyfile(ROOT / 'out/kernel/mach.x1slte', ROOT / 'out/mach.x1slte')
        run('bash', 'userland/build-gpu-y2s.sh', env=env)
        for name in ('root.apfs', 'system.apfs'):
            if not (ROOT / 'userland' / name).is_file():
                raise ValueError(f'image builder produced no {name}')
        run('bash', 'x1slte.sh', 'embed', env=env)
        run('bash', 'x1slte.sh', 'payload', (ROOT / 'x1slte.cmdline').read_text().strip(), env=env)
        run('bash', 'x1slte.sh', 'pack', env=env)
        verify()
        print('Private development images built. Next: build.sh package --output dist/CANDIDATE')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        sys.exit(f'build: {exc}')
