#!/bin/bash
# twrp-install.sh [--erase] [--yes] [--no-reboot] ZIP — install a Jesvi iOS ZIP (or the Restore ZIP) on a phone that is
# in TWRP, from a Mac or PC over USB. You confirm once, here, before anything is copied: "yes" for an update that
# keeps the phone's data, "ERASE" for anything that erases it (a fresh install, Jesvi-iOS-Restore). After that it
# runs by itself, with no key presses on the phone, and restarts the phone when done.
#   --erase       allow erasing the phone's data (fresh install); without it an install only ever updates
#   --yes         no question for an update that keeps the data (never for an erase)
#   --no-reboot   stay in TWRP afterwards
# TWRP's own Install button refuses on an iOS phone and ADB Sideload streams 2.6 GB over a link that can drop:
# this copies the ZIP into TWRP's RAM once, checks it, and runs the installer there, detached.
# Needs: adb, the phone in TWRP 3.7.1 or newer, the cable straight into the computer (not a hub).
set -euo pipefail
erase=0 yes=0 reboot=1 zip=
for a in "$@"; do case "$a" in
	-h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
	--erase) erase=1;; --yes) yes=1;; --no-reboot) reboot=0;;
	-*) echo "unknown option: $a (try --help)"; exit 2;;
	*) zip=$a;; esac; done
B=$'\033[1m' R=$'\033[1;31m' G=$'\033[32m' N=$'\033[0m'; [ -t 1 ] || B= R= G= N=
[ -n "$zip" ] || { echo "usage: twrp-install.sh [--erase] [--yes] [--no-reboot] Jesvi-iOS_<version>_<device>.zip   (--help)"; exit 2; }
[ -f "$zip" ] || { echo "no such file: $zip"; exit 1; }
command -v adb >/dev/null || { echo "adb is missing: brew install android-platform-tools (or Android's platform-tools)"; exit 1; }
adb get-state 2>/dev/null | grep -q recovery || { echo "no phone in TWRP over USB (boot TWRP, plug it straight into the computer)"; exit 1; }

# what this package is, and the approval the installer will accept (bound to this exact package)
meta=$(unzip -p "$zip" installer.json) || { echo "$zip: not a Jesvi iOS package (no installer.json)"; exit 1; }
sha=$(unzip -p "$zip" installer.json | shasum -a 256 | cut -d' ' -f1)   # exact bytes: what the installer was built with
action=$(printf '%s' "$meta" | python3 -c 'import json,sys; print(json.load(sys.stdin)["action"])')
[ "$action" = restore ] && erase=1
printf '\n%sJesvi iOS: %s%s\n' "$B" "$(basename "$zip")" "$N"
if [ $erase = 1 ]; then
	if [ "$action" = restore ]; then echo "This puts Android's layout back: ALL iOS DATA ON THE PHONE WILL BE ERASED."
	else echo "Install. If the phone has no usable iOS data, ALL DATA ON THE PHONE WILL BE ERASED (fresh install)."; fi
	printf '%sBack up first (tools/install/twrp-backup.sh).%s Type ERASE to continue: ' "$R" "$N"
	read -r a; [ "$a" = ERASE ] || { echo "cancelled: nothing was copied or written"; exit 1; }
	kind=erase
else
	echo "Update: iOS's system is replaced; your data, apps and settings are kept."
	echo "(A phone without iOS data is NOT erased: the installer stops; use --erase for a fresh install.)"
	if [ $yes = 0 ]; then read -r -p "Type yes to install: " a; [ "$a" = yes ] || { echo "cancelled: nothing was copied or written"; exit 1; }; fi
	kind=keep
fi

echo; echo "copying $(du -h "$zip" | cut -f1) into TWRP's memory (about a minute; leave the cable alone)..."
adb push "$zip" /tmp/jesvi.zip
want=$(shasum -a 256 "$zip" | cut -d' ' -f1); have=$(adb shell sha256sum /tmp/jesvi.zip | cut -d' ' -f1)
[ "$want" = "$have" ] || { echo "copy damaged (checksum differs): try again"; exit 1; }
adb shell "echo '$sha $kind' > /tmp/jesvi.approve"
adb shell "twrp print 'Jesvi iOS: installing from the computer (progress here and on the computer)'" >/dev/null 2>&1 || true
adb shell 'cd /tmp && rm -rf ub && mkdir ub && cd ub && unzip -o /tmp/jesvi.zip META-INF/com/google/android/update-binary >/dev/null && chmod 755 META-INF/com/google/android/update-binary && rm -f /tmp/jesvi.out && (nohup sh -c "./META-INF/com/google/android/update-binary 3 3 /tmp/jesvi.zip 3>/dev/null >/tmp/jesvi.out 2>&1; echo EXIT=\$? >> /tmp/jesvi.out" >/dev/null 2>&1 &)'
echo "installing (no need to touch the phone):"
seen=0 l=
while :; do
	out=$(adb shell 'cat /tmp/jesvi.out 2>/dev/null' 2>/dev/null | tr -d '\r' || true)
	n=$(printf '%s\n' "$out" | grep -c . || true)
	if [ "$n" -gt "$seen" ]; then
		new=$(printf '%s\n' "$out" | grep . | tail -n +$((seen + 1))); seen=$n
		printf '%s\n' "$new" | sed 's/^/  /'
		# the same lines on the phone: TWRP's console (OpenRecoveryScript "print"); best effort, the install goes on
		printf '%s\n' "$new" | grep -v '^EXIT=' | tr -d "'\"\\\\" | while IFS= read -r m; do
			adb shell "twrp print '$m'" </dev/null >/dev/null 2>&1 || true; done
	fi
	l=$(printf '%s\n' "$out" | grep . | tail -1)
	case "$l" in EXIT=*) break;; esac
	sleep 2
done
adb shell 'rm -f /tmp/jesvi.approve' 2>/dev/null || true
printf '%s\n' "$out" > "jesvi-install-$(date +%Y%m%d-%H%M%S).log"
if [ "$l" = EXIT=0 ]; then
	if [ $reboot = 1 ] && [ "$action" != restore ]; then
		printf '%sdone:%s restarting the phone into iOS\n' "$G" "$N"; adb reboot
	else printf '%sdone%s\n' "$G" "$N"; fi
	exit 0
fi
echo "the installer stopped (its log is next to you); it writes nothing before the approval"; exit 1
