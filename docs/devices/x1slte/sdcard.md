# microSD card on the S20 (design, A, 2026-11-20)

Goal: the S20's microSD slot as external storage in iOS (Files > Locations, like a USB drive), FAT32/exFAT.

## What iOS already brings

iOS 16 mounts external drives through DiskArbitration and the UserFS file provider
(`com.apple.filesystems.UserFS.FileProvider`, already registered on our phone), with FAT32 / exFAT in user space.
So the work is the kernel side: a driver that presents the card as a removable block device (IOMedia), the way a
USB mass-storage disk appears. To find out by test: which properties (Removable, Ejectable, protocol) make
diskarbitrationd hand it to UserFS.

## Hardware (Samsung's device tree, ExtremeXT kernel: exynos9830.dts + exynos990-x1slte_eur_open_21.dts)

| Item | Value |
|---|---|
| Controller | Synopsys DesignWare MMC ("samsung,exynos-dw-mshc"), `dwmmc2` at **0x132E0000** (0x2000), SPI interrupt **0x139** |
| Clocks | "ciu" and "ciu_gate" (CMU ids 0x2d5 / 0x2c1); CIU source 800 MHz (`clock-frequency`), `dw-mshc-ciu-div` 3 |
| Bus | 4-bit, high-speed, SDR50, SDR104; cmd23, erase; FIFO depth 64; IDMAC descriptors (`desc-size` 4), dma-coherent (sysreg offset 0x700 mask 0x30000) |
| Timings (sdr/ddr/sdr50/sdr104) | `<3 0 2 0>`, `<3 0 2 1>`, `<3 0 4 2>`, `<3 0 3 0>` (Samsung's CLKSEL fields) |
| Pins | GPF1-0 clk (func 2, drv 4), GPF1-1 cmd (func 2, pull-up, drv 3), GPF1-2 data0, GPF1-3..5 data1-3 |
| Card detect | **GPA1-5**, external interrupt, no pull; card present = level per `sec-sd-slot-type` 2; debounce 200 ms |
| Card power (vmmc) | S2MPS19 main PMIC **LDO15**, 2.95 V |
| Signal power (vqmmc) | S2MPS19 main PMIC **LDO2**, 2.95 V (default speed / high speed) or 1.8 V (UHS SDR50/104) |
| Timeouts | data 200 ms, card-detect delay 200 ms, HTO 80 |
| PMIC registers (S2MPS19, type 1 PMIC, our redwood_acpm_pmic.c) | LDO15 CTRL **0x54**, LDO2 CTRL **0x47**; group 2: 1.8 V + 25 mV a step, CTRL = 0xC0 (on) \| vsel: **2.95 V = 0xEE**, 1.8 V = 0xC0. Samsung also has `ext_en,ldo2,use_tf_sel` (LDO2 switched by the TF pin): check |
| Pin blocks | GPF1 in **pinctrl@13040000** (HSI1), GPA1 (card detect) in **pinctrl@15850000** (ALIVE) |
| Clock / power domain | block **HSI1**: gate `GOUT_BLK_HSI1_UID_MMC_CARD_IPCLKPORT`, mux `MUX_CLKCMU_HSI1_MMC_CARD` (cal-if exynos9830 cmucal-node.*); HSI1's power domain may need to be on |

## Plan

1. **Probe (safe)**: from Log access (no iOS running), read DWMMC2's VERID (0x6C), HCON (0x70), CDETECT (0x50)
   with the loader's peek; if the block reads 0 or hangs, its clock gate / power domain must be opened first
   (CMU HSI? or PERIC). Never peek it from iOS before knowing it is clocked: a read of a gated IP can stop the bus.
2. **Power and pins**: LDO15 on (2.95 V), LDO2 at 2.95 V through our ACPM PMIC access; GPF1-0..5 to function 2 with the
   pulls above; GPA1-5 as input for card detect.
3. **Controller bring-up at default speed**: reset (CTRL), clock 400 kHz for identification (CLKDIV from the CIU
   clock), CMD0, CMD8, ACMD41 (HCS), CMD2, CMD3, CMD9 (CSD: size), CMD7, ACMD6 (4-bit), then 25 MHz. PIO first,
   IDMAC after. Log CID/CSD and read sector 0 (MBR) as the first proof.
4. **Block device**: an IOBlockStorageDevice subclass -> IOMedia (Removable, Ejectable, Whole), card-detect
   interrupt for insert/remove. Check that diskarbitrationd mounts it and Files shows it.
5. **Speed**: high speed 50 MHz, then UHS (switch vqmmc to 1.8 V, CMD11, tuning for SDR104) with Samsung's timings.

Boot argument for every step (project rule): `sd=0` off (default until it works), `sd=1` probe/log, `sd=2` block device.
