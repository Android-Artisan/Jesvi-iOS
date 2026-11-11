# Galaxy S20 cellular: implementation log and checklist

Status 2026-11-11: **LTE and calls do not work on our S20 yet.** This branch
contains the upstream comparison, firmware/framing tools, memory audit, compiled
loader changes, matching UEFI source, and a built Settings UI. It does not enable
a modem or publish invented registration state. UI deployment status is tracked
separately in [cellular-ui.md](cellular-ui.md).

Latest UI checkpoint: corrected `PSUIPrefsListController` hook is installed;
the user confirmed the Cellular page on the phone. No modem/loader/UEFI runtime
change was deployed with it.

## Isolation and ownership

- Branch: `x1slte-cellular`, based on `x1slte` commit
  `378c9cdd4c25d94daeee181ad5959bf36106d6a0`.
- Worktree: `work/hackios-cellular` under the research workspace. The existing
  `work/hackios-xnu-kernel` worktree stays on `x1slte` with other sessions' work.
- Owner/session: `codex-cellular`. Shared coordination:
  `sessions/codex-cellular.md`, `tools/team.sh`, `tools/phone-lock.sh` in the research root.
- B holds the phone for camera work. No boot, modem reset, flash, network change
  or shared build performed for this branch. Keep kernel sources/build output
  local to this worktree when kernel implementation starts; do not symlink its
  `xnu-src` to the main worktree. No push requested or performed.
- Evidence from another session's untracked `tools/cellular/README.md` was read
  and checked against Samsung source. That directory was left in place.

## What the requested Pixel branches actually contain

