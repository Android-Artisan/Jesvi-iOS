# x1slte (iOS 16.5 on the Galaxy S20, SM-G980F) — shipping log

Local branch `x1slte` only (not pushed). One entry per **verified** change: what it does, the commit(s), the boot
args that control it (default in **bold**), and how it was tested on the phone. Newest first. When cutting a release,
this file is the changelog; "Open" lists what a release must mention as known issues.

## 2026-11-12 — Dynamic Island pill moved up over the camera hole (user-verified on the phone)
- RedwoodIsland tweak (`userland/src/island_tweak.m`, SpringBoard): pill top gap 5 pt instead of Apple's 11.33,
  so it covers the top of the hole. Width/height tunable (`/var/jb/etc/redwood-island.conf`, respring only).
  **Withdrawn:** as a /var/jb tweak it can crash-loop SpringBoard (see the doc); to move into libredwood-sb. Commit e9a23d6. Doc: docs/x1slte-dynamic-island.md.

## 2026-11-12 — Dynamic Island over the camera hole (user-verified on the phone)
- iOS's own Dynamic Island (SpringBoard SystemAperture, Pill status bar), no overlay: the loader reports the
  iPhone 14 Pro subtype (`artwork-device-subtype` 2556) and, as a 14 Pro's tree does, `/buttons home-button-type` 2.
  Without the second, BaseBoard counts only subtype 2436 as buttonless and iOS acts like a home-button iPhone.
  `island=` **1** (in x1slte.cmdline). Commits 1b25aa4, 7f188ae.
- Tested: MobileGestalt ArtworkDeviceSubType 2556, HomeButtonType 2; pill shown, home-bar swipe and touch normal.
- Switching it: just the flag (`island=1` on, leave it out for the iPhone X notch). `init` removes MobileGestalt's
  cache when the flag changed since the last boot, so iOS rebuilds its answers (needs the next root image build).
  Doc: docs/x1slte-dynamic-island.md.

## 2026-11-12 — camera: videos save with sound; front camera tuned (user-verified on the phone)
- Videos finish and save with sound: the audio source node saw a 430-million-frame "gap" in the HAL plug-in's
  times on every buffer and zeroed 1.7 GB per buffer, so no sound reached the movie writer and every recording hung
  (mediaserverd SIGTRAP "Timed out waiting for the capture graph to stop"). Its gap limit is set out of reach
  (`inject_media.c`). Commits 552fd8a.
- Orientation: preview a mirror, photos and videos as others see you (`cam_turn=` **5**, kernel default; user-checked).
- Speed: drawing on its own thread, 48 ISP pieces, no sensor restart on start/flip (preview 22 -> ~28 fps, sensor-bound).
- Camera.app opens front + Photo after its reset time (libredwood in Camera). Rear camera black for now.
- Image: generic phone colour matrix (tunable on the phone), clipped highlights stay white, white balance starts where
  the last stream left it, smoother AE steps (user: "auto exposure is perfect").
- Tap-to-expose (meters the tapped spot), the exposure slider (ExposureBias, a multiplier with 256 = 1) on the picture
  at once, AE lock. Front screen flash (SpringBoard white window + full backlight, photo 10 frames into the light; user: "perfect").
- Encoder: 1.6x iOS's bitrate, OpenH264 low complexity, 6 threads.
- Kernel: REDWOOD_CAMERA_SENSOR's mode chooses full/binned for the S20 front sensor (stream stays full: binned looked worse).
- Dev loop: `tools/camera/media-dev.sh` loads libredwood-media from /var/redwood without a reboot.
- Commits 552fd8a..41db5a2. Tested by the user in Camera.app throughout.
- Open: Live Photo makes no movie (staging node "no pre-roll", -16958); filters greyed (no Metal in mediaserverd);
  portrait and slo-mo (no depth, no 120 fps mode); video ~20 fps when recording (CPU ISP + encoder under thermal
  throttling); two whole-phone freezes while streaming (kernel log just stops; cause unknown).

## 2026-11-12 — video encoding: our own H.264 encoder for VideoToolbox (the S20 has no AVE)
- `userland/src/h264_encoder.c`: a VideoToolbox encoder (interface reverse-engineered from AppleVideoEncoder, see
  tools/camera/CHECKLIST.md) over OpenH264 2.7.0 (BSD, `work/third-party/openh264`, `make OS=ios ARCH=arm64`), registered
  in mediaserverd by libredwood-media (iOS runs compression sessions there; `h264=0` leaves it out). Baseline profile,
  constant SPS/PPS, 4 slices / 4 threads, key frame every second, ~12 Mbit/s at 1080p30.
