# Native AOD appearance test — software only

User request: test the iPhone14Pro-style AOD display with a SpringBoard refresh,
without panel/kernel changes or a hardware reboot. The user explicitly authorized
the test, but another agent retains phone priority. Do not interrupt that session.

## What this tests

The actual iOS16.5 native method
`-[CSCoverSheetViewController _updateAppearanceForAODTransitionToInactive:]`.
In the locally owned20F66 iPhone10,3 cache its unslid address is `0x1c48950e0`.
The method creates a UIKit animation; its block starts at `0x1c489517c`, changes
CoverSheet appearance components, and has a corresponding normal-appearance path.
Calls include appearance components, legibility, wallpaper/notification choices
and content-view presentation. The inspected path is separate from the
BacklightServicesHost hardware-AOD state machine.

The tweak checks the native implementation address plus image slide and ABI
before calling it. It does not patch that method or globally enable a capability.
Only SpringBoard loads the test. It is idle until explicitly armed from root.

**This is not a full AOD service enablement.** It tests native appearance only.
The display remains under its existing sleep/brightness policy and may turn off
normally during the test. No clock replacement, screenshot overlay or imitation
of the native rendering is supplied. If the native rendering requires additional
scene/backlight state, it may show only part of the effect or none; that is an
outstanding test result, not reason to silently fake the result.

Full AOD still needs policy/capability integration, BacklightServices scene and
frame scheduling, platform/kernel power-state support, panel low-power entry/exit,
reliable wake, privacy handling and measured power behavior. Merely enabling a
MobileGestalt capability does not establish those parts.

## Test behavior

- Root creates a fresh30-second one-shot arming token. Replaying an already-used
  token in the same SpringBoard process cannot extend/restart the test.
- Start requires the native locked keybag state, a locked UI, awake display,
  visible main lock-screen page, and neither inline nor full passcode entry.
- The native AOD appearance is requested for at most12seconds. An observing
  gesture recognizer ends the preview on touch without recognizing/cancelling
  the original touch. A watchdog also ends it on display-off, passcode visibility,
  leaving the main page, unlocking, timeout, or an explicit `end` notification.
- End calls the native appearance transition with false, removes the observer
  and invalidates the watchdog. Native exceptions produce fixed diagnostic text;
  credential or view contents are not dumped.
- No change to UIScreen brightness, idle-sleep assertions, lock timers, native
  authentication, saved passcode/keybag, MobileGestalt, BacklightServices,
  refresh rate, DECON/DSIM/panel commands, boot arguments or kernel.
- The exact framework method has not yet been invoked on the phone by this
  session. Host compile/tests do not prove native appearance or restoration.

## Build and package

```sh
tools/aod/build.sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/aod/test_token.c -o out/aod/test_token
out/aod/test_token
python3 tools/aod/package.py
```

Uses the local iOS16.5 SDK and Apple clang; no full userland/kernel rebuild.
Artifacts and hash manifest are under ignored `out/aod/`. The tar bundle contains
only the test dylib, SpringBoard filter, control helper, installer, this document
and manifest. It contains no Apple binaries or personal data.

## Device procedure, when the priority agent releases the phone

Acquire the shared phone lock as this session, or have the current holder run the
test. `device.sh` requires explicit `CLAUDE_SESSION` matching the existing owner;
it refuses another holder before making any SSH connection. No forced takeover.

1. Record passcode/relock baseline and current SpringBoard PID. Do not request,
   print or record the passcode. The user enters it only on the phone.
2. `CLAUDE_SESSION=codex-aod tools/aod/device.sh install`. This copies only the
   independent idle tweak and helper, preserving the passcode/relock tweak.
3. `CLAUDE_SESSION=codex-aod tools/aod/device.sh respring`. This terminates only
   SpringBoard so launchd reloads it. It is not a hardware/kernel reboot.
4. Confirm SpringBoard returns and software-passcode unlock/relock works. Native
   appearance has still not been requested; loading the tweak is idle.
5. User locks, wakes and leaves the main lock screen visible. When ready, run
   `CLAUDE_SESSION=codex-aod tools/aod/device.sh begin`. Observe native wallpaper,
   clock, widgets and notification appearance. Do not promise all will change.
6. Verify normal appearance returns on timeout, then repeat a fresh test and
   verify touch abort. Verify display-off/passcode transitions exit as well.
7. Confirm passcode unlock -> lock -> passcode unlock still works. Record which
   native effects actually rendered and whether restoration was complete.

To stop: `CLAUDE_SESSION=codex-aod tools/aod/device.sh end`.
To remove: end first, remove only `RedwoodAODPreview.dylib` and its plist from
`/var/jb/Library/MobileSubstrate/DynamicLibraries/`, plus
`/var/jb/redwood/bin/aod-preview` and `/var/tmp/redwood-aod-preview/arm`.
Coordinate a SpringBoard refresh to unload the removed tweak. Leave every other
tweak, keybag, passcode, image and phone-owner session alone.

If the native method/ABI differs, start is refused. If UI restoration is wrong,
stop and remove the test before trying another capability or backlight change.
A successful preview must not be documented as working low-power AOD.

## Live installation and timeout check (15 November, 03:07)

The user explicitly transferred phone priority from A to codex-aod. The independent
tweak and helper were installed, and SpringBoard refreshed (PIDs9 ->337 ->374);
no kernel flash or hardware reboot command was issued. Direct notify status
confirms the tweak loaded and found the pinned native appearance method.
A manually armed begin returned `declined-lockscreen-state`: **the native AOD
appearance transition has not yet been invoked or visually validated**. No
capability, brightness, authentication or passcode-setting change was made.

The user reported missing passcode prompting after the refresh and subsequently
reported it returned. Before that report, read-only status showed native MKB1,
raw AKS0x5, protected private saved bag present, softpasscode1/autounlock0 and the
existing passcode-relock tweak/filter intact. Cause is not established. The AOD
request had been refused; do not claim it ran or that a keybag repair was applied.

User also requested screen timeout. The documented mode1 setting was applied,
but their first12-second test stayed on. Source and copied installed-library
disassembly confirm mode1 is the wake-only heuristic; any later input disables
that branch until another detected wake. A read-only diagnostic of three globals
in the installed backboardd library showed touch_mode0x101, button service present
and input_idle274s; no process memory was written.
Switched `/var/jb/etc/redwood-idle.conf` to documented `mode=2`, preserving the
user's effective Auto-Lock60seconds. The first SSH write failed during USB loss;
a subsequent write/read verified mode2. It reloads on the next detected wake.
Current boot console logs show the10-second untouched-wake timeout firing, and
independent display state/touch_mode0x1fe confirm display-off. After-interaction
60-second behavior still needs user confirmation. Reference: x1slte-background.md.

Uptime reset while USB was unavailable; this session issued no reboot command.
Reset log reads PINRESET with PMIC WTSR. Those bits alone do not identify the
cause. User was asked whether it was a manual restart. Keep AOD idle while
resolving that observation. Fresh boot loads the idle test but does not arm it.

The helper now has `status`, reporting only native keybag flags, display-awake
and preview state. Native notify states distinguish ready, declined, active
appearance-requested, normal-appearance-requested, unsupported and exception.
These are operational diagnostics, never authentication or proof of rendering.
Deployment uses explicit-file SCP because the phone's tar could not find gzip.
Updates were staged separately and atomically renamed before refreshing so mapped
executables were not overwritten in place.
