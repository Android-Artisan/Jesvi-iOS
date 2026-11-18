# Install Jesvi iOS from TWRP

Qualified on the SM-G980F (`G980FXXSNHYB7`) on 2026-11-18: [record](qualification-x1slte.md).

## You need

- The phone with an unlocked bootloader, at least 50 % charged.
- **TWRP 3.7.1 or newer** for the S20 (ExtremeROM's unified Exynos 990 build works). The official 3.7.0 (2022) is too
  old for current bootloaders: its USB drops out.
- A computer with `adb`, and a cable **straight into the computer** (a USB hub made TWRP's adb vanish).

## Steps

1. Flash TWRP with Odin (AP slot), then boot it: Volume Down + Power until black, then at once Volume Up + Power.
2. From the computer, in this repository:

   ```sh
   tools/install/twrp-install.sh Jesvi-iOS_<version>_x1slte.zip
   ```

   It copies the ZIP into TWRP's memory, checks it, and starts the installer there.
3. When the phone asks, **hold Volume Up for 3 seconds**. On a phone that already runs Jesvi iOS your data is
   kept; on Android it asks a second time before it initializes the data partition.
4. `adb reboot`: the Apple logo, then iOS. TWRP is replaced by the iOS runtime.

Why not TWRP's Install button or ADB Sideload: Install stops before the ZIP runs on an iOS phone ("Unable to find
partition for path '/system_root'"), and Sideload streams 2.6 GB over a link that can drop. Coming from Android, TWRP's
Install from microSD/USB-OTG should pass that check (not yet tried).

## Back to Android

Flash `Jesvi-iOS-Restore_<version>_x1slte.zip` the same way, then without rebooting *Wipe > Format Data* and
flash an Android ROM. See [rollback](rollback.md).

---

## Background: design and earlier plan

Status: **a local build produces a verified private image package, but this branch
has no qualified install command yet.** The package manifest deliberately says
`installer_validated: false`. `./build.sh verify|plan` supports offline `verify` and `plan` only.
A successful build/cache check does not establish that this exact candidate boots.

An [experimental TWRP ZIP](twrp.md) is now implemented, including a preflight-only
mode and an explicitly enabled writer. It still needs controlled hardware
qualification; the guide below describes the transaction and the older tar archive.

## Pick the correct path

| Starting point | Intended path | Current limit |
|---|---|---|
| Stock Android on a new supported S20 | Exact-device Download Mode/TWRP preparation, backups, explicit fresh installation | Experimental ZIP implements initialization/transaction; hardware qualification remains |
| Existing iOS development phone | Controlled update preserving existing HFSX userdata and software keybags | The new candidate and update/readback/rescue path need device testing |
| Other Exynos 990 model or storage variant | Add and qualify that board first | The S20 package cannot be reused solely because the SoC matches |

The current measured target is **SM-G980F, Exynos 990, 8 GB / 128 GB**, bootloader
`G980FXXSNHYB7`. The development profile is `x1slte`; recovery may identify its
family as `x1s`. Read [device evidence](device-evidence.md). This is not a general
S20/S20+/Ultra/Note20 installer and does not provide dual boot.

## What you can run now

From the release checkout on a prepared Mac:

```sh
./build.sh local --output dist/s20-001
./build.sh verify --bundle dist/s20-001
```

The retained local validation candidate is at `dist/local-validation/`. The
redundant transport tar was removed during authorized cleanup; its TWRP ZIP is
retained. New local builds generate a TWRP ZIP by default. See [local build](local-build.md) for
one-time inputs, host tools, caching and logs.

Verify a newly built ZIP from its parent directory before any TWRP trial:

```sh
(cd dist && shasum -a 256 -c s20-001.zip.sha256)
./build.sh verify --bundle dist/s20-001
```

Use the checksum received through a trusted channel when distributing packages;
a hash supplied by the same untrusted sender does not authenticate a release.

The archive contains:

| File | Intended use after qualification |
|---|---|
| `boot.img` | Validated Android/Samsung boot head; BOOT written last |
| `recovery.img` | Loader, kernel and small root APFS; replaces TWRP |
| `system.apfs` | iOS system in the measured `super + 0` window |
| `manifest.json` | Exact target, image sizes/hashes and source/input identities |
| `SHA256SUMS` | Full-file checksums |

**Do not select this archive in Odin's AP slot or install it as a TWRP ZIP.** It is
an ordinary transport tar, with no Odin layout or TWRP update-binary. Renaming it
`.tar.md5` or `.zip` does not create an installer. `system.apfs` is not an Android
logical-system image, and generic `fastboot flash system` instructions do not apply.

## Fresh installation from Android: intended transaction

The following is the installation design, **not executable flashing instructions**.
There are deliberately no raw block-write commands until the writer and recovery
path are tested against the actual device.

1. Record exact model, bootloader, board and physical partition geometry. Confirm
   unlock state and the explicitly selected transport/device. Reject mismatches.
2. Prepare exact-device stock firmware and TWRP on the host. Demonstrate Download
   Mode rescue independently of the iOS runtime. Store verified EFS/sec_efs,
   important partition backups and personal-data backups off the phone.
3. Enter the qualified TWRP/root recovery environment. Verify package identity,
   every full-image hash and expected sizes. Confirm adequate power and host space.
4. Show the destructive scope and require explicit fresh-install confirmation:
   Android in `super` is replaced, userdata is initialized for HFSX and TWRP is
   overwritten. Keep the package/backup on the host or suitable external storage;
   its only copy must not be on userdata.
5. Unmount all targets and release device-mapper holders. Verify those conditions
   rather than assuming a root recovery shell is sufficient.
6. Write/read back the system range, retaining the separate super log tail.
   Explicitly initialize and validate HFSX userdata **only for fresh install**;
   Android/TWRP's ordinary Format Data is not this project's HFSX initializer.
7. Write/read back the runtime recovery image. Commit BOOT **last**, only after
   system, data initialization and runtime verification succeed. This is an
   in-place layout, not an atomic/A-B transaction.
8. Save the installation record and logs off-device. After successful checks and
   deliberate reboot authorization, boot the candidate and run acceptance tests:
   cold boot, storage/persistence, passcode/relocking, charging/thermals and the
   exact declared feature set. Record a successful rescue test as well.

TeamWin's [official S20 Exynos page](https://twrp.me/samsung/samsunggalaxys20.html)
provides its own Odin/TWRP installation guidance and explains dynamic partitions.
It does **not** qualify this port's physical-super/APFS installation scheme.

No installer in this design changes EFS/sec_efs, modem, bootloader, PIT/GPT,
param/up_param, DTBO or VBMETA. Any actual prerequisite must be separately tested
and documented for the exact device/bootloader. Never relock with custom images.

## Updating the existing iOS phone

Do not repeat the fresh-install data initialization. Preserve userdata, keychains
and saved software passcode bags; updates must never format on a failed read/mount.
This branch's init mounts existing HFSX and halts if persistent data is unavailable.

Before a controlled update, preserve a known-good package, stock/TWRP rescue and
private backups. Compare the installed image identities with the candidate to
choose writes; a build-host cache hit does not identify the bytes on the phone.
Check root/system/data-schema compatibility as a set. A system change requires its
write/readback; a loader/kernel/root change requires the appropriate recovery and
BOOT checks. No OTA, dirty-update or downgrade compatibility is promised yet.

Retain `softpasscode=1`, `autounlock=0`, native keybagd, the guarded pre-launchd
restore and repeat-lock bridge. Do not clear keybags to recover from a failed
update. See [passcode traps and recovery](../devices/x1slte/passcode.md#traps-and-recovery).

The legacy `x1slte.sh` and workspace phone-update tools were used for development.
They can write/reboot, contain historical assumptions and are not a substitute
for the release installer. The current guide does not authorize phone operations;
coordinate the shared phone and explicit reboot permission before any trial.

## What remains before an install command can be published

- Qualify the implemented device probe, readback writer and physical confirmations.
- Qualify offline HFSX initialization; prepare and verify backups separately.
- Verify complete APFS integrity, actual AVB/BOOT behavior and release authenticity.
- Test first install, preserving updates, wrong-target rejection, interrupted
  writes, corrupt data, power loss and independent Download Mode rescue.
- Record hardware acceptance for the exact candidate, then enable the installer
  through a reviewed code change; flipping the profile boolean is insufficient.

See [flashing design](flashing.md), [validation](validation.md) and the
[release checklist](checklist.md). Until these gates close, the package is for
controlled developer qualification rather than an end-user installation release.

[Rollback / return to stock Android](rollback.md) covers the separate recovery
paths. Restoring BOOT alone is not an Android restoration.
