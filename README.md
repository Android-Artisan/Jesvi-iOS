# Jesvi iOS

iOS 16.5 running natively on the **Samsung Galaxy S20 (SM-G980F, Exynos 990)**. The XNU kernel,
loader, drivers and user-space glue are built from this tree; the iOS system files come from an IPSW
you download yourself. Built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) and laid out
the same way: a common tree, plus one directory per device.

> Experimental. It replaces Android on the phone. Read [Install](docs/release/install.md) and
> [Rollback](docs/release/rollback.md) before you start.

## What works on the S20

### Hardware

| Area | What works |
|---|---|
| Display | QHD, FHD+, and FHD modes, 60 Hz and 120 Hz, True Tone, auto-brightness, and Always-On Display (off by default) |
| Touch and buttons | Multitouch, lock-screen pocket rejection, power and volume buttons |
| Sensors | Accelerometer, gyroscope, compass, proximity, and ambient light |
| Location | Standalone GPS through Core Location |
| Cameras | Front and three rear lenses with zoom switching, photos, video with sound, and main-camera autofocus |
| Audio | Speaker, earpiece, microphone, and Bluetooth A2DP audio |
| Haptics and flashlight | Vibration and the rear LED. Both pause at the 45 °C battery temperature limit. |
| Power and charging | Battery level and temperature, charging, CPU frequency scaling, screen-off idle, thermal throttling, and the native iOS temperature screen |

**Performance presets.** Balanced is the default and limits peak clocks to keep heat down.
Performance is the highest preset. It gives animations and scrolling more headroom, but the
phone can run substantially hotter. Select it with the `perf_profile=2` boot argument.
Use `perf_profile=1` to return to Balanced.

### iOS and connectivity

| Area | What works |
|---|---|
| Graphics | Metal on the Mali-G77 and OpenGL ES through Mesa for supported apps |
| Cellular | 4G data, incoming and outgoing VoLTE calls, and carrier SMS in Messages |
| Wi-Fi | 2.4 GHz and 5 GHz Wi-Fi |
| USB | USB networking, serial console, and Finder file access |
| Internal storage | Persistent iOS data on UFS and local files under On My iPhone |
| microSD | Native exFAT read/write in Files. Boot with the card inserted. |
| Lock screen | Passcode setup and unlock |

The full status per part is in [`devices/exynos990/x1slte/device.conf`](devices/exynos990/x1slte/device.conf),
and every change is logged in [SHIPPING.md](docs/SHIPPING.md).

A clean exFAT card with an MBR partition mounts automatically under **Files → Browse → Locations**.
The card keeps its volume label. Automatic mounting and read/write persistence were verified on the
SM-G980F across reboots. Shut down before removing the card. Live removal and reinsertion remain
unsupported. See the [microSD notes](docs/devices/x1slte/sdcard.md).

**Limited or unavailable**

- **Touch ID** is implemented. Fingerprint matching on the S20 sensor is still a work in progress.
- **App Store** FairPlay DRM, including protected app downloads and purchases, is unfinished.
- **iMessage, FaceTime, and related Apple services** are unavailable. Carrier SMS works.
- **Apple Account (Apple ID)** sign-in works with a Hackintosh to supply MID. Standalone sign-in on the S20 remains pending.

## Install

Everything is in the **[install guide](docs/release/install.md)**: before you start, then

- **A. From Android** (ExtremeROM, UN1CA, stock...): TWRP 3.7.1+, then the install ZIP from microSD/USB-C or with
  `tools/install/twrp-install.sh` from a computer; hold Volume Up to confirm.
- **B. Update** an S20 already on Jesvi iOS (keeps your data): `tools/install/twrp-install.sh <zip>` in TWRP.
  Tested on the phone 2026-11-18.
- **C. Back to Android:** Odin TWRP, the restore ZIP, *Format Data*, flash any ROM. Nothing is repartitioned.
- **D. If something goes wrong**, and troubleshooting.

## JESViOS TWRP recovery

The [TWRP feature branch](https://github.com/jesvi-eulah/Jesvi-iOS/tree/feat/twrp-x1s) builds the
SM-G980F recovery on GitHub Actions. A successful run provides a `twrp-x1s` artifact containing
`recovery.img`, an Odin TAR, source revisions, and SHA-256 checksums.
[Recovery builds](https://github.com/jesvi-eulah/Jesvi-iOS/actions?query=branch%3Afeat%2Ftwrp-x1s)

The JESViOS recovery build adds raw backups for **System (Super)** and **Data (Userdata)**.
**Wipe → JESViOS Wipe** opens the installer ZIP picker for a confirmed fresh install or reinstall.
The recovery can mount microSD and USB-OTG. Its kernel lacks APFS and HFS+ support, so the internal
iOS system and data volumes use raw backups. The new recovery menus and restore flow still need
hardware validation. The iOS installer ZIP is built separately. [Installer guide](docs/release/twrp.md)

## Build

On macOS, with your own iOS 16.5 IPSW and the phone's firmware:

```sh
make doctor          # what this Mac is still missing
make check           # source checks and tests (what CI runs)
make kernel          # the kernel        -> out/kernel/mach.x1slte
make release         # TWRP + restore ZIPs -> dist/release/
```

`./build.sh --help` lists every command; settings live in [`release.conf`](release.conf).
Guides: [building](docs/release/building.md), [one-command local build](docs/release/local-build.md),
[CI and releases](docs/release/operations.md).

## Layout

| Path | What |
|---|---|
| `build.sh`, `Makefile`, `release.conf` | the entry points and release settings |
| `devices/` | `manifest.conf`; `exynos990/` = SoC layer (`soc.conf`) and one directory per phone (`x1slte/`: `device.conf`, boot line, partition profile) |
| `xnu/` | the kernel: patch against Apple's `xnu-8796.121.2` plus our own files (`xnu/files/`) |
| `xnuloader/` | the UEFI loader that starts XNU |
| `userland/` | the system image: daemons, injected libraries, boot jobs, defaults |
| `prebuilt/` | binaries the image takes when they are not rebuilt |
| `tools/` | per-subsystem tools (`camera/`, `cellular/`, `recovery/`, `release/`, ...) |
| `x1slte.sh` | the S20 developer loop: payload, pack, flash over USB |
| `docs/` | `release/` install and build, `devices/x1slte/` per-feature notes, `dev/` and `research/` |
| `ci/`, `.github/` | checks and workflows |

## More devices

The Exynos S20+ (`y2s`) and S20 Ultra (`z3s`) have experimental local kernel
builds for port contributors. Boot and installation still need hardware
validation. The Note20 (`c1s`/`c2s`) remains a porting candidate.
See the [local build commands and bring-up requirements](docs/release/porting.md#experimental-s20-and-s20-ultra-kernel-builds).

## Credits and licenses

HakiOS (oosexgang), Apple's open-source XNU (APSL 2.0), Mu-Silicium, Mesa and the other components
in [NOTICE](NOTICE) and [THIRD_PARTY_LICENSES.md](docs/legal/THIRD_PARTY_LICENSES.md). Nothing of Apple's or
Samsung's is in this repository: the build takes it from your own IPSW and phone. Jesvi iOS is not
affiliated with Apple or Samsung. [LEGAL.md](docs/legal/LEGAL.md).
