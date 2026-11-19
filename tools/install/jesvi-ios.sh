#!/bin/bash
# jesvi-ios.sh — one command from nothing to Jesvi iOS on your Galaxy S20 (SM-G980F).
#   curl -fsSL <raw url of this file> | bash        or        bash tools/install/jesvi-ios.sh [--no-flash]
# 1. gets (or updates) the source into ~/Jesvi-iOS        4. builds the TWRP install ZIP (make release)
# 2. checks this Mac has the tools (make doctor)          5. if the phone is in TWRP over USB: offers a full backup,
# 3. iOS 16.5 parts are downloaded from Apple by the build   then installs the ZIP (tools/install/twrp-install.sh)
# Your own phone's firmware files are pulled from TWRP the first time (tools/s20-firmware.sh --adb).
set -euo pipefail
REPO=${JESVI_REPO:-https://github.com/jesvi-eulah/Jesvi-iOS.git}
DIR=${JESVI_DIR:-$HOME/Jesvi-iOS}
flash=1; [ "${1:-}" = --no-flash ] && flash=0
say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

say "1/5 source"
if [ -d "$DIR/.git" ]; then git -C "$DIR" pull --ff-only; else git clone --depth 1 "$REPO" "$DIR"; fi
cd "$DIR"

say "2/5 checking this computer"
make doctor || { echo "install what doctor lists above, then run this again"; exit 1; }

phone() { adb get-state 2>/dev/null | grep -q recovery; }
if [ ! -d fw-wifi ] && phone; then say "your phone's firmware files (first time only)"; tools/s20-firmware.sh --adb; fi

say "3-4/5 building (downloads iOS 16.5 parts from Apple; first time ~1 h)"
make release
zip=$(ls -t dist/release/Jesvi-iOS_*_x1slte.zip | head -1)
echo "built: $zip"

[ $flash = 1 ] || exit 0
say "5/5 install"
if ! phone; then
	echo "Boot the phone into TWRP (Vol Down+Power, then Vol Up+Power), plug it straight into this computer,"
	echo "then run:  tools/install/twrp-install.sh $zip"; exit 0
fi
read -r -p "Make a full backup of the phone first? (strongly recommended) [Y/n] " a
case "$a" in n|N) ;; *) tools/install/twrp-backup.sh "${JESVI_BACKUP_DIR:-$HOME/S20-backups}";; esac
tools/install/twrp-install.sh "$zip"
