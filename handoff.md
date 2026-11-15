# Latest live sensor progress — 2026-11-15

Installed source8b39c494 after ONE explicit user-approved install/reboot. Loader
verified the kernel-slot write/readback. Running11:17:58IST. Protected bag flags1
before first unlock and after probe; user confirms front/main and repeat unlocking
work. Original front media baseline remains active; brightness tweak loads on boot.

**2LA positively identified twice: model20ca, revisiona202, cleanup_ok=true.**
Complete PMIC map unchanged across repeat. No RAW capture/auxiliary app lens yet.
Private evidence and exact rollback slot: build/camera-aux-20261115 outside git.
First approved reboot is consumed; no further reboot permission implied.

The next source/candidate now adds revision-gated2LA native12MP sensor-counter
stream test and GW2 identity probe with two-PMIC rollback. Host sanitizer/fault
and catalogue tests, passcode integration and GICv2 build pass. NOT installed.
See docs/x1slte-camera-aux.md for commands, hashes, rail values and runtime gates.
Read the CORRECTION: GW2 active I2C pinctrl is gpp0-2/3; gpc0 recovery-GPIO metadata
must not be used as active I2C routing. Previous suspected MCLK pin conflict was
based on that metadata and is superseded for the native bus route.

Next candidate slot SHA256bdb51280424393a8abbee7f8a9e979c1811e0782d09095a58a031a9bb876a7b9.
Current running rollback candidate is kernel-slot-2la-id.bin (SHA14f70dab...);
pre-session AOD kernel backup SHA256b686b63aae218f958e6212ee5bce95f44b2c5e92a4357a6bea0a8b858b17d266.
Do not confuse these. Phone lock currently codex-camera-aux; verify before action.
Main/master remain accepted8eaa52cf; all research stays feat/s20-rear-aux.

---

# Latest: 2LA identity probe built, awaiting approved hardware boot

Branch feat/s20-rear-aux now integrates camera-quality and accepted master8eaa52cf
through merge commits08fc65d3/3fa430b3. Main/master unchanged. The accepted
brightness tweak remains installed, original front default preserved.

New root-only `cam.s20.2la.id` implements cold/idle-only model/revision reads and
transactional power/clock/pin cleanup. No auto probe or streaming. Dedicated core
rail needs selector0x24=1.05V, not baseline0x20=1.0V. Do not attribute this aux
finding to main-rear grain. See docs/x1slte-camera-aux.md for exact design, guards,
hashes, tests and limits. Host fault tests, four inventory tests, passcode checks
and full GICv2 kernel build pass. Tracked/build overlay sources match.

Candidate: build/camera-aux-20261115/kernel-slot-2la-id.bin (outside git), SHA256
14f70dab3b2d63d0bf09e22b138f2d03c4cd7d9fd15609918262a5957da19700.
NOT installed. No auxiliary sensor has been identified or streamed. No smooth
zoom/lens switching implemented. GW2 probe still pending its separate power plan.

Phone is now held by codex-camera-aux after A released it; check actual lock.
During an earlier acquisition race, acquisition failed but one read-only uname/
PMIC snapshot command ran because the shell sequence lacked fail-fast guarding;
no rail/system writes occurred. Subsequent commands stopped until lock was free.
Use set -e / explicit successful lock checks for every dependent device action.

Next: obtain explicit approval for ONE diagnostic-kernel install/reboot, then
back up current AOD kernel slot, verify write/readback, preserve passcode chain,
ask user to unlock on-device, close Camera/wait20s and invoke identity command.
Never treat previous camera reboot approval or elapsed time as new approval.
Successful ID is a gate to revision-correct setfiles and bounded RAW capture,
not proof of a functioning auxiliary Camera lens. Preserve all private logs and
backups outside git; no firmware or personal captures should enter commits.

---

> Push status (2026-11-15): local commits complete, remote push blocked.
> Both feature-branch pushes to configured upstream
> https://github.com/oosexgang/hakios-xnu-kernel.git returned HTTP403:
> authenticated user jesvi-eulah lacks write access. No existing fork found at
> jesvi-eulah/hakios-xnu-kernel. No branches were published. Retry after access
> is granted or a writable remote is provided. Do not claim a successful push.

# Next agent: continue rear camera implementation — 2026-11-15

User accepted current Camera brightness behaviour (“all good”) and requested
handover for the remaining camera issues and two additional rear cameras.
Latest camera-quality acceptance/handoff commit: `f7d505e2` on
`feat/s20-camera-quality`. Integrate that branch here before implementation;
the live brightness change42b1dd4d is not yet integrated into this aux branch.
Also explicitly integrate accepted main/master before full image/kernel builds.

