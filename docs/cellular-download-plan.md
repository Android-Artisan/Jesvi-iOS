# S20 download backend: next stage after live PCIe endpoint

Current proof (Nov12 15:08): BOOT64496B + PCIe L0 + distinct Type0 S5123 endpoint;
verified CPoff cleanup, no restart. Full firmware/ONLINE/network is unproven.
No repeated link runs are needed to start this source work. Keep SIM out.

## Board facts and queue layout

Source: own device/dts/dtbo.1.dtb, /fragment@modemif/__overlay__/cpif, not the
unrelated live-fdt dump. Only nonidentity properties decoded; private inventory
out/cellular/live/download-queue-properties.json. Samsung implementation pinned
ExtremeXT/android_kernel_samsung_exynos990@69515fbb7a4395898c05a8624f76a12afbac11c5,
drivers/soc/samsung/cpif/ and drivers/pci/controller/dwc/.

Protocol0=SIPC, link_type1=PCIe, ipc_version0x32, link_attrs0xfc9. DPRAM magic,
legacy memory BOOT/DUMP, BOOT/DUMP alignment, bootloader-SPI and SBD IPC are set;
SBD_BOOT is absent. umts_boot0 channel215/format4, attrs2 (SIPC5); no
NO_LINK_HEADER bit, so boot writes receive SIPC5 framing.

All offsets/sizes below are hexadecimal, relative to IPC AP reservationF0000000+8MiB:

| Object | Offset/size | Source meaning |
| --- | --- | --- |
| magic/access | 0/4 | 32-bit fields; BOOT magic424f4f54, access1 |
| FMT indices | 8,c,10,14 | TXhead/TXtail/RXhead/RXtail |
| RAW indices | 18,1c,20,24 | TXhead/TXtail/RXhead/RXtail |
| FMT TX | 1000+1000 | 4KiB circular queue |
| FMT RX | 2000+1000 | 4KiB circular queue |
| RAW TX | 3000+1fd000 | Ends200000, boot/download stream |
| RAW RX | 200000+200000 | Ends400000, fits IPC reservation |

link_device_memory_legacy.c:create_legacy_link_device supplies exactly this
geometry from DT properties. init_legacy_link initializes magic/access/indices
and then writes IPC magic/access; link_start_normal_boot overrides BOOT magic
for DPRAM_MAGIC. Do not initialize queues while CP is using them.

## Actual kernel call path and limits

bootdump_io_device.c:bootdump_write adds link headers and calls ld->send.
link_device.c:shmem_send -> xmit_to_cp classifies BOOT/DUMP channels and routes
them to xmit_to_legacy_link(...IPC_MAP_NORM_RAW). This path explicitly assumes
PCI link ready and does NOT send IPC IRQ for each boot write. The producer
validates indices/free space, copies a bounded circular frame, then publishes
head with a memory barrier. CP consumes/polls it. The class/function name shmem
is shared with PCIe and does not imply the S20 MAIN payload uses AP_SHMEM image
copy ioctl. IOCTL_LOAD_CP_IMAGE routes this board's BOOT to SPI; the SHMEM-only
bulk copy branch must not be used for MAIN on this board.

SIPC5 framing parser exists (shannon.py); the inner download command/ack protocol
and allowed firmware ordering are still required. A Linux queue transport is
not itself a replacement for cbd. Never substitute Google SIT commands merely
because Pixel6 uses S5123.

Doorbell pci_db_addr14d60000 is a PCI address. s51xx_pcie_probe programs/reads
BAR0 and derives an offset before mapping root memory; BAR granularity must be
measured with BMEoff. No guessed CPU doorbell address or broad inboundATU/S2MPU
change. Existing diagnostic only configures outbound CFG0 and clears EP BME;
its result does not authorize/establish DMA mappings.

## Implementation/proof checklist

- [x] BOOT power/PIO lifecycle, live L0 and actual endpoint proof.
- [x] Root BMEoff, endpoint BMEclear-only, verified shutdown and fault models.
- [x] Derive ring geometry/header route from own DT and pinned kernel.
- [ ] Dedicated XNU backend maps retained IPC RAM with proven NormalNC attributes;
  avoid cached aliases and Device mappings for bulk memcpy. Bounds/ownership
  checks, barriers and one producer/consumer owner required.