Fetched and inspected full history from
[ios-redfin](https://github.com/DenialOfGravity/ios-redfin).
There is no remote `master`; use `main` for the requested comparison.
Pin revisions, since these branches can change:

| Reference | Commit | Meaning |
| --- | --- | --- |
| `main` | `0c4433cf0060e5f0cd8947e558841c113bbad4db` | Old default branch, README rename on Oct 29 |
| `ws-n-celldata` | `27efaa0b29ea0e12a4959630fdec03dcde98d01f` | Latest requested commit, Nov 9; Settings data toggle |
| `ws-n-int` | `163a3ad419d80afc4141fc8230bfd01e619b6a72` | Cellular status/SIM/mode work before UI branch |
| `ws-n-ipa-gsi1` | `feb2e67b880150617b939e154169bda8e84a4e34` | Separate packet-data branch and later test notes |
| LTE known-good binary source | `19073fc` (on `ws-n-ipa-gsi1`) | Author records user-confirmed browsing Nov 9 |

`main` and `ws-n-celldata` have merge base `745c4341af86089efb0df851fa8a27fb8face624`;
575 commits are reachable from the latter but not from `main`. Those include
display, camera, audio and other work; a blind merge is inappropriate.

The recent relevant commits are:

| Commit | Actual change | S20 relevance |
| --- | --- | --- |
| `27efaa0` | Replace Settings `PSCellularDataSettingsDetail +isEnabled/+setEnabled:` with `/var/tmp/sweetcell.celldata` | UI/control contract only; requires a real data backend |
| `d26ed01` | Retain CoreTelephony reply arguments to prevent a Settings crash | Necessary if reusing that proxy |
| `763cfff` | In-process stand-in for CommCenter through `CoreTelephonyClientMux` | Answers subscription/context methods; other replies are empty, not voice support |
| `72b7e88`, `d115781`, `d6eccfa` | Feed telephony state from `cell.status`; fix orphaned status-bar views | Reusable approach once Samsung reports actual state |
| `c53aa0d` | SIM PIN UI bridged to UIM Verify PIN1 | Qualcomm request cannot be sent to Samsung; never test arbitrary PINs |
| `163a3ad`, `c7f945c` | Opt-in UIM slot/mode changes, bounded status requests | Qualcomm-specific protocol; useful retry/bring-up discipline |
| `1b7b579` | UIM/NAS/DMS client on wland's QRTR link | Not a Samsung modem transport |

Sources:
[toggle change](https://github.com/DenialOfGravity/ios-redfin/commit/27efaa0b29ea0e12a4959630fdec03dcde98d01f),
[CoreTelephony proxy](https://github.com/DenialOfGravity/ios-redfin/blob/27efaa0b29ea0e12a4959630fdec03dcde98d01f/phone/fps/SweetCT.m),
[status client](https://github.com/DenialOfGravity/ios-redfin/blob/27efaa0b29ea0e12a4959630fdec03dcde98d01f/phone/wifi/cell.c).

**The requested branch alone does not contain the working packet driver.** Its
SweetCT comment names `ipagsi.c`, but that file is absent from its tree. The
companion `ws-n-ipa-gsi1` branch supplies `ipa`, `ipaqmi`, `ipagsi`, `ipaprobe`
and extended WDS/WMS handling in `cell.c`. Cherry-picking only the toggle would
create a switch without working LTE.

The packet branch's final [NOTES](https://github.com/DenialOfGravity/ios-redfin/blob/feb2e67b880150617b939e154169bda8e84a4e34/NOTES.md)
records the evidence more accurately than its older `README.ipagsi.md`:

- GSI transfer/completion types and early IPA table/header initialization were
  needed before packets flowed. Control-plane registration was insufficient.
- QMAPv5 checksum format and metadata mux byte order were critical: `p2le=1`
  selected the wrong route and exception header. Removing it and enabling
  `qmapv5=1` produced DNS, ICMP and TCP round trips and Safari with Wi-Fi off
  (`834b392`, Nov 9).
- `cellroute=1` publishes the utun through `netsvc`. `a16ecd3` follows the UI
  switch, withdrawing the route and dropping outgoing packets when off. This
  leaves the packet session itself established; it is not radio airplane mode.
- Idle pipe stalls lasted 80–100 seconds. Recorded working configuration uses
  `wdaps=1 p2keep=3 p2keepidle=120`, retaining a 3-second gateway keep-alive for
  120 seconds after recent iOS traffic. An 8-second interval stalled again.
- Recorded connected results: roughly 150 ms RTT and 1.6–2.5 Mbit/s for a
  1 MB download. These are the author's Pixel results, not S20 measurements.
- WMS SMS sending worked after adding the SMSC length byte. Incoming SMS and
  Messages integration were still open/deferred. **No implemented/verified
  voice-call or IMS path was found in these audited branches.** WDS "call"
  handles in this code are packet-data sessions, not voice calls.

## Our hardware and missing stack

| Layer | Pixel | S20 SM-G980F |
| --- | --- | --- |
| Modem | Qualcomm MPSS | External Samsung Shannon S5123 |
| Boot | Qualcomm firmware/PAS | Power/reset GPIOs, BOOT over SPI16, later image/link boot |
| Transport | GLINK/QRTR | PCIe channel 1, CPIF shared queues and doorbells |
| Control | QMI UIM/NAS/DMS/WDS/WMS | Samsung IPC on SIPC5 channels; commands still to implement |
| Packets | IPA/GSI/QMAP | CPIF raw channels + pktproc v2; mappings/flow control to implement |
| iOS | utun + SystemConfiguration, UI hooks | Can reuse the architecture after Samsung packets work |
| Voice | No verified implementation in audited branches | Samsung call/IMS control + ABOX/VSS + iOS call integration |

Our inherited `hakios.mpss.c`, `hakios.ipa.c`, `hakios.cell.c` and
`docs/cellular.md` target Qualcomm. Existing Wi-Fi PCIe code targets channel 0
with different host glue. Changing its addresses is insufficient for channel 1.

Samsung source pinned at `ExtremeXT/android_kernel_samsung_exynos990`
`69515fbb7a4395898c05a8624f76a12afbac11c5`:

- [S20 revision-21 overlay](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/arch/arm64/boot/dts/samsung/exynos990-x1slte_eur_open_21.dts):
  S5123, protocol 0 (SIPC), IPC version `0x32`, link attributes `0xfc9`,
  22 iodevs, PCIe ch1, doorbell `0x14d60000`, pktproc v2.
- [SoC tree](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/arch/arm64/boot/dts/exynos/exynos9830.dts):
  SPI16 at `0x10960000`, FIFO 64 bytes; board max clock 2,150,000 Hz,
  8-bit boot transfer. PCIe ELBI `0x13d20000`, PHY `0x13d50000`,
  DBI `0x13800000`, PCS `0x13d40000`, SYSREG `0x13c20000`.
- [Power/link lifecycle](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/drivers/soc/samsung/cpif/modem_ctrl_s5100.c):
  AP status high, dump notification low, reset low/50 ms, optional CP_WRESET_WA
  power low/50 us, power high/50 ms, reset high/50 ms. This follows link cleanup
  and control-message initialization, not just bare GPIO writes. Normal boot
  checks CP status and registers PCIe; phone-active IRQ follows completion.
- [SPI image upload](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/drivers/soc/samsung/cpif/boot_device_spi.c):
  one TX transfer, maximum 128 KiB, no DMA requirement for this boot path.
- [SIPC5 header facts](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/drivers/soc/samsung/cpif/include/sipc5.h):
  4-byte ordinary / 5-byte control / 6-byte extended length header. This ARM64
  implementation calculates padding to **8** bytes (`BITS_PER_LONG / 8`),
  so do not assume all SIPC5 implementations use 4-byte padding.

Local device-tree evidence from B: `device/dts/boot-dtb.0.dtb` +
`device/dts/dtbo.1.dtb` (X1SLTE EUR OPEN 21). `live-fdt.dtb` came from a z3s
TWRP and is not the base S20's wiring. The gpm GPIO block is **CMGP** at
`0x15c30000`, distinct from ALIVE `0x15850000`.

| Signal | Pin | Bank offset in controller |
| --- | --- | --- |
| CP power on | CMGP gpm20-0 | `0x280` |
| CP reset | CMGP gpm9-0 | `0x120` |
| AP/PDA active | CMGP gpm25-0 | `0x320` |
| CP phone active | CMGP gpm28-0 | `0x380` |
| CP wake up | CMGP gpm30-0 | `0x3c0` |
| CP PS hold | CMGP gpm31-0 | `0x3e0` |
| AP wake up | PERIC1 gpg1-3 | `0xc0` |
| Dump notification | PERIC1 gpp8-6 | `0x60` |

**Pin multiplexing needs an explicit lifecycle:** SPI16 uses gpp8-4/5/6 and
gpp8-7 (CS). gpp8-6 is also AP dump notification after boot. Setting it to a
GPIO while loading BOOT would steal an SPI pin. Resolve the Samsung ordering
and restore the correct mux after SPI, under our existing pinctrl lock.

## Concrete blockers found in our layout

| CP region | Required range (exclusive end) | Current conflict |
| --- | --- | --- |
| IPC `cp_rmem` | `0xf0000000–0xf0800000` | Loader DOWNLOAD staging `0xf0000000+0x01000000`; UEFI labels it XNU Download |
| MSI `cp_msi_rmem` | `0xf6200000–0xf6201000` | No overlap with current audited audio source allocations; UEFI ownership still needs verification |
| Packet/shared `cp_rmem_2` | `0xd0000000–0xd4000000` | Saved boot's display pool `0xd3ff0000+0x0c000000`: 64 KiB overlaps |

The saved `work/xnulog.txt` reports XNU's window at `0x880000000+0x17e000000`
and Wi-Fi DMA at `0xe8630000+0x02000000`, neither overlapping these CP regions.
These are observations from a saved boot, not proof of the next allocation.
Allocator alignment/headroom must also be reserved, beyond just usable pool bytes.

Before any CP boot/DMA: reserve all three CP regions from UEFI and every pool
allocator, relocate loader staging coherently in UEFI and loader, and verify
rollback/recovery still works. Then verify channel-1 S2MPU/DMA and cache policy.
Do not merely choose another unexplained physical address.

Call audio is another separate issue: `redwood_exynos_sound.c` maps 64 MB of
voice IOVA onto a dummy spare MB and maps a dummy PCI region. That was sufficient
for existing audio bring-up; it cannot carry real CP/VSS audio. **Correction to
the first progress message: SLOG ends at `0xf4a00000`; it does not cover MSI at
`0xf6200000`.** The audit and checklist use the corrected ranges.

## Changes and validation, 2026-11-11

1. Created isolated branch/worktree; no changes staged from other agents.
2. Added this pinned comparison, hardware map, blockers and milestone checklist;
   marked inherited Qualcomm documentation clearly.
3. Added `tools/cellular/shannon.py`: read-only firmware TOC inventory, bounded
   payload spans, SPI BOOT-size check and optional CRC verification; NV and OFFSET
   are explicitly not treated as firmware payloads. CP load addresses are not AP
   physical addresses. SIPC5 framing validates lengths, control/extended fields,
   truncation, padding and a frame-size cap; it is not a Samsung RIL.
4. Added `tools/cellular/memory-audit.py`: compare source memory ranges and saved
   loader allocations against the CP map. Missing dynamic evidence stays unknown;
   a conflicting report exits 1, missing evidence exits 2, no conflicts/unknowns
   exits 0. It does not grant hardware access or validate every UEFI allocation.
5. Ran against our local RADIO backup: BOOT (64,496 bytes), MAIN, APM, VSS and
   INFO all passed CRC32. NV contents were not read. CRC validates accidental
   corruption, not firmware authenticity, compatibility or secure boot.
6. Saved-log memory audit found the staging and 64-KiB display overlaps above.
   Expected nonzero exit is evidence of blockers, not a completed fix.
7. Host parser tests cover malformed/truncated images, overlapping regions,
   bad CRCs, extended headers, short prefixes, 4/8-byte padding and boundary
   overlaps. Exact test result is recorded in `tools/cellular/README.md`.

## Checklist and acceptance criteria

- [x] Compare requested branch against actual default branch and inspect latest commits.
- [x] Find companion data branch and distinguish recorded LTE/SMS evidence from calls.
- [x] Identify Samsung-specific boot, IPC, packet and UI layers; pin source revisions.
- [x] Isolate work from camera/audio sessions on `x1slte-cellular`.
- [x] Validate RADIO layout and firmware CRCs offline without EFS reads/writes.
- [x] Add malformed-input tests and detect concrete memory overlaps.
- [x] Build a native Settings Cellular page and persistent data preference,
  with explicit unavailable service/call status; see [UI checklist](cellular-ui.md).
- [x] Implement CP-aware loader allocations and matching UEFI source reservations.
- [x] Compile the ARM64 UEFI loader; validate 163,840 allocator boundary cases
  with ASan/UBSan and pass 19 Python format/audit/reservation tests.
- [ ] M0: relocate loader staging and reserve CP regions in UEFI/pool allocation.
  Verify USB recovery, display, Wi-Fi and sound still work; audit each new boot.
- [ ] M1: bounded, opt-in read-only GPIO/clock snapshot from actual S20 wiring.
  No unclocked PCIe register reads; verify CMGP and shared SPI pin ownership.
- [ ] M2: implement CP power/reset and SPI BOOT, one explicit attempt per boot;
  timeouts and rollback. Prove BOOT accepted / CP status handshake, not just TX.
- [ ] M3: implement channel-1 host clocks/PHY, PCIe enumeration/BAR setup,
  S2MPU/DMA permissions, remaining firmware stages and boot completion.
- [ ] M4: bounded IPC queues, doorbells/interrupts, cache discipline, crash
  detection and RAM-backed RFS handling. Preserve all persistent NV/EFS state.
- [ ] M5: Samsung IPC SIM state, registration, operator and signal; validate
  reconnect/reject/no-SIM states without logging subscriber identifiers.
- [ ] M6: APN/PDP session, real address/gateway/DNS/MTU, raw channel and pktproc
  data transport. Prove bidirectional packets with interface-bound probes.
- [ ] M7: independent iOS cellular interface + SystemConfiguration routing,
  working DNS/HTTPS with Wi-Fi excluded, preserving USB management access.
- [ ] M8: reconnect/idle/wake tests; record actual RTT/throughput and packet
  counters. Evaluate power/flow control rather than importing Pixel keep-alives.
- [ ] M9: CoreTelephony/Settings/status bar from real Samsung state. Data toggle
  must change actual traffic/routes; no fake SIM-ready success or blank replies
  that falsely indicate call/PIN success. Review iOS 16.5 ABI before porting hooks.
- [ ] M10: incoming/outgoing ordinary calls, actual call state/hangup/ringing,
  two-way audio through ABOX/VSS, proximity, speaker/Bluetooth routing and IMS/
  VoLTE where required by the carrier. Validate independently from data.
- [ ] M11: SMS transport and native Messages integration; no test messages or
  calls sent without a user-provided destination and explicit instruction.

**Next hardware milestone:** build the matching full UEFI image and validate M0;
the loader/source implementation is prepared below. Investigate firmware/CPIF boot
ordering only as phone-free research. Hardware milestones wait for
the existing phone holder to release it; obtaining a lock is not evidence that
an unvalidated stage is safe. Record commit, kernel/loader identity, exact
arguments, command, observation, rollback and open issue for each hardware test.

Do not mark this branch as shipping LTE/calls until M7/M10 are demonstrated on
our SM-G980F. No firmware, EFS data, Apple files, phone logs or credentials belong
in its commits.

## M0 source implementation and UI checkpoint — 2026-11-11

- UI implementation is a separate commit (`d35f0c8`) so it can be installed
  independently while the modem and memory changes remain untested. It uses
  only the existing Preferences tweak loader and mobile preferences. No system
  image/recovery/UEFI flash is needed for the menu.
- Loader staging no longer dereferences fixed address `0xf0000000`; a 16-MiB
  buffer is allocated before download or kernel loading, excluding CP regions.
  If no safe buffer exists, the loader holds with a diagnostic instead of using
  the CP region as a fallback.
- `xnuloader/s20_memory.h` selects aligned, exact-size ranges. Display and Wi-Fi
  pools, USB buffers, large transfer staging and ramfb use the same CP exclusion
  policy. This avoids the earlier extra 64-KiB alignment reservation; a 192-MiB
  free span starting at `0xd4000000` can fit a 192-MiB pool exactly.
- The real firmware memory map is checked for complete `EfiReservedMemoryType`
  coverage of the three CP regions, with `cellular memory: reserved N/3` logged
  at each kernel run. A new loader on an old UEFI does not report a fictional
  reservation. Warm runs repeat allocation evidence after the menu resets logs.
- Matching x1slte UEFI overlay reserves IPC/MSI/packet memory with ARM
  `UNCACHED_UNBUFFERED` mappings. The private UEFI repo also has an isolated
  `x1slte-cellular` worktree at `work/g980f-uefi-cellular`, commit **`6e75773`**;
  its original `main` worktree was not changed. Nothing pushed/CI-triggered.
- Memory audit now requires an observed address for dynamically allocated
  staging and a deployed `reserved 3/3` marker. An old boot log cannot validate
  new allocations. It isolates the latest boot when logs contain multiple runs.
- Verification: **19 Python tests pass**, including legacy overlap reproduction,
  incomplete-boot handling and UEFI descriptor non-overlap. The actual C range
  selector passes **163,840** comparisons against an independent brute-force
  oracle under ASan/UBSan, plus high-memory/invalid-input cases.
- Loader cross-compiles/links successfully to ARM64 PE/COFF with Apple clang and
  the local `tools/bin/lld-link`. One pre-existing unused `reboot` warning.
  Build-only dependencies `identity.h` and `menu_font.h` were copied into this
  isolated worktree from the existing build; they remain ignored and uncommitted.
- **Not yet:** full Mu-Silicium image build, phone boot, recovery/display/Wi-Fi/
  audio regression checks, actual `reserved 3/3`, modem GPIO/SPI/link bring-up.
  M0 remains unchecked until those device checks pass. No loader/UEFI changes
  from this branch have been installed on the phone.

Reproduce the host checks:

```sh
bash tools/cellular/check.sh
CC=/usr/bin/clang LD="$PWD/../../tools/bin/lld-link" bash xnuloader/build.sh
bash tools/cellular/build-ui.sh
```

Use the matching full UEFI build plus this branch's loader for future modem
tests. The standalone compiled loader does not retrofit firmware reservations
into the currently installed UEFI. Current UI installation is independent.

## Native UEFI build and first hardware baseline — 2026-11-11

The full matching UEFI FD now **builds locally on the Mac**, pinned Mu-Silicium
`4cba43c9` in isolated `work/cellular-mu`. Main source checkouts are untouched.
The normal Stuart update failed because NuGet requires Mono and the prebuilt
Mu-Basetools package has no macOS ARM64 build. Native BaseTools C compilation
(`HOST_ARCH=AARCH64`) succeeds, so neither a Linux VM nor those binary packages
are needed. Direct EDK2 `build.py` using Apple clang, Zig's LLVM library/linker
tools and Python 3.12 built the FD in **18 seconds** (10:15:57). FD size 2 MiB;
compressed FV uses `0x7b518`, leaving the embedded payload slot free.

Local, ignored build scripts and logs: `build/cellular-basetools-native.sh`,
`build/cellular-uefi-build-native.sh`, corresponding `.log`/`.exit` files.
Python/UV dependencies are isolated under `build/cellular-*`; shell/default
Python and other agents' toolchains were not changed. All six top-level
submodules match the pinned revision. An unnecessary recursive download of
OpenSSL unit-test submodules was stopped; only the isolated fetch process tree
was terminated. Sources already fetched were retained.

At 10:10, `tools/cellular/s5123-gpio.py --read` performed six `r PA 3` commands
on documented CMGP CON/DAT/PUD registers through the existing kernel. It needs
the phone lock, accepts only six reviewed bank bases, times out and stops on
an error, and never changes a GPIO or reads an unclocked SPI/PCIe block.
Private evidence: `logs/cellular/gpio-20261111-101006-365488/report.json`.

| Signal | Function | Physical level | Pull |
| --- | --- | --- | --- |
| reset_n (gpm9-0) | input | 1 | down |
| power_on (gpm20-0) | input | 0 | down |
| pda_active (gpm25-0) | input | 0 | down |
| phone_active (gpm28-0) | input | 0 | down |
| cp_wakeup (gpm30-0) | input | 1 | up |
| ps_hold (gpm31-0) | input | 1 | up |

All six reads succeeded and iOS/SSH stayed up. Input highs with pull-ups do not
prove CP execution; no SIM, registration or modem-boot claim follows. This
completes the GPIO baseline portion of M1, not its clock/SPI portion.

SPI16 facts resolved from the final overlay and CAL source:

- Use `__symbols__/usi_16` at **`0x10721048`**, not the differently indexed
  `aliases/usi16` (`0x10721028`). Fragment 68 sets UART, but later fragment 131
  overrides the same `usi_16` to SPI. Do not stop at the earlier overlay.
- CMU_PERIC1 is `0x10700000`: USI16 mux `+0x690` (source-select bit 4),
  divider `+0x1820` (4 bits), IP gate `+0x20fc`, Q-channel `+0x3070`.
- Samsung's SPI code explicitly uses IP clock = four times SCLK. CP BOOT's
  max SCLK is 2.15 MHz. Its CAL LUT supports 8.6 MHz IP using the 26-MHz
  oscillator path and divider 3 (register value 2), unlike the fast 400-MHz
  path used by sensor SPI. These remain source facts, not measured clocks.

### Prepared M0 boot test

The installed recovery image was backed up over SSH to
`out/cellular/m0-20261111/preinstalled-recovery.img`. It is 69,009,408 bytes,
byte-for-byte identical to the current main recovery artifact; kernel/root tail
also matches. AVB signature and recovery hash verified with the existing AOSP
test key. The phone remains on its existing kernel at preparation time.

`tools/cellular/prepare-m0.py` builds a candidate from that backup, changing
only the UEFI FD, the embedded loader and required Android/AVB hashes. It keeps
the installed BootShim, Samsung DT table, every payload argument, kernel,
root and log body intact. The new loader is built with `GIC_V2=1`,
`SUPER_TAIL_START=0x1e8b04000`, `SUPER_ROOT_LEN=0x100000000`. Original and
candidate AVB verification both pass; footer bytes are unchanged, so only the
first 16 MiB of recovery needs writing. Rollback head is prepared too.

Candidate/manifest/write segments live under
`out/cellular/m0-20261111/candidate/`. No bootloader/modem/EFS partition is part
of this test. Hardware boot/rollback results must be appended before M0 is marked
complete. Host checks now pass **22 Python tests** plus the allocator oracle.
