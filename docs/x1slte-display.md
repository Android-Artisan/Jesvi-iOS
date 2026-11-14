# S20 display modes and brightness

## Hardware and status (2026-11-07)

SM-G980F / x1slte, S6E3HAB hubble1, panel ID `81 01 03` (96.5 MHz oscillator variant).
Sources: `reference/dts/dtbo.img.0.dts`, hubble1 panel commands in Samsung's
`drivers/video/fbdev/exynos/panel/s6e3hab/s6e3hab_hubble1_a3_s0_panel.h`,
and DSC calculations/tests in `proposals/dsc/`.

| Panel output | Refresh | Status |
| --- | --- | --- |
| 1440×3200 (WQHD+) | 60 Hz | Native boot, user reports correct blur |
| 1080×2400 (FHD+) | 60 Hz | User confirmed normal/sharp; measured 6 DECON frames/100 ms |
| 1080×2400 (FHD+) | 120 Hz | User confirmed usable; measured 12 DECON frames/100 ms; initial purple/washed-out appearance; updated driver retest: user confirms correct blur/colors |
| 720×1600 (HD+) | 60 Hz | Live switch succeeded; 6 DECON frames/100 ms immediately and after 0.5 s; user confirmed normal |
| 720×1600 (HD+) | 120 Hz | 12 DECON frames/100 ms at both checks; user confirms normal image and correct blur with default CA timer |

The stock device tree exposes 120 Hz only at the two lower resolutions. It also
lists 48 NS, 60 HS, and 96 HS variants; these have not been implemented/tested here.
Do not equate the raw TE counter delta with pulse count: it contains packed bits.
DECON completed-frame deltas are the refresh verification used above.

## Display presets: what each mode sets (2026-11-14)

Pick a preset with `fb_mode=` in `x1slte.cmdline`, then rebuild the boot image and flash it:
`./x1slte.sh payload "$(cat x1slte.cmdline)" && ./x1slte.sh pack && CLAUDE_SESSION=NAME tools/x1slte-auto.sh --recovery`.
One restart; everything below follows the mode automatically, with nothing set by hand.

| `fb_mode` | Panel | iOS draws at | Scale | Touch scan |
| --- | --- | --- | --- | --- |
| 0 (none) | 1440x3200 @ 60 | 1125x2436 | 3x | normal 60 |
| 1 | 1080x2400 @ 120 | 1125x2436 | 3x | HS120 |
| 2 | 1080x2400 @ 60 | 1125x2436 | 3x | normal 60 |
| 3 | 720x1600 @ 120 | 750x1624 | 2x | HS120 |
| 4 | 720x1600 @ 60 | 750x1624 | 2x | normal 60 |

iOS always lays out 375x812 points (the iPhone X); only the pixels per point change. 720x1600 at 3x drew 2.4 times
the pixels the panel shows; at 2x it is 1.2 Mpx, about the panel's own count. UIKit has no fractional scale (720/375
would be 1.92), so 2x is the nearest.

What follows the mode, and where it lives:

- **Render size** (kernel, `MiniIOMobileFramebuffer.cpp`): at `fb_mode` 3/4 with `fb_scaler=1`, the framebuffer is
  750x1624 and `/chosen display-scale` + `/product artwork-scale-factor` are 2. `fbscale=N` overrides (`fbscale=3`
  restores 3x at 720p). Full-screen surfaces stay contiguous from 1.2 Mpx (`MiniIOSurface.cpp`), so the scaler still
  reads the frame directly.
- **MobileGestalt scale** (init, `userland/src/init.c` `mg_scale_sync`): MobileGestalt takes main-screen-scale/pitch
  from its built-in iPhone X table (3, 458 ppi), not from the device tree, and caches them. With a 750-pixel display
  that is a 250-point screen (everything cramped). Init patches the cached scale and pitch in place at every boot to
  match the kernel (2 -> 305 ppi, 3 -> 458). If iOS rebuilds the cache, the first boot after that comes up at 3x and
  the next boot corrects it.
