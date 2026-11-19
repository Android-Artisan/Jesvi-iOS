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
STEPS=5; source "$(dirname "$0")/ui.sh"
[ -n "$zip" ] || { echo "usage: twrp-install.sh [--erase] [--yes] [--no-reboot] Jesvi-iOS_<version>_<device>.zip   (--help)"; exit 2; }
title "Jesvi iOS installer: $(basename "$zip")"

step "Checking the package and the phone"
[ -f "$zip" ] || die "no such file: $zip"
command -v adb >/dev/null || die "adb is missing: brew install android-platform-tools"
meta=$(unzip -p "$zip" installer.json 2>/dev/null) || die "$zip is not a Jesvi iOS package (no installer.json)"
sha=$(unzip -p "$zip" installer.json | shasum -a 256 | cut -d' ' -f1)   # exact bytes: what the installer was built with
action=$(printf '%s' "$meta" | python3 -c 'import json,sys; print(json.load(sys.stdin)["action"])')
[ "$action" = restore ] && erase=1
ok "package: $([ "$action" = restore ] && echo "back to Android (Restore)" || echo "Jesvi iOS install"), $(du -h "$zip" | cut -f1)"
adb get-state 2>/dev/null | grep -q recovery || die "no phone in TWRP over USB: boot TWRP (tools/install/flash-twrp.sh), cable straight into the computer"
ok "phone in TWRP $(adb shell getprop ro.twrp.version 2>/dev/null | tr -d '\r'), battery $(adb shell cat /sys/class/power_supply/battery/capacity 2>/dev/null | tr -d '\r') %, $(( $(adb shell cat /sys/class/power_supply/battery/temp 2>/dev/null | tr -d '\r' || echo 0) / 10 )) C"

step "Your confirmation"
if [ $erase = 1 ]; then
	if [ "$action" = restore ]; then warn "This puts Android's layout back: ALL iOS DATA ON THE PHONE WILL BE ERASED."
	else warn "Fresh install if the phone has no usable iOS data: ALL DATA ON THE PHONE WILL BE ERASED."; fi
	info "Back up first if you need anything (tools/install/twrp-backup.sh)."
	printf '      Type %sERASE%s to continue: ' "$B" "$N"; read -r a
	[ "$a" = ERASE ] || die "cancelled: nothing was copied or written"
	kind=erase
else
	say "Update: iOS's system is replaced; your data, apps and settings are kept."
	info "A phone without iOS data is NOT erased: the installer stops (--erase for a fresh install)."
	if [ $yes = 0 ]; then printf '      Type %syes%s to install: ' "$B" "$N"; read -r a; [ "$a" = yes ] || die "cancelled: nothing was copied or written"; fi
	kind=keep
fi
ok "confirmed ($kind); no key presses needed on the phone from here"

step "Copying the package to the phone"
info "about a minute; leave the cable alone"
adb push "$zip" /tmp/jesvi.zip 2>&1 | tr '\r' '\n' | grep -E "pushed|error" | sed 's/^.*: //' | sed 's/^/      /' || true
want=$(shasum -a 256 "$zip" | cut -d' ' -f1); have=$(adb shell sha256sum /tmp/jesvi.zip | cut -d' ' -f1)
[ "$want" = "$have" ] || die "the copy is damaged (checksum differs): run it again"
ok "copied and checked (SHA-256)"
adb shell "echo '$sha $kind' > /tmp/jesvi.approve"
adb shell "twrp print 'Jesvi iOS: installing from the computer'" >/dev/null 2>&1 || true

step "Installing"
info "progress here and on the phone's screen; do not unplug"
adb shell 'cd /tmp && rm -rf ub && mkdir ub && cd ub && unzip -o /tmp/jesvi.zip META-INF/com/google/android/update-binary >/dev/null && chmod 755 META-INF/com/google/android/update-binary && rm -f /tmp/jesvi.out && (nohup sh -c "./META-INF/com/google/android/update-binary 3 3 /tmp/jesvi.zip 3>/dev/null >/tmp/jesvi.out 2>&1; echo EXIT=\$? >> /tmp/jesvi.out" >/dev/null 2>&1 &)'
seen=0 l=
while :; do
	out=$(adb shell 'cat /tmp/jesvi.out 2>/dev/null' 2>/dev/null | tr -d '\r' || true)
	n=$(printf '%s\n' "$out" | grep -c . || true)
	if [ "$n" -gt "$seen" ]; then
		new=$(printf '%s\n' "$out" | grep . | tail -n +$((seen + 1))); seen=$n
		printf '%s\n' "$new" | grep -v '^EXIT=' | while IFS= read -r m; do
			case "$m" in
				"INSTALL STOPPED"*|*"STOPPED:"*) printf '      %s✗ %s%s\n' "$R" "$m" "$N";;
				"Writing"*|"Setting"*|"Putting"*|"Clearing"*|"Installing"*) printf '      %s→%s %s\n' "$C" "$N" "$m";;
				"All written"*|"Every written"*|"Approved"*|*"passed"*|"Package OK"*) ok "$m";;
				"Battery warm"*|"FRESH INSTALL"*|"ALL "*) warn "$m";;
				*) info "$m";;
			esac
			# the same line on the phone: TWRP's console (OpenRecoveryScript "print"); best effort
			adb shell "twrp print '$(printf '%s' "$m" | tr -d "'\"\\\\")'" </dev/null >/dev/null 2>&1 || true
		done
	fi
	l=$(printf '%s\n' "$out" | grep . | tail -1)
	case "$l" in EXIT=*) break;; esac
	sleep 2
done
adb shell 'rm -f /tmp/jesvi.approve' 2>/dev/null || true
log="jesvi-install-$(date +%Y%m%d-%H%M%S).log"; printf '%s\n' "$out" > "$log"

step "Finishing"
[ "$l" = EXIT=0 ] || die "the installer stopped (log: $log); it writes nothing before the approval"
ok "every written byte read back and checked (log: $log)"
if [ $reboot = 1 ] && [ "$action" != restore ]; then
	adb reboot; done_ "Done: the phone restarts into iOS (the Apple logo, then iOS)"
elif [ "$action" = restore ]; then
	done_ "Done: now Wipe > Format Data, then install your Android ROM (TWRP stays open)"
else
	done_ "Done: restart when you like (adb reboot)"
fi
