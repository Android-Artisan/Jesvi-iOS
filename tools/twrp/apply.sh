#!/bin/bash
# apply.sh TREE: our changes on top of TeamWin's x1s tree, after repo sync (twrp.yml). Kept small and readable.
set -euo pipefail
T=$1; D=$T/device/samsung/x1s
# 1. who we are: the S20 LTE this port runs on (TeamWin's says x1s, the Artisan build said z3s)
sed -i 's/^PRODUCT_MODEL := .*/PRODUCT_MODEL := Galaxy S20 (SM-G980F)/' "$D/twrp_x1s.mk"
grep -q 'TW_DEVICE_VERSION' "$D/BoardConfig.mk" || echo 'TW_DEVICE_VERSION := Jesvi' >> "$D/BoardConfig.mk"
echo "apply.sh: device tree ready ($(git -C "$D" log --oneline -1))"
