"""Back up the live RECOVERY through its read-only UFS node, without rebooting."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare_trial import PARTITION_SIZE, boot_components, read_payload


def capture(directory):
    if directory.exists():
        raise ValueError('Backup output directory must be new')
    directory.mkdir(parents=True, mode=0o700)
    prefix = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 's20']
    before = subprocess.check_output(prefix + ['uname -v'], timeout=15)
    pending = directory / 'recovery.img.part'
    with pending.open('wb') as target:
        os.chmod(target.name, 0o600)
        # Fixed read-only node and exact partition extent; no device/path input.
        command = f'/var/jb/usr/bin/dd if=/dev/rufs_recovery bs=65536 count={PARTITION_SIZE // 65536}'
        result = subprocess.run(prefix + [command], stdout=target, stderr=subprocess.PIPE, timeout=180)
    (directory / 'read-stderr.txt').write_bytes(result.stderr)
    os.chmod(directory / 'read-stderr.txt', 0o600)
    if result.returncode or pending.stat().st_size != PARTITION_SIZE:
        raise RuntimeError('Read-only RECOVERY backup did not complete; partial file retained privately')
    after = subprocess.check_output(prefix + ['uname -v'], timeout=15)
    if before != after:
        raise RuntimeError('Phone kernel changed during backup')
    data = pending.read_bytes()
    boot_components(data)
    read_payload(data)
    if data[-64:-60] != b'AVBf':
        raise ValueError('Live recovery has no expected AVB footer')
    pending.replace(directory / 'recovery.img')
    (directory / 'manifest.json').write_text(json.dumps(dict(
        status='LIVE_RECOVERY_READ_ONLY_BACKUP', bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(), kernel_version=before.decode().strip(),
        rebooted=False, written_to_phone=False), indent=2) + '\n')
    print('Live RECOVERY backed up and structurally checked without a reboot or device write.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    capture(args.output_dir)
