# S20 cellular tools

Branch `x1slte-cellular`; status and resumable checklist:
[docs/cellular-s20.md](../../docs/cellular-s20.md).

These tools run offline. They neither contact nor boot the phone.

From this worktree:

```sh
python3 -B -m unittest discover -s tools/cellular/tests -v
python3 -B tools/cellular/shannon.py /path/to/own/radio.img --verify-crc
python3 -B tools/cellular/memory-audit.py --boot-log /path/to/saved/xnulog.txt
```

`shannon.py` prints metadata only. It never extracts firmware or reads NV from
EFS; the NV TOC entry is an external object, even though its offset is zero.
The SIPC5 parser is importable for future IPC/capture validation. It does not
implement SIM queries, packet sessions, Samsung IPC commands or voice calls.

`memory-audit.py` uses the branch's loader/audio sources plus saved-log XNU,
display and Wi-Fi allocations. Use `--root OTHER_WORKTREE` to inspect another
source tree. It reports source candidates even if audio is disabled at runtime.
Exit codes: 0 = no identified conflict/unknown; 1 = conflict; 2 = incomplete
evidence or input error. This is a partial audit, not proof of safe modem DMA.

Validated 2026-11-11:

- Local S20 RADIO: BOOT/MAIN/APM/VSS/INFO CRCs all pass. NV bytes never read.
- Existing saved boot: IPC overlaps loader staging by 8 MiB; packet region
  overlaps display pool by 64 KiB. Audit exits 1 as intended.
- Host tests: **17 passed** (`python3 -B -m unittest discover -s tools/cellular/tests -v`).
  Synthetic fixtures contain no vendor bytes.

No runtime changes have been installed. Keep generated JSON and private logs
outside the source tree.
