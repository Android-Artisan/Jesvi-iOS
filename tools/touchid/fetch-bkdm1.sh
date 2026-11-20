#!/bin/bash
# fetch-bkdm1.sh: Touch ID's sensor module for the system image: the iPhone 8's /usr/lib/libBKDM1.dylib (740 KB), cut out
# of the iPhone 8's iOS 16.5 (20F66) dyld shared cache, which is fetched from Apple's CDN for that (ipsw extract
# --remote: only the cache, ~3 GB, deleted again afterwards). NOT USED YET: loaded alone from /usr/lib, biometrickitd
# crashed (SIGSEGV, 2026-11-20): the extract still calls into other iPhone 8 libraries at their cache addresses; it
# needs a relinker. Until then biometrickitd runs on the whole iPhone 8 cache (push-iphone8-cache.sh).
# Same build and SoC as the iPhone X image, so its dependencies (BiometricSupport, BiometricKit, ...) are already there.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=sysimage/iphone8/libBKDM1.dylib
[ -f "$OUT" ] && { echo "fetch-bkdm1: $OUT already there"; exit 0; }
IPSW=${IPSW8_URL:-https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85662/3AF4C04F-D804-4EC7-8289-7DBD52EB3B9E/iPhone_4.7_P3_16.5_20F66_Restore.ipsw}
command -v ipsw >/dev/null || { echo "fetch-bkdm1: needs ipsw (brew install blacktop/tap/ipsw)"; exit 1; }
work=sysimage/iphone8/dsc
mkdir -p "$work"
cache=$(find "$work" -name dyld_shared_cache_arm64 -type f | head -1)
if [ -z "$cache" ]; then
	echo "fetch-bkdm1: the iPhone 8's shared cache from Apple (about 3 GB, once)"
	ipsw extract --dyld --remote "$IPSW" --output "$work"
	cache=$(find "$work" -name dyld_shared_cache_arm64 -type f | head -1)
fi
ipsw dyld extract "$cache" /usr/lib/libBKDM1.dylib --output sysimage/iphone8 >/dev/null
[ -s "$OUT" ] || { echo "fetch-bkdm1: extraction failed"; exit 1; }
[ "${KEEP:-0}" = 1 ] || rm -rf "$work"
echo "fetch-bkdm1: $OUT ($(wc -c < "$OUT") bytes)"
