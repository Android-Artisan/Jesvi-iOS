#!/bin/bash
# flash-twrp.sh [TWRP.img] — put TWRP into the S20's recovery partition and boot it, with no buttons.
# From iOS (USB cable; the developer image's log server at 10.77.0.2:8080) the phone is sent to Download Mode, or it
# is already there. heimdall writes RECOVERY and lets the phone restart: our loader keeps misc's "boot-recovery"
# flag set (xnuloader misc_keep_recovery), so S-Boot starts recovery, which is now TWRP. Then wait for TWRP's adb.
# (heimdall --no-reboot left the phone in Download Mode until someone held Volume Down + Power.)
# Default image: device/twrp/twrp-3.7.1_12-artisan-x1s.img (until our own build: branch feat/twrp-x1s).
set -euo pipefail
case "${1:-}" in -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
img=${1:-$HOME/ios-port-research/device/twrp/twrp-3.7.1_12-artisan-x1s.img}
[ -f "$img" ] || { echo "no such file: $img"; exit 1; }
for t in heimdall adb; do command -v $t >/dev/null || { echo "$t is missing (tools/bin, or brew install heimdall android-platform-tools)"; exit 1; }; done
if ! heimdall detect >/dev/null 2>&1; then
	if [ "$(curl -s -m 4 -o /dev/null -w '%{http_code}' http://10.77.0.2:8080/ 2>/dev/null)" = 200 ]; then
		echo "iOS is running: asking it for Download Mode"
		curl -s -m 10 "http://10.77.0.2:8080/menu?to=3" >/dev/null || true
	else
		echo "Put the phone in Download Mode (Volume Down + Volume Up while plugging the cable in), or start iOS."
	fi
	for i in $(seq 1 60); do heimdall detect >/dev/null 2>&1 && break; sleep 2; done
	heimdall detect >/dev/null 2>&1 || { echo "no phone in Download Mode"; exit 1; }
fi
echo "writing TWRP ($(du -h "$img" | cut -f1)) into recovery; the phone restarts into it by itself"
heimdall flash --RECOVERY "$img" --stdout-errors 2>&1 | tr '\r' '\n' | grep -v '^[0-9]*%$' | grep -E 'upload successful|ERROR' || true
for i in $(seq 1 60); do adb get-state 2>/dev/null | grep -q recovery && break; sleep 3; done
adb get-state 2>/dev/null | grep -q recovery || { echo "TWRP did not come up over USB within 3 minutes (check the screen)"; exit 1; }
echo "TWRP is up: $(adb shell getprop ro.twrp.version | tr -d '\r')"