Start with docs/x1slte-camera-aux.md and the metadata-only inventory. Two saved
overlays agree; three fixup tests pass. No auxiliary sensor has been powered,
identified or streamed by this work. Bring up one sensor at a time with bounded
model/revision reads, revision-correct tables, shared pin/rail rollback, then RAW
packing/CFA/VC validation. Do not expose lens controls until capture and switching
work. Preserve original front default and accepted display rule. Main-rear
noise/tint and native12MP remain separate unresolved tasks. Rejected dark-grid
trial must stay disabled. Native/diagnostic kernel candidate remains uninstalled.

Authoritative installed-device state and rollback: camera-improve/handoff.md.
Phone ownership was released. Read workspace NEXT.md/TASKS.md and acquire the
phone lock before device changes. Never reboot without asking the user.
Private captures/firmware/calibration/backups must remain outside git.


---

> Push status (2026-11-15): local commits complete, remote push blocked.
> Both feature-branch pushes to configured upstream
> https://github.com/oosexgang/hakios-xnu-kernel.git returned HTTP403:
> authenticated user jesvi-eulah lacks write access. No existing fork found at
> jesvi-eulah/hakios-xnu-kernel. No branches were published. Retry after access
> is granted or a writable remote is provided. Do not claim a successful push.

# User acceptance and next-agent handover — 2026-11-15

User reports “all good” after the Camera-only brightness installation and asks
for commit/push and handover. Treat this as user acceptance of current behaviour;
separate instrumented entry/exit/manual/lock tests and reboot persistence were
not performed by this agent. No reboot approval is implied.

Next agent: keep the accepted brightness rule and original front-camera path.
Continue remaining main-rear grain/gradient/red-green/tint investigation and
bring up the two additional rear cameras on `feat/s20-rear-aux` in
`work/hackios-camera-aux`. Read docs/x1slte-camera-aux.md and its generated
inventory before hardware writes. That branch contains offline wiring evidence,
not a functioning auxiliary driver. First integrate this latest camera-quality
branch and accepted main/master explicitly; preserve unrelated accepted work.

For main rear, compare bounded unpacked16-bit format6 against packed format7,
then validate native12MP separately from upscaled JPEG dimensions. Existing
native-mode/diagnostic kernel candidates remain uninstalled; any reboot needs
user approval. For auxiliary sensors, read model/revision before selecting
revision-dependent tables, activate one sensor at a time, and restore shared
pins/rails/routes. Do not copy front-camera configuration or promise every
stock resolution/FPS before live validation. Phone lock is released; check
workspace NEXT.md, TASKS.md and lock status before taking it.

Current installed brightness artifact/rollback details follow. Private captures,
firmware and device backups remain outside git. No push changes main/master.

---

# Start here: live Camera brightness installation (latest checkpoint)

Worktree: `/Users/jesvi/ios-port-research/work/hackios-camera-improve`, branch
`feat/s20-camera-quality`. Separate aux inventory is on `feat/s20-rear-aux`,
commit22256e72, in `work/hackios-camera-aux`; read docs/x1slte-camera-aux.md.

## Installed now

- `/var/jb/usr/lib/TweakInject/RedwoodCameraBrightness.dylib` and matching filter.
  Signed arm64 SHA25684f57fa867544e048c22ff078d830848709375b662c8c73c2724409decd289fe;
  downloaded readback matches byte-for-byte. SpringBoard resprung and loader log
  confirms it opened this exact named tweak. No kernel flash or reboot performed.
- Revised rule reads SpringBoard `_accessibilityFrontMostApplication` identity;
  known API/nil app yields Camera=false, unknown shape falls back to heartbeat.
  This removes dependence on adding Camera injection to the existing read-only
  image. Remount attempt failed before system-file/tweak mutation; injection
  config and runtime list remain unchanged. Original front media path untouched.
- Initial live state outside Camera: native auto1, raw30241/65536 (UI45.94%).
  After activation it remains auto1 at the same level; foreground log Camera=0.
  Camera has not launched yet after respring: requested user unlock/open it.
  Do not claim entry/exit/manual/lock or reboot tests passed without fresh evidence.
- Phone lock held by codex-camera-quality during this installation. Always check
  tools/phone-lock.sh status before touching it; never reboot without permission.

## Complete validation / rollback

1. User unlocks and opens Camera. `/var/root/set-display-70 --status` should show
   auto0, raw45220/65536 (UI70%). Check foreground Camera=1/hold log and recovery
   record at `/var/mobile/Library/Preferences/com.redwood.camera-brightness.restore.plist`.
