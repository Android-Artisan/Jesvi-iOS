# x1slte (iOS 16.5 on the Galaxy S20, SM-G980F) — shipping log

Local branch `x1slte` only (not pushed). One entry per **verified** change: what it does, the commit(s), the boot
args that control it (default in **bold**), and how it was tested on the phone. Newest first. When cutting a release,
this file is the changelog; "Open" lists what a release must mention as known issues.

## Open (known issues)
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
