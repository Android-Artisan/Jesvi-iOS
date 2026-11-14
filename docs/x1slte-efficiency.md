# Opt-in efficiency profile

Requires a kernel containing this change and `dvfs=2` (already in `x1slte.cmdline`).
The profile is disabled by default. The standard boot line is unchanged.

Enable at boot by appending `dvfs_efficiency=1` to the boot arguments when preparing
an image. Omit it or use `dvfs_efficiency=0` for the existing normal behavior.

After installing that kernel, root can switch the profile without restarting:

```sh
sysctl -w kern.redwood.efficiency=1
sysctl kern.redwood.efficiency
sysctl -w kern.redwood.efficiency=0
```

The runtime setting lasts until reboot; the boot argument selects the initial
setting on each boot. Changing a boot argument requires a new boot; merely editing
the local cmdline does not change the running phone.

| Domain | Efficiency ceiling |
| --- | ---: |
| A55 (4 cores) | 1248 MHz |
| A76 (2 cores) | 1794 MHz |
| M5 (2 cores) | 1898 MHz |
| GPU | 455 MHz |
| Memory | 1352 MHz |

These are existing frequency levels handled by ACPM, which controls voltage and
clock transitions. All cores remain available. The governor still scales down
under light load. Touch, frame, wake and slow GPU-job boosts cannot exceed these
ceilings. Efficiency-mode demand estimation uses the capped clock instead of an
unreachable boost rung. Normal-mode demand estimation is unchanged.

Thermal caps, lower explicit frequency targets, Low Power Mode and screen-off
caps take precedence whenever stricter. Domains explicitly disabled with a zero
clock target remain unmanaged. Fixed-clock configurations (`dvfs_gov=0` or
`dvfs_gov_gpu=0`) also honor the profile. The governor applies runtime changes on
its next tick, normally within 50 ms plus ACPM request time. Initial requests are
capped too, once the ACPM thread starts; this does not control S-Boot clocks or
the period before ACPM initialization (default 10 seconds).

`acpm.rate` is a developer override and can bypass the policy until the governor
next updates the domain; do not use it to validate sustained profile ceilings.
`dvfs=1` is monitoring only and cannot enforce this profile.

The profile trades peak CPU/GPU performance and memory bandwidth for potentially
lower power and temperature. Cooling and battery improvement have not yet been
measured on the phone. No scheduler priorities or lock algorithms are changed.

Validation: `python3 tools/power/test_efficiency.py` compiles the actual C cap
policy on the host and checks normal restoration, each ceiling, stricter targets,
and thermal/LPM/screen-off combinations. The Exynos GICv2 DEVELOPMENT kernel also
builds. On-device toggle, clock, temperature and call/UI tests remain pending;
no image was flashed and no device rebooted during implementation.

General optimization candidates, separate from this profile:

- Retry failed governor clock requests: the existing normal governor caches the
  requested clock even when ACPM rejects it, which can suppress retries.
- Measure screen-off wakeups before reducing governor polling; retain timely
  temperature checks and responsiveness to touch/display/profile events.
- Measure energy and frame latency before replacing CPU pairing with a true
  cluster-capacity-aware scheduling policy.