2. Leave Camera for Settings/Home: auto must return to prior1; journal removed.
   Check manual slider change is retained while Camera stays open. Repeat with
   auto initially off and confirm exit leaves it off. Restore user's normal auto1.
3. Check Control Center, Camera termination and display off/wake, then request
   explicit approval before any reboot persistence test. No such approval yet.
4. Emergency release: create `/var/jb/etc/redwood-camera-brightness.disabled`;
   wait for restoration and verify status, then remove dylib+filter and respring
   only if uninstalling. Keep rule loaded until recovery completes. If UI cannot
   be used, restore auto via `/var/root/set-display-70 --auto-on` after disabling.
5. Private backups/artifacts: build/camera-device-20261114/brightness-install;
   device `/var/root/camera-brightness-rollback`. Backed up injection lists were
   never replaced. Diagnostic helper supports --status, --auto-on, --auto-off,
   --slider 0..1, --set70 (last is GLOBAL, not lifecycle behaviour).

Host sanitizer policy tests, strict signed iOS builds and passcode integration
checks pass. Runtime foreground API inside SpringBoard still needs Camera-entry
validation. Accepted master02636ecf must be explicitly integrated before future
full image/kernel builds; do not flash old staged rear candidates over it.
Main rear grain/tint is unresolved, native12MP and auxiliary bring-up remain
uninstalled. Front stays original; optional experimental profile stays uninstalled.

---

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

- Additional source trace: Samsung module DT separates VC0 RAW10 image from VC1 tail-PDAF in both binned and full modes. Port selects VC0/0x2b and packed Android10. Source-level routing matches; this does not establish every RAW sample's correctness, but does not support blindly changing the CFA or treating PDAF rows as normal image pixels. Details in the dark-grid document.

## Fresh covered repeats validate a partial correction; ready for userland-only trial

- User confirmed opaque coverage again. Two independent 8x/20436-line captures agree. Existing inside-table maximum absolute tile bias ~23 counts becomes 1.618 / 1.633; per-site spatial-bias RMS ~8.8..11.9 becomes ~0.51..0.58 counts.
- Additional 14.5x (interpolated gain 464) and 16x holdouts improve spatial-bias RMS from ~13..21 to ~1.55..2.24 counts, but maximum local bias remains 7..8 counts and random RMS remains high (~21..28 counts). Do NOT label this full noise reduction or stock-quality acceptance.
- Earlier failed holdout retained, cause unresolved (coverage/state/timing). Fresh independent repeats provide substantially better evidence. Evaluator now uses the real gain-dependent original inside table at every gain; prior high-gain numbers incorrectly used an 8x comparison and are superseded.
- Explicit partial-improvement gate: spatial-bias RMS reduced at least 50% at every CFA site and mean signed bias below 2 counts. All four fresh holdouts pass this gate. The earlier absolute <5 worst-tile gate is not claimed passed at high gain. Edge-slope extrapolation experiment did not help; rejected in favour of bounded interpolation.
- Full host sanitizer/regression suites and signed iOS candidate build pass; loader/trailing-input and front/exposure/gain-gate tests added. Candidate library remains SHA-256 3afdae361482abb7b6c9ec62a2552bb91c841302de4ccd667b044a4e592e226b (verify build output before installation).
- Next authorized action: back up active dev library, upload/read-back-check candidate and private module calibration, restart only mediaserverd, verify loaded/active logs and performance in app. No front tuning or kernel installation in this trial.
- Separately built rear diagnostic kernel: root-only `camera_rear_diag` options snapshotted per session (bit0 stock no-hold sequence, bit1 cold sensor); default 0 retains existing behaviour. Host I2C fault tests and isolated GICv2 kernel build pass. Tracked/build overlay sources match. NOT installed; any future reboot requires user approval.

## Live dark-grid trial rejected; original userland baseline restored

