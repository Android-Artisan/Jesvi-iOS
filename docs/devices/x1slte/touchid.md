# Native Touch ID on the S20 — status and handoff (2026-11-19)

Branch `feat/s20-touchid` (worktree `work/hackios-touchid`) = `main` + `feat/s20-fingerprint` + this work. Boot argument
**`touchid=1`** switches everything on; without it nothing changes. Plan and background:
[docs/research/proposals/touchid/README.md](../../research/proposals/touchid/README.md).

## What works (verified on the phone)

- Settings → Touch ID & Passcode → Add a Fingerprint: Apple's own screen drives the real ultrasonic sensor. Phase 1
  runs until Samsung's matcher reaches 100 % and commits the template; phase 2 ("adjust your grip") completes.
- Lock screen: "Touch ID or Enter Passcode"; a finger starts a real identify; a failure shakes the lock; the keystore
  enforces iOS's rules (passcode since boot, 48 h, 5 failed fingers).
- Sensor images: accepted almost every touch once calibration runs (see "the key fix").

## Not working yet

1. **Matching.** Identify with good images and IPC line high still returns NO MATCH (TA error 0x20), sometimes
   0x1d (contamination). Leads, most likely first:
   - the secure user ID / auth token Samsung binds at enrolment (CONTROL 0x2d SAVE_SECAUTH_ID refused: "sid is 0");
   - PREPARE never returns matcher metadata (0 bytes) although `fp prepare` passes it on when it does;
   - TA identify, `init_identify_stub_d` in finge.elf around 0x12146c (decompiled in tools/re/out), returns 0x20
     from several branches: find which one fires;
   - the TA's own log (FUN_00144604) would show it: find where TEEGRIS keeps it.
2. ~~Enrolled finger forgotten at reboot~~ **fixed, verified on a clean reboot (2026-11-19).** Three causes: RedwoodMesa
   refused the master component (user ~0) in 61, so the catacomb stayed in Catacomb/prepare/; GetCatacombState listed
   only the user's component, so biometrickitd sent NoCatacomb instead of LoadCatacomb (now both, cap 16); and
   redwood-fpd now answers before TEEGRIS is up and restarts biometrickitd on the first unlock (never reopen the
   bridge mid-run: it stops receiving requests).
3. ~~Ring on the lock screen~~ done (2026-11-19): drawn inside the cover sheet view (no window), faint, only while a
   match waits on the lock screen itself; enrolment uses a secure window with its home indicator hidden.
   A failed or rejected lock-screen capture shakes the lock (verified 2026-11-19 10:02).
   The second grey home bar on the lock screen is NOT from the ring (seen with libredwood-touchid-ui unloaded) nor
   from libredwood-alwayson. A view dump of SpringBoard (2026-11-19 10:10) shows CSHomeAffordanceView (alpha 0) with
   MTLumaDodgePillView and CSAlwaysOnHomeAffordancePillView hidden but MTStaticColorPillView visible (alpha 1) at the
   home bar's place: the likely grey bar. Hooking CSHomeAffordanceView layoutSubviews to hide it or set alpha 0 made
   SpringBoard abort at every start (userabort, no reason logged; auto-restored). Next: find why (try the hook in the
   image build instead of a /var/mobile copy; check whether loading any rebuilt copy from /var/mobile aborts), or find
   which SpringBoard setting picks the static pill (luma dodge unsupported by the GL renderer?).
6. Overheating (2026-11-19): 109 C big / 105 C little before PMIC resets (RST_STAT PINRESET + WTSR) a few minutes
   after each of many back-to-back boots; 55 C with an external cooler. A stronger kernel thermal guard is needed.
4. Popup text shows raw TOUCH_ID_ISSUE_* keys (strings only in a Touch ID iPhone's iOS).
5. Screen-off / AOD unlock: needs the sensor's WUHB line (Samsung qbt2000_wuhb_irq_handler); we use the touch panel.

## Pieces

| Piece | Where |
|---|---|
| Fake Secure Enclave Touch ID (redfin SweetMesa port, MIT) | `xnu/files/iokit/Kernel/RedwoodMesa.cpp` |
| Unlock rules | `MiniAppleKeyStore.cpp` `redwood_bio_*` |
| Sensor daemon (bridge ↔ `kern.redwood.tee` "fp ..." commands) | `userland/src/redwood-fpd.c` |
| iPhone 8 Touch ID driver (libBKDM1) | biometrickitd runs on the iPhone 8 dyld cache: `userland/src/dsc_exec.c` (`DYLD_SHARED_CACHE_DIR` + `DYLD_SHARED_REGION=private`), cache at `/var/redwood/iphone8-dsc` (`tools/touchid/push-iphone8-cache.sh`, 2.5 GB, once per data volume) |
| biometrickitd job | `userland/com.apple.biometrickitd.mesa.plist` (dsc-exec must be ad-hoc signed: launchd panics on unsigned) |
| Device tree `/arm-io/mesa` | `xnuloader/loader.c` (touchid=1) |
| Sensor ring | `userland/src/fp_ring.m` (+ `touchid_ui.m`, Settings marker now unused) |
| Calibration on IPC | `redwood_teegris.c` fp_run (identify) + redwood-fpd `fp cbge` before enrolment |

## The key fix: calibration

The sensor's IPC line (GPA1-3) going low is `CBGE_REQUIRED` (Samsung `qbt2000_common.c`: every IPC irq is that event).
Samsung's `FPBAuthService::new_cbge_mode` (libbauthserver 0x15abc0) then runs CONTROL 69 → TEE opcodes 68/70/67 → 71;
67 exports each slot (slot byte + 0x96000 room, as `update_cbge` 0x14c794) — the kernel's `fp cbge`. Captures taken
with the line low were PARTIAL / DUPLICATED / LIGHT_TOUCH; with it, accepted in a row. A calibration taken with a
finger on the sensor makes identify fail the contamination check (0x1d): calibrate finger-off.

## Apple ↔ Samsung flow (redwood-fpd)

- Apple's progress follows Samsung's percentage; phase 1 ends at the template commit (100 %).
- Apple sends Cancel between its phases and MatchMode for phase 2: the enrolment session lasts from EnrollMode to
  the commit; phase-2 touches on the sensor count as grip samples (`grip_phase`, enrolling client only).
- Rounds `fp run 3 1 45 4` under a 120 s step limit (a 60/60 s pair sent OP_TIMEOUT and Apple aborted).

## Dev loop

- Daemon only: build `userland/src/redwood-fpd.c`, `ldid -S`, copy to `/var/root/redwood-fpd`, kill both running
  copies (`ps -A -o pid=,comm=` loop; no awk on the phone), start `/var/root/redwood-fpd`. No reboot.
- Kernel/root: `/private/tmp/.../touchid-kfix.sh` steps = `./x1slte.sh kernel`, `userland/build-gpu-y2s.sh`,
  `./x1slte.sh payload "$(cat devices/exynos990/x1slte/x1slte.cmdline) touchid=1"`, `./x1slte.sh pack`,
  `CLAUDE_SESSION=B tools/x1slte-auto.sh --recovery`.
- If the USB network stays inactive after a replug: serial `usb restart` (`tools/x1slte-usbcon.py /dev/cu.usbmodem101 "usb restart"`).
- Logs: `curl http://10.77.0.2:8080/log | grep -E "RedwoodMesa|redwood-tee: fp|redwood-fpd"`.
