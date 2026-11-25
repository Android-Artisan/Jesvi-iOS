# Charging: Samsung's rules for the S20 (SM-G980F) and ours (A, 2026-11-20)

Source: ExtremeXT kernel, `arch/arm64/boot/dts/samsung/exynos990-x1slte_eur_open_21.dts` (battery_v2 node;
temperatures in 0.1 C). Ours: `xnu/files/bsd/dev/redwood_max77705.c`.

## Charge power the S20 supports
| Mode | How | Power | Hardware |
|---|---|---|---|
| USB (computer) | 5 V, Type-C current | ~2.5-7.5 W | MAX77705 |
| AFC / USB PD fixed 9 V | `pd_charging_charge_power` 15000 mW, input 1650 mA @ 9 V | **15 W** | MAX77705 (enabled by `bms_hv=1` in the S20 boot profile) |
| PPS "Super Fast" | direct charging, `dc_step_chg_charge_power` 22000 mW | **25 W** | **PCA9468** direct charger (no driver of ours yet) |
| 45 W | - | not on the S20 (S20 Ultra only) | - |

Limits: max input 9 V / 3000 mA, max charge current 3150 mA, float 4380 mV, total time 10 h on USB.
Direct-charging steps: 4110 / 4210 / 4380 mV -> 4690 / 3600 / 3220 mA.

## Protection (Samsung's values)
| Rule | Trigger | Action |
|---|---|---|
| Battery hot (swelling) | >= 42.0 C (back at 40.0) | charge current 1350 mA, float 4150 mV |
| Battery cool 1 / 2 / 3 | < 18 / 15 / 5 C (back +2 C) | 2750 / 900 / 350 mA |
| Battery too hot / cold | >= 50.0 C (back 48.0) / <= 0 C (back 2.0) | **charging stops** |
| Battery extreme | >= 70.0 C (back 68.0) | stop, warning |
| Charger IC hot | >= 60.0 C (back 58.0) | input 1000 mA, charge 1900 mA |
| Phone in use (SIOP) | screen on / thermal event | 5 V: input 1200 / charge 1000 mA; 9 V: 700 / 1000; PPS: 1200 / 2000 |
| Direct charging | battery outside 18-42 C | falls back to 15 W |
| Full / recharge | 93 % to full check; recharge below 98 % or 4310 mV | |
| USB port thermistor | table in the DTS (usb_temp_table) | cut charging when the port is hot |

## Gap in ours
- **No real battery temperature**: the fuel gauge has no thermistor; Samsung reads it on the AP's ADC. Ours falls
  back to 25.0 C, so every temperature rule above is blind. First task: the battery/USB/charger thermistors on the
  Exynos ADC (channels from the DTS `sec_thermistor` nodes).
- 9 V (15 W) is behind `bms_hv=1`; no 25 W (PCA9468 driver + PPS requests through the MAX77705's PD firmware).
- No SIOP: no lower charge current while the screen is on and the phone is hot.

## Plan
1. Thermistors (battery, USB port, charger) -> real temperatures in `bms:` and to iOS.
2. Samsung's protection table above, in charger_limits(), always on (safety, not a choice).
3. 15 W by default once (1)+(2) hold; SIOP limits while the screen is on.
4. 25 W: PCA9468 + PPS, after 15 W is proven.

## Done (2026-11-20, kernel a35118ca)
Thermistors read on the Exynos ADC v3 (0x15C40000; battery ch 4, charger ch 3, USB ch 1, Samsung's tables), every
battery poll: `bms:` log line, `kern.redwood.batt_temp/chg_temp/usb_temp` (0.1 C), and the battery's to iOS.
Protection above in charger_limits() (always on; only lowers). First reading while charging from the Mac:
battery 43.9 C, charger 52-56 C, USB 40.7 C -> 1350 mA (Samsung's swelling step).
The S20 boot profile enables fixed 9 V charging under the wall-adapter conditions above. PPS (PCA9468) and SIOP
(screen-on limits) remain open.

## 9 V retry after a temperature fallback

If an active 9 V session drops to 5 V because the battery leaves 10–42 C, the driver can negotiate again after the
battery returns to 12 C or warmer and below 40 C. The same adapter can stay plugged in. A failed read of the
VBUS ADC preserves the active source limit until a valid sample arrives. A confirmed charger voltage drop or
a VBUS reading above 10.5 V still ends fast charging for that plug-in. The USB host check and charger current
limits continue to apply.

For a wall-adapter report, capture the `bms:` lines during the first minute after plugging in with the battery
below full. The useful fields are measured input voltage/current, battery and charger temperature, and the
`bms: hv:` negotiation messages. Use network logging over Wi-Fi while the phone charges from the wall.
