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
        raise ValueError('installer hardware qualification is not recorded; see docs/release/twrp.md')
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
    sub.add_parser('identity')
    p = sub.add_parser('local')
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--check-only', action='store_true')
    p.add_argument('--twrp', action='store_true', help='also build a TWRP preflight ZIP')
    p.add_argument('--experimental', action='store_true', help='with --twrp, enable unqualified writes after physical confirmation')
    p = sub.add_parser('twrp')
    p.add_argument('--bundle', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--mode', choices=['choose', 'update', 'fresh'], default='choose')
    p.add_argument('--experimental', action='store_true', help='enable unqualified device writes after physical confirmation')
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
                           help='acknowledge unqualified private development candidate')
    a = parser.parse_args()
    if a.command == 'twrp':
        from twrp_package import package
        package(a.bundle, a.output, a.mode, a.experimental)
        return 0
    if a.command == 'identity':
        tree = ROOT / 'sysimage/DeviceTree.d22ap.im4p'
        if not tree.is_file():
            run('python3', 'sysimage/fetch-from-ipsw.py', 'Firmware/all_flash/DeviceTree.d22ap.im4p', str(tree) + '.part')
            Path(str(tree) + '.part').replace(tree)
        run('bash', 'xnuloader/mkidentity.sh', str(tree))
        return 0
    if a.command == 'local':
        from local_build import build
        if a.experimental and not a.twrp:
            raise ValueError('--experimental requires --twrp')
        build(a.output, a.check_only, a.twrp, a.experimental)
        return 0
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
        from extraction import verify as verify_extraction
        verify_extraction(ROOT)
        verify()
        hfs_kit(ROOT / 'userland/hfs-kit.bin', profile(a.device)['partitions']['userdata'])
        env = dict(os.environ, SHARE='1', REST='1', IOS_INPUT_DIR='../sysimage', REQUIRE_SOURCE='1')
        env['HACKIOS_IMAGE_CACHE'] = str(Path(env.get('HACKIOS_IMAGE_CACHE', ROOT / '.cache/private-images')).resolve())
        # Build locally, never export xnu-src over the source under review.
        run('bash', 'ci/build-kernel.sh', '4')
        shutil.copyfile(ROOT / 'out/kernel/mach.x1slte', ROOT / 'out/mach.x1slte')
        run('bash', 'ci/build-openh264.sh', env=env)
        env['OPENH264'] = str(ROOT / '.cache/release-deps/openh264-source')
        run('bash', 'userland/build-gpu-y2s.sh', env=env)
        for name in ('root.apfs', 'system.apfs'):
            if not (ROOT / 'userland' / name).is_file():
                raise ValueError(f'image builder produced no {name}')
        run('./build.sh', 'identity', env=env)
        run('python3', 'tools/release/loader_font.py', env=env)
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
