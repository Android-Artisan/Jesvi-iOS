"""One explicitly approved, backed-up layout-only boot test over USB.

Never enables native execution and never writes BOOT/EFS/modem/bootloader parts.
Runs locally through a temporary internet loss, restores the previous hotspot,
and attempts RECOVERY rollback if the checked boot does not return correctly.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time
import urllib.request

from flash_preflight import verify
from prepare_trial import PARTITION_SIZE, SIDECAR_AT, KERNEL_AT


class Trial:
    def __init__(self, args):
        self.args = args
        self.repo = Path(__file__).resolve().parents[2]
        self.workspace = self.repo.parents[1]
        self.fastboot = self.workspace / 'tools/bin/fastboot'
        self.directory = args.run_dir.resolve()
        self.directory.mkdir(parents=True, mode=0o700)
        os.chmod(self.directory, 0o700)
        self.entered = self.write_attempted = False
        self.backup = self.directory / 'recovery-before.img'

    def event(self, message):
        print(time.strftime('%H:%M:%S'), message, flush=True)
        subprocess.run([str(self.workspace / 'tools/team.sh'), 'beat', self.args.session,
                        'Approved layout-only test: ' + message], stdout=subprocess.DEVNULL, check=True)

    def command(self, args, *, timeout=120, check=True):
        result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        with (self.directory / 'commands.log').open('ab') as handle:
            os.chmod(handle.name, 0o600)
            handle.write(result.stdout + b'\n')
        if check and result.returncode:
            raise RuntimeError('Trial command failed; see private commands.log')
        return result

    def ssh(self, command, timeout=8, check=True):
        return self.command(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3',
                             '-o', 'ServerAliveInterval=3', '-o', 'ServerAliveCountMax=1',
                             's20', command], timeout=timeout, check=check)

    def fastboot_ready(self):
        result = self.command([self.fastboot, 'devices'], timeout=5, check=False)
        return result.returncode == 0 and b'fastboot' in result.stdout

    def wait_fastboot(self, seconds=120):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if self.fastboot_ready():
                return True
            time.sleep(2)
        return False

    def wait_ios(self, seconds=240):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            try:
                response = self.ssh('uname -v', timeout=6, check=False)
                if response.returncode == 0 and b'Darwin Kernel Version' in response.stdout:
                    return response.stdout.decode(errors='replace').strip()
            except subprocess.TimeoutExpired:
                pass
            time.sleep(3)
        return None

    def restore_hotspot(self):
        state = json.loads(self.args.hotspot_state.read_text())
        if not state.get('active'):
            return True
        if not state.get('restore_ready'):
            self.event('Hotspot parameters unavailable; automatic restart was not attempted')
            return False
        if state.get('native'):
            for attempt in range(12):
                status = self.ssh('/var/jb/usr/sbin/sysctl -n kern.redwood.wifi_ap', check=False)
                if status.returncode == 0 and b'ON THE AIR' in status.stdout:
                    self.event('Native hotspot is back on the air')
                    return True
                if attempt == 0:
                    self.ssh('/redwood/bin/misctl on', timeout=25, check=False)
                    self.ssh('/redwood/bin/misctl wifi on', timeout=25, check=False)
                    self.ssh('/redwood/bin/misctl discover on', timeout=25, check=False)
                time.sleep(5)
            self.event('Native hotspot restart pending; iOS remains reachable over USB')
            return False
        name, channel = state['name'], int(state['channel'])
        if not 1 <= len(name.encode()) <= 32 or not 1 <= channel <= 196:
            raise ValueError('Invalid captured hotspot parameters')
        command = '/var/root/hotspot-up.sh start ' + shlex.quote(name) + ' ' + str(channel)
        for attempt in range(1, 13):
            result = self.ssh(command, timeout=25, check=False)
            if result.returncode == 0:
                self.event('Previous hotspot restarted')
                return True
            time.sleep(10)
        self.event('Automatic hotspot restart did not finish; iOS remains reachable over USB')
        return False

    def collect_kernel(self):
        until = time.monotonic() + 40
        while True:
            try:
                with urllib.request.urlopen('http://10.77.0.2:8080/log', timeout=10) as response:
                    data = response.read(16 * 1024 * 1024)
                break
            except OSError:
                if time.monotonic() >= until:
                    raise
                time.sleep(3)
        target = self.directory / 'kernel-after.log'
        target.write_bytes(data)
        os.chmod(target, 0o600)
        return data.decode(errors='replace')

    def execute(self):
        owner = (self.workspace / 'build/phone.lock/owner').read_text().split()[0]
        if owner != self.args.session:
            raise RuntimeError('This session does not hold the phone')
        manifest = verify(self.args.image, self.args.manifest)
        original = self.args.template.read_bytes()
        if len(original) != PARTITION_SIZE:
            raise ValueError('Invalid known-good template size')
        # Every boot/write path below uses only this exact checked image.
        out = self.repo / 'out'
        out.mkdir(exist_ok=True)
        shutil.copy2(self.args.image, out / 'x1slte-recovery.img')
        shutil.copy2(self.args.manifest, out / 'fairplay-layout-manifest.json')
        self.event('Preflight accepted; syncing the phone and saving its previous version')
        before = self.ssh('uname -v; sync').stdout
        (self.directory / 'version-before.txt').write_bytes(before)
        os.chmod(self.directory / 'version-before.txt', 0o600)
        result = dict(status='STARTED', native_execution=False, image_sha256=manifest['image_sha256'])
        try:
            self.entered = True
            with urllib.request.urlopen('http://10.77.0.2:8080/menu?to=1', timeout=12) as response:
                response.read(4096)
            if not self.wait_fastboot():
                raise RuntimeError('Phone did not reach Log access')
            self.event('Log access ready; backing up RECOVERY before any write')
            self.command([self.fastboot, 'oem', 'read', 'recovery', '0', f'{PARTITION_SIZE:x}'])
            self.command([self.fastboot, 'get_staged', self.backup], timeout=180)
            os.chmod(self.backup, 0o600)
            saved = self.backup.read_bytes()
            # The established split flasher preserves 16..32 MiB, so old log
            # bytes can differ there. The boot image/kernel/ramdisk must match.
            if len(saved) != PARTITION_SIZE or saved[:0x1000000] != original[:0x1000000] or saved[KERNEL_AT:] != original[KERNEL_AT:]:
                raise RuntimeError('Live RECOVERY differs from the known-good template; refusing the trial write')
            sidecar_span = (manifest['sidecar_bytes'] + 0x3fff) & ~0x3fff
            if any(saved[SIDECAR_AT:SIDECAR_AT + sidecar_span]):
                raise RuntimeError('Live native sidecar slot is not empty; refusing overwrite')
            self.event('Known-good backup verified; writing only the checked layout-trial RECOVERY')
            environment = os.environ.copy()
            environment.update(FAIRPLAY_LAYOUT_TRIAL='1', FASTBOOT=str(self.fastboot), UP_PARAM='')
            self.write_attempted = True
            call = subprocess.run(['bash', './x1slte.sh', 'uflash'], cwd=self.repo, env=environment,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
            with (self.directory / 'commands.log').open('ab') as handle:
                handle.write(call.stdout)
            if call.returncode or b'whole layout-trial RECOVERY written and read back' not in call.stdout:
                raise RuntimeError('RECOVERY write/readback did not complete')
            self.event('RECOVERY readback verified; rebooting once with native execution disabled')
            self.command([self.fastboot, 'reboot'])
            version = self.wait_ios()
            if not version:
                raise RuntimeError('Layout-test iOS did not return over USB')
            (self.directory / 'version-after.txt').write_text(version + '\n')
            os.chmod(self.directory / 'version-after.txt', 0o600)
            self.event('iOS returned over USB; checking the native loading gate')
            log = self.collect_kernel()
            expected = re.compile(r'redwood_native_fairplay_gate_v1: requested=0 enabled=0 omitted=2 headers=2/2')
            if not expected.search(log):
                # IOLog also goes through unified logging on some profiles.
                query = '/usr/bin/log show --last 5m --style compact --predicate \'eventMessage CONTAINS "redwood_native_fairplay_gate_v1"\''
                extra = self.ssh(query, timeout=30, check=False).stdout.decode(errors='replace')
                if not expected.search(extra):
                    raise RuntimeError('Expected disabled gate and two loaded headers were not observed')
            result.update(status='LAYOUT_BOOT_PASS_NATIVE_DISABLED', gate_headers=2,
                          hotspot_restored=self.restore_hotspot())
            self.event('Layout-only boot passed; FairPlay code remains disabled')
        except Exception as exc:
            result.update(status='TRIAL_FAILED', reason=str(exc))
            self.event('Layout trial stopped; checking recovery access for return to the previous image')
            if self.entered and not self.fastboot_ready():
                running = self.ssh('uname -r', timeout=6, check=False)
                if running.returncode == 0:
                    try:
                        self.ssh('sync', timeout=10)
                        with urllib.request.urlopen('http://10.77.0.2:8080/menu?to=1', timeout=12) as response:
                            response.read(4096)
                    except OSError:
                        result['hotspot_restored'] = self.restore_hotspot()
            if self.entered and self.wait_fastboot(seconds=45):
                if self.write_attempted and self.backup.exists():
                    self.command([self.fastboot, 'stage', self.backup], timeout=180)
                    response = self.command([self.fastboot, 'oem', 'write', 'recovery', '0'], timeout=180)
                    if b'written and read back' not in response.stdout:
                        raise RuntimeError('Rollback readback failed; leave phone in Log access') from exc
                    result['rollback_written_and_verified'] = True
                self.command([self.fastboot, 'reboot'])
                if self.wait_ios():
                    result['previous_ios_returned'] = True
                    result['hotspot_restored'] = self.restore_hotspot()
                    self.event('Previous iOS is back over USB')
            raise
        finally:
            (self.directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            os.chmod(self.directory / 'result.json', 0o600)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('template', type=Path)
    parser.add_argument('--hotspot-state', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--session', required=True)
    parser.add_argument('--reboot-approved', action='store_true', help='Pass only after current user approval of reboot timing')
    args = parser.parse_args()
    if not args.reboot_approved:
        parser.error('An explicitly approved reboot window is required')
    Trial(args).execute()
