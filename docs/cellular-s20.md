# Galaxy S20 cellular: implementation log and checklist

**Latest hardware status, 2026-11-11 ~11:49:** memory bring-up now passes on
our ORIGINAL working UEFI: all three CP ranges reserved, every CP page verified
Normal Non-cacheable, iOS boot and kernel-return checks pass, memory audit exits
0. This uses opt-in boot-service reservations, avoiding the failed native FD.
Read-only GPIO/clock/pin baselines are complete. Original BOOT was restored after
each bounded test; phone is back on its original images and released. No CP
power/reset/firmware/DMA/NV action has occurred. Later SPI internal-loopback
results are recorded below; no modem BOOT was sent. LTE/calls remain unavailable.

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
- Phone operations use the shared lock as `codex-cellular`. B's original
  camera ownership was respected; expired-lock takeover and later test locks
  were logged. Bounded cellular boot tests are recorded below. Main kernel
  sources/build output remain untouched; do not symlink its `xnu-src` here.
  No push requested or performed.
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
- [x] M0 memory: relocated staging, reserved CP regions through working UEFI,
  verified live Normal-NC mappings on every CP page before and after iOS.
  Full saved-log audit exits 0; bounded tests restore exact original BOOT.
- [ ] M0 regression completion: USB recovery/iOS boot/Wi-Fi driver are verified;
  independent display/touch/audio regression remains before shipping broadly.
- [x] M1: opt-in read-only CMGP GPIO, PERIC1 CMU/SYSREG and shared GPP8 pin
  snapshots collected. No SPI/PCIe controller read or write. Active pin/clock
  ownership and restore sequences must still be enforced before M2 writes.
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

### M0 hardware result: failed boot, verified rollback complete

- First attempt: wrote only the candidate recovery head through existing Log
  access; device readback succeeded. Cold reboot returned iOS/SSH in 70 seconds,
  same kernel and arguments. However the display pool was still `0xd3ff0000`;
  `/log` is kernel-only and contained no new loader reservation marker. The
  active standalone BOOT copy was still the old one. This was not an M0 pass.
- Backed up the installed standalone BOOT prefix (2,445,312 bytes); it matched
  the pre-test recovery prefix exactly. Candidate had the same Samsung DT
  table and Android boot layout as the usual deployment path.
- Second attempt: wrote the candidate's matching standalone BOOT prefix;
  device readback succeeded. Reboot did not return USB/iOS within ~270 seconds.
  No proof that the new loader ran; the new FD and new loader were changed
  together, so the failing component is not isolated. No modem was started.
- Rollback completed at ~11:02 after the user entered Download Mode. Heimdall
  restored the saved original BOOT prefix successfully; iOS/SSH returned. The
  known-good loader then restored the original 16 MiB recovery head with device
  readback confirmation. Cold reboot returned iOS/SSH in 63 seconds, with the
  original kernel and boot arguments. Independent SSH reads of both installed
  prefixes matched their saved originals byte-for-byte. Cellular UI files remain
  installed. Logs: `build/cellular-m0-rollback.log` and
  `build/cellular-m0-restore-recovery.log` in the research root.
- Before another device test, isolate the changed loader from the changed FD
  offline. Both were replaced together in this failed test; no root cause has
  been established. Preserve the verified original images and rollback path.
- Backup BOOT prefix: `out/cellular/m0-20261111/rollback-boot-head.bin`, SHA256
  `5fa3cc24b57946e88a9795fac06f39bd0139ac62cae4f3df32e9520d294a2579`.
  Original recovery head: `out/cellular/m0-20261111/candidate/rollback-head.bin`.
  Full recovery backup: `out/cellular/m0-20261111/preinstalled-recovery.img`.
- All firmware artifacts/logs stay ignored locally. Samsung bootloader,
  modem/EFS/calibration partitions and iOS kernel/root/data were not modified.
  M0 remains incomplete despite the successful host builds/tests.


### Allocator-only hardware isolation — 2026-11-11 ~11:17

The actual allocator path passed on the original working UEFI. The preflight
application loaded the exact installed loader as `fallback.efi` before probing,
allocated/free'd the three pools, wrote a bounded report in its dedicated
`probe.report` payload RAM entry, then started the original loader. Its PE base
is `0xa3000000`, leaving the original loader's `0xa1000000` base free. No CP
register, firmware, power/reset, cache-policy or modem DMA action occurred.

Observed: download `0xff000000+0x1000000`, Wi-Fi `0xfd000000+0x2000000`, display
`0xd4000000+0xc000000`; all allocations and frees succeeded. Original firmware
reports **reserved 0/3**, as expected. This proves allocation/exclusion under
real boot services, not full loader boot or safe modem DMA. iOS returned through
the original loader in 76 seconds. Probe BOOT was removed immediately; original
BOOT readback matched, and iOS returned again in 67 seconds. Recovery was unchanged.

