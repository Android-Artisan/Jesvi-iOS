# Cellular Settings UI

Status: **built and signed, installation pending the phone lock** (2026-11-11).
This can be installed independently of the untested loader/UEFI memory changes.

What it adds to Settings:

- A green Cellular row after Bluetooth, replacing the native row if present.
- A native Cellular page with a Cellular Data switch that saves the user's
  preference across Settings launches.
- Network: **No Service**; Calls: **Unavailable**. The footer explains that the
  setting applies when cellular service is ready.

The switch stores an intended setting. It does not claim to start a radio or
route traffic before the Samsung driver exists. No fake SIM, signal bars,
carrier name, CommCenter replies or call-success state is installed.

## Build and install

From `work/hackios-cellular`:

```sh
bash tools/cellular/build-ui.sh
python3 -B tools/cellular/install-ui.py install --session YOUR_SESSION
```

The installer requires that YOUR_SESSION already owns the research workspace's
normal phone lock. It refuses a free lock or another session's lock and does
not acquire, steal or extend one itself. At this checkpoint B owns the phone;
the attempted install exited 3 before SSH/uploads. Read-only SSH confirmed
that the tweak directory and Preferences tweak loader exist on the phone.

The current phone holder can install the prepared artifact using their own
session name without merging branches, rebuilding the system image or rebooting.
Close and reopen Settings after installing. Only the Preferences process loads
this tweak. No automatic process restart is performed.

The installer puts `RedwoodCellular.dylib` and its executable filter into
`/var/jb/usr/lib/TweakInject`. Files are first uploaded under inert names, then
renamed; an already mapped dylib is never overwritten in place. Previous copies
are saved under a timestamped `RedwoodCellular-backup-*` directory. Source and
artifacts contain no Apple binaries or vendor firmware; generated artifacts
remain ignored in `out/cellular`.

Remove, under the same lock rule:

```sh
python3 -B tools/cellular/install-ui.py remove --session YOUR_SESSION
```

Removal moves only this tweak and its filter into a backup directory. Reopen
Settings to unload it. The saved data preference can remain for a later install.

## Implementation and backend contract

- Source: `userland/src/cellular_settings.m`, an independent Objective-C dylib.
  It uses the port's existing Preferences tweak loader; no edits to the shared
  `libredwood`, Settings executable, MobileGestalt or system image.
- Root hook: only `PSUIPrefsRootController -specifiers`. An inherited method
  receives a subclass override instead of patching `PSListController` globally.
  The hook waits for dyld images, runs on the main queue, and refreshes an
  already-created root controller. iOS 16.5's local cache confirms these class
  names and `PSListController`'s `_specifiers` NSArray backing ivar; it is looked
  up by name without hardcoded offsets.
- Preference: mobile user's `com.redwood.cellular`, boolean
  `CellularDataEnabled` (default off). Change notification:
  `com.redwood.cellular.data-changed`. Future Samsung session service must read
  this domain in the mobile-user scope, enforce the actual session/route state,
  and publish real registration/call status before these static status values
  can be replaced. A preference notification alone is not a packet transport.
- Filter restricts loading to executable `Preferences`.

## Validation checklist

- [x] Compile arm64 for iOS 16.5 with `-Wall -Wextra -Werror`.
- [x] Ad hoc sign and pass `codesign --verify --strict`.
- [x] Inspect linked frameworks and parse the executable filter plist.
- [x] Confirm local iOS 16.5 class/backing-ivar names without committing Apple data.
- [x] Confirm installer refuses another session's phone lock without device writes.
- [ ] Install as phone holder; close/reopen Settings; confirm one Cellular row.
- [ ] Open the page; verify the switch/status/footer, back navigation and no crash.
- [ ] Toggle, close/reopen Settings and verify preference persistence.
- [ ] Remove/reopen and verify the normal Settings root returns.
- [ ] Reinstall and leave the visible UI available to the user.

No screenshot or runtime UI success has been claimed yet.