- **Status bar** (`userland/src/statusbar_shift.h`, `sbs_vps`): UIKit chooses the status bar layout by pixel width and has
  none for an iPhone X at 750, so it fell back to its red debug bar ("rdar:45025538"). Anything not "Split" becomes
  `_UIStatusBarVisualProvider_Split1125`, the notch layout of the 3x screen. Log: `/var/mobile/Library/Logs/redwood-statusbar.log`.
- **Touch scan rate** (kernel, `redwood_sec_ts.c` `ts_set_scan_rate`): the controller scans at 60 Hz unless told otherwise.
  At 120 Hz modes the driver sends Samsung's set_scan_rate (0x94 `{1, 2}` = in step with vsync, HS120), like Android
  does on a mode change. Before: about 60 finger reports a second at 120 Hz. After: several times that, and scrolling
  went from 56-69 to about 94 frames a second. `touch_scan=N` sets the mode byte; 255 turns it off. The read-back says
  `0 0` even when the command takes effect.
- **iOS refresh**: the driver publishes the mode's refresh in `TimingElements` (`SyncRate` 7864320 = 120 in 16.16).
  At boot that is in place before backboardd starts.

Idle heat (same day, affects every mode): the USB mux worker (`redwood_usbmux.c`) polled every 1 ms and the haptics
loop (`redwood_cs40l25.c`) every 4 ms, even idle: about 1,100 wake-ups a second of the big cores, which sat at 84-92 C
and throttled. Both now sleep when quiet and are woken by a packet, link change or haptic sample. Big-core timer
wake-ups fell about 8x; idle temperature after boot was 72-75 C.

Open (2026-11-14): scrolling at 120 Hz still has about a quarter of frames at 60-Hz spacing and a few 35-57 ms
hitches. CPU and GPU are not saturated (backboardd ~30 %, GPU 70-95 % idle while scrolling). The M5 cores averaged
1.6 GHz (the cooling cap is 1794 MHz above 76 C; the frame floor is 1898 MHz).

## Live switch

`rwsys kern.redwood.poke "fb.mode"` reads counters without changing mode.
The current driver supports a single transition from the native boot state:

| Command | Target |
| --- | --- |
| `fb.mode 1 1` | 1080×2400, 120 HS |
| `fb.mode 2 1` | 1080×2400, 60 NS |
| `fb.mode 3 1` | 720×1600, 120 HS |
| `fb.mode 4 1` | 720×1600, 60 NS |

The second argument permits S-Boot's different underrun-reference starting value;
it does not bypass the other register checks. A cold restart is required between
mode tests. Reversible transitions are not implemented yet.

The source feeding the DECON window must change in the same shadow update as
the window: crop the firmware IDMA/DPP input, and scale the active channel to
the target size. Updating only the window freezes completed frames (fixed in
`469ca87`). HD uses 360×80 DSC slices, not FHD's 540×40; its PPS matches the
Samsung table in the host test. The live mode command serializes panel commands
against the brightness worker using the DSIM lock.

Panel output size and iOS render size are different. iOS still renders at its
existing logical size; lower panel resolution alone does not establish lower
GPU load or lower temperature. `CA_HEARTBEAT_RATE=120` was tried by restarting
backboardd: logs show rendering above 60 fps, but sustained 120 rendered fps has
not been established. Panel refresh at 120 is independently measured.

## Open defects

* Brightness minimum: backboardd respring, not a kernel reboot. At 19:17:05,
  CoreBrightness logged `Unexpected nits type`, then raised
  `-[__NSCFNumber objectForKey:]: unrecognized selector`. The failing unslid
  return PC is `0x1af037458`, in `CBSliderCommitTelemetry`'s
  `fillEntry:withTimestamp:slider:apce:andTrackedState:`. It requests
  `DisplayBrightness` and expects a dictionary containing `NitsPhysical`,
  `AuroraFactor`, and `EDRHeadroom`; the value returned was a number. The injected library now checks the metadata on the telemetry logging queue
  before recording a commit; non-dictionary records are skipped. Three live
  zero→40% cycles preserved backboardd PID 303; the rebuilt image also passed
  zero→40% while scanning at 720×1600/120. This is a telemetry crash guard,
  not a repair of the incomplete brightness metadata/calibration pipeline.
