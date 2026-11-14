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

## Checkpoint: kernel written and independently verified

- Wrote only recovery kernel offset 0x2000000. Loader reported written/read-back verified.
- Independently read/downloaded the whole 16-MiB slot afterward and compared byte-for-byte with the padded candidate: identical.
- Installed slot SHA-256: `dcc5b9e7e87ae854927fdddf79497ef7a90923bf95486f1f9014b1517ae233f8`.
- Exact rollback slot is preserved. Loader/profile/root/system/userdata were not rewritten.
- Next: execute the user-authorized test reboot, wait for SSH, verify protected/locked keybag before user unlock, fingerprint exchange-v2 status and camera device.

## Checkpoint: combined kernel booted; live captures verified

- Authorized test reboot completed. Live kernel build time: 2026-11-14 21:16:40 IST. Camera node present; fingerprint exchange-v2 preserved, TEEGRIS not started, no fingerprint commands issued.
- Correction to earlier activation checkpoint: postboot inspection found the uploaded library still named `.camera-new`. It had not been activated. Renamed it live, verified the active file byte-for-byte against the signed build, restarted only mediaserverd. Its log confirms the development copy loaded. No second reboot.
- Native MKB state is 0 / AKS 0x4 (unlocked). Repeat passcode locking is NOT verified; requested physical lock/unlock checks from the user without asking for their code.
- Added bounded `tools/camera/s20_capture_probe.c`: explicit binned rear/front mode, EAGAIN startup handling, 36 headers per session, odd exposure requested after stream initialization. Commands before stream initialization are reset; an initial probe demonstrated this, so it was corrected rather than counted as a pass.
- Rear/front/rear sessions pass (108 frames). Rear 2016x1512, line_ns 1468; requested 1001 lines settles to 1000. Kernel register readback at frame 31 independently reports exposure 1000, gain 0x20, frame length 22564 matching the request.
- Production ISP host executable captures 40/40 rear frames, no skips, approximately 30 fps; renders 1008x756 NV12 in 21 ms. Register readback tracks AE requests (3374 lines).
- Captured scene is nearly black (~1% samples above black+8). Exposure transport passes, but image quality cannot be accepted from this scene. Asked user whether lens is covered/dark and to provide a lit scene. Do not claim stock quality or visual validation.
- Evidence is outside git: `/Users/jesvi/ios-port-research/build/camera-device-20261114/` (`rear-probe-2.txt`, `front-probe-1.txt`, `rear-probe-3.txt`, `rear-isp-test.txt`, `test-final-kernel.log`, active library readback). Earlier failed probe logs preserved too.
- Remaining: physical repeat-lock confirmation, lit-scene RAW/preview inspection and app still/switch testing. Full-resolution rear remains disabled; stock GRAS/CCM/noise transplantation is not yet enabled.
