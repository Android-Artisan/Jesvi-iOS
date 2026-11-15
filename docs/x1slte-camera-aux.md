# S20 auxiliary rear cameras: offline bring-up inventory

Branch `feat/s20-rear-aux`. Neither auxiliary camera is enabled or capture-tested.
The original front-camera path remains the baseline. Main rear quality remains
unresolved; this inventory is not a noise/tint fix.

## Reproduction

Run `python3 tools/camera/aux-inventory.py BASE.dtb OVERLAY0.dtb OVERLAY1.dtb`.
The checked-in JSON records SHA-256 identities and selected hardware properties
from the locally cached phone trees. No DTB, photo, calibration or firmware is
included. External overlay references are resolved through base `__symbols__`;
unresolved references stay symbolic. Cell arrays retain source order, including
power-sequence parameters. Numbered power-step names must be ordered numerically
when implementing sequencing; JSON object order is not execution order.

`python3 tools/camera/test_aux_inventory.py` covers external bus resolution,
missing-symbol preservation and invalid fixup offsets. Both cached overlays give
identical selected camera nodes; separate checks establish the addresses below.

## Wiring that must not be copied from the main/front camera

| Property | 2LA | GW2 |
|---|---|---|
| CIS seven-bit address | 0x2d | 0x2d |
| I2C controller | hsi2c_45, 0x15d70000 | hsi2c_1, 0x10560000 |
| Framework sensor instance | 4 | 2 |
| CSIS base | 0x17070000 | 0x17050000 |
| Logical MCLK | 3, gpc0-3 | 0, gpc0-0 |
| Reset | gpm33-0 | gpm32-0 |
| Declared active size | 4032×3024 | 9248×6936 |

GW2 module-node suffix `@5A` is not its CIS bus address. Linux sensor enums53/54
are not hardware model IDs. Both channel-mode nodes declare WDMA0 at0x170c1000
and a mux register at0x17020414; initial bring-up must be exclusive. The saved
GW2 controller GPIO mapping includes gpc0-3, also used for 2LA MCLK. Restore pin
ownership deliberately on sensor changes; do not activate both at once.

The source reference is ExtremeXT/android_kernel_samsung_exynos990 under
`drivers/media/platform/exynos/camera/sensor/module_framework/`: module_dt and
cis `is-cis-2la*` / `is-cis-gw2*`. Samsung's 2LA initialization branches on
revision A202: earlier revisions include Global_1, later revisions skip it;
initial_setting and Global_2 still apply. GW2 has separate TNP and Global tables.
Neither is a plain replacement of a front-camera setfile. Generic driver
aperture defaults do not establish this handset's optics. Physical lens roles,
FOV, model/revision and calibration must be verified before publishing metadata.

## Next hardware gates

1. Integrate latest accepted main/master into this branch before kernel/image
   builds. Do not flash an older candidate over another agent's accepted work.
2. Under exclusive phone ownership, implement bounded power/reset/MCLK control
   with rollback of shared rails and pins. Read model/revision before choosing
   revision-specific initialization. Do not invent expected IDs from enum values.
3. Add one sensor at a time with the actual controller/CSIS/WDMA route. Validate
   a bounded RAW capture, dimensions, packing, CFA and embedded/PDAF separation.
4. Validate calibrated preview and front/main/aux switching before exposing a
   Camera lens choice. Stock resolution/FPS tables are not verified iOS modes.

No auxiliary power writes, ID probes, streaming, kernel install or reboot were
performed for this checkpoint. Main-rear RAW format6 versus packed format7 and
native12MP readout remain separate pending experiments.

## 2026-11-15: bounded 2LA identity kernel prepared

Integrated `feat/s20-camera-quality` and accepted master8eaa52cf, including AOD
changes, in this auxiliary branch. No main/master pointers were moved.

Added explicit root debug command `cam.s20.2la.id` (no arguments). It reserves
idle camera ownership; refuses an open camera, active session, warm main sensor
or earlier failed cleanup. Device opens return EBUSY during the transaction.
The warm-main cooldown and sensor startup gaps are covered by the reservation.
Wait at least20seconds with Camera closed before a probe; busy is not a failed
sensor identity. It runs only when explicitly invoked, never during boot.

The probe sets reset low, applies the fixed rail transaction, takes split-bank
I2C pins, raises reset with the DT delay, enables CIS_CLK3 at26MHz on gpc0-3,
waits2ms and reads model0000/revision0002 at seven-bit address0x2d. It does not
scan addresses, write setfiles, start streaming, access CSI/PHY/DMA or change
front-camera tuning. Read identity is reported without assuming enum53 is an ID.
GPIO wire direction is based on the existing Samsung bus-pair convention;
a successful live identity read is still required to validate that assignment.

Power transaction (stock S2MPB02 formulas and phone DT):

| Rail | Register | Requested value |
|---|---|---|
| BUCK2 | 0x0d enable, 0x0e selector | Require existing selector0x90=1.3V; preserve lower control bits |
| LDO12 analog | 0x29 | selector0x58=2.8V, enable bit7 |
| LDO3 core | 0x20 | selector0x24=1.05V, enable bit7 |
| LDO8 IO | 0x25 | selector0x30=1.8V, enable bit7 |

Snapshot all control bytes before any writes. Reject any already-enabled dedicated
rail, or an unexpected shared buck voltage. The read-only live baseline showed
core selector0x20=1.0V, analog0x58 and IO0x30: merely toggling enable bits would
leave the core below the DT requirement. This is a bring-up finding for2LA,
not evidence explaining main-rear grain. Nothing was written to the live rails.

Cleanup holds reset low, stops/restores MCLK, restores I2C pin configuration,
restores all rail bytes in reverse order and restores reset/MCLK pin state.
Readback verifies writable clock fields and pin functions/pulls; input DAT and
clock status bits are not mistaken for persistent output latches. A PMIC NACK
may occur after a write latched, so the transaction marks the register before
writing it. Restore continues after individual errors. Cleanup failure poisons
auxiliary ownership and refuses later camera opens, rather than continuing with
unknown power state. Recovery then requires investigation and an approved reboot.

Validation: four inventory tests; ASan/UBSan fault injection of every rail
transaction operation (including latched-write NACKs, live-rail and shared-voltage
guards, cleanup error reporting); passcode build integration; full GICv2 kernel
build and incremental rebuild pass. Changed tracked and build-overlay files match.
These tests do not verify physical sensor identity or camera-open arbitration on
the device. The expanded inventory includes controller overlay properties and
supply voltages. Local pinctrl phandle numbers differ between saved overlays
(33/34 and36/37); these are references, not physical addresses. Previously
compared module/CIS wiring and supply voltages agree.

Private uninstalled candidate:
`/Users/jesvi/ios-port-research/build/camera-aux-20261115/`.
Stripped kernel15196056bytes, SHA256
`bc8e5120c206d404dba1b865a0fbe0e62197057960c62e60188de0d5493dfa54`.
Padded16MiB slot SHA256
`14f70dab3b2d63d0bf09e22b138f2d03c4cd7d9fd15609918262a5957da19700`.
Back up the CURRENT slot after acquiring exclusive ownership before deployment;
old rollback slots predate AOD. No flash or reboot yet. Explicit reboot approval
is required. GW2 still has no identity/stream implementation; its stock TNP and
shared OIS sequence must be handled separately after the first sensor is verified.