* Brightness range: current nits/DBV model and interpolated AOR are not a measured
  calibration. Minimum does not appear fully black; min/max range needs work.
* At 120 Hz, user reports purple in blurred Reachability and power-off backgrounds,
  plus a mild washed-out appearance in the original FHD+ run. Native mode looks correct.
  Retesting both 720/120 and 1080/120 with serialized DSIM commands gave correct
  colors/blur. 720/120 also looked correct with CA_HEARTBEAT_RATE=120. A rendering,
  color-management, or panel-mode issue remains to be isolated; Metal is a
  hypothesis, not a confirmed cause.
* Returning to HackiOS after changing resolution leaves menu scaling wrong:
  loader GOP geometry remains native while the display is in the lower mode.
  Existing `loaderQuiesce()` restores the firmware channel but does not restore
  panel/DSIM/DSC/window geometry. The return protocol now has version marker `XNULIVE3` and active output
  dimensions. The loader projects its native menu coordinates into the cropped
  firmware channel, preserving the native backing-store stride. A staged firmware-framebuffer capture after returning from 720×1600/120
  confirms that Log Access fits the 720×1600 output, including its bottom hint. Cold reboot restores native firmware setup.
* Saved Log Access output also contains `Console I/O from interrupt-disabled
  context @serial_console.c:168` on return. This is separate from the slider
  exception. No claim that either is fixed.

## Verification

DSC host test: `clang -Wall -Wextra -std=c99 -pedantic -O2 -o /tmp/dsc-test
proposals/dsc/test.c proposals/dsc/redwood_dsc.c && /tmp/dsc-test`: PASS,
including byte-for-byte Samsung PPS checks at all three resolutions.
Kernel with HD profiles builds successfully; flash readback passes. Record live
counter results and visual confirmation below as each mode is tested.

## iOS refresh reporting

Live switching originally left `TimingElements` at the boot-time 60 Hz. The
driver now republishes timings and notifies clients after a mode change. A
backboardd restart is still needed to rebuild its cached display description.
Measured at FHD+/120 after the restart: `UIScreen.maximumFramesPerSecond=120`,
`CADisplay.refreshRate=0.008333333333333333` seconds, registry SyncRate=7864320
(120 in 16.16). Before restarting, UIKit still reported 60 despite registry
metadata having changed. `CA_HEARTBEAT_RATE=120` was enabled for the retest.
User reports Safari/UFO measurements are unreliable; sustained 120 fps in Safari
is not established; hardware scanout, advertised maximum,
and actually delivered application frames are distinct measurements.

## Brightness register cache and factory data

The worker now compares both DBV and AOR before skipping a write. It only caches
a successful transfer, invalidates the cache on failure, and schedules at most
three 50 ms retries instead of spinning. On-device test passed: 0 → 0.0064 → 0 model nits emitted DBV 0 → 0x0a → 0
at unchanged AOR 0xc27. All writes succeeded; backboardd stayed at PID 16.

Factory data extraction succeeded through hardware SPI20: JEDEC `c2 28 14`,
normal checksum `411d`, 120 HS `d472`, both marker 1. The initial GPIO SPI
attempt returned all zeros and was rejected. The hardware reader uses USI mode
`0x15c22010`, CMU_CMGP gates `0x2064/0x2084/0x2088`, divider `0x1820`, and
Q-channel `0x3044`, from Samsung cmucal-sfr.c. Pin functions, USI mode and clocks
are restored after each read. Only JEDEC/READ commands are implemented.

The factory gamma/AOR path interpolates the 74 normal-range points (2..420
nominal nits), selecting separate normal/120 HS data. It decodes packed 10-bit
gamma values before interpolation. `fb.factory 1` loads, validates, saves the
original gamma, and enables the path; `fb.factory 0` restores the prior gamma
and approximate AOR. `fb_cal=1` opts in at boot. Visual validation is pending.
VINT/ELVSS/IRC and HBM are outside this first gamma/AOR implementation. See
`proposals/panel-calibration/README.md` for extraction, hashes and host tests.
