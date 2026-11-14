# Persistent Camera-only display-brightness rule

**Staged, compiled and host-tested; NOT installed/device-verified.** The other
agent owns the phone. This checkpoint performs no SSH, install, process restart
or reboot. The earlier manual auto-off70 diagnostic was not this rule.

## Requested behaviour

- Stock Camera foreground: save current auto-brightness state and requested
  manual level, pause ALS-driven auto-brightness, request70% once.
- Manual slider changes during Camera are accepted and held. Do not continuously
  force70% against the user's slider.
- Camera background/exit/crash: restore the previous auto-enabled/disabled state.
  If the user adjusted the slider in Camera, retain that manual choice; otherwise
  restore the previous manual level. Normal auto adaptation resumes if enabled.
- Preserve thermal caps. No camera exposure, focus, sensor/ISP, passcode, keybag,
  authentication, wake or unlock changes. The ALS is not physically powered off;
  its automatic-display-brightness consumer is paused.

## Implementation

`userland/src/camera_brightness_rule.m` is one standalone tweak filtered to
Camera and SpringBoard. Camera sends a renewable foreground notification lease.
SpringBoard owns brightness changes, independently of Camera's lifetime.
Background clears the lease; a dead/hung Camera expires within two seconds.
Control Center's inactive-foreground phase does not intentionally end the lease;
only background does, and the timer is in common run-loop modes.

Use the verified native `BrightnessSystemClient` property setter for
`DisplayBrightnessAuto`; the earlier BKS/UIScreen setters failed read-back.
Manual requests use AppleARMBacklight's legacy slider curve, the same conversion
verified by `set-display-70.m`. Native requested brightness is distinguished from
UIScreen's stale cached value and physical/thermal-capped panel luminance.

A versioned atomic0600 recovery record stores only auto mode and a manual level
in `/var/mobile/Library/Preferences/com.redwood.camera-brightness.restore.plist`.
It is saved before changing settings. SpringBoard crash/reboot recovery restores
that state before accepting another Camera session. Restoration reads back auto
mode on a later run-loop turn before clearing the record. If the display is off,
restore auto mode but defer the nonzero brightness request until display-on.
Malformed/unreadable records or failed persistence prevent a new override.
A runtime Objective-C registration guard prevents duplicate timers if both tweak
paths load the library. No methods in SpringBoard or Camera are hooked.

## Build and validation

```sh
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/camera/test_camera_brightness_rule.c -lm -o /tmp/test-camera-brightness
/tmp/test-camera-brightness
sh tools/camera/brightness/build.sh /absolute/private/output
python3 tools/passcode/test_build_integration.py
```

The policy tests cover enter/steady/manual/exit, preserving disabled auto mode,
locked/display-off exit, disabled rule, unknown service state and expired/future
leases. Signed iOS arm64 compilation succeeds with warnings treated as errors.
These tests do not establish live SpringBoard permissions, Control Center
behaviour or reboot persistence. Those remain mandatory device checks.

## Installation after exclusive phone access is granted

1. Preserve current injection configuration and existing display preference state.
   Remove the temporary global diagnostic setting: outside Camera restore the
   user's normal auto-enabled state through the native setter before starting
   the rule. Do not mistake the diagnostic auto-off state for their preference.
2. Install the built dylib and matching filter plist together into
   `/var/jb/usr/lib/TweakInject/` (readable dylib,0644 plist). The filter includes
   only `com.apple.camera` and `com.apple.springboard`.
3. Ensure the persistent `/redwood/etc/inject.conf` and current runtime injection
   list include the existing tweak loader for Camera. Append Camera to that
   entry while retaining every unrelated injection. `userland/build.sh` now adds
   Camera to the same loader list for subsequent image builds. SpringBoard
   already loads that loader. Do not overwrite the running root image to install
   this tweak or replace the passcode bridge.
4. Restart Camera and activate the new SpringBoard tweak in an agreed respring
   window. No phone reboot is needed for initial testing. Test auto-on and
   auto-off entry/exit, a manual slider change, Control Center, lock/wake and a
   killed Camera; verify native state and driver requests after each transition.
5. A later user-approved reboot must confirm startup loading and recovery.
   Merely placing a file in a directory is not reboot validation. If another
   agent flashes an image, confirm its Camera tweak-loader entry is retained.

Create `/var/jb/etc/redwood-camera-brightness.disabled` to release an active hold
and prevent new holds. Keep the library present until restoration is confirmed;
then remove the dylib/filter and restart the affected processes if uninstalling.
The disabled flag is persistent and must be removed to re-enable the rule.
