#!/bin/bash
# apply.sh TREE: our changes on top of ExtremeXT's unified Exynos 990 TWRP tree, after repo sync (twrp.yml).
set -euo pipefail
T=$1; D=$T/device/samsung/exynos990
# 1. who we are: the S20 (the unified tree names it SAMSUNG_x1s; the Artisan build on the phone said z3s)
echo 'PRODUCT_MODEL := Galaxy S20 (SM-G980F)' >> "$D/twrp_x1s.mk"
sed -i 's/^TW_DEVICE_VERSION := \(.*\)$/TW_DEVICE_VERSION := \1-Jesvi/' "$D/BoardConfig.mk"
grep -q 'TW_DEVICE_VERSION := .*-Jesvi' "$D/BoardConfig.mk"
echo "apply.sh: device tree ready ($(git -C "$D" log --oneline -1))"
