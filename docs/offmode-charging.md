# Charging with the phone off (branch feat/offmode-charging, 2026-11-20)

Today: with a cable in, S-Boot powers the phone up for charging (it shows up_param's lpm.jpg, now iPhone's charging
picture), then starts recovery = our loader, which boots iOS. So "Power off" from the menu, or Shut Down in iOS with
a cable in, ends up in iOS again (loader.c power_off note), and iOS's Shut Down lands in the HackiOS menu.

Signal: the PMIC's power-on source, S2MPS19 PWRONSRC (0x10; OFFSRC 0x11), names from Samsung's
sec_debug_reset_reason.c: bit0 PWRON (power key), bit1 JIGONB, bit2 ACOKB (charger), bit3 MRST, bit4/5 ALARM,
bit6 SMPL, bit7 WTSR. The kernel already reads and logs it at boot (xnu/files/bsd/dev/redwood_exynos_sound.c,
"reset: last run ended by RST_STAT ... PMIC on 0x.. ( .. )"). ACOKB without PWRON = a charger start.

Plan
1. Measure: power off with the cable in, read the boot log's "PMIC on" line (expect ACOKB).
2. Loader: read PWRONSRC itself (port redwood_pmic_read / speedy to xnuloader), and on a charger start show
   iPhone's charging screen (the battery art, level from the charger), screen off after a few seconds, Power = boot
   iOS, cable out = power off (PS_HOLD low).
3. iOS Shut Down: power off directly (no menu) in release mode; with a cable in, the charging screen.