- Front camera's video format 1920x1080 (was 3840x2160).
- Tested: `vt_enc_test 1920 1080 90` (public VTCompressionSession API on the phone): 90 of 90 frames at 52 frames/s;
  the stream decodes 90/90 on macOS VideoToolbox with the test pattern intact. Camera.app recording: to be confirmed
  (set Settings > Camera > Formats > Most Compatible: there is no HEVC encoder).

## 2026-11-12 — front camera orientation default (`cam_turn=2`)
- User-confirmed orientation (preview and photos): libredwood-media's turn plus a mirror; default from boot now
  (`cam_turn=` **2**), live via `tools/poke 'cam.turn N'` (read at stream start). Built (kernel 04:36:50), not flashed yet
  (phone with codex-cellular).

## 2026-11-11 — front camera autofocus, steadier exposure, anti-flicker, orientation
- Autofocus: the front lens motor (AK7374, ID 0x0e at 0x0c on HSI2C24, gpp8-0 SCL / gpp8-1 SDA, x1slte DT fragment@18) is
  driven from `redwood_camera_s20.c` (Samsung's is-actuator-ak737x.c sequence: standby, ID, first position 250, active;
  soft landing on stop). libredwood-media's contrast AF and tap-to-focus move it through REDWOOD_CAMERA_FOCUS.
- AE: frames are tagged with the exposure asked two frames earlier (when the sensor applies it), so AE no longer
  overshoots and swings (seen in photo mode); 50 Hz anti-flicker exposure steps (`camera_isp.c`); front camera turned
  180 degrees in libredwood-media (user: upside down and mirrored without it).
- Tested (kernel 22:03:35): kernel log "front focus: AK737x (id 0xe) up"; `camera_test raw 60`: 60 of 60 at 3648x2736.

## 2026-11-11 — front camera at full 3648x2736, 30 frames/s, no tearing
- The stream runs Samsung's `3648x2736_30fps` mode with the FULL MIPI PLL (`cam_s20_full=` **1**; 0 = binned 1824x1368).
  Double-buffered 12.5 MB frames take two adjacent display-pool slots (`MiniIOSurface.cpp`).
