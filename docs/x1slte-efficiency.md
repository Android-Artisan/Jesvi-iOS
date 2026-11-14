# Automatic cooling and efficiency

`lpmd` now enables cooling automatically: no boot flag or reboot is required to
run the new daemon on the existing kernel. The live service uses the existing
Low Power Mode caps. The new kernel's stronger efficiency profile and revised
thermal thresholds require that kernel to be installed at an approved reboot.
No device reboot or flash was performed for this change.

## Automatic policy

| Zone | Engage | Recover at or below |
| --- | ---: | ---: |
| M5 super cores | 76 C | 68 C |
| A76 | 80 C | 72 C |
| A55 efficiency cores | 82 C | 74 C |
| GPU | 80 C | 72 C |

Engage when any zone reaches its own mark. Release only after all zones stay at
or below their own recovery marks for **30 seconds**. M5 intervenes first; an
A55 at 78 C alone no longer forces the same cap.
- Check every **5 seconds**, using the kernel's existing **10-second** reports.
  This is comfort/power control; the kernel's faster thermal protection remains.
- Missing, invalid, or unchanged telemetry for 30 seconds holds/enables cooling;
  fresh, cool reports and a full recovery dwell are required to release it.
- Preserve a cap across daemon restarts. The service has launchd KeepAlive.
- Preserve the user's Settings Low Power Mode choice. Automatic cooling does
  not change the notification or show a false Settings switch state.

On the running old kernel, `kern.redwood.lowpower` is Settings LPM OR automatic
cooling. On a kernel with `kern.redwood.efficiency`, automatic cooling instead
controls that node, and Settings continues to control `kern.redwood.lowpower`.
The daemon owns the efficiency node while automatic control is running; manual
writes can be replaced at the next poll. `lpmd --settings-only` runs only the
original Settings mirroring behavior, if manual control is wanted.

These are provisional die-temperature policy values, not battery/case limits or
measured optimums. Battery savings, sustained temperatures and UI/call latency
still need real-use comparison. CPU utilization alone does not trigger a cap.

## Clock ceilings

| Domain | Normal maximum | Automatic on existing kernel (LPM) | New kernel efficiency |
| --- | ---: | ---: | ---: |
| A55 (4 cores) | 2002 MHz | 1456 MHz | 1456 MHz |
| A76 (2 cores) | 2504 MHz | 1794 MHz | 2106 MHz |
| M5 (2 cores) | 2730 MHz | 1794 MHz | 1794 MHz |
| GPU | 800 MHz | 572 MHz | 572 MHz |
| Memory | 2028 MHz | 1352 MHz | 1352 MHz |

All cores remain available. The load governor still scales down. Touch, frame,
wake and slow GPU-job boosts cannot exceed the selected profile's caps. In the
new efficiency profile, demand estimation uses the capped clock instead of an
unreachable boost rung. Normal-mode estimation is unchanged. Thermal, screen-off,
user LPM and lower explicit targets take precedence whenever stricter. The new
profile cuts M5 peak clocks by 34%, A76 by 16%, A55 by 27%, and GPU by 29%: the
strongest CPU cut is on the M5s. The existing kernel only exposes whole-device
LPM control; cluster-specific clock changes and earlier hard thermal steps need
the new kernel.
Explicit zero targets remain unmanaged; `dvfs=1` is monitoring only.

The new profile also caps initial ACPM requests, once ACPM starts (default 10 s
into boot), and fixed-clock configurations. It does not control S-Boot clocks.
The developer override `acpm.rate` bypasses profile policy until a later governor
update and should not be used to validate sustained ceilings.

## Revised kernel thermal steps (staged, not live)

| Policy | Existing kernel | New kernel |
| --- | --- | --- |
| M5 zone steps | 85 / 95 C | 78 / 86 C |
| A55/A76 zone steps | 85 / 95 C | 82 / 90 C |
| GPU zone steps | 87 / 97 C | 82 / 90 C |
| Step release hysteresis | 10 C | 8 C |
| Brightness reduction starts | 90 C | 82 C |
| Stronger brightness reduction | 96 C | 88 C |

Per-zone clock steps remain checked every 250 ms. M5 step 1 caps at 1586 MHz
and step 2 at 1196 MHz, releasing below 70/78 C respectively. Other domains
release their steps below 74/82 C. Automatic cooling acts earlier and affects memory
as well. `dvfs_hot=C` overrides thermal step 1, with step 2 now C+8. Brightness
retains its separate 4 C release hysteresis and `fb_hot` override.

## Live deployment and rollback (2026-11-14)

The original service was replaced in its existing `user/501` launchd domain:

- Binary: `/var/root/lpmd-auto`
- Job: `/var/root/com.redwood.lpmd-auto.plist` (label `com.redwood.lpmd`)
- Transition/error log: `/var/tmp/redwood-power.log`
- Probe: `ssh s20 /var/root/lpmd-auto probe`
- Original binary backup: `/var/root/lpmd-before-auto-20261114`
- Original job backup: `/var/root/lpmd-job-before-auto-20261114.plist`

The live override is loaded for this boot; files in `/var/root` survive reboot,
but the old boot image still starts its original service. Updated source and
`prebuilt/userland/lpmd` make automatic control the default in future rebuilt
userland images. Merely rebooting the current old image does not install it.

Rollback the live service (the original daemon restores Settings' kernel value):

```sh
ssh s20 'launchctl bootout user/501/com.redwood.lpmd && launchctl bootstrap user/501 /var/root/lpmd-job-before-auto-20261114.plist'
```

For standalone testing of the new kernel without the automatic daemon owning the
node: `sysctl -w kern.redwood.efficiency=1` enables its profile; `=0` disables it.
`dvfs_efficiency=1` remains an optional initial boot setting, not a requirement
for automatic cooling. These sysctl commands require root and `dvfs=2`.

## Validation

```sh
python3 tools/power/test_efficiency.py
cc -std=c11 -Wall -Wextra -Werror tools/power/test_auto.c -o /tmp/test-redwood-auto
/tmp/test-redwood-auto
```

Tests cover cap combinations, normal restoration, hysteresis, cool dwell,
interrupted recovery, missing telemetry, embedded NULs, incomplete records,
invalid zones and freshness despite unrelated log writes. lpmd cross-compiles
with warnings treated as errors. The Exynos GICv2 DEVELOPMENT kernel builds.
Live service/probe/telemetry and natural threshold transitions are checked without
synthetic heating; the new kernel's thermal steps are not yet tested on-device.

Further optimization candidates: retry failed governor clock requests instead
of caching failed requests; replace the temporary log-based telemetry with a
small, timestamped temperature sysctl to avoid copying the log; measure screen-off
wakeups before slowing governor polling or changing scheduler capacity policy.
