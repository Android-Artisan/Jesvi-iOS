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
| 1080×2400 (FHD+) | 120 Hz | User confirmed usable; measured 12 DECON frames/100 ms; purple blur and slightly washed-out appearance reported |
| 720×1600 (HD+) | 60 Hz | Live switch succeeded; 6 DECON frames/100 ms immediately and after 0.5 s; user confirmed normal |
| 720×1600 (HD+) | 120 Hz | 12 DECON frames/100 ms at both checks; user confirms normal image and correct blur with default CA timer |

The stock device tree exposes 120 Hz only at the two lower resolutions. It also
lists 48 NS, 60 HS, and 96 HS variants; these have not been implemented/tested here.
Do not equate the raw TE counter delta with pulse count: it contains packed bits.
DECON completed-frame deltas are the refresh verification used above.

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
  plus a mild washed-out appearance. Native mode looks correct. A rendering,
  color-management, or panel-mode issue remains to be isolated; Metal is a
  hypothesis, not a confirmed cause.
* Returning to HackiOS after changing resolution leaves menu scaling wrong:
  loader GOP geometry remains native while the display is in the lower mode.
  Existing `loaderQuiesce()` restores the firmware channel but does not restore
  panel/DSIM/DSC/window geometry. The return protocol now has version marker `XNULIVE3` and active output
  dimensions. The loader projects its native menu coordinates into the cropped
  firmware channel, preserving the native backing-store stride. Runtime return
  validation is pending. Cold reboot restores native firmware setup.
* Saved Log Access output also contains `Console I/O from interrupt-disabled
  context @serial_console.c:168` on return. This is separate from the slider
  exception. No claim that either is fixed.

## Verification

DSC host test: `clang -Wall -Wextra -std=c99 -pedantic -O2 -o /tmp/dsc-test
proposals/dsc/test.c proposals/dsc/redwood_dsc.c && /tmp/dsc-test`: PASS,
including byte-for-byte Samsung PPS checks at all three resolutions.
Kernel with HD profiles builds successfully; flash readback passes. Record live
counter results and visual confirmation below as each mode is tested.
