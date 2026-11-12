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
