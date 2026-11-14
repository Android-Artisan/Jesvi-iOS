# S20 authentication and build handoff

Before changing the S20 boot profile, keybag driver, SpringBoard hooks or userland
assembly, read [docs/x1slte-passcode.md](docs/x1slte-passcode.md), especially
**Traps and recovery**, and the latest [SHIPPING.md](SHIPPING.md) entry.

- Preserve the complete software-passcode chain: `softpasscode=1`, `autounlock=0`,
  native keybagd job, fail-closed pre-launchd restore and display-off immediate-lock
  bridge. Settings-enabled or one successful boot unlock is insufficient; verify
  repeat locking too. Do not use an unconditional authentication-success stub.
- Userland source and tracked prebuilts both contain the bridge. The installed
  standalone tweak also persists in `/var/jb`. A clean image must work without
  depending on that existing tweak. The duplicate-registration guard is required.
- S20 defaults follow `x1slte.cmdline`; common device builds explicitly enable
  `SOFTWARE_PASSCODE=1`. Other explicit DEVICE profiles retain their defaults.
  Do not silently disable the backend on a device with a saved protected bag.
- Never print/ask for the user's code or enable verbose native keybagd logging.
  Keep firmware, credential/keychain backups and device images out of git.
- Kernel edits must exist in tracked `xnu/files/`, and in the corresponding
  `xnu-src/xnu-8796.121.2/` build files before compiling. Avoid exporting unrelated
  edits from another worktree. `out/` is disposable and cannot be the only source.
- Run `python3 tools/passcode/test_build_integration.py` after build/profile changes;
  the other relevant validation commands are in the passcode document.
- Ask the user before rebooting. Phone access is shared: consult the workspace's
  `NEXT.md`, `TASKS.md` and phone-lock status before a device-changing operation.
- Local `main` and `master` track the accepted work. Other worktrees/branches must
  explicitly integrate these commits; do not overwrite their unrelated work.
