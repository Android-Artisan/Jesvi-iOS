# Clean install: why features went missing, and the rule that stops it (2026-11-20)

## What happened

The phone has two parts:

- **The image** (kernel, root file system, `/redwood/...`): built from this repo, flashed, the same on every install.
- **The data volume** (`/var`: `/var/root`, `/var/mobile`, `/var/jb`, preferences): survives updates, but a clean
  install starts it empty.

Several features worked only because files had been copied onto the data volume by hand during development. They
were never in the image or made at first boot, so a clean install (2026-11-19/20) lost them all at once: SIM and
data (the modem files), Location Services (so Compass and Maps), Files' "On My iPhone", keyboard haptics, Live Photo
default, and hooks in `/var/jb`. The code was all on `main`; what each feature needed on the data volume was not.

## The rule (every session)

**Anything a feature needs at run time must come from the image, or be made by the phone at first boot.**
Never leave a feature depending on a file you copied to `/var` by hand. When you `scp` something to the phone to
make a feature work, the same change goes into one of these before the work counts as done:

| Kind | Where it goes |
|---|---|
| Our own program or library | the image (`userland/build.sh` → `root/bin`, `root/lib`) |
| A file that must live on the data volume (`/var/jb`, `/var/root`) | `root/etc/seed.list` (init copies it when missing) |
| A preference default (Location Services, haptics, camera) | the first-boot defaults (see "Defaults" below) |
| Device firmware we may not ship (modem, NV, calibration) | made on the phone from its own partitions, read-only (example: `tools/cellular/cell-provision.c`) |

Test it the honest way: move the files aside (or use a clean data volume), reboot, and see the feature come back by
itself. Write in `docs/SHIPPING.md` that it survived a clean install.

## Status of what went missing on 2026-11-20

| Feature | Needed on the data volume | Status |
|---|---|---|
| SIM, data, VoLTE | `/var/root/cell-*` modem files | **made automatically** by `cell-provision` from RADIO + EFS ([cellular/provisioning.md](devices/x1slte/cellular/provisioning.md)) |
| Location Services (Compass, Maps, GPS) | `LocationServicesEnabledIn7.0` in locationd's preferences | first-boot default: to do |
| Keyboard haptics on | Sounds & Haptics preference | first-boot default: to do |
| Live Photo off | Camera preference | first-boot default: to do |
| Files: On My iPhone | the local file provider's setup | to do |
| Touch ID enrolment | `/var/root/fp-*` (made by enrolling) | by design: enrol after a clean install |
| Apple account layer | `/var/jb/usr/lib/libredwood-account-*`, boot job | needs the image (sessions/B-apple-account-handoff.md) |

## Defaults

Preferences that iOS keeps on the data volume are set once per data volume by a first-boot step (planned:
`redwood-defaults`, a boot job with a marker file so the user's later choices are never overwritten).