- Tearing fix: the finished buffer is chosen from the DMA frame-start/frame-end status (a frame start flagged together
  with a frame end is the next frame's), not just the active slot read after the fact.
- `kern.redwood.camera_turn` (boot-arg `cam_turn`): bit 0 makes libredwood-media turn the front camera's pictures half
  round, read at each stream start, to settle orientation without a new image.
- Tested (kernel 21:53:59): `camera_test raw 60`: 3648x2736, stride 4560, 60 of 60 frames ~33 ms apart; kernel frame
  copy ~1-5 ms.

## 2026-11-11 — front camera at 30 frames/s
- WDMA1 writes MIPI RAW10 directly (DMA format 7, lines padded to 16 bytes = the `/dev/redwood-camera` layout), read
  through a cached mapping (on by default, `cam_dma_cached=0` / `cam.s20.cached 0` to disable): one ~1 ms copy per frame
  instead of packing 2.5 M pixels from write-combined memory. No turning in the kernel any more; `camera_device.c`
  front camera `turned = false` (the S20's module is the right way up).
- Tested (kernel 21:47:05, root.apfs with the rebuilt libredwood-media.dylib): `camera_test raw 90`: 90 of 90 frames,
  33-35 ms apart; kernel log: frame copy 0.36-1.2 ms.

## 2026-11-11 — camera survives a cold start: S2MPB02 buck voltages set at boot
- After a hard reset (forced power-off) the camera PMIC has BUCK1 at 1.6 V (reg 0x0c = 0xc0) and BUCK2 at 0.8 V
  (0x0e = 0x40); Samsung's kernel programs both to 1.3 V (0x90) and those values had survived warm restarts since the
  last Samsung reference boot, which is the only reason the camera worked. `redwood_s2mpb02_torch.c` now writes
  0x0c/0x0e = 0x90 and 0x10 = 0x48 (buck-boost 3.5 V) before enabling BUCK1, the buck-boost and LDO18 (`cam_pmic_base`).
- Tested (kernel 18:54:31, first boot after a forced power-off + Download Mode flash): `cam.pmic 1` reads
  0b=d8 0c=90 0e=90 0f=da 10=48 2f=ec; `camera_test raw 45`: 45 of 45 frames, 33.9 ms apart.
- Also in this kernel: `cam.s20.cached [0|1]` (debug port, off at every boot) reads stream frames through a cached
  mapping; the kernel turns frames 180 degrees (`cam_s20_rot=1` default) to undo the old userland's `turned`.

## 2026-11-11 — live front camera for iOS (`cam=5 cam_s20=1`)
- `/dev/redwood-camera` now serves the S20's real front SAK3J1 instead of the chart: `redwood_camera_s20.c` keeps the
  proven sequence running (WDMA1 alternates two buffers over its 32 slots), `redwood_camera.c` `s20_session()` packs each
  frame (RAW10, turned 180 degrees: the module is mounted upside down; `cam_s20_rot=0` = as sent) into the ring the iOS
  bridge reads. Exposure/gain/frame length from the userland AE reach the sensor (0x0340/0x0202/0x0204, 0x20 = 1x).
- `x1slte.cmdline`: `cam_fake=1` replaced by `cam_s20=1` (`cam_fake=1` still gives the chart).
- Tested (kernel 13:56:21): `camera_test raw 45` on the phone: 45 of 45 frames, AE converges (32 ms, 9x indoors), white
  balance R 1.27 B 1.84, Bayer GB/RG after the turn; user saw live frames in Camera.app (before the turn: upside down).
- User verified in Camera.app: live preview and **photo saves to Photos**.
- Open (roadmap in `tools/camera/CHECKLIST.md`): video does not save and has no mic; ~15 frames/s; binned 1824x1368
  only and no colour tuning (soft, dated look); rear cameras, zoom switching and flash not done.

## 2026-11-11 — front camera link works: first real sensor frame under XNU
- **Root cause of "sensor programs, zero CSI packets":** the camera PMIC (S2MPB02) supplies BUCK1 (1.3 V), buck-boost
  (3.5 V) and LDO18 (3.3 V) are always-on/boot-on in Samsung's x1slte device tree but off after S-Boot; XNU never
  enabled them, so the SAK3J1 answered I2C but never started its MIPI link. Now enabled at boot in
  `redwood_s2mpb02_torch.c` (`cam_pmic_base=` **1**, 0 = leave as the bootloader did). Registers 0x0b/0x0f/0x2f read
  d8/da/ec = Samsung's.
- Tested on the phone (kernel built 13:40:44): `cam.s20.front.stream 14 0 4` and `6 0 0` → 17 frames in 500 ms, frame
  start/end, PHY active; `cam.s20.dma.test 1 14` → 1824x1368 RAW10 frame, DMA clean, guards intact; decoded picture in
  `logs/camera/pmic-base-boot-20261111/frame-1-0.png` (real scene). Proof on Samsung's kernel:
  `logs/camera/baseoff-split-20261111/` (same XNU sequence: supplies on 15 frames, off 0).
- Diagnostics added (opt-in third argument bits of `cam.s20.front.stream`: 4 line probe, 8 273-register dump, 16 match
  Samsung leftovers, 32 CSIS power-domain cycle — once per boot only, 64 base supplies per run), `cam.pmic 1` full map,
  shared register list + `tools/camera/compare-regdump.py`, Samsung reference split modes in `tools/reference-boot/`.
- Not yet: live preview in Camera.app (still the synthetic bridge), rear cameras, exposure/colour tuning.

## Open (known issues)
- Camera: front only, photos only. Video recording, microphone in video, full resolution, colour tuning, rear
  cameras, zoom switching and flash are open (`tools/camera/CHECKLIST.md`, section ROADMAP).
- Proximity is registered/enabled but near/far operation is not verified; raw diagnostics are available through `shub.prox`.
- True Tone is a fixed mild warmth approximation, not an ambient colour measurement. User can disable it normally.
- CoreMotion raw gyro samples work; the combined device-motion/attitude stream still returns no samples.
- The screen timeout is our stand-in (backboardd), not iOS's own idle timer: native fix is TASKS R1b.
- Mic: the kernel's diagnostic peak counter reads full scale on every capture although recorded files do not clip (only
  with `snd_debug=1`).
- The thermal fix below was confirmed on one cold start after the flash (peak 83 C, no restart); watch the
  `reset: last run ended by ...` line at the top of each boot log.

## 2026-11-08

### Compass heading from the phone's magnetometer (locationd, software fusion)
- locationd took the iPhone X's capability table at its word and waited for the motion coprocessor's fused
  streams (HID page 0xFF0C), so heading stayed at north. libredwood now runs inside locationd too
  (src/inject_location.c) and clears the coprocessor-fusion bits 3/4/5/7 of CLPlatformInfo::motionCaps() right
  after locationd computes them (dispatch_once GOT hook, data only: patching code drops CS_VALID and CloudKit
  then kills locationd). locationd now builds its software compass ("Legacy compass") and AP sensor fusion.
  Boot-arg loc_swfusion=0 turns it off.
- Phone verified live (runtime kern.exec_inject, no reboot): locationd stable, CLLocationManager heading
  calibrates (accuracy +-30 deg) and reads 88-89 deg with the phone pointed east (user's reference).
- Apple's Compass app: apps read device motion in-process through CoreMotion, which has its own copy of the
  table (motionCaps() at cache 0x1919a4580). libredwood now also loads into Compass and pre-fills CoreMotion's
  cached word (0x1e20a7290, token 0x1e20a7288) without the SPU bits, so CMDeviceMotion comes from locationd.
  User verified: the Compass app dial works. Other apps that want CMDeviceMotion need their process name added
  to the libredwood INJECT group in userland/build.sh.
- Gyro units for locationd's fusion: it took our degrees/s as radians/s, so resting noise turned the heading by
  ~2 deg/s and a reopened Compass drifted off. IOHIDEventGetDouble/FloatValue are hooked through locationd's GOT
  and convert gyro X/Y/Z only (apps' own CoreMotion conversion untouched). Boot-arg loc_gyro_rad=0 disables.
  Verified: heading steady for 40 s at rest; after a figure-8 calibration the Compass app read 94-99 deg with the
  phone hand-aligned east (user).
- Open: magnetometer scale. A figure-8 sphere fit gives Earth's field as ~11.4 "uT" at sens_mag_k=15000 (expected
  ~41.5 uT here), i.e. ~0.054 uT/count. Try boot-arg sens_mag_k=54000 at the next reboot; it should give the
  calibrator a field it trusts (better accuracy than +-30 deg).
- Flashed (recovery, 2026-11-08 10:02): locationd + Compass hooks permanent; magnetometer default now 0.054 uT/count
  (sens_mag_k=54000; 15000 restores). The hub sends Samsung's *calibrated* field (accuracy byte), so Apple's
  calibrator on top learned bogus biases (8 stale rows in consolidated.db CompassCalibration, cleared) and the app
  read 317 deg pointing north. CompassIgnoreCalibration=1 (mobile's com.apple.locationd via prefset at boot; also
  set in root's) makes iOS take the field as calibrated: user says Compass works, but turns slower than the phone.
- Open: dial lags turns -> check gyro scale (sens_gyro_k=61) with a stationary quarter-turn test (tools: b-gyrocmp);
  native alternative: feed Samsung's uncalibrated stream (GEOMAGNETIC_UNCALIB_SENSOR) to Apple's calibrator.
- Every app now (kernel): kern.exec_inject words starting with "/" match a path prefix; libredwood goes into
  /Applications/ and /var/containers/Bundle/Application/ apps, whose CoreMotion then gets device motion from
  locationd (verified: an unnamed program in the app folder got device motion; one outside got none).
  locationd's hook sets CompassIgnoreCalibration itself at every start (root's and mobile's prefs; boot-arg
  loc_magcal=1 lets Apple's calibrator work). Flashed 10:2x; after the reboot the user confirms Compass shows a
  real heading and moves, nothing set by hand.

### Optional mild True Tone and corrected gyro units
- True Tone's Settings toggle controls a Samsung DQE CGC 3x3x3 colour cube, R100% / G98% / B94%. No per-frame
  CPU/GPU filter, no edits to the panel's factory gamma/AOR tables. Preference persists; initially off.
  The earlier gamma-LUT attempt corrupted colours and was replaced. Generated neutral cube nodes match all81
  values in Samsung's bypass reference; runtime updates run between presentations with scanout briefly masked.
- Phone verified: toggle on/off leaves backboardd PID16 unchanged; OFF restores control0x80, path0xb1, size0.
  User confirmed "Mild warmth, other colours normal" on the colour-cube build. The original unsupported selector
  crash is fixed; a dedicated bounded display_warmth sysctl lets mobile backboardd apply the preference safely.
- Gyro bridge sends degrees/s to Apple's HID gyro service. CoreMotion itself converts to rad/s; removing the
  duplicate conversion fixes app readings that were57.3x too small. sens_gyro_k still specifies millionths of
  degrees/s per raw count (default61). Existing accelerometer/auto-rotation path is unchanged; user confirms it works.
- Verified with public CMMotionManager: timestamped live gyro samples, near-zero at rest, substantial response to
  user rotations (observed peak12.36rad/s). tools/sensors/gyro-probe.m checks samples, rates and fused stream status.
  Combined attitude/device-motion still provides no samples and is not claimed working.

### Adaptive brightness — `00f5122`, `1f8099b`
- Native Auto-Brightness setting controls the port-side policy. Stable light is accepted after 3 seconds brighter /
  5 seconds darker (user-adjusted from 5/8), then ramped smoothly. Manual slider changes anchor the preference at the current lux;
  substantial ambient changes blend back toward the base curve. Anchors persist after 10 seconds without edits.
- Wake holds the current level for at least 5 seconds; screen-off and stale-sensor gates stop brightness writes.
  Settled sampling is 2 Hz, inactive 1 Hz, with up to 20 Hz during ramps. No GPU brightness filter.
- Phone boot and matching native/panel nits verified. User confirmed brightness responds to cover/light tests and
  the True Tone option appears. User subsequently accepted auto-brightness as done. Host policy tests cover
  debounce, spikes, manual anchors, wake, monotonicity and bounds.
- Download Mode recovery: stop `tools/serial-watch.sh` and its `cat /dev/cu.usbmodem...` child before Heimdall;
  they hold the serial interface. Stock Heimdall then flashes successfully without administrator privileges.

## 2026-11-07 (evening)

### Display: FHD+/HD+ modes up to 120 Hz, factory brightness, smooth power-off — `469ca87`..`b37e85a`
- Resolution switch fixed (DECON refused every frame: the window's source still delivered 1440x3200): live dev
  command `fb.mode N 1` (1 = 1080x2400/120, 2 = 1080x2400/60, 3 = 720x1600/120, 4 = 720x1600/60); boot `fb_mode=`.
  iOS reports 120 (UIScreen.maximumFramesPerSecond) after the timing metadata refresh. Apps' actual fps varies.
- Factory brightness: the panel's own gamma/AOR tables from its SPI flash (60 and 120 Hz), checksum-verified,
  on by default (`fb_cal=1`). User: range, direction and colours correct.
- Power-off: the picture fades (DECON window alpha) instead of the backlight; then display off; frames stopped
  while dark (trigger masked) so the screen-off profile (memory 421 MHz) engages. User: "much much better".
- Dev knobs: `fb.fade [dd [offms [alpha]]]`, `fb.factory 0|1`, `fb.mode`.

## 2026-11-07

### Sleep: screen timeout, screen-off power profile, Low Power Mode
- **Screen timeout works** (it never did): SpringBoard's idle timer depends on AttentionAwareness, which hands it an
  untagged lost-attention event here ("attention client lost event tag is not a number") and it never dims. Our own
  idle timer in backboardd (`userland/src/inject.c`) presses the lock button after **10 s** when woken and not touched
  (`idle_lock_s=`), or the **Auto-Lock** time once touched (SpringBoard's lock state is unreliable here). **Off by default** (user, 18:55): `idle_lock=0` off, `1` only an untouched wake, `2` also Auto-Lock (Settings value; Never = off), not while sound played in the
  last 5 s (`kern.redwood.snd_quiet_ms`). `idle_lock=0` turns it off. Tested by the user: lock screen 10 s, home screen 60 s. Known gap: a silent video unlocked still locks.
- **Screen-off profile** (automatic, no switch): memory 421 MHz (was 2730 always; 2.7 vs 15.7 GB/s measured), every
  CPU cluster at its low step, GPU lowest (`bsd/dev/redwood_exynos_acpm.c`, `dvfs_sleep=0` off). Full clocks again
  the moment the display comes on (before the first frame).
- **Low Power Mode** (Settings switch, also iOS's automatic one): `lpmd` (`userland/src/lpmd.c`, job
  `com.redwood.lpmd`) passes it to `kern.redwood.lowpower`: memory 1352 MHz, clusters at their first step, GPU 572
  (`dvfs_lpm=0` ignores it).
- Tested: lock screen idle -> "idle timer: 10 s ... display off" -> "power: screen off: memory 421 MHz"; Low Power Mode
  on -> "memory 1352 MHz, clusters and GPU capped".
- Unused here but kept: backboardd's `com.apple.private.attentionawareness` entitlement lookup answers yes if it
  comes back empty (`aw_ent=0` off).

### Fix: restarts at the lock screen after a cold start / flash — thermal throttling
- Cause (measured): the big M5 cluster reached 108-111 C during boot (all clusters at top clock); the SoC's thermal
  trip reset the phone through the PMIC (`RST_STAT` PINRESET, S2MPS19 PWRONSRC WTSR, OFFSRC 0), 2-3 times, until a boot
  started cool enough. The governor read temperatures once a second, throttled at 86/96 C and went back to full clock
  6 C below the mark.
- Now: temperatures every 250 ms; CPU steps at 80/90 C, GPU 82/92 C; full clock again only 10 C below the mark
  (`bsd/dev/redwood_exynos_acpm.c`; `dvfs_hot=` still overrides). Idle is 55-60 C, so normal speed is unchanged.
- Tested: cold start after flash: peak 83 C, throttled at 80 C, back to 2730 MHz at 69 C, no restart.
- Every boot now logs why the last run ended, decoded with Samsung's names:
  `reset: last run ended by RST_STAT ... ; PMIC on ... off ...`.
- Log access / back-to-menu no longer panics ("Console I/O from interrupt-disabled context"): `redwood_dsay` and
  `snd_log` skip the console when interrupts are off (the sensor hub's stop message did it).

### Audio: debug off by default, self-checks, quieter speaker and vibration — `26dfb5b`
- `snd_debug=` **0**: routine log lines (amps on/off, 60 s stats, mic start/stop), mic per-sample peaks and the
  adapter's per-write peak scan + report only with `snd_debug=1`. Errors are always logged.
- Self-checks: amplifier fault -> reset and set up again (3 per boot; before: off until restart); every quiet minute
  both amps must answer on I2C, else recovery (3 per boot); mic WDMA4 stuck 200 ms -> capture path restarted; playback
  DMA stalls counted. `rwsys kern.redwood.poke snd.health` reports it all (memory only, safe while playing).
- Speaker maximum 70 % of before: `snd_below=` **109** (1/8 dB under the amps' full level; was 84).
- Vibration maximum 60 % of before: `haptic_max_pct=` **34** (was 56).
- Debug command table 32 -> 48 entries (it was full).
- Tested: tone, 4 s record with vibration, `snd.health` clean, log shows `109/8 dB` and `at 34 %`.

### Microphone: faster start, no idle CPU; amp I2C fix — `ac6c313`
- Mic worker sleeps while nothing records (was waking every 10 ms), polls every 4 ms while recording, 20 ms settle:
  capture starts in ~40 ms (was 100-120 ms).
- GPIO: mic power / haptic reset pins share a bank with the speaker amps' bit-banged I2C; a neighbour's
  read-modify-write could set the I2C lines' output latch high. Lines are now driven low explicitly every time.
- Tested: record, record + vibration, record + system sounds (6 s, 0 dropped frames), both amps answer after.

### Microphone works in iOS — `2dc3b49` (and `d206416`..`976b8d7`)
- `/dev/redwood-mic` (48 kHz, 16-bit mono) fed from the three DMIC converters (VTS S_LIF -> UAIF6 -> NSRC4 -> WDMA4);
  iOS sees "iPhone Microphone"; Voice Memos records. `mic_enable` (default on with `snd=1`).
- Watchdog WDT_CLUSTER0 switched off at boot (`wdt_off=` **1**): it restarted iOS at the lock screen (`36fc39a`).

### Speaker sound works — `150a7cd`, `4ffbc65`, `4ed2fa0`, `976b8d7`
- ABOX RDMA3 -> ASRC -> UAIF2 -> two CS35L41 amps, Samsung's register set (`snd=1 snd_samsung=1`). Amp start
  retried at boot and recovered on the playback worker.

## Earlier (2026-11-06, see git log)
Charging (watchdog kick, PD/AFC 9 V, limits), live battery %, USB serial + USB networking + SSH, flashlight, Wi-Fi
signal and power save, Samsung DVFS tables, iOS-only storage (system on `super`, 108 GB `/var`), UFS TurboWrite
(`ufs_tw=1`, `6dbc8c5`), rotation (`shub=6`), vibration (`haptic=5`), Location Services, Bluetooth audio fixes.
