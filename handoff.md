# S20 camera deployment handoff

## Current state

- Branch: `feat/s20-camera-quality`; camera implementation `e917479d`.
- User explicitly handed over the device and authorized camera flash/testing and the test boot.
- Phone lock transferred from `codex-fingerprint` to `codex-camera-quality` after verifying no running flashing process.
- Live baseline: kernel built 2026-11-14 20:38:33 IST; fingerprint status `exchange-v2`, no active command; native MKB unlocked, AKS flags 0x4.
- No camera binary installed or kernel flashed at this checkpoint.

## Progress

1. Checked interruption state, workspace NEXT/TASKS, phone lock and live device.
2. Confirmed the standalone camera kernel would omit newer fingerprint work. Integrated committed fingerprint lineage through `7e8181fe` into the camera branch, preserving the installed exchange-v2 capability. The fingerprint worktree's later uncommitted experiments are untouched and excluded.
3. Retained both camera and fingerprint shipping notes while resolving their documentation-only merge conflict.

## Next

- Back up current camera library and exact current recovery kernel slot locally; keep private artifacts outside git.
- Mirror combined tracked kernel sources/build-list change into the isolated build tree; build and validate passcode/camera guards.
- Install only the recovery kernel slot with read-back verification, preserving loader, boot profile, root/system/data and fingerprint calibration/templates.
- Install signed development camera library with a reversible fallback. Boot once, verify locked protected keybag, exchange-v2 and camera timings; ask the user to unlock on-device without sharing the code.
- Test rear/front repeat capture, switching, exposure and still output. Record evidence and limitations and commit this file after each meaningful checkpoint.

## Safety and rollback

- Preserve `softpasscode=1`, `autounlock=0`, native keybagd, fail-closed restore and repeat-lock bridge.
- No fingerprint commands, enrollment, template/calibration changes or authentication bypass.
- Current user authorization covers this camera installation/test reboot; do not inherit unrelated fingerprint reboot approvals.
- Exact backup paths/hashes will be recorded before writing. Stop on unverified writes. Do not publish binaries or private device material.

## Checkpoint: exact rollback slot saved; ready to write

- Combined GICv2 build passes; passcode integration and eight fingerprint exchange host tests pass. Combined sources/build mirrors match.
- Unstripped kernel was 17484464 bytes and failed the slot-size assertion. No kernel write occurred. Applied standard `strip -x` to the local candidate copy, then validated slot size and camera/exchange-v2 markers.
- System media library backed up locally (881040 bytes). No prior dev override existed. Signed development library uploaded/read back byte-identically, then activated for the next mediaserverd launch.
- Phone is in Log access. Exact current 16777216-byte recovery kernel slot saved locally as `build/camera-device-20261114/kernel-slot-before.bin`.
- Backup SHA-256: `58f5543786af33e8480a777a1a61679e42f99afea78a335d75233ec0950e6f58`. It matches the previously installed exchange-v2 slot byte-for-byte.
- Candidate hashes/sizes: `build/camera-device-20261114/candidate.json`; backup verification: `backup.json`. Private binaries remain outside git.
- Next: write ONLY recovery offset 0x2000000; verify loader readback plus independent full-slot readback; commit checkpoint, then use authorized test reboot.
