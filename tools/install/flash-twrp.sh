#!/bin/bash
# flash-twrp.sh [TWRP.img] — put TWRP into the S20's recovery partition and boot it, with no buttons.
# From a phone running Jesvi iOS (USB cable; its log server at 10.77.0.2:8080): iOS hands over to our loader's Log
# access, TWRP is written into recovery there (fastboot stage + "oem write recovery 0", read back), and
# "oem recovery-mode" restarts with Samsung's recovery reason (0x12345604, sec_reboot.c), so S-Boot starts TWRP.
# A loader without that command (before 0.1.1 of 2026-11-20), or a phone already in Download Mode: heimdall writes
# RECOVERY, and you start TWRP with Volume Down + Power, then at once Volume Up + Power.
# Default image: device/twrp/twrp-3.7.1_12-artisan-x1s.img (until our own build: branch feat/twrp-x1s).
set -euo pipefail
case "${1:-}" in -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
img=${1:-$HOME/ios-port-research/device/twrp/twrp-3.7.1_12-artisan-x1s.img}
[ -f "$img" ] || { echo "no such file: $img"; exit 1; }
for t in fastboot adb; do command -v $t >/dev/null || { echo "$t is missing (tools/bin, or brew install android-platform-tools)"; exit 1; }; done
ios() { [ "$(curl -s -m 4 -o /dev/null -w '%{http_code}' http://10.77.0.2:8080/ 2>/dev/null)" = 200 ]; }
wait_twrp() {
	for i in $(seq 1 60); do adb get-state 2>/dev/null | grep -q recovery && break; sleep 3; done
	adb get-state 2>/dev/null | grep -q recovery || { echo "TWRP did not come up over USB within 3 minutes (check the screen)"; exit 1; }
	echo "TWRP is up: $(adb shell getprop ro.twrp.version | tr -d '\r')"
}
keys() { echo "now start TWRP: hold Volume Down + Power until the screen goes black, then at once Volume Up + Power"; }

if ios; then
	echo "iOS is running: handing over to the loader's Log access"
	curl -s -m 10 "http://10.77.0.2:8080/menu?to=1" >/dev/null || true
	for i in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q fastboot && break; sleep 2; done
	fastboot devices 2>/dev/null | grep -q fastboot || { echo "the phone did not reach Log access"; exit 1; }
	echo "writing TWRP ($(du -h "$img" | cut -f1)) into recovery..."
	fastboot stage "$img" >/dev/null 2>&1 || { echo "stage failed"; exit 1; }
	out=$(fastboot oem write recovery 0 2>&1); echo "$out" | grep -q "written and read back" || { echo "recovery: $out"; exit 1; }
	echo "written and read back; restarting into recovery"
	if fastboot oem recovery-mode 2>&1 | grep -q "unknown command"; then
		echo "(this loader has no recovery restart yet)"; keys; fastboot reboot >/dev/null 2>&1 || true
	fi
	wait_twrp; exit 0
fi
command -v heimdall >/dev/null || { echo "heimdall is missing (tools/bin, or brew install heimdall)"; exit 1; }
if ! heimdall detect >/dev/null 2>&1; then
	echo "Start Jesvi iOS with the cable in, or put the phone in Download Mode (Volume Down + Volume Up while plugging in)."
	for i in $(seq 1 60); do heimdall detect >/dev/null 2>&1 && break; sleep 2; done
	heimdall detect >/dev/null 2>&1 || { echo "no phone in iOS or Download Mode"; exit 1; }
fi
echo "Download Mode: writing TWRP ($(du -h "$img" | cut -f1)) into recovery"
heimdall flash --RECOVERY "$img" --no-reboot --stdout-errors 2>&1 | tr '\r' '\n' | grep -E 'upload successful|ERROR' || true
keys; wait_twrp
