#!/bin/bash
# twrp-install.sh ZIP — install a Jesvi iOS ZIP on a phone that is in TWRP, from a Mac or PC over USB.
# TWRP's own Install button refuses on an iOS phone (it cannot unmount Android's /system) and ADB Sideload
# streams 2.6 GB over a link that can drop. This copies the ZIP into TWRP's RAM once, checks it, and runs
# the installer there, detached: after the copy the cable no longer matters.
# Needs: adb, the phone in TWRP 3.7.1 or newer, the cable straight into the computer (not a hub).
set -euo pipefail
case "${1:-}" in -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
command -v adb >/dev/null || { echo "adb is missing: brew install android-platform-tools (or Android's platform-tools)"; exit 1; }
[ $# -ge 1 ] || { echo "usage: twrp-install.sh Jesvi-iOS_<version>_<device>.zip   (--help for more)"; exit 2; }
zip=$1
[ -f "$zip" ] || { echo "no such file: $zip"; exit 1; }
adb get-state 2>/dev/null | grep -q recovery || { echo "no phone in TWRP over USB (plug it straight into the computer)"; exit 1; }
echo "copying $(du -h "$zip" | cut -f1) into TWRP's memory..."
adb push "$zip" /tmp/jesvi.zip
want=$(shasum -a 256 "$zip" | cut -d' ' -f1); have=$(adb shell sha256sum /tmp/jesvi.zip | cut -d' ' -f1)
[ "$want" = "$have" ] || { echo "copy damaged (checksum differs): try again"; exit 1; }
adb shell 'cd /tmp && rm -rf ub && mkdir ub && cd ub && unzip -o /tmp/jesvi.zip META-INF/com/google/android/update-binary >/dev/null && chmod 755 META-INF/com/google/android/update-binary && rm -f /tmp/jesvi.out && (nohup sh -c "./META-INF/com/google/android/update-binary 3 3 /tmp/jesvi.zip 3>/dev/null >/tmp/jesvi.out 2>&1; echo EXIT=\$? >> /tmp/jesvi.out" >/dev/null 2>&1 &)'
echo "installer started: when the phone asks, hold VOLUME UP for 3 seconds"
last=""
while :; do
	l=$(adb shell 'tail -1 /tmp/jesvi.out 2>/dev/null' 2>/dev/null | tr -d '\r' || true)
	[ -n "$l" ] && [ "$l" != "$last" ] && { echo "  $l"; last=$l; }
	case "$l" in EXIT=*) break;; esac
	sleep 2
done
adb shell cat /tmp/jesvi.out > "jesvi-install-$(date +%Y%m%d-%H%M%S).log" 2>/dev/null || true
[ "$l" = EXIT=0 ] && { echo "done: reboot with  adb reboot"; exit 0; }
echo "the installer stopped (log saved next to you); nothing is written before the Volume Up confirmation"; exit 1
