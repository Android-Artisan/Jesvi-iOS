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
