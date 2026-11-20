# CPU, GPU, thermal and idle: Samsung's tuning vs ours (A, 2026-11-20)

Sources: Samsung's device tree (ExtremeXT's exynos990 kernel, `arch/arm64/boot/dts/exynos/exynos9830.dts`, on the SSD:
`iOS-Jesvi/git/ExtremeXT__android_kernel_samsung_exynos990`), the live ExtremeROM capture
([android-reference.md](android-reference.md) section 2: cpusets, schedtune, cpuidle), and ours
(`xnu/files/bsd/dev/redwood_exynos_acpm.c`: governor, thermal steps, power profiles). Parser for the thermal zones:
the trip table below (values decoded from the decompiled DTB).

## Why it matters

The phone resets itself at 105-110 C (PMIC WTSR, "RST_STAT PINRESET") after a few minutes of heavy work (boot after
flashing, many reboots, 2026-11-19 and 11-20). Samsung's own kernel never lets it get there: it starts a continuous
power budget at 83 C and switches the big cores off at 96 C.

## Thermal: zones and trips

| Zone (sensor) | Samsung governor | Samsung control point | Samsung alerts / actions | Ours |
|---|---|---|---|---|
| BIG (M5, cpu6-7) | power_allocator (IPA), 50 ms while hot | **83 C** (passive) | 95/100/105/110 alerts; **hotplug out at 96 C, back in at 91 C**; hot 115 | steps at 78/86 C to 1586/1196 MHz, every 250 ms |
| MID (A76, cpu4-5) | IPA, 50 ms | **83 C** | 95/100/105/110 | steps 82/90 C to 1794/1482 MHz |
| LITTLE (A55, cpu0-3) | step-wise | none | 76, 81, 86, 91, 96, 101 (one cooling step each) | steps 82/90 C to 1456/1053 MHz |
| G3D (Mali) | IPA, 100 ms | **88 C** | 105/110/115; GPU's own ladder 800 -> 702 -> 572 -> 455 -> 377 -> 260, sustainable **455 MHz** | steps 82/90 C to 572/455 MHz |
| ISP, NPU | step-wise | - | 76..101 every 5 C | not read |

All Samsung trips have 5 C hysteresis (2 C on the "switch-on" points: 55 C BIG/MID, 78 C GPU, where IPA starts
tracking). IPA is not a ladder: it shares a power budget between clusters and the GPU every 50 ms so the hottest zone
converges on the control temperature, instead of dropping a rung and overshooting.

## CPU frequencies

| Cluster | Samsung min / default floor / max | Ours: rungs / touch / frame floor |
|---|---|---|
| A55 (cpu0-3) | 442 / **806** / 2002 MHz | 806 .. 2002 (touch 1456/1248, frame 1456) |
| A76 (cpu4-5) | 507 / **845** / 2600 MHz | 1066 .. 2504 (touch 1404/1222, frame 2106) |
| M5 (cpu6-7) | 546 / **962** / 2730 MHz | 1196 .. 2730 (touch 1066/858, frame 1898) |
| GPU | 156 / start 260 / 800 MHz, interactive governor at 94 % load, every 5 ms | 156 .. 800, 50 ms tick |

Our idle floors are higher than Samsung's on A76 (1066 vs 845) and M5 (1196 vs 962): the lowest rung is where idle
cores sit when they are not power-gated, so that is idle heat for nothing.

## Where work runs (the biggest difference)

Samsung's scheduler (EMS, `ems { emstune set@0000 "normal default" }`) is energy-aware:

- **weights**: little 100, mid **200**, big **20-25** (idle_weight 200 / 400 / 40-50): a task goes to a mid core first;
  the big cores are nearly a last resort.
- **ontime**: a task moves little -> mid above ~10 % of a core's capacity (105/1024) and mid -> big only above
  ~57 % (586/1024), and only after running that long.
- cpusets: background/system-background on **0-2**, foreground 0-2 + 4-7 (not 3), top-app 0-7.
- top-app gets a 5 % frequency boost, nothing else.

XNU here has one performance level for all eight cores (`hw.nperflevels 1`): it spreads work evenly, so the M5s - the
hottest, least efficient cores (weight 20 against the A76's 200) - get as much as any other. That alone explains
most of the heat in plain UI use.

## Idle and screen off

| | Samsung | Ours |
|---|---|---|
| core idle (C1) | per-core power-down: A55 230 us / 2000 us residency, A76 340/1000, M5 455/1000 | cpuidle=1 (per-core power-off, same PSCI states) |
| cluster idle (C2) | cpuidle-ucc: cluster power-down allowed on 4-7 (and 6-7 in the other configs) | not used |
| screen off | EMS "power-scenario" mode; big cores mostly idle by weight; ACPM sleep | small cores only (0-3) + 500 ms tick + low memory clock (2026-11-15) |

## What to change, in order of value

1. **Big cores off when hot**: hotplug cpu6-7 out at 96 C, back in at 91 C (Samsung's thresholds). We already have
   the mechanism (screen-off small-cores mask); this is the step that keeps the phone from ever reaching 105-110 C.
2. **Faster, earlier thermal control**: poll every 50 ms above the switch-on point (55 C), aim at 83 C for BIG/MID and
   88 C for the GPU, with 5 C hysteresis; release a step only below control - 5. Use the GPU's own ladder down to its
   sustainable 455 MHz before touching anything else.
3. **Keep work off the big cores** (EMS weights + ontime): mask cpu6-7 from the scheduler unless the A76s have been
   above ~60 % busy for several ticks (Samsung's ontime boundary), as the screen-off mask already does for 4-7.
4. **Idle floors at Samsung's defaults**: A76 845, M5 962 (now 1066 / 1196).
5. **GPU**: tick its governor faster (Samsung 5 ms, ours 50 ms) or at least jump on a slow job (we already do).
6. Later: cluster power-down (C2) for 4-7 when both cores of a cluster are idle.

Each one goes behind its own boot argument first (project rule), tested with the overheating scenario (flash +
boot + heavy UI), then made default.
