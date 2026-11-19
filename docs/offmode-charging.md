# Charging with the phone off (branch feat/offmode-charging, 2026-11-20)

Today: with a cable in, S-Boot powers the phone up for charging (it shows up_param's lpm.jpg, now iPhone's charging
picture), then starts recovery = our loader, which boots iOS. So "Power off" from the menu, or Shut Down in iOS with
a cable in, ends up in iOS again (loader.c power_off note), and iOS's Shut Down lands in the HackiOS menu.

Signal: the PMIC's power-on source, S2MPS19 PWRONSRC (0x10; OFFSRC 0x11), names from Samsung's
sec_debug_reset_reason.c: bit0 PWRON (power key), bit1 JIGONB, bit2 ACOKB (charger), bit3 MRST, bit4/5 ALARM,
bit6 SMPL, bit7 WTSR. The kernel already reads and logs it at boot (xnu/files/bsd/dev/redwood_exynos_sound.c,
"reset: last run ended by RST_STAT ... PMIC on 0x.. ( .. )"). ACOKB without PWRON = a charger start.

Measured 2026-11-20 (cable in, menu Power off, no button): S-Boot showed lpm.jpg, then our loader started and booted
iOS; the kernel logged "RST_STAT 0x40010000 (PINRESET PORESET); PMIC on 0x8 (MRST)". Not ACOKB: S-Boot's charging
restart looks like a manual reset, which a long Volume Down + Power hold also gives. So MRST alone is not enough.

Design
- power_off() (menu, and iOS Shut Down in release mode) first writes a note: misc block 1 "jesvi-off <RTC seconds>".
- At start the loader reads and clears the note. Note younger than 60 s and PWRONSRC without PWRON (the kernel reads
  it; or the loader through speedy) = the charger brought it back: boot the kernel with charge=1.
- Kernel, charge=1: before launchd, draw iPhone's battery (Apple's art: outline, fill by the real level, bolt; red
  low, green charging) from the battery driver, dim after 5 s; Power key = continue into iOS; charger out = power off
  (PS_HOLD low). Keeps the CPU on the small cores at the lowest clock meanwhile.

Plan
1. Measure: done (MRST, above).
2. Loader: read PWRONSRC itself (port redwood_pmic_read / speedy to xnuloader), and on a charger start show
   iPhone's charging screen (the battery art, level from the charger), screen off after a few seconds, Power = boot
   iOS, cable out = power off (PS_HOLD low).
3. iOS Shut Down: power off directly (no menu) in release mode; with a cable in, the charging screen.
