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
- Host tests: **30 Python tests passed** plus **163,840** C allocator boundary
  comparisons under ASan/UBSan (`bash tools/cellular/check.sh`). Synthetic
  fixtures contain no vendor bytes.
- Full ARM64 loader boots on the original UEFI. Opt-in `CELLULAR_RESERVE_CP=1`
  reserves CP memory through existing boot services and applies Normal NC through
  DXE. Hardware ownership/live translation checks pass 3/3 before and after iOS;
  saved-log audit exits 0. The separately rebuilt native FD failed and must not
  be deployed as working; it is unnecessary for this tested reservation path.
- Cellular UI compiles with warnings as errors, is ad hoc signed, and passes
  strict signature verification. Installed after the normal expired-lock takeover;
  corrected page is visible and user-confirmed. No image flash or reboot.

Bounded loader tests were installed and removed afterward; the phone currently
had its original BOOT/recovery copies restored after each test. Other agents have
since updated the phone; refresh the baseline before the next deployment.
Internal SPI loopback and reset/power LOW-only GPIO writes are proven. No actual
CP power-on, BOOT firmware transmission, modem DMA or NV access yet.
UI remains installed. Keep generated JSON/private logs outside the source tree.

Additional tools:

- `prepare-m0.py`: offline verified image packaging; `--keep-installed-fd`
  preserves working firmware. Preflight requires `--with-installed-fallback`.
- `preflight-report.py`: bounded RAM report reader; phone lock required.
- `s5123-gpio.py --read`: six read-only CMGP GPIO banks; phone lock required.
- `s5123-clocks.py --read`: 13 read-only clock/USI/GPIO/PHY-isolation words; lock
  required. Default is plan only. No SPI/PCIe controller access.
- Memory audit requires both `reserved 3/3` and `normal-nc 3/3` from the latest
  boot. Boot-services memory descriptors alone did not prove active cache policy.

### Guarded CP BOOT probe (compiled and host tested, not deployed)

`s5123-boot.h` sequences CRC validation, ownership checks, power/reset, bounded
SPI and shutdown. The UEFI adapter requires all memory/MMIO/loopback/GPIO
preflight flags plus `CELLULAR_CP_BOOT_PREFLIGHT=1`. Normal builds omit it.
Packaging requires explicit `--cp-boot-radio PATH` with original firmware and
installed-loader fallback; only CRC-checked BOOT is embedded, never NV/EFS.

Shutdown must verify reset/power LOW, settle delay, controller/pad/clock restore
and PHY isolation before releasing CP RAM. Otherwise the loader retains RAM
and exposes cleanup-only `oem cp-stop`. It never retries power-on/transmission.
Critical reset/power pins stay owned output-low after an attempted power-up.
A subsequent probe must account for that ownership rather than blindly reuse it.

Validation: 23 ownership/cache cases, 163840 allocator comparisons, eight PIO
streams/six PIO failures, and 22 injected boot-lifecycle failures under sanitizers.
Even a fully transmitted BOOT is not proof of CP acceptance, SIM or LTE.
