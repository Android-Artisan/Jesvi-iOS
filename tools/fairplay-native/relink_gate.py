"""Relink an isolated gate prototype from the existing kernel object archive.

Does not modify canonical sources, objects or out/mach.x1slte. Output is an
offline prototype based on the archived build, not a current release kernel.
Run under xnu-env.sh. All archive members retain their original order.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relink(root, gated, output):
    root, gated, output = root.resolve(), gated.resolve(), output.resolve()
    if output.exists():
        raise ValueError('Output directory must be new')
    obj = root / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837'
    ar = shutil.which('llvm-ar')
    if not ar:
        raise ValueError('llvm-ar is required to preserve the thin Mach-O archive format')
    name = 'mach.development.bcm2837'
    linkdir = obj / (name + '.link')
    source_archive = linkdir / (name + '.a')
    before = digest(source_archive)
    output.mkdir(parents=True)
    archive = output / 'kernel.a'
    shutil.copy2(source_archive, archive)
    if digest(archive) != before or digest(source_archive) != before:
        raise ValueError('Kernel archive changed during snapshot')
    members = subprocess.check_output([ar, '-t', str(archive)], text=True).splitlines()
    if members.count('bootstrap.cpo') != 1:
        raise ValueError('Expected one archived bootstrap object')
    replacement = output / 'bootstrap.cpo'
    shutil.copy2(gated, replacement)
    setsegname = root / 'BUILD/obj/SETUP/setsegname/setsegname'
    for option, old, new in (('-s', '__TEXT', '__KLD'), ('-i', '__KLD', '__KLDDATA')):
        temp = output / 'bootstrap.tmp.cpo'
        subprocess.run([str(setsegname), option, old, '-n', new, '-o', str(temp), str(replacement)], check=True)
        temp.replace(replacement)
    subprocess.run([ar, 'r', str(archive), str(replacement)], check=True)
    updated = subprocess.check_output([ar, '-t', str(archive)], text=True).splitlines()
    if members != updated:
        raise ValueError('Archive member order changed; constructor sentinel order unsafe')
    args = shlex.split((linkdir / (name + '.linkarguments')).read_text())
    # This saved response also contains a kcgen-only option. A raw XNU image
    # must retain __LASTDATA_CONST separately, as the original raw kernel does.
    args = [arg for arg in args if arg != '-Wl,-rename_segment,__LASTDATA_CONST,__LAST']
    libraries = shlex.split((linkdir / (name + '.linkarchives')).read_text())
    for original, target in ((obj / 'all-alias.exp', output / 'all-alias.exp'),
                             (obj / 'symbolsets.plist', output / 'symbolsets.plist')):
        shutil.copy2(original, target)
    binary = output / 'kernel-gated.macho'
    args += ['-Wl,-force_load,' + str(archive),
             '-Wl,-alias_list,' + str(output / 'all-alias.exp'),
             '-Wl,-sectcreate,__LINKINFO,__symbolsets,' + str(output / 'symbolsets.plist'),
             '-Wl,-segprot,__LINKINFO,r--,r--']
    subprocess.run(['xnu-clang'] + args + libraries + ['-o', str(binary)], cwd=root, check=True)
    subprocess.run(['/usr/bin/xcrun', 'strip', '-x', str(binary)], check=True)
    if b'redwood_native_fairplay_gate_v1' not in binary.read_bytes():
        raise ValueError('Gate marker absent after relinking')
    if binary.stat().st_size > 0x1000000:
        raise ValueError('Relinked kernel exceeds loader read window')
    if digest(source_archive) != before:
        raise ValueError('Canonical archive changed during relink; check concurrent build')
    (output / 'manifest.json').write_text(json.dumps(dict(
        status='ISOLATED_ARCHIVE_BUILD_OFFLINE_ONLY', source_archive_sha256=before,
        output_sha256=digest(binary), bytes=binary.stat().st_size,
        bootstrap_only_replaced=True, archive_order_preserved=True,
        current_release_source_build=False), indent=2) + '\n')
    print('Isolated gate kernel relinked:', binary.stat().st_size, 'bytes; canonical artifacts unchanged.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xnu_root', type=Path)
    parser.add_argument('gated_object', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    relink(args.xnu_root, args.gated_object, args.output_dir)
