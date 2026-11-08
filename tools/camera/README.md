# Galaxy S20 camera bring-up tools

The S20 is Exynos 990. Keep its camera diagnostics separate from `cam=` and
`bsd/dev/redwood_camera.c`, which target the Qualcomm POCO X5 Pro. Priority is
**rear main S5K2LD, then front SAK3J1**; secondary rear modules follow.

## Read-only snapshot

Run from the repository root:

```sh
python3 tools/camera/s20-diag.py
```

Requires the existing `ssh s20` alias. It records source revision and build
artifact timestamps, kernel boot arguments and device-tree camera nodes,
camera device nodes, Camera.app bundle, MobileGestalt camera capabilities,
SpringBoard icon reference, recent media logs, and the HackiOS kernel log. The
complete kernel log is saved beside the report. The report is timestamped under
`logs/camera/`. It does not reboot, respring, or otherwise change phone state.
Use `--out PATH` to choose a different report path.

## Hardware gates

Each camera change should produce a report and pass these gates in order:

1. **Boot/recovery baseline:** record boot ID, source revision, kernel artifact
   hash and phone logs. Keep a known-good recovery image available.
2. **Inventory (`cam_s20=1`):** read pin mux/pull/drive/input, MCLK and bus
   clocks, PMIC identity/rail states, CSIS/PHY version and status. Write nothing.
3. **Sensor identification (`cam_s20=2 sensor=main|front`):** after exact bus
   and pin mappings are confirmed, sequence rails/reset/MCLK, read ID/revision,
   then restore the exact initial state. Log every phase and rollback result.
4. **CSI pattern (`cam_s20=3 sensor=main|front`):** validate CSI/WDMA/DMA
   independently of sensor operation; save a frame and report packet/CRC/ECC,
   DMA completion and timeout counters.
5. **Single frame (`cam_s20=4 sensor=main|front`):** capture one frame, stop,
   and confirm rails, pins and clocks return to the recorded baseline. Decode
   with `tools/camraw.py` only when framing matches its documented format.
6. **Application preview:** after repeatable single frames, connect the iOS
   capture bridge and make Camera.app discoverable on the Home Screen. Verify
   main and front independently before lens switching or sustained streaming.

Failures must identify a bounded phase: controller timeout, bus NACK, bad ID,
rail/clock mismatch, CSI packet error, DMA timeout, or userspace delivery stall.
A camera failure must never reboot automatically. Save logs and frame artifacts
before any explicit reboot or flash.

## Pin/I/O trace format

Emit stable JSON events with boot ID, module, stage, phase, timestamp,
resource/register, prior value, requested value, readback, result and error.
Use pin names from the x1slte DT and register block/base/offset, not unlabeled
numbers. On failure, report the first failing event and every reverse-order
cleanup result. A bounded flight recorder should retain recent events across a
userspace service crash. Do not expose arbitrary raw-register writes.

Use existing `tools/phone-lock.sh`, `x1slte.sh`, and `tools/x1slte-auto.sh`
for reboot and recovery workflows. Camera diagnostics never acquire the phone
or initiate a flash/reboot.
