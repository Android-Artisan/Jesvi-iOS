# Our own TWRP for the S20 (SM-G980F, x1s) — plan (branch feat/twrp-x1s, 2026-11-19)

Today's TWRP is a prebuilt community image (device/twrp/twrp-3.7.1_12-artisan-x1s.img). It reports the wrong model
(ro.product.model SAMSUNG_z3s = S20 Ultra) and its Install button refuses every ZIP on an iOS phone
("Unable to find partition for path '/system_root'").

Goals
1. Built in GitHub Actions (ubuntu, hosted): twrp-12.1 minimal manifest (~30-40 GB synced, shallow), free the runner's
   disk first (~14 GB free by default, 60+ GB after removing preinstalled toolchains), ccache in actions/cache,
   6 h job limit. Output: recovery.img + Odin tar (AP), attached to a draft release. Linux minutes only.
2. Correct device identity (x1s / SM-G980F), one tree for Exynos 990 (x1s; y2s/c1s later).
3. Works on an iOS phone: fstab entries that do not need /system_root; Install runs our ZIPs directly.
4. Works for Samsung stock, ExtremeROM and AOSP ROMs (dynamic partitions in super, FBE decryption where possible).
5. Our tools inside: full backup (as tools/install/twrp-backup.sh, skipping whole-disk names), restore (never
   efs/modem/bootloader/param), and the Jesvi iOS installer as a menu entry.

Steps
- Find a maintained x1s/exynos990 device tree (TeamWin/community), pin it; write .github/workflows/twrp.yml.
- First CI build; flash with heimdall (--RECOVERY, --no-reboot), boot with Vol Down+Power then Vol Up+Power.
- Qualify: backup, restore, iOS install (update + fresh), an Android ROM flash.