- [ ] Trace exact PCIe DMA aperture/address translation/S2 permission grants for
  these reserved ranges. Preserve UFS and unrelated HSI2 clients. Keep BMEoff
  until ready; verify root/EP state, never broad-disable protections.
- [ ] Recover/verify S20 cbd download command/ack formats and BOOT/MAIN/APM/VSS
  ordering (source or private binary analysis). Validate signed firmware/CRC
  metadata. No guessed firmware addresses, NV mutation or partition writes.
- [ ] Implement small command/ack transaction with timeout and crash/shutdown path;
  prove CP consumption/response before a full MAIN transfer.
- [ ] Load remaining required firmware, bounded handshake and true modem ONLINE.
- [ ] Samsung IPC control/SIM queries, then request Jio SIM insertion when useful.
- [ ] Data-session state, packets/interface/routes/DNS and cellular-only HTTPS.
- [ ] IMS/VoLTE/call/audio is a separate remaining stage.

Batch any required kernel additions into one notified restart only after the
backend and host checks are ready. Continue source/debug preparation live.

## Related references evaluated

The [S7 boot/RIL prototype](https://gist.github.com/tonyg/4ea14f4dfe414422c0648c6e0a8bcb5d)
shows an older shared-memory loader and finishing handshake. Its SHMEM uploads
and security ioctls do not establish S20 PCIe download compatibility. No code
copied or commands executed from it. The [S5123 code skeleton](https://github.com/grant-h/shannon_S5123)
is a possible navigation aid, not a working loader. A Samsung S5123 module
manual surfaced at FCC report5137238 but fetch returned403; its full contents
have not been reviewed, so no implementation depends on the search excerpt.

## Priority clarification and queue core — Nov12 continuation

User priority: 4G data, SMS and ordinary calls. Deliver data first and test SMS
as soon as modem control is available. Ordinary Jio LTE calls still require
IMS/VoLTE internally; hiding the VoLTE label does not remove that dependency.
SMS routing also needs carrier verification; do not assume an IMS-free route.
[Jio's LTE explanation](https://www.jio.com/help/faq/mobile/services/high-speed-internet/what-is-4g-lte-service-provided-by-jio/)
identifies VoLTE as its voice transport. Progress25–35% is a rough engineering
estimate for the full requested outcome, not measured completion or a deadline.

New s5123-ring.h provides bounded single-owner producer/consumer operations,
board RAW queue geometry, index/range validation, split wrapping copies and
explicit acquire-load/release-publish callbacks. A failed publish returns
COMMIT_UNKNOWN and must be resynchronized, never blindly retransmitted. Partial
copy failure leaves the published index unchanged. No hardware mapping, protocol
payload, DMA enable or modem command is included. Backend still pending.

Host ASan/UBSan test passed7680 independently enumerated position/length/direction
cases, wrap/full/empty behavior, partial-copy failures, uncertain publication,
malformed peer indices and overflow/metadata-overlap guards. Integrated into
check.sh. This is host proof only; no new phone operation/restart.

Source investigation: stock-g980f contains an encrypted972MiB archive left from
an earlier download; it has not been validated as complete or usable. No cbd/RIL
binary found in the extracted vendor firmware inventory. Do not treat that
archive as a recovered working loader or execute guessed legacy S7 commands.

## XNU cache audit and loader handoff — Nov12

Built, host-tested, not installed. New root debug command `cp.memory` requires
three exact 64-bit `/chosen` values from the loader: memory-version1, IPC base
F0000000 and size800000. The loader emits these only after rechecking actual
CP reservation/cache state. The audit checks the kernel physical aperture,
then translates each of2048 existing direct-map pages via AT S1E1R/PAR_EL1.
It reports NormalNC44, other cache types, faults, wrong PA and prior generic
debug aliases. It never reads/writes IPC contents, creates mappings, grants
DMA access or powers the modem. Missing certificate refuses access.

`ml_io_map_wcomb` is unsuitable for the whole8MiB IPC: it allocates from the
shared8MiB io_submap with KMA_NOFAIL. Use the existing Wi-Fi driver's approach,
`ml_io_map_unmappable(..., VM_WIMG_WCOMB)`, which uses kernel_map instead.
The XNU MAIR entry is44 (Normal outer/inner noncacheable). Before doing so,
resolve any incompatible existing alias; the audit is evidence, not permission
to ignore cached aliases. No hardware IPC backend is claimed yet.

Samsung s5100_poweron_pcie grants per-region access with
exynos_set_dev_stage2_ap("hsi2",0,base,size,ATTR_RW), excluding MSI. Its helper
uses HVC0xc6000100 and packs `(vid<<16)|instance_index`; own boot DT and Samsung
DTS identify HSI2 as25. Existing Wi-Fi uses HVC0xc6000101/HSI1 index24/VID1,
so those arguments must not be copied. No cellular HVC or DMA grant issued.

Validation: isolated kernel relink and loader build pass. All42 Python tests
and C sanitizer suites pass, including all2048 audit-address cases and cache/
fault/certificate rejection. Build logs in research-root build/
cellular-memory-build-final.log/.exit (0), loader log in private out/cellular/
live/memory-loader-build.txt. Current phone remains on the previous proven
SPI8/reservation base; this audit awaits a batched notified restart.

## External SIPC loader recovered from source — Nov12 continuation

Pinned upstream: [Samsung cpboot_v3 via LineageOS,27ec634140f0e8798fc626fa2f6f4a081dbd23cf](https://github.com/LineageOS/android_hardware_samsung_slsi-linaro_exynos/tree/27ec634140f0e8798fc626fa2f6f4a081dbd23cf/cpboot_v3).
ProtocolSipc.cpp/.h, Protocol.cpp and BootDumpExt.cpp carry Samsung2020 Apache2
notices. The [S20 common board configuration](https://github.com/LineageOS/android_device_samsung_universal9830-common/blob/0d6675d52807f129d624bc0d671dcdf6d7af71ce/device-common.mk)
builds cbd with SIPC. More importantly, our own extracted init.baseband.rc invokes
`cbd -t s5100 -bs -me -B umts_boot0` (SPI BOOT, PCIe MAIN), matching BootDumpExt
and the device kernel/DT. This is a source-derived compatibility case, not proof
that our CP has accepted these commands. No full vendor download is required
for this protocol investigation; the partial encrypted archive is untouched.

Actual RADIO table validates BOOT/MAIN/APM/VSS/INFO CRCs. TOC's final word is
count7; the per-image final words are not used to assign stages. BootDumpExt
moves BOOT to stage0 and starts the queue loop at TOC1. It stops building at
OFFSET (index6), places FIN there, and does not send trailing INFO. Our profile:

| Stage | Image | Bytes | 2048-byte fragments | Host sequence |
| --- | --- | ---: | ---: | --- |
| 0 | BOOT | 64496 | SPI, already proven | Not repeated in UDL loop |
| 1 | TOC | 800 | 1 | START, SEND image, DONE |
| 2 | MAIN | 59502592 | 29054 | START, SEND image, CRC, DONE |
| 3 | APM | 29728 | 15 | START, SEND image, DONE |
| 4 | VSS | 4827808 | 2358 | START, SEND image, DONE |
| 5 | NV | 1048576 | 512 | External source unresolved; blocked |
| F | FIN | 0 | 0 | START only, after all earlier stages |

UDL command is little-endian u32: request9000 or responseA000, OR stage<<8,
OR operation START0 / SEND1 / CRCc / DONEd (FAILf). Data contains four u32 words:
command,total-frame-count,one-based-current-frame,payload-length, then up to2048
bytes. One SEND acknowledgement follows the complete image, not each fragment.
CRC message is command+CRC, MAIN only in this external loop. Each write is
wrapped by our kernel's ordinary4-byte SIPC5 header, channel215, length excluding
padding to8 bytes. First intended live transaction is TOC START9100 -> A100;
its full TX bytes are `fc d7 08 00 00 91 00 00`. Do not send before IPC init,
cache mapping and DMA prerequisites. No UDL bytes have been sent to the phone.

New original MIT s5123-udl.h encodes these wire facts (no upstream implementation
copied), checks exact chunk sizes/stages/output bounds, and rejects malformed,
failed or unexpected ACKs. No automatic retransmission or device I/O. C sanitizer
tests pass golden vectors, all2048 final lengths and padding/capacity guards,
UINT32_MAX image arithmetic and malformed replies. udl_plan.py reads RADIO only,
CRC-verifies, requires the reviewed exact profile, emits stage metadata, and
explicitly blocks NV/FIN;3 plan tests pass. It never opens EFS/NV. Existing
kernel SHMEM transport facts remain applicable; MODAP/SIT paths are not used.

Additional mapping evidence: latest known-good loader log puts XNU RAM at
880000000+17e000000, wholly above the IPCF0000000+800000 range. Thus the new
cp.memory command is expected to report outside physical aperture without
calling phystokv. Future mapping should require certificate, disjoint kernel
aperture, no generic-debug alias, then validate every new NormalNC translation.
This narrows the alias issue; it is not yet a mapped IPC/backend proof.

## Live firmware download, first attempts — Nov12 23:15-23:35 (session A, codex-cellular out of usage)

Installed (two notified restarts, root/camera/GPU untouched): the reserving loader again (the 21:21 loader
had been built without CELLULAR_RESERVE_CP: "reserved 3/3, normal-nc 3/3" back) and a kernel with
`kern.redwood.poke "cp.s2mpu"`: HVC 0xc6000100, (VID0<<16)|HSI2 index 25, RW, for f0000000+800000 and
d0000000+4000000 (not the MSI page f6200000), as s5100_poweron_pcie. Only with the loader certificate.

`live-ipc --map-check` PASS (2048 Normal-NC pages, unmapped). `live-ipc --dump/--unmap` added.
`live-modem --reserved-base-verified BOOT INFO --download TOC` (tools/cellular/live-modem.c): after SPI
BOOT, init_legacy_link + BOOT magic (424f4f54, access 1, indices 0); after L0: cp.s2mpu, root and EP
command MSE|BME, EP BAR0 = pci_db_addr 0x14d60000 (reads 14d00004: 64-bit BAR), then TOC START
(fc d7 08 00 00 91 00 00) into RAW TX, head 0->8, poll RAW RX; bus master off again at the end.

Results, twice, CP shut down after (second time by hand, see below):
- SPI BOOT 64496 B OK, GPIO first 2 last 6 (WAKE high from the start, PS_HOLD rises; PHONE_ACTIVE never).
- PCIe L0, endpoint a5a5144d; **hypervisor grants both regions: ret 0**; BME on both ends verified.
- **CP never consumed**: RAW TX tail stays 0, RX head 0, nothing written anywhere in the header (dump).
- The second run "hung": the RX wait counted usleep(2000) iterations, which sleep far longer on this
  kernel -- fixed to a clock deadline. The process was killed; reset+power were set low by hand
  (m 0x15c30124/0x15c30284 bit0 = 0), IPC unmapped with --unmap, then a plain restart reset PCIe/SPI.

Not yet done that Samsung does before the UDL loop (next hypotheses, cheapest first, all live):
1. EP MSI (cap 0x50/0x54/0x58) pointed at the RC's MSI target; CP may wait for it before polling.
2. RC setup from dw_pcie_setup_rc: bus numbers (0x18), RC outbound MEM ATU for the BAR window, so the
   AP->CP doorbell (s51xx_pcie_send_doorbell_int, int_pcie_link_ack) can be written; Samsung sends one
   on every power-on after the first registration.
3. Wait for CP2AP_WAKEUP edge after the magic (check_cp_status) before link; WAKE is high from the
   first sample, so it may be a pull-up, not the CP.
4. Read the S5123 BOOT image's strings/handshake (private analysis) for what it waits for.

### MILESTONE Nov12 23:40: the CP answers -- TOC stage acknowledged

The missing step was MSI, as Samsung's exynos_pcie_rc_msi_init does for EP_SAMSUNG_MODEM: RC DBI
0x13800820/824 = f6200000/0 (the shm MSI page), 0x828 (INTR0 enable) = 0xf1, and the endpoint's MSI
capability (0x50: control 000a7005, 32-bit) address 0x54 = f6200000, data 0, enable bit16. With that,
right after it the CP consumed TOC START and replied in RAW RX:
  START  fc d7 08 00 00 91 00 00 -> fc d7 08 00 00 a1 00 00 ACK
  SEND   800 B TOC, one frame    -> ...01 a1 ACK  (TX head/tail 340)
  DONE                           -> ...0d a1 ACK  (TX 348, RX 18)
CP shut down after, cleanup_verified 1, no restart. Transcript: logs/cellular/a-download-*/ (private).
Next: MAIN (59 MB, 29054 frames, + CRC), APM, VSS, NV (nv_data.bin from the Mac's EFS backup image),
FIN START; then the CP should boot its main firmware (complete_normal_boot waits for its init on IPC).

### MILESTONE Nov12 23:45: full firmware download, main firmware running (PHONE_ACTIVE high)

`live-modem --download-all TOC PLAN` (PLAN lines "STAGE FILE OFFSET SIZE CRC", "15" = FIN), files staged
in /var/root (cell-radio.img = the RADIO backup, cell-nv.bin = nv_data.bin from the Mac's EFS backup):
  TOC 800 B ACK; MAIN 59502592 B / 29054 frames queued in 1.5 s, SEND ACK, CRC 7c455a85 ACK, DONE ACK;
  APM 29728 B ACK; VSS 4827808 B ACK; NV 1048576 B ACK; FIN START ACK.
Then GPIO 6 -> 7 within 500 ms: **PHONE_ACTIVE high and held for 10 s** = the CP's main firmware is up.
IPC header unchanged (magic still BOOT; RAW TX/RX 1794a8/88, all consumed). CP shut down after
(cleanup_verified 1), no AP restart.

Next (Samsung's complete_normal_boot and after): wait for the CP's init message (cp2ap msg / MSI at
f6200000; we have no MSI handler yet -- poll the MSI page/INTR0 status and the FMT RX queue), switch
the link to IPC (magic MEM_IPC_MAGIC, FMT/RAW queues), then Samsung IPC (SIPC FMT, RIL protocol) for
SIM status (SIM is out: expect "no SIM"), then radio power / registration, data (RAW channels),
SMS, calls. Keep the CP off between runs; nothing here needs an AP restart.

### MILESTONE Nov13 00:05: Samsung IPC handshake complete (INIT_START / PHONE_START / INIT_END)

After FIN (polled, no MSI handler needed): control words at IPC+0x400 ap2cp_msg, +0x404 cp2ap_msg,
+0x408/+0x40c united status (DT DRAM_V1; int = 0x80|0x40|cmd). Before boot ap2cp_united = ds_det 2<<14.
Doorbell AP->CP: iATU outbound region1 (DBI+0x300200: MEM 14d00000..14dfffff -> 14d00000 = EP BAR0),
write 0x10000+n to 0x14d60000 (pci_db_addr; DOORBELL_INT_ADD 0x10000, int_ap2cp_msg 0).
Seen live: cp2ap 0x83 (MSI status 1), then 0xC1 INIT_START -> AP 0xCD PIF_INIT_DONE + doorbell ->
CP 0xC8 PHONE_START -> AP: queues zero, magic 0xAA, access 1, 0xC2 INIT_END + doorbell. CP stops
repeating PHONE_START = accepted. MSI from CP arrives (RC 0x13800830 status bit0).

NEXT (blocking IPC): link_attrs 0xfc9 has LINK_ATTR_SBD_IPC; umts_ipc0 (ch 0xEB, FMT, ul 16x4K,
dl 32x4K), umts_rfs0, csd, router, dm0, cass ... carry attrs 0x82 = SIPC5|SBD_IPC. So FMT/RIL traffic
goes over SBD rings: create_sbd_link_device + init_sbd_link (link_device_memory_sbd.c, 616 lines)
must write the descriptor region into shmem at INIT_START, before PIF_INIT_DONE (we skipped it).
rmnet* (attrs 0x180) = packet processor (pktproc) for data. Port SBD layout to the live helper next.

### SBD rings, first live tries — Nov13 ~06:00 (session A)

live-modem now writes Samsung's SBD layout at INIT_START (before PIF_INIT_DONE), from this board's DT
iodevs (create_sbd_mem_map/init_sbd_link): 10 link channels in DT order -- cass 35, ipc0 235, ipc1 236,
rfs0 245, csd 1, router 25, dm0 28, loopback 244, multipdp_hiprio -> QoS ch 10, multipdp -> 11 (rmnet*
PS channels and the kor_skt/usa_spr option-region smd4/ciq0 skipped). g_desc at IPC+0x800 (version 1,
10 channels, rpwp_array_offset 0xf0c), buff_pos/len arrays to 0x85dc, buffers 0x10000..0x7a8000.
ap2cp_united = ds_det 2<<14 | ap_status 1<<1 | pda_active 1<<5. One request sent on umts_ipc0 UL:
SIPC5 f8 eb 0c 00 + IPC fmt (len 8, mseq 1, MISC 0a ME_VERSION 01 GET 02, ff), ap2cp 0x81 + doorbell.

Seen: CP keeps raising MSI after INIT_END (cp2ap_msg stays c8 -- data MSIs or PHONE_START repeats;
INIT_END resent up to 5x does not change it). ipc0 UL rp stays 0 (request NOT consumed). The 4th rpwp
block (DL wp) gets values that are not ring indices (256, ~10264 rising, 13184, 19918, 4096, 65535)
while the DL buffers stay zero: the CP writes some structure of its own there, i.e. our SBD layout does
not match the CP's (version, table sizes, or a region offset). dm0 DL wp=4 looked valid but its buffer
is zero too.

Next checks (live, no restart): read back the region after FIN and before INIT_START to see what the CP
itself writes (it may publish its own layout/version); compare with Samsung's g_desc for SIT/other
versions (sbd.h variants), try placing the SBD area elsewhere (mld->base may not be the IPC base for
SBD: check cp_shmem_get_nc_region / SZ_2M split in link_device.c ~3926) -- the 2 MB "nc region" hint
suggests the SBD base could be IPC+0 but sized 2 MB, or a different SHMEM index. Loop bug fixed: the
DL dump skips out-of-range indices (one run spun; tool killed, CP held off by hand, restarted).
Raw dump after INIT_END (IPC+0x400): ap2cp c2 | **cp2ap 0x83** (INT_VALID|SEND_FMT|SEND_RAW: "data for
you", so the MSIs are data notices, not PHONE_START repeats) | ap2cp_united 0x8022. At IPC+0xf48 the CP
wrote 20 bytes of its own: 00 01 17 28 80 33 ce 4d e0 33 ce 4d 04 00 00 10 ff ff ff ff -- inside our DL
wp array (0xf0c + 3*20). Legacy queue indices stay 0, SBD DL buffers zero. Our buff_pos arrays (from
0xf5c) are intact. => find what the CP places at 0xf48 (a CP-side descriptor at a fixed offset? then our
rpwp array must not sit there: compare the S20's real g_desc size -- if the stock cpif has
MAX_SBD_LINK_IDS 32 but an extra field, the rpwp array moves). Next: decode those 20 bytes, read the
stock S20 kernel's cpif (Samsung opensource G980F kernel, not ExtremeXT) sbd.h.

### MILESTONE Nov13 ~06:40: the modem talks Samsung IPC on umts_ipc0

The missing piece was the packet processor v2 info block (stock S20 config: CONFIG_CP_PKTPROC_V2=y,
JUMBO off, MODEM_IF_QOS=y): cp_rmem_2 (rmem_index 2) = d0000000+64MB = SHMEM_PKTPROC, CP view 0x28000000.
At INIT_START, before the SBD rings and PIF_INIT_DONE: info[0] = num_queues 1 | mode 1 (SKTBUF) <<4 |
max_packet 2048 <<8 = 00080011, q_info[0] = desc_base 28000100, num_desc 0x1933 (6451), data_base
28100000, fore 0, rear 0. With that, all SBD channels come up; DL umts_ipc0 (ch 235) frames:
  f8 eb 1d 00 | 19 00 00 00 13 05 03 0f 00 "\r\nAST_POWERON\r\n"
  f8 eb 0c 00 | 08 00 01 00 01 01 03 ad          PWR_PHONE_PWR_UP noti
  f8 eb 0d 00 | 09 00 02 00 0d 0f 03 06
  f8 eb 0f 00 | 0b 00 03 ff 26 01 01 00
  f8 eb 15 01 | 11 01 04 01 0a 01 02 ...         MISC_ME_VERSION response to our GET: "G980FXXSNHYB1 ... REV0"
(SBD buff_len_array values are not lengths here; use the SIPC5 length.) The legacy-FMT duplicate request
is not used by this CP (TX tail stays 0). Next: SIM status (SEC group), then radio power / registration.

### MILESTONE Nov13 ~06:50: SIM read and network seen (40 s live run, SIM inserted)

Decoded umts_ipc0 traffic (Samsung IPC: len u16, mseq, aseq, group, index, type; type 3 = NOTI, 2 = RESP):
- 13/05 "AST_POWERON"; 01/01 PWR_PHONE_PWR_UP; 26/01, 0d/0f early notices; 0a/01 ME_VERSION response.
- SIM: 05/13 carries the card ATR (3b 9f 96 80 1f c6 ...), 05/11, 05/10, 05/07, then 05/01 SIM status
  notices (00 00 / 82 00 alternating). Our SEC_SIM_STATUS GET (05 01 02) got GEN_PHONE_RES 80/01
  "05 01 02 03 80" -- error/unsupported form; the CP reports the SIM by itself anyway.
- NET (08): current PLMN notices with the Jio PLMN (405-8xx), NET_REGIST (08/05) updates, 08/3c, 08/19;
  DISP (07/06, 07/01) signal/icon info; 27/01, 0d/19. The CP powers RF and camps on LTE by itself after
  PWR_UP (stock NV behaviour); it is shut down at the end of every run (cleanup_verified 1).
Logs with SIM/cell identifiers stay private (logs/ and out/ are gitignored); do not copy them into docs.

Next: a long-running daemon instead of the 40 s test (keep CP up, rings serviced, MSI driven), decode
SIM status / NET_REGIST properly (libsamsung-ipc tables as a guide), then data: PDP context (GPRS group
0x0d) + pktproc RX/TX path -> a utun interface in iOS; SMS (group 0x04); calls/IMS last.

### Nov13 ~08:10: RFS server, AT channel, first data-call attempts (session A)

- Service (`redwood-cellulard`, launchd `com.redwood.cellular`) has a control FIFO /var/tmp/redwood-cell/cmd:
  `at <cmd>` (umts_router ch 25) and `fmt <grp> <idx> <type> <hex>` (Samsung IPC on umts_ipc0).
  Send ONE line per write after the FIFO exists (a write to a stale FIFO with no reader blocks).
- RFS server (after postmarketOS Note 8 mifmon.c / libsec-ril): byte stream over ch 245 in 2044-byte
  SIPC5 frames (not multi-frame headers). Seen: NV_WRITE 154330 B @0x80000 and 30394 B @0 (applied to the
  private /var/root/cell-nv.bin, never the EFS partition), OPEN "FLL.bin" (answered ENOENT; the EFS backup
  has root/FLL.bin -- serve the real file next).
- AT: "AT" -> OK. Any further command only echoes (AT+CPIN?, AT+CGDCONT=..., AT+CGATT=1): the AT engine
  stalls after the first command, with or without RFS answered first. Unknown yet why (Note 8's CP
  answered CPIN/COPS/CGATT).
- IPC data call (Note 8 layouts): 0d01 DEFINE (127 B: 01 cid 06 apn[100]...), 0d04 ACTIVATE (105 B: 01 cid)
  -> GEN_RES 0x8000 OK both, then GPRS_CALL_STATUS 0d10 "01 03 00 00" (cid 1, state 3 = not active, cause 0);
  no 0d09 IP config. Same with APN "jionet" or "", and after IPC PS attach 0d03 (0x8000). Note 8 saw the
  same: the CP only attaches for data after AT+CGDCONT + AT+CGATT=1. PWR 01 07 SET 0202 -> 0x8001 refused.
- NET_REGIST 08 05 data "21 03 07 ...": status field unclear (libsamsung-ipc numbering does not fit).
Next: make AT work past the first command (check the router UL/DL ring indices after each command, try
"\r\n" endings, check whether results arrive on another channel, e.g. umts_csd/ipc1), serve real EFS files
(FLL.bin), then AT attach -> IPC activate -> 0d09 IP config -> packet path (pktproc DL, multipdp UL) -> utun.
