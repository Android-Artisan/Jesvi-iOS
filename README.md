# Jesvi iOS — Exynos 990

An experimental iOS 16.5 / XNU port for Samsung Exynos 990 devices, developed on
**Galaxy S20 LTE, SM-G980F (`x1slte`), 128 GB**. Working project name: **Jesvi iOS**;
proposed repository: `ios-exynos990`.

The S20 runs the port; a general-purpose installer is **not yet qualified**.
The current storage layout replaces Android in `super`, uses all `userdata` for
HFS+, and uses `recovery` for the runtime. **Fresh installation requires explicit
userdata initialization and replaces TWRP. Normal boot never formats userdata.** Do not follow upstream Qualcomm `fastboot boot`
instructions on Samsung hardware.

## Start here

| Task | Guide |
|---|---|
| Understand release readiness and design decisions | [Release research](docs/release/research.md) |
| Build/check from a fresh clone | [Build guide](docs/release/building.md) |
| Add another Exynos 990 device | [Porting guide](docs/release/porting.md) |
| Build an experimental TWRP ZIP | [TWRP package](docs/release/twrp.md) |
| Installation steps, current limits and package verification | [Installation guide](docs/release/install.md) |
| Choose TWRP / Odin / host script and understand recovery | [Flashing design](docs/release/flashing.md) |
| Configure GitHub, CI and image retention | [Repository and CI](docs/release/operations.md) |
| Roll back or return to stock Android | [Recovery/rollback guide](docs/release/rollback.md) |
| Cut a release | [Release checklist](docs/release/checklist.md) |
| Find subsystem notes | [Documentation index](docs/README.md) |

## Device roadmap

| Family | Profile | State |
|---|---|---|
| Galaxy S20 | `x1slte` | Active SM-G980F development; only this exact measured variant |
| Galaxy S20+ | `y2s` | Maintainer needed; no supported model allowlist yet |
| Galaxy S20 Ultra | `z3s` | Maintainer needed |
| Galaxy Note20 | `c1s` | Maintainer needed |
| Galaxy Note20 Ultra | `c2s` | Maintainer needed |
| Galaxy S20 FE, Exynos variant | `r8s` | Later candidate, outside the first five |

These are family roadmap entries, not six working ports. Snapdragon variants,
other RAM/storage sizes and even S20 5G need separate evidence. Shared Exynos 990
code helps, but does not establish panel, camera, memory or partition compatibility.

## Developer commands

Host checks need Python 3.9+, Git and a C compiler. The kernel adapter currently uses
macOS and native compiler tools; see the build guide for dependencies.

```sh
./build.sh devices
./build.sh validate
bash ci/check.sh
./build.sh doctor --device x1slte
./build.sh kernel --device x1slte --jobs 4
```

The kernel output is `out/kernel/mach.x1slte`, with provenance and SHA-256 checksums.
**It is not a flashable boot image.** No command above connects to a phone.

Private image assembly additionally requires the builder's own IPSW extraction,
firmware, exact UEFI/DT table, toolchain and HFS kit. `./build.sh doctor --images`
lists missing inputs. Source-only CI does not download or publish complete iOS images.
`./run.sh` verifies private candidates and prints offline plans; it does not flash.

## What is known to work

The development log records boot to the iOS UI, Mali GPU rendering, touch, Wi-Fi,
charging, persistent storage, several cameras/sensors, USB integration, LTE data and
SMS on the test phone. These are per-feature observations, not a release certification.
Calls, carrier coverage and other unfinished behavior must be taken from the exact
release commit's [SHIPPING.md](SHIPPING.md) and tested on that release candidate.
No claim of complete iPhone compatibility or Apple services support is made.

## Source and generated inputs

- `xnu/`: exported patches and drivers; `xnuloader/`: loader.
- `userland/`, `tools/`: source, image tools and development utilities.
- `devices/exynos990/`: shared platform and per-board evidence profiles.
- `ci/`, `.github/`, `tools/release/`: build, cache, checks and release tooling.
- `inputs/`, `.cache/`, `out/`, `dist/`, `fw-*`, `sysimage` extraction: local inputs/output.

The inherited tree still contains prebuilts, donor UEFI binaries and research files
requiring publication review. `python3 tools/release/audit.py --output out/audit.json`
inventories them; do not assume that `.gitignore` removes files already in Git history.

This work builds on HakiOS/Redwood, the y2s overlay, Mu-Silicium, Apple's open-source
XNU and other credited components. Preserve upstream attribution and per-file
licenses: [NOTICE](NOTICE), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md),
[LICENSE](LICENSE), [LICENSE.APSL](LICENSE.APSL). The
[historical upstream README](docs/upstream/README.md) describes Qualcomm/QEMU work.
Jesvi iOS is independent of Apple and Samsung.

Local full builds produce a TWRP ZIP by default (preflight-only until explicitly
enabled with `--experimental`). Local private package automation: [one-command build](docs/release/local-build.md).
