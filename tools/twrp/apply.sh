#!/bin/bash
# Customize the x1s recovery tree after GitHub Actions syncs its sources.
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
device=$1/device/samsung/exynos990
flags=$device/recovery/root/system/etc/twrp.flags

# These are raw aliases. Keep Android's normal mount entries in recovery.fstab.
# EFS remains outside the JesviOS recovery's backup and restore menus.
if ! grep -q '^/jesvios_system ' "$flags"; then
    sed -i.bak -E '/^\/(efs|sec_efs)[[:space:]]/d' "$flags"
    rm "$flags.bak"
    printf '\n# JesviOS raw partition backups\n' >> "$flags"
    cat "$script_dir/jesvios.flags" >> "$flags"
fi
for partition in boot recovery; do
    grep -Eq "^/$partition[[:space:]]+emmc[[:space:]]" "$flags"
done
for entry in jesvios_system jesvios_data; do
    test "$(grep -c "^/$entry " "$flags")" -eq 1
done
if grep -Eq '^/(efs|sec_efs)[[:space:]]' "$flags"; then
    echo "Unexpected protected partition in recovery backup flags" >&2
    exit 1
fi

model='PRODUCT_MODEL := Galaxy S20 (SM-G980F)'
grep -Fxq "$model" "$device/twrp_x1s.mk" || printf '\n%s\n' "$model" >> "$device/twrp_x1s.mk"
sed -i.bak -E 's/^(TW_DEVICE_VERSION := .*)$/\1-Jesvi/; s/-Jesvi-Jesvi$/-Jesvi/' "$device/BoardConfig.mk"
rm "$device/BoardConfig.mk.bak"
grep -q '^TW_DEVICE_VERSION := .*-Jesvi$' "$device/BoardConfig.mk"
