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

## Front-camera scope clarification

- User requests preserving the existing front-camera values. No front sensor tables, clock/timing descriptors, exposure/gain limits, white-balance gains, colour matrix, shading, noise tuning or tap-metering formula were changed by the camera-quality implementation.
- Shared ISP plumbing DID change: per-site black levels moved from global variables into each ISP instance; RAW decoding/statistics/rendering now consume that instance; preview cache invalidation includes frame/sensor/black metadata; sharpness receives that state. These apply to front as well as rear, preserving existing front calibration numbers but changing state ownership/cache behavior. Front visual parity is not yet demonstrated.
- Live front probe temporarily requested 1001 exposure lines and gain register 32 in binned mode for 36 frames; this was a diagnostic command, not a saved front tuning change. Subsequent rear selection completed. Front was only checked for switching regression.
- Keep front tuning values frozen during subsequent rear research. Any proposed front-specific numerical or processing change must be described to the user first.

## Rear visual feedback and active tuning audit

- User reports rear still has grain, gradient and tint, with fewer glitches. Treat this as a failed visual-quality acceptance, despite passing transport tests.
- Read back active phone tuning files without modifying either camera. Rear uses the legacy alias `camera-s5khm2sd.isp` (actual hardware S5K2LD); front uses `camera-ov16a1q.isp` (actual SAK3J1). Names are inherited routing aliases, not proof that different sensors are installed.
- Rear RWCISP02 is 3632 bytes: one CCM across all light/CCT ranges; all 884 shading floats (4 x 13 x 17) are exactly 1.0. Therefore its active map applies no spatial correction. This does not establish whether every observed gradient comes from lens shading; black offsets/readout still require isolation.
- Rear CCM rows: [1.588,-0.342,-0.246], [-0.407,1.764,-0.357], [-0.437,-0.239,1.676]. These are existing local tuning values, not verified stock coefficients.
- Live app logs show rear 8–16x analog gain and up to 2x digital gain. ISP source explicitly has no noise reduction stage; grain at those settings is unsurprising, but requires controlled RAW noise measurements to separate temporal, row and fixed-pattern components.
- Existing binned rear black correction uses hand-measured gain/exposure tables; this and one fixed CCM can contribute to shadow casts. Do not substitute decoded Samsung GRAS or packed CCM9 coefficients blindly: mapping/calibration/composition remains unresolved.
- Local evidence: `live-tuning-files.txt`, `rear-live.isp`, `front-live.isp`, `live-tuning-summary.json`, `quality-observation.log` in the private device artifact directory. Front values remain untouched.
- Next controlled evidence: lit neutral/flat field plus covered-lens frames at identical rear exposure/gain, then separate additive black/row offsets from multiplicative colour shading. No new reboot required.

## Latest photographs inspected (IMG_0205 / IMG_0206)

- Copied the two latest JPEGs (21:35) read-only into the private device artifact directory and visually inspected both. IMG_0206 clearly shows dense coloured speckle plus a broad red-to-blue/green spatial cast; IMG_0205 shows a lit room/ceiling with pronounced coloured grain too. This is not only a completely dark test scene.
- IMG_0206 is 4032x3024 JPEG output. Do NOT equate output dimensions with sensor readout: active rear capture is 2016x1512, and StillImage uses the software demosaic/rescale path. Enlarging a 3-MP stream to a 12-MP file does not restore sensor detail.
- JPEG cannot establish whether the speckle is Bayer-phase/readout mismatch, channel-dependent offsets amplified by gain, or demosaic noise. No evidence yet supports calling this a Quad Bayer/four-in-one CFA mismatch. Need the current RAW buffer and header to distinguish them.
- Armed existing rear-only `redwood-raw-dump` in the active mediaserverd tuning directory. It consumes one upcoming rear frame and overwrites the old private `redwood-raw.bin`; no colour/exposure/tuning values changed, no service restart or reboot. This is a nearby preview RAW, not guaranteed the exact JPEG exposure. Retrieve promptly after rear Camera is opened; record that limitation.
- Optional JPEG band-stat script did not run because the default host Python lacks Pillow; visual inspections succeeded. No numerical JPEG statistics claimed.

## Covered-lens RAW diagnosis, 21:47–21:51

- User reports phone was restarting during previous transfer; no reboot issued by this session. Reconnected to same 21:16:40 kernel; development library persists. Partial RAW transfer excluded. Initial overlapping app/probe session excluded; all subsequent captures run with Camera closed.
- User explicitly confirmed rear lens fully covered and Camera closed. Captured 1000 and 20000 lines at gains 32/128/256/512, plus AE's 20436 lines at gains 32/64/128/256/384/512. Independent gain-256/20436 holdout captured afterward.
- Covered RAW confirms large channel/spatial pedestal errors, not merely JPEG tint: at 8x/1000 lines, tile means span roughly 15..95 depending on CFA site and location. At 20436 lines and 8x, per-site ranges are 14.9..47.8, 23.5..61.3, 12.0..50.4, 21.2..54.2. Existing four uniform inside-table offsets cannot remove these spatial biases. Short exposure has an additional row transition; do not extrapolate one calibration across that state change.
- Building a rear-only additive dark-grid correction, gated by exact binned readout/exposure/frame timing and measured gain range. This is module calibration, not stock GRAS lens shading or proof of a Quad Bayer layout. Holdout evaluation required before deployment.

## Dark-grid candidate validation failed; not deployed

- Built a frame-owned 17x13 additive black-grid backend and exact-state private calibration loader; front rejected, no calibration file installed. Candidate library built locally, not copied to phone. Host sanitizer tests and iOS build pass.
- Crucial holdout FAILED: corrected max tile bias 38.108 RAW counts, site mean biases [7.982,4.508,2.969,7.466]. Most lower rows repeat, upper region differs. Do not claim a fix or assume sensor sequencing alone; opaque coverage/capture timing must be ruled out too. Requested another covered repeat.
- Detailed reproduction/format/limits: docs/x1slte-camera-dark-grid.md. Private candidate SHA-256 3afdae361482abb7b6c9ec62a2552bb91c841302de4ccd667b044a4e592e226b. Existing phone library remains the earlier verified implementation.
- Samsung source explicitly does not list binned 2016x1512 as LN2-supported. Its full-mode LN2 writes/constraints cannot be applied to this mode as a one-register fix.
