#!/bin/sh
# x1slte: the format kit for iOS's /private/var in the pinned /data/darwin/var.img (common/devices/x1slte.conf,
# VAR_UD_BLOCKS). A sparse disk image of exactly that size, formatted by macOS's own newfs_hfs as a case-sensitive
# journaled HFS+ (as tools/mk-userdata-hfs.sh does for the S20+'s whole userdata), turned into userland/hfs-kit.bin
# by tools/hfs-kit-any.py. Only files in a temporary directory are touched.
set -e
cd "$(dirname "$0")/.."
. common/devices/x1slte.conf
[ -n "$VAR_UD_BLOCKS" ] || { echo "mk-var-hfs.sh: VAR_UD_BLOCKS is empty in x1slte.conf (tools/x1slte-pinfiles.sh create)"; exit 1; }
BYTES=$((VAR_UD_BLOCKS * 4096))
D=$(mktemp -d)
trap 'diskutil eject "$DEV" >/dev/null 2>&1 || hdiutil detach "$DEV" >/dev/null 2>&1; rm -rf "$D"' EXIT
diskutil image create blank --format ASIF --size "$BYTES" --fs None "$D/var.asif" >/dev/null
DEV=$(diskutil image attach --noMount "$D/var.asif" | awk '{print $1}' | head -1)
case "$DEV" in /dev/disk*) ;; *) echo "mk-var-hfs.sh: attach gave no device: $DEV"; exit 1 ;; esac
RDEV="/dev/r${DEV#/dev/}"
echo "mk-var-hfs.sh: $BYTES bytes ($((BYTES >> 20)) MiB) as $DEV"
/sbin/newfs_hfs -J -s -b 4096 -v Data -U 0 -G 0 -M 755 "$RDEV"
python3 tools/hfs-kit-any.py "$RDEV" userland/hfs-kit.bin