- Uploaded/read-back-verified the candidate library and private 21288-byte profile; restarted mediaserverd only. Library hash 3afdae361482abb7b6c9ec62a2552bb91c841302de4ccd667b044a4e592e226b; profile c80546377e708f12ef65c7d559e12cb765ae077a51912f36d399f162ebeb5997.
- User took IMG_0207.JPG and reports the rear still looks the same. Inspected saved image: pronounced coloured grain/tint remains. App logs confirm rear dark-grid ACTIVE, gain512, exposure20436, and StillImage1 delivered. This was a rear trial, although Camera defaults to front and must be switched explicitly in every app test.
- Trial app ran ~13–14fps; stats ~47–54ms and render ~68–74ms. No matched thermal baseline, so cannot attribute all slowdown to the candidate, but it is not acceptable to retain the added work for an unsuccessful visual result. Rolled back candidate to earlier dev build first; profile renamed .disabled.
- User then reports front dark-face/exposure regression despite unchanged numerical tuning. Restored the ORIGINAL system library by removing the dev override entirely, rather than assuming shared state changes are harmless. Active system bytes match the original backup: 881040 bytes, SHA-256 fcc5cef59533ad963aa05dfee622e554afb876bc20ed638c3e47d981a836a7ce. Dev override now .disabled-camera-quality; private dark profile .disabled. No new userland quality experiment active. Awaiting user check of front baseline.
- Phone restarted again (live boot time22:22:25). This session issued no reboot then. Brief SSH refusal delayed rollback; restored after SSH returned. No second kernel has been flashed. Existing combined21:16:40 kernel remains.
- User requests display fixed at70% whenever Camera is open; separate display-only task, not exposure/ISP tuning. Investigating reversible Camera lifecycle brightness hold with automatic-brightness restoration. Do not conflate display brightness with sensor exposure.
- User emphasizes sensor/protocol/layout reused from front. Confirmed Samsung2LD bayer_order GR/BG; module DT image VC0 RAW10/PDAF VC1; port rear uses C-PHY3/CSIS0/WDMA2 and packed format7, unlike front D-PHY4/CSIS1/WDMA1. Continue checking actual readout and state, not just declared metadata. Diagnostic kernel prepared, NOT installed; reboot permission still required.

## Front baseline confirmed; brightness request withdrawn

- User confirms front camera is back to normal with original system library. Keep that as the default front path, not merely its numerical tuning values. User requests an opt-in flag for the newer experimental front profile, independently of rear research; do not mislabel it verified Samsung tuning.
- Traced ALS/display path versus AE: inject_sensors uses automatic_lux/DisplayBrightnessAuto/DisplayNitsKey; camera AE uses RAW statistics, exposure/gain and frame timing. No direct ALS-to-AE dependency found. Screen light can indirectly illuminate a front scene; preview/display changes must still be distinguished experimentally.
- User withdrew the70%-brightness/auto-brightness-pause request after this explanation. No brightness override, notifier, daemon or setting was installed/changed. Read-only --probe ran only. Uninstalled prototype sources/builds retained privately under build/camera-device-20261114/brightness, removed from working source. Leave normal display behaviour alone.

## Front opt-in switch prepared offline; device pause respected

- User says wait and will announce readiness before installation/reboot. Do not treat elapsed time as permission. Pending question for ONE diagnostic-kernel installation/reboot is unanswered.
- Built dual-profile source dispatcher retaining ORIGINAL front worker+ISP by default and independent experimental rear worker. `redwood-front-profile=experimental` opts front into newer code for next stream; missing/other value uses original. Not labelled Samsung factory tuning.
- Build rejects shared stream/output/camera/device ABI drift; original device lifecycle retains cross-camera exclusion. Separate ISP symbol namespaces prevent old front globals and new rear state interacting. Two host tests pass; signed library builds. NOT installed; needs visual front + rear switching regression before acceptance.
- Reproduction and toggle commands: docs/x1slte-camera-front-profiles.md. Candidate sha ea002ab83015bab7bdcd3cf59e85971567b43918c881bb05d634b25e61b1fbd2.
- Rear protocol audit continues: stock CSI hardware declares Android10 format7 as supported, but stock RAW10 DMA selects Samsung-packed4 or unpacked16-bit6 according to output format. Current live port chooses Android10/7, previously primarily validated on front. Need bounded rear format6 versus format7 captures to check the packed decoder independently; source-level support alone is insufficient. No such captures run during user pause.

- Private live artifacts preserved separately from disposable build output at `/Users/jesvi/ios-port-research/discovery/s20-camera-quality/2026-11-14-live-rear/camera-device-20261114/` (mode700 parent). Contains raw measurements, JPEG inspections, failed and accepted host evaluations, logs, exact rollback binaries, and uninstalled candidates. Partial transfers/failed trials are retained as evidence, not validated inputs. Repo is clean after checkpoints; no private images/firmware/calibration blobs committed.

## Priority: focus/object-dependent dimming; manual display70 verified

