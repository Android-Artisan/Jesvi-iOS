# S20 cellular tools

Branch `x1slte-cellular`; status and resumable checklist:
[docs/cellular-s20.md](../../docs/cellular-s20.md).

Firmware/memory checks run offline. The separate UI installer contacts the
phone only when its caller already owns the normal phone lock; it never flashes
or reboots. See [Cellular UI](../../docs/cellular-ui.md) for installation status.

From this worktree:

```sh
bash tools/cellular/check.sh
python3 -B tools/cellular/shannon.py /path/to/own/radio.img --verify-crc
python3 -B tools/cellular/memory-audit.py --boot-log /path/to/saved/xnulog.txt
```

`shannon.py` prints metadata only. It never extracts firmware or reads NV from
EFS; the NV TOC entry is an external object, even though its offset is zero.
The SIPC5 parser is importable for future IPC/capture validation. It does not
implement SIM queries, packet sessions, Samsung IPC commands or voice calls.

`memory-audit.py` uses the branch's loader/audio sources plus saved-log XNU,
display and Wi-Fi allocations. Use `--root OTHER_WORKTREE` to inspect another
source tree. Dynamic staging also requires its logged address, and deployed
firmware reservations require `cellular memory: reserved 3/3`. It reports source
candidates even if audio is disabled at runtime, using only the latest boot.
Exit codes: 0 = no identified conflict/unknown; 1 = conflict; 2 = incomplete
evidence or input error. This is a partial audit, not proof of safe modem DMA.

Validated 2026-11-11:

- Local S20 RADIO: BOOT/MAIN/APM/VSS/INFO CRCs all pass. NV bytes never read.
- Existing saved boot: IPC overlaps loader staging by 8 MiB; packet region
  overlaps display pool by 64 KiB. Audit exits 1 as intended.
- Host tests: **19 Python tests passed** plus **163,840** C allocator boundary
  comparisons under ASan/UBSan (`bash tools/cellular/check.sh`). Synthetic
  fixtures contain no vendor bytes.
- ARM64 UEFI loader cross-compile/link passes; full UEFI image and phone boot
  pending. Matching UEFI branch/worktree: `work/g980f-uefi-cellular`, `6e75773`.
- Cellular UI compiles with warnings as errors, is ad hoc signed, and passes
  strict signature verification. Installer currently refuses B's phone lock.

No loader/modem runtime changes have been installed. UI deployment is recorded
in its own checklist. Keep generated JSON/private logs outside the source tree.
