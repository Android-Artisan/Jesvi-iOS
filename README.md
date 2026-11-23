# Jesvi iOS

Jesvi iOS runs iOS 16.5 (20F66) on the Exynos 990 **Galaxy S20 LTE** and includes a development port for the **Galaxy S20+ 5G**. The project builds the XNU 8796.121.2 kernel, UEFI loader, drivers and device services. The iOS system image comes from Apple's iPhone X firmware.

[Releases](https://github.com/jesvi-eulah/Jesvi-iOS/releases) · [Development builds](https://github.com/jesvi-eulah/Jesvi-iOS/actions/workflows/private-images.yml) · [Telegram](https://t.me/eulah_rom)

[![Jesvi iOS running on a Galaxy S20. Watch the demo on YouTube.](docs/IMG_5690%202.jpg)](https://www.youtube.com/watch?v=-4pcSUe0ny0)

## Feature status

The following features are reported for the **S20 LTE**. For S20+ hardware coverage, use the [y2s evidence record](docs/release/device-evidence-y2s.md).

| Area | S20 LTE |
|---|---|
| Display and graphics | 60/120 Hz, touch, True Tone, auto-brightness and Always-On Display. Metal on Mali-G77 and OpenGL ES through Mesa. |
| Network and calls | Dual-band Wi-Fi, hotspot, Bluetooth audio, LTE data, VoLTE calls and carrier SMS. |
| Camera and audio | Front and three rear cameras, speaker, earpiece and microphone. |
| Sensors and power | Motion, compass, proximity, ambient light, haptics, flashlight, GPS, USB PD charging and thermal management. |
| Storage and USB | Persistent UFS storage, exFAT microSD in Files, USB networking, serial console and Finder file access. |
| Security and accounts | Passcode and Touch ID enrolment. Fingerprint matching and unlock remain incomplete. Apple ID setup currently requires a companion Hackintosh. |

App Store downloads, iMessage and FaceTime remain unavailable. On builds with the native clock service, the phone uses saved time or image build time until it can synchronize over the network. Offline time is approximate. [Device configuration](devices/exynos990/x1slte/device.conf) · [Clock behavior](docs/devices/x1slte/background.md#clock-restoration-and-network-synchronization) · [Development log](docs/SHIPPING.md)

## Download and install

1. Match the **model number** in the [device list](#tested-devices) to the target in the ZIP name. Get the package and checksum from [GitHub Releases](https://github.com/jesvi-eulah/Jesvi-iOS/releases). For a development build, extract the artifact from a successful [TWRP workflow run](https://github.com/jesvi-eulah/Jesvi-iOS/actions/workflows/private-images.yml).
2. Verify the installer against its supplied SHA-256 checksum. Keep a backup off the phone. A ZIP ending in `_preflight.zip` runs checks only.
3. Choose the procedure for your phone below. **Fresh install erases existing Android or iOS userdata.** Write-enabled installers built from current main start after a 45-second cancel window when launched from TWRP's Install screen. Press Volume Down or Power during that window to cancel.

**S20 LTE:** Use TWRP 3.7.1 or newer. With the phone connected directly to your computer, run one of the following commands, substituting the actual path to the verified ZIP. A preserving update requires an existing iOS data volume of the matching size.

```sh
# First install or deliberate erase
tools/install/twrp-install.sh --erase /path/to/fresh_x1slte.zip

# Preserve an existing installation
tools/install/twrp-install.sh /path/to/update_x1slte.zip
```

The computer helper asks for approval before copying the package. For instructions on returning to TWRP or Android, see the [install guide](docs/release/install.md) and [rollback guide](docs/release/rollback.md).

**S20+ 5G:** Use the y2s fresh-install ZIP for initial setup. To update an existing installation, select **Log access (USB)** in the loader and follow the y2s guide. `./y2s.sh rflash` updates boot and recovery, including the root image. `./y2s.sh tflash` also updates the system image. Reinstalling the TWRP ZIP erases userdata. [S20+ setup and update instructions](tools/y2s/README.md)

## Build

For the default S20 LTE target on a prepared macOS checkout:

```sh
make doctor    # Check tools and inputs
make check     # Run source checks and host tests
make kernel    # Build the x1slte kernel
make release   # Package the outputs selected in release.conf
```

The checked-in configuration selects fresh mode, a 64 GB iOS data volume and read-only packages. To build a writable S20 LTE installer, run:

```sh
./build.sh release --set TARGETS=x1slte --set MODE=fresh --set WRITES=true --set OUTPUTS=twrp
```

For S20+ 5G, use the [y2s build guide](tools/y2s/README.md) to prepare the board firmware, loader and candidate bundle. For build prerequisites and packaging options, see [local builds](docs/release/local-build.md), [release configuration](docs/release/config.md) and [CI operations](docs/release/operations.md).

## Repository layout

| Path | Contents |
|---|---|
| [devices/exynos990](devices/exynos990) | Shared platform settings and per-device profiles |
| [xnu](xnu) and [xnuloader](xnuloader) | Kernel, board drivers and UEFI loader |
| [userland](userland) and [sysimage](sysimage) | Native services, first-boot setup and system-image assembly |
| [tools/install](tools/install) and [tools/y2s](tools/y2s) | Recovery helpers and S20+ build tools |
| [docs](docs) | Device evidence, release procedures and development notes |

## Tested devices

### x1s · Galaxy S20

- **SM-G980F** (LTE, `x1slte`) — **Update tested.** The phone booted with user data intact after a TWRP update on `G980FXXSNHYB7`. Use the LTE fresh-install or update ZIP. [Test record](docs/release/qualification-x1slte.md) · [Install guide](docs/release/install.md)
- **SM-G981B** (5G) — **Preflight only.** Use the read-only `x1slte` ZIP to check the device. The 5G model has a different modem and RAM configuration.

### y2s · Galaxy S20+

- **SM-G985F** (LTE) — **Preflight only.** Use the read-only `y2s` ZIP. The LTE boot chain and partition layout still need measurement.
- **SM-G986B** (5G) — **Boot-tested.** The board and partitions were measured on `G986BXXSNHYB1`. The development port uses TWRP for fresh installs and the USB loader for updates. Fresh-install and peripheral testing is pending. [Board evidence](docs/release/device-evidence-y2s.md) · [Install guide](tools/y2s/README.md)

For regional G980 and G986 devices, recovery accepts firmware revisions that identify the phone’s model and warns on untested revisions. It also checks the Exynos SoC, bootloader unlock state and partition sizes. These builds support Exynos hardware only.

## Credits and licenses

Jesvi Jonathan maintains Jesvi iOS. [Android-Artisan](https://github.com/Android-Artisan) contributed the S20+ 5G port, board measurements, first-install tooling and loader update workflow in [PR #7](https://github.com/jesvi-eulah/Jesvi-iOS/pull/7).

The project builds on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), Apple's open-source XNU (APSL 2.0), Mu-Silicium and Mesa. Please credit Jesvi iOS and its contributors when building on this work. Jesvi iOS is independent of Apple and Samsung. [Notices](NOTICE) · [Third-party licenses](docs/legal/THIRD_PARTY_LICENSES.md) · [Legal information](docs/legal/LEGAL.md)
