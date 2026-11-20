# Jesvi iOS

iOS 16.5 running natively on the **Samsung Galaxy S20 (SM-G980F, Exynos 990)**. The XNU kernel,
loader, drivers and user-space glue are built from this tree; the iOS system files come from an IPSW
you download yourself. Built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) and laid out
the same way: a common tree, plus one directory per device.

> Experimental. It replaces Android on the phone. Read [Install](docs/release/install.md) and
> [Rollback](docs/release/rollback.md) before you start.

## What works on the S20

| | |
|---|---|
| Display | FHD+ up to 120 Hz (real ProMotion), auto-brightness, Always-On Display |
| Graphics | Metal on the Mali-G77; OpenGL ES games through Mesa |
| Cellular | 4G data, VoLTE calls both ways, SMS in Messages |
| Cameras | front + three rear lenses by zoom, photo and video with sound |
| Connectivity | Wi-Fi 2.4/5 GHz, Bluetooth audio, USB networking, Finder file access |
| microSD | Native exFAT read/write in Files. Boot with the card inserted. |
| System | touch, buttons, vibration, charging, sensors and compass, passcode, persistent data, thermal control |
| Not yet | fingerprint, full GPS, App Store purchases |

The full status per part is in [`devices/exynos990/x1slte/device.conf`](devices/exynos990/x1slte/device.conf),
and every change is logged in [SHIPPING.md](docs/SHIPPING.md).

A clean exFAT card with an MBR partition mounts automatically under **Files → Browse → Locations**.
The card keeps its volume label. Automatic mounting and read/write persistence were verified on the
SM-G980F across reboots. Shut down before removing the card. Live removal and reinsertion remain
unsupported. See the [microSD notes](docs/devices/x1slte/sdcard.md).

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

The Exynos 990 layer is shared, so the S20+ (`y2s`), S20 Ultra (`z3s`) and Note20 (`c1s`/`c2s`) can be
added as directories next to `x1slte`. See [porting](docs/release/porting.md).

## Credits and licenses

HakiOS (oosexgang), Apple's open-source XNU (APSL 2.0), Mu-Silicium, Mesa and the other components
in [NOTICE](NOTICE) and [THIRD_PARTY_LICENSES.md](docs/legal/THIRD_PARTY_LICENSES.md). Nothing of Apple's or
Samsung's is in this repository: the build takes it from your own IPSW and phone. Jesvi iOS is not
affiliated with Apple or Samsung. [LEGAL.md](docs/legal/LEGAL.md).