Reproduce build/pack offline from this worktree:

```sh
CELLULAR_PREFLIGHT_ONLY=1 GIC_V2=1 CC=/usr/bin/clang LD=/path/to/lld-link bash xnuloader/build.sh
# Preserve this artifact separately, then rebuild the normal loader without the flag.
python3 -B tools/cellular/prepare-m0.py --backup /private/preinstalled-recovery.img \
  --keep-installed-fd --with-installed-fallback --loader /private/preflight.efi \
  --output /private/preflight-candidate
```

The packaging tool verifies AVB and preserves all argument entries, original
firmware code and exact fallback bytes. It rejects preflight without fallback,
recursive fallback backups and combining preflight with new firmware. Actual
phone deployment uses the normal phone lock and bounded standalone BOOT prefix;
the local visible test runner automatically restored the original after capture.
`preflight-report.py --manifest /private/preflight-candidate/manifest.json
--output /private/report.txt` reads only the bounded report RAM slot, using the
existing debug port. It refuses missing markers and malformed addresses/lengths.

Evidence stays ignored in `out/cellular/m0-20261111/`; visible job
`build/cellular-allocator-preflight.log` exited 0. **27 Python tests** and the
163,840-case allocator oracle pass. Next isolate the full changed loader on
original UEFI before blaming or retesting the native FD.


### Retained CP reservations on the original UEFI — 2026-11-11 ~11:43

The full changed loader booted the original iOS kernel in 62 seconds on the
original FD. That isolated the earlier failure to the native FD or its interaction;
allocator and ordinary loader boot work without it. A subsequent experiment
claims the CP regions through existing UEFI services, preserving firmware code:

1. Validate the complete memory map and DXE/GCD capabilities before changes.
   Only the exact legacy `0xf0000000+16 MiB` BootServicesData descriptor may
   be relinquished. Packet/MSI ranges must already be free or reserved.
2. Allocate CP IPC 8 MiB, packet 64 MiB, MSI **64 KiB** as ReservedMemoryType.
   MSI is rounded from its actual 4-KiB span to the firmware's 64-KiB reserved
   allocation granule. No firmware bytes, modem partition or NV are accessed.
3. Set Normal Non-cacheable through DXE SetMemorySpaceAttributes, preserving
   unrelated attributes. The pinned ARM implementation maps **EFI_MEMORY_WC**
   to Normal NC (MAIR 0x44); EFI_MEMORY_UC instead maps Device memory. Initial
   Device-UC preflight was rolled back; the corrected Normal-NC path passed.
