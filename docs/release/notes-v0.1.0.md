# Jesvi iOS 0.1.0 — Galaxy S20 (SM-G980F)

The first packaged build: iOS 16.5 on the Exynos 990 Galaxy S20 LTE, installed from TWRP.

## Files

| File | What |
|---|---|
| `Jesvi-iOS_0.1.0_<date>_x1slte.zip` | install / update iOS from TWRP (keeps your iOS data when it is already installed) |
| `Jesvi-iOS-Restore_0.1.0_<date>_x1slte.zip` | back to Android: puts an empty Android partition map back, then flash any ROM |
| `*.sha256` | checksums |

## Install

1. Flash TWRP for the S20 (`twrp-3.7.0_11-0-x1s.img`, Odin AP slot) and boot into it.
2. Put the ZIP on microSD / USB-OTG, or `adb sideload` it.
3. *Install*, then hold **Volume Up** when the installer asks.
4. Reboot: the Apple logo, then iOS.

Only the SM-G980F 8 GB / 128 GB with bootloader `G980FXXSNHYB7` is accepted; the installer checks
before it writes. EFS, modem, bootloader and param partitions are never touched.

## Works

Display at 120 Hz with auto-brightness and AOD; Metal and OpenGL ES games; 4G data, VoLTE calls and SMS;
front and three rear cameras with video; Wi-Fi, Bluetooth audio, USB networking and Finder; touch,
buttons, vibration, charging, sensors and compass, passcode, thermal control; the screen sleeps on the
lock screen and stays on while you use the phone.

## Not yet

Fingerprint, full GPS, App Store purchases.
