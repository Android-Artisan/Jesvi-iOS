#!/usr/bin/env python3
"""Build a native arm64 TWRP ZIP. Experimental writes require an explicit opt-in."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile
from cache import digest, fingerprint
from flash_plan import verify
from go_toolchain import ensure, environment
from images import hfs_kit
from manage import ROOT

ENTRIES = {'installer.json', 'hfs-kit.bin', 'candidate.json', 'README.txt',
           'META-INF/com/google/android/update-binary', 'META-INF/com/google/android/updater-script',
           'boot.img', 'recovery.img', 'system.apfs'}

def package(bundle, output, mode='choose', experimental=False):
    bundle, output = bundle.resolve(), output.absolute()
    if mode not in ('choose', 'update', 'fresh'):
        raise ValueError('unsupported installer mode')
    if (ROOT / 'dist').resolve() not in output.resolve().parents or output.suffix != '.zip':
        raise ValueError('TWRP output must be a new .zip under this checkout’s dist/')
    if any(p.exists() or p.is_symlink() for p in (output, Path(str(output) + '.sha256'), Path(str(output) + '.build.json'))):
        raise ValueError('output already exists')
    sources = {str(p.relative_to(ROOT)): p for p in (ROOT / 'tools/recovery').iterdir() if p.suffix == '.go' or p.name == 'go.mod'}
    sources['builder'] = Path(__file__)
    sources['toolchain'] = Path(__file__).with_name('go_toolchain.py')
    sources['filesystem-checks'] = Path(__file__).with_name('filesystem_checks.py')
    source_key = fingerprint(sources, {})
    manifest, device = verify(bundle)
    avb, avb_key = ROOT / 'inputs/tools/avbtool.py', ROOT / 'inputs/tools/testkey_rsa4096.pem'
    for tool in (avb, avb_key):
        if tool.is_symlink() or not tool.is_file():
            raise ValueError('reviewed AOSP AVB tool/public test key inputs required')
    ledger = json.loads((ROOT / 'inputs/private-inputs.lock.json').read_text())
    if fingerprint({'inputs/tools': ROOT / 'inputs/tools'}, {}) != ledger['inputs']['inputs/tools']:
        raise ValueError('AVB tool inputs differ from reviewed input lock')
    subprocess.run(['python3', '-I', str(avb), 'verify_image', '--image', str(bundle / 'recovery.img'),
                    '--key', str(avb_key)], cwd=ROOT, check=True)
    kit = ROOT / 'userland/hfs-kit.bin'
    hfs_kit(kit, device['partitions']['userdata'])
    from filesystem_checks import verify as verify_filesystems
    verify_filesystems(bundle / 'recovery.img', bundle / 'system.apfs', kit, device)
    config = {'schema': 1, 'device': device['id'], 'mode': mode,
              'allow_writes': experimental, 'installer_validated': False,
              'models': device['models'], 'bootloaders': device['bootloaders'],
              'partitions': device['partitions'], 'files': manifest['files'],
              'kit_sha256': digest(kit), 'kit_bytes': kit.stat().st_size}
    raw = (json.dumps(config, indent=2, sort_keys=True) + '\n').encode()
    sha = hashlib.sha256(raw).hexdigest()
    output.parent.mkdir(parents=True, exist_ok=True)
    work_root = output.parent
    work_root.mkdir(parents=True, exist_ok=True)
    go = ensure()
    with tempfile.TemporaryDirectory(prefix='build-', dir=work_root) as work:
        work = Path(work)
        binary = work / 'update-binary'
        base = environment()
        flags = ['-p', '4', '-trimpath', '-buildvcs=false', '-ldflags', f'-s -w -buildid= -X main.configSHA={sha}']
        subprocess.run([str(go), 'build', *flags, '-o', str(binary), '.'],
                       cwd=ROOT / 'tools/recovery', env=dict(base, GOOS='linux', GOARCH='arm64', GOARM64='v8.0'), check=True)
        if binary.read_bytes()[:4] != b'\x7fELF':
            raise ValueError('recovery executable is not ELF')
        pending = work / 'candidate.zip'
        with zipfile.ZipFile(pending, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as z:
            def small(name, data, executable=False):
                info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                z.writestr(info, data)
            small('installer.json', raw)
            small('META-INF/com/google/android/update-binary', binary.read_bytes(), True)
            small('META-INF/com/google/android/updater-script', b'# Native Jesvi iOS recovery installer\n')
            small('candidate.json', (bundle / 'manifest.json').read_bytes())
            small('README.txt', (f'Jesvi iOS x1slte TWRP candidate. Mode: {mode}. Writes enabled: {experimental}.\n'
                  'HARDWARE QUALIFICATION PENDING. No partition-table changes, no automatic reboot.\n'
                  'Use external storage or sideload; unmount all targets and release super/userdata holders.\n'
                  'Keep verified off-device backups and demonstrated Download Mode rescue.\n'
                  'Existing HFSX data is preserved in update/choose mode. Fresh install destroys userdata.\n'
                  'Physical Volume Up holds are required before writes. Volume Down/Power cancels.\n'
                  'To return Android: independently qualified Download Mode/full stock BL/AP/CP/CSC restore, then stock recovery factory reset.\n'
                  'BOOT-only restoration cannot restore Android super/data. Do not use HOME_CSC for cross-filesystem restoration.\n'
                  'Previous compatible iOS candidates use update-only ZIPs; arbitrary data downgrades are not guaranteed.\n'
                  'This is not an authenticated public release. See docs/release/twrp.md and rollback.md.\n').encode())
            z.write(kit, 'hfs-kit.bin')
            for name in ('boot.img', 'recovery.img', 'system.apfs'):
                z.write(bundle / name, name)
        with zipfile.ZipFile(pending) as z:
            if set(z.namelist()) != ENTRIES or len(z.namelist()) != len(ENTRIES):
                raise ValueError('unexpected ZIP inventory')
            if z.testzip() is not None:
                raise ValueError('ZIP CRC validation failed')
        # Run exactly the recovery parser/validators on host before publishing ZIP.
        inspector = work / 'inspect'
        subprocess.run([str(go), 'build', *flags, '-o', str(inspector), '.'],
                       cwd=ROOT / 'tools/recovery', env=base, check=True)
        subprocess.run([str(inspector), '--inspect', str(pending)], check=True)
        if fingerprint(sources, {}) != source_key:
            raise ValueError('installer sources changed during packaging; rerun from stable sources')
        # Atomic no-clobber publication, including competing standalone builders.
        os.link(pending, output)
        pending.unlink()
    Path(str(output) + '.sha256').write_text(f'{digest(output)}  {output.name}\n')
    Path(str(output) + '.build.json').write_text(json.dumps({'installer_source_key': source_key,
        'candidate_manifest_sha256': digest(bundle / 'manifest.json'), 'zip_sha256': digest(output),
        'go_version': subprocess.check_output([str(go), 'version'], text=True).strip(),
        'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=ROOT)),
        'hardware_qualified': False, 'writes_enabled': experimental, 'avb_test_key_verification': True, 'native_apfs_checks': True, 'hfs_seed_fsck_check': True}, indent=2) + '\n')
    print(f'TWRP ZIP: {output}; writes={experimental}; hardware qualification pending')