- User renewed the request to disable display auto-brightness and make it70% before further camera work. This explicitly authorizes that display change; it does not authorize a reboot. Rear research/device experiments paused for this check.
- Initial legacy BKS/UIScreen setter did NOT work: new-process readback stayed auto1 and raw30241/65536. Do not count those attempts as successful.
- Native BrightnessSystemClient `setProperty:NO forKey:DisplayBrightnessAuto` succeeded. Explicit AppleARMBacklight legacy write uses the port's documented mapping raw=(.025+.95*UI)*max. With UI=.7, current raw45220/65536. Two immediate independent status reads confirm auto0 and requested UI70.00%; no looping override/daemon installed. User can still manually change the slider. This is currently a normal global manual setting for diagnosis, not an installed Camera-only lifecycle policy.
- Reproducible helper: tools/camera/set-display-70.m, signed with display-control.entitlements. Default/--status read only; --set70 explicitly writes and verifies. Actual panel thermal protection is unchanged. UIScreen's client cache reported50% while raw driver request correctly represented70%; do not confuse cached framework data, requested brightness, and measured physical luminance.
- Logs show genuine thermal display caps at83/88C in this session, independently of ALS; protection retained. The latest actual cap needs its own fresh status before attributing every dimming event to heat.
- Separate direct source finding: Camera's focus-window tap changes AE metering: local mean gets75% weight, global mean25%, before isp_ae. This is camera auto-exposure, not ALS/display auto-brightness. Front original remains active and its AE/AF/tuning has NOT been changed for this diagnostic.
- Asked user whether the unwanted change stopped, affects only preview, or affects the whole screen after auto-off/70. Await feedback before changing camera exposure behaviour.

## Offline checkpoints prepared before brightness priority

- Other-ports research: docs/x1slte-camera-other-linux-ports.md and source-comparison JSON. UbuntuTouch downstream2LD driver, set-A and CSIS-v5.3 files match our Samsung reference byte-for-byte; runtime trace retains DDK and hardware ISP. S20+ matrix status unknown; x1s/r8s/c1s camera marked working. No new sensor-layout/register workaround established. Research not deployed.
- User requests native non-binned rear, all supported resolutions/FPS. Extracted17 stock mode slots with PLL/register cross-checks into docs/x1slte-camera-mode-catalogue.json. Full4032x3024 includes24/30/60 labels;4032x2268 includes24/30/60/120;3328x1872@120, plus binning/slow-motion entries. SSM slots are special capture machinery and one is empty; do not expose their labels as ordinary iOS FPS. Source inventory is not runtime capability verification.
- Prepared explicit rear full/binned mode requests in the kernel for controlled next-session testing (sensor command b3 native4032x3024, b4 binned2016x1512, b2 existing boot-profile selection). Original front commands/defaults unchanged. New bounded probe accepts explicit rear mode and checks native2740ns line timing. Isolated kernel compile and exposure/passcode checks passed. NOT installed, no modes unlocked in the running iOS app, no new reboot approved. Pause retained while brightness issue is checked first.

## Persistent Camera-only brightness rule written offline

- User clarified precise scope again: hold70% with ALS-driven display auto paused only in Camera; resume auto on exit ONLY if it was enabled beforehand. Confirmed other agent still uses phone and explicitly requested writing/committing offline. No SSH, phone writes, restart, respring or reboot in this implementation turn.
- Added standalone Camera+SpringBoard tweak: Camera foreground heartbeat/explicit background release; SpringBoard owns verified native auto setter and legacy manual70 request. Two-second stale lease restores after Camera crash. Manual slider adjustments accepted. No camera AE/AF/ISP values or authentication hooks changed.
- Atomic0600 recovery record precedes overrides. SpringBoard restart/reboot recovery restores prior auto/manual state, checks auto read-back before clearing. Display-off restoration avoids a nonzero panel request until awake. Duplicate-library registration guard, malformed-state rejection and persistent disabled-file release included.
- Added Camera to tracked tweak-loader injection defaults for future image builds. Live installation must retain the same entry in persistent and current injection lists while preserving unrelated entries. Data-volume tweak alone is not sufficient if Camera never loads the tweak loader.
- ASan/UBSan policy tests pass (entry/steady/manual/exit, auto-off preservation, display-off, rule disable, unknown service, expired/future lease). Signed arm64 iOS dylib builds with -Wall -Wextra -Werror; passcode build integration passes. Latest dylib SHA-256 e746a68f6a164e2675832bf7789cf1ed42d8d963659446f7aaac92091f254fad.
- Documentation/install/rollback and unverified cases: docs/x1slte-camera-brightness-rule.md. NOT installed/device-verified. Restore the temporary diagnostic global-auto-off setting to the user's normal enabled mode before the eventual Camera-only trial, otherwise that diagnostic value would be saved as their prior preference.
