# Apple Files on the HFS+ data volume

Apple Files now lists local storage, Downloads and available capacity on the
S20. The user confirmed the UI; real `FPCopyOperation` and `FPMoveOperation`
checks passed, including contents and source/destination existence. The check
creates a UUID-named test directory and removes only that directory on success.

## Default image build

`userland/build.sh` now builds and includes:

- `libredwood-files.dylib`, injected into only `fileproviderd` and
  `LocalStorageFileProvider` through the existing kernel mechanism;
- a hidden `org.hakios.LocalFilesHost` app containing the user's original local
  provider extension. This registers its data and app-group containers.

The provider is **moved**, not duplicated, while preparing the system image.
Its manifest entries move with it, preserving owners, links and empty locale
directories. The original extraction and its hardlinked files are not edited.
The host has no UI or background work; no polling service or kernel change is
required. Caller-supplied INJECT is still an explicit override of defaults.

`sysimage/select-system.py` now retains the 8,981,616-byte provider executable,
which the generic 4 MiB framework resource limit previously discarded.
A previously pruned extraction needs to be refreshed from the user's own IPSW
before rebuilding the system image. Adding the library to an old boot image
alone does not restore a missing extension on a fresh data volume.

For an existing phone, the tested runtime equivalent is a hidden host under
`/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app`, registered with
`uicache -p` using the existing containers. Its Apple executable/resources come
only from the user's extracted IPSW and are never committed here. No user
documents or FileProvider databases were reset.

## Build and checks

```sh
tools/storage/build.sh
```

Outputs under `out/files/`: the compatibility library, LocalFilesHost,
`storage-probe` and `check-operations`. SDK defaults to the local Theos
16.5 SDK; SDK/CC/LDID can be overridden. The library/host are also provided in
`prebuilt/userland` for the existing prebuilt image workflow.

`storage-probe` is read-only by default. It compares statfs, Foundation
filesystem attributes and the four NSURL capacity APIs. Its explicit
`register` command calls LaunchServices for the original framework extension;
this alone does not populate its container metadata and is not a complete fix.

`check-operations PATH_TO_DOWNLOADS` exercises FileProvider's real copy/move
operations on a newly created UUID folder. Sign this diagnostic with
entitlements extracted from the user's own fileproviderctl executable before
running it as mobile; it does not belong in the boot image. Failures keep the
UUID directory for diagnosis. No fixed device UUID is compiled into the tool.

Validation completed:

- library/host/diagnostics compile, shell syntax and diff whitespace pass;
- actual IPSW listing selects the omitted executable;
- staging keeps one provider, relocates locale metadata and preserves source
  hardlinks;
- mobile `fileproviderctl enumerate com.apple.FileProvider.LocalStorage`
  completes, lists Downloads and reports available capacity;
- both transfer operations pass and their disposable directory is removed;
- the original main boot image rebuilds byte-for-byte from its staging tree;
  the updated boot image changes only inject.conf and the Files library,
  growing by 8 KiB.

The latest camera kernel and boot arguments were preserved for the Files
boot-volume update. Recovery and BOOT writes were read back successfully;
post-reboot persistence verification is pending the camera task's phone slot.

## Root causes and source

Main upstream checked at `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` still drops
the provider executable. The Sapphire port has the matching HFS+ compatibility:

https://github.com/Angelpro09xd/ios-sapphire/commit/04f94d5c6577d65bf9e112ed801c8b1fce9fff47

`userland/src/inject_files.c` adapts that implementation. Its origin and pending
file-specific license coverage are recorded in NOTICE/THIRD_PARTY_LICENSES.md.
It permits a writable local HFS+ FileProvider library, handles the port's absent
sandbox-container/URL-token services, and supplies guarded fallbacks for six
APFS-only filesystem helpers. The function entry signatures matched on this
16.5 phone; other signatures are logged and left untouched.

There was also a stale standalone registration from an earlier repair attempt
at `/var/mobile/Library/LocalStorageRepair`. It had no data/app-group metadata.
After targeted unregistration of that stale entry and an incomplete system
entry, registering the hosted extension supplied the existing containers and
allowed discovery. Plain registerPlugin: alone left container metadata empty.
The provider's bundle identifier stays com.apple.FileProvider.LocalStorage.

Only /private/var is writable data: statfs and Foundation correctly report
about 108 GiB total and 105 GiB free. The full read-only APFS system image is
expected to report zero free bytes; it is not the data volume.