4. Verify reserved ownership separately from cache policy. GetMemoryMap cache
   flags remained stale for these allocations, so GCD and live EL1 translation
   are checked for **every 4-KiB CP page**, not inferred from return codes.
   PAR_EL1 fault, output address and Attr are validated; PAR_EL1 is restored.
   [Arm's PAR_EL1 register description](https://documentation-service.arm.com/static/5e7b6b167158f500bd5bf124)
   documents the status/address/attribute fields. Local pinned ArmMmuLib/CpuDxe
   sources establish the Normal-NC 0x44 mapping.

`CELLULAR_RESERVE_CP=1` opts in when building the loader. It is not automatically
enabled in the default build yet. Preflight uses the same operations, then undoes
all attributes/ownership before the original loader fallback. A failed normal
reservation attempts rollback and reports unavailable; no modem is started.

Hardware evidence:

- Normal-NC preflight: reserved **3/3**, live Normal-NC **3/3**; every allocation,
  cache setter, free and restore returned 0. Original loader booted iOS; probe
  was removed and original BOOT verified byte-for-byte.
- Full retained-reservation iOS boot: **70 seconds**, same original kernel.
  `reserved-normal-v2-full-log.txt` and `reserved-normal-v2-after-return-log.txt`
  prove reserved **3/3**, Normal-NC **3/3**, staging `ff000000+1000000`, Wi-Fi
  `fd000000+2000000`, display `d4000000+c000000`, same high XNU window.
  The after-return audit exits **0**, with no listed conflict or unknown.
- Wi-Fi: all 512 DMA grants accepted, firmware loaded, station joined, RX/TX
  packets observed. The collector's `.106` HTTP check failed because the actual
  address announced by rwlogd was `.105` in both baseline and test logs.
  USB/SSH worked. This is not an independent Wi-Fi throughput/sound/touch test.
- Original BOOT restored afterward, independently read back identical, and iOS
  returned in **66 seconds**. Recovery stayed original throughout isolation.
  Visible job `build/cellular-reserved-normal-v2.log` exited **0**.
- An earlier retained boot succeeded in 67 seconds but optional `/sbin/ifconfig`
  was absent, interrupting log capture. Its finally-block restored BOOT correctly.
  The corrected collector makes optional network checks nonfatal.

All evidence is private/ignored in `out/cellular/m0-20261111/`. Host checks:
**29 Python tests**, **19 UEFI ownership/cache cases**, **163,840 allocator
oracle comparisons** under ASan/UBSan. This remains a partial DMA audit; PCIe1,
S2MPU/IOMMU, IPC synchronization and modem/audio ownership are separate stages.

### M1 completed: clock and shared pin snapshot — 2026-11-11 ~11:49

`tools/cellular/s5123-clocks.py --read` requires the phone lock and performs nine
single-word reads: PERIC1 mux/divider/two gates/Q-channel, SYSREG USI16 mode,
and GPP8 CON/DAT/PUD. The latter bank is **0x10730060**, not 0x10740060.
No SPI controller or unclocked PCIe register is touched. Evidence:
`logs/cellular/clocks-20261111-114840-552162/report.json`.

Observed: mux `0x10` (CMU_TOP source, busy 0), divider 0 (divide 1, busy 0),
both gates `0x200000`, Q-channel `0x3f0002`, USI16 mode **0**. GPP8 pins 4..7
are all inputs, level 0, pull-down. These observations do not prove the SPI
controller can be accessed. Pins must be explicitly assigned for SPI; GPP8-6
is later shared with dump notification and must be handed back appropriately.

Clock planning correction: the CAL table's nominal 8.6-MHz row cannot justify
exceeding the 2.15-MHz BOOT limit. With a 26-MHz oscillator and IP=4*SCLK,
divide **3** gives ~2.1667 MHz SCLK; conservative divide **4** (register 3)
gives **1.625 MHz**. The new tool/tests enforce that bound and avoid inventing
a rate when CMU_TOP is selected. This is a source-based plan, not measured SCLK.

SPI16 board behavior resolved for the next implementation: mode 0, 8-bit,
64-byte FIFO, status RX count at bit 15/TX count at bit 6 (mask 0x7f), TX_DONE
bit 25, controller-managed auto-CS with NSC count 2 (overlay mode 1). Samsung's
PIO path splits a logical transfer into <=64-byte packets, receives/drains
ignored RX even for TX-only, resets/flushed FIFOs between packets, and uses no
DMA. Preserve that wire behavior rather than assuming a continuous manual CS.
Controller completion alone must not be called CP BOOT acceptance.


### SPI PIO transport core (host tested; not connected to hardware)

`tools/cellular/s5123-spi.h` is a freestanding callback-based engine with no
physical MMIO access of its own. It matches board auto-CS/64-byte packets,
mode0/8-bit/no DMA, drains RX for TX-only, waits for both packet RX and TX_DONE,
and stops channels/packet generation/CS on every attempted-transfer exit.
Input bounds, <=128-KiB image size, <=2.15-MHz SCLK, wire-time budget, global
deadline, bounded reset waits and a stalled-timer poll cap are enforced. A latch
prevents another attempt after success or failure in the same caller boot state.
Readiness is mandatory; no current device path calls the engine.

The sanitizer controller model validates exact byte order/length for 1,63,64,65,
64496,65536,131072 bytes and timer wrap; all RX is drained and FIFO never exceeds
64. Six failure cases cover stalled RX, missing TX_DONE, controller error,
impossible FIFO count, stuck reset and frozen timer. Cleanup and retry refusal
are checked, as are invalid input/rate/time/readiness paths. These are host
transport tests, **not** CP execution, real SPI timing or firmware acceptance.
The firmware BOOT bytes have not been transmitted to the phone.

User-facing flow/comparison and remaining proof:
[cellular-flow.md](cellular-flow.md).


### UEFI mappings and real SPI internal loopback — 2026-11-11 ~12:16

The original firmware can add missing peripheral MMIO mappings through DXE.
A preflight verified every page as Device/identity before known-safe reads.
CMU PERIC1 and SPI16 were GCD holes and were added; SYSREG/GPIO already had
Device mappings. UEFI reads matched the kernel GPIO/clock baseline. SPI was
mapped/translated but not read during that first test. Added mappings live only
until cold reset. Original BOOT was restored, iOS returned in63s; job exit0.

The next preflight exercised actual SPI16 INTERNAL LOOPBACK with a generated
16-byte pattern, not firmware. It required reserved/live Normal-NC3/3, no USI
owner, external GPP8 pads left as GPIO inputs, stable/enabled gates and safe
CP control levels. It selected oscillator/divide4, enabled USI SPI mode and
internal loopback, then used the same bounded PIO engine. **Completed16,
received16, mismatches0, error0. Controller readback/restoration1; clock/USI/pin
restoration1.** External pins remained inputs; no CP power/reset/pinmux write,
CP firmware, NV or subscriber operation occurred. Fallback booted iOS in98s.
Original BOOT was restored byte-for-byte; iOS returned in74s. Visible job
`build/cellular-spi-loopback.log/.exit`: exit0. Private report
`out/cellular/m0-20261111/spi-loopback-preflight-report.txt`.

Build-only guards require CELLULAR_SPI_LOOPBACK together with the original-loader
preflight, MMIO preflight and CP reservations. They cannot enter a normal loader
build. Probe RAM reports now allow16KiB and reject truncation; the reader remains
compatible with old8KiB reports. No caller sends real BOOT yet.

Further board facts verified before CP control writes:

- **AP2CP_WAKE is GPG1-3**, PERIC1 bank `0x107300c0`; baseline input/low/pulldown.
  It shares a GPIO bank with Bluetooth, so future RMW must preserve other pins.
- **GPM30-0 is CP2AP_WAKE**, an input; renamed in the read-only GPIO tool to
  avoid driving the wrong direction.
- Modem PCIe endpoint host `pcie@13D20000`, ch-num1, uses DT **pmu-offset0x720**.
  Do not copy the neighboring host's0x71c or a driver's default macro. Actual
  PMU `0x15860720` reads0, PHY isolation asserted. No PCIe controller read.
- Updated read-only13-word baseline evidence:
  `logs/cellular/clocks-20261111-122121-801233/report.json`.

User now targets Jio India LTE/VoLTE with a physical SIM inserted. This does not
mean our backend has read/detected the SIM. See [cellular-jio.md](cellular-jio.md).

### GPIO ownership proven; powered BOOT probe prepared — 2026-11-11

The GPIO-low preflight passed on hardware: all five AP outputs verified LOW,
then original direction/value fields and neighboring pins verified restored.
POWER_ON was never raised. `build/cellular-gpio-low.log/.exit` records exit0,
byte-exact original BOOT restoration and iOS return in74s.

The real BOOT adapter now compiles and the lifecycle/packaging host tests pass.
It includes a one-attempt latch, CRC validation, existing baseline/PHY guards,
2-second PIO limit, shutdown readback and a RAM-retaining failure hold. Review
fixed cleanup treating a failed settling delay as safe; the failure model now
allows timer failure before any elapsed time. Cleanup-only retry also checks
the delay and AP status/wake outputs. No actual powered BOOT test yet.

**Deployment baseline changed:** camera/Files/Developer Mode and Finder work
advanced while cellular was inactive. codex-files currently owns the phone.
Do not run old m0 deployment/rollback scripts against this newer installation.
Under the next phone lock, capture fresh BOOT/recovery/kernel baselines, verify
matching installed payload and original-loader fallback, then build a fresh
candidate. Preserve the newer kernel/root/arguments. Old backups remain only
historical recovery evidence, not a current rollback target.

Next acceptance checklist:

- [x] GPIO LOW-only writes, neighbor preservation and restoration on phone.
- [x] CRC-checked BOOT-only packaging and guarded power lifecycle compiled.
- [x] Host tests:30 Python,23 memory/cache,163840 allocator comparisons,
  eight SPI streams/six failures,22 lifecycle callback failures.
- [ ] Fresh baseline and isolated candidate after current phone holder finishes.
- [ ] One powered BOOT transfer with bounded shutdown and current BOOT restored.
- [ ] Prove CP acceptance through PCIe1/firmware handshake; TX alone is insufficient.
- [ ] MAIN/APM boot, IPC, SIM state, Jio LTE registration/data, then IMS/VoLTE/audio.

Samsung source clarifies the next boundary: `cpif/boot_device_spi.c:33` only
reports SPI transfer status. `modem_ctrl_s5100.c:649` polls CP2AP_WAKE up to
200 times with20ms sleeps, then `start_normal_boot` registers PCIe.
`link_device.c:3120` waits200ms, powers PCIe, requires a discovered modem PCI
device and initializes MSI. Our CP2AP_WAKE already reads high with a pull-up
before boot, so copying that GPIO check alone would falsely imply acceptance.
The first bounded BOOT probe deliberately leaves PCIe isolated and reports
acceptance unproven; PCIe enumeration is a separate implementation/test stage.
