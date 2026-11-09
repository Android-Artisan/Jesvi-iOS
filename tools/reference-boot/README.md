# Temporary Samsung reference boot

The microphone was brought up by briefly booting the phone's saved Samsung
kernel with a small custom initramfs, capturing the hardware state while its
driver ran, then restoring the HackiOS BOOT image. The reusable artifacts are
in `work/stock-audio-test/`: `boot-mic-ref.img`, `build-mic-reference.py`,
`microot/init`, `capture.sh`, and the saved reference logs. This does not need
Android's system partition; the current `super` partition contains iOS.

## Repeatable procedure

1. Record hashes of the active HackiOS BOOT head and recovery image. Keep
   `out/x1slte-boot.img` (Samsung DT table magic `d7b7ab1e`) and
   `out/x1slte-recovery.img` available for restoration. The original phone
   partitions are backed up under `device/backup/20261105-203534/`.
2. Repack the saved Samsung kernel and DTB with a task-specific initramfs.
   The image builder must update the ramdisk size and Samsung boot header SHA1.
   The initramfs mounts proc, sysfs, devtmpfs and debugfs, supplies only the
   needed firmware and static utilities, and never exits.
3. The initramfs writes bounded logs to the dedicated super tail window used
   by the mic run, `super+0x1e9b04000`, after checking the partition size.
   Do not touch the iOS APFS window at `super+0` or its log at
   `super+0x1e8b04000`. A USB ACM gadget can stream the same log to the Mac.
4. From Download Mode, flash the temporary image to **BOOT only** with
   Heimdall. Let the reference kernel run its bounded capture, then have its
   initramfs reboot to Download Mode. Retrieve the log and restore the
   previously saved HackiOS BOOT image; verify the write and boot iOS.

For this phone, build the camera image with `python3
tools/reference-boot/build-camera-reference.py` after compiling
`camera-ref.c` as a static AArch64 binary at
`work/stock-camera-test/camera-ref`. The output is
`work/stock-camera-test/boot-camera-ref.img`. The known HackiOS restore image
is `out/x1slte-boot.img`; its recovery payload is already in the RECOVERY
partition. From Download Mode, flash with `heimdall flash --BOOT IMAGE
--stdout-errors`. The reference init writes its log at `super+0x1e9b04000`
(4096-byte block 2005764). Once iOS is back, retrieve it with
`ssh s20 'dd if=/dev/rufs_super bs=4096 skip=2005764 count=256
2>/dev/null' > reference-log.raw`. The log is separate from the loader's
`ulog` window. The current init also writes a rolling camera helper and
kernel breadcrumb at block 2006020 (512 KiB); retrieve it with the same
command using `skip=2006020 count=128`. If the reference kernel stalls or loops at the boot logo,
manually enter Download Mode and restore `out/x1slte-boot.img`; do not wait
for the initramfs watchdog indefinitely.

The mic run initially bootlooped when its test init exited. The initramfs must
remain alive until it explicitly reboots to Download Mode. The reference
kernel's debugfs regmap may show cached values; use `cache_bypass` when the
task needs real hardware registers, and limit direct MMIO reads to registers
verified safe for the active power domain.

## Camera adaptation

The earlier reference log confirms Samsung's kernel registered
`is_sensor1`, front SAK3J1, and CSIS-related devices during early boot.
It did not start a camera stream. The camera reference init now selects front
position 1 on `/dev/video102`, sets 1824×1368 RAW10, and calls Samsung's
`V4L2_CID_IS_S_SENSOR_SIZE` and `VIDIOC_S_PARM` for 30 fps, sets 1824×1368
RAW10, then calls `V4L2_CID_IS_S_STREAM` for five seconds. `VIDIOC_STREAMON` on the
buffer queue alone is insufficient: the sensor driver starts the CIS and CSIS
from this private control. The run captures CSIS clocks, interrupts and
Samsung camera logs while the sensor is active. To make the comparison more
decisive, capture CSIS1 D-PHY status,
`FRM_CNT_CH0` (`0x0500` on CSIS v5.3), packet interrupts, sensor registers,
clock gates and PMU state while streaming. The stock firmware files
`is_lib.bin` and `setfile_3j1.bin` are already extracted under
`device/firmware/vendor-firmware/`. Confirm the V4L2 request sequence and
firmware requirements before making the temporary image.

The first camera run on 2026-11-09 proved the restore and log path, but the
helper opened `/dev/video2` and failed with `ENOENT`. Samsung adds
`EXYNOS_VIDEONODE_FIMC_IS=100` to the internal video ID; sensor1 is therefore
`/dev/video102`, and the run's device list confirms that node exists. No
camera stream was started in that first run.

The second run on 2026-11-09 opened `/dev/video102`, selected the front
camera and read valid SAK3J1 EEPROM calibration. `VIDIOC_S_FMT` then failed:
Samsung logged `sensor mode(0x0@30fps) is not found`. The driver takes sensor
dimensions from `V4L2_CID_IS_S_SENSOR_SIZE`, not the V4L2 format structure.
The third helper version supplies that private control before `S_FMT`.

The third run accepted sensor selection and size, but `VIDIOC_S_PARM` failed
because Samsung's sensor driver insists on
`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` for the frame rate ioctl, despite the
node's output capabilities. The fourth helper uses that type.

The fourth BOOT flash uploaded successfully, but the phone did not return to
Download Mode within 95 seconds and then cycled through boot logo and black
screen. The user manually entered Download Mode; HackiOS BOOT was restored and
iOS SSH returned. The protected log has only early camera registration, ending
at `cis_3j1_probe done`; no helper result or stream result was flushed. This
run cannot be counted as evidence of a camera frame. A separate rolling
breadcrumb log was added for the next run to preserve progress across stalls.

The fifth run also bootlooped and required manual Download Mode entry;
HackiOS BOOT was restored and iOS SSH returned. Its main log again ends after
early `cis_3j1_probe done`, before a helper result. The new breadcrumb was
written, but its unfiltered dmesg content filled the 512 KiB window before
the helper started, so it did not identify the failing ioctl. The logger now
filters for camera, sensor, CSIS and panic messages before writing that
window. Do not infer that Samsung streamed a frame from these runs.

## GPS and cellular adaptation

The same boot and restore mechanism can capture kernel driver state for GPS
and modem work. Each run needs its own firmware, userspace trigger and
read-only telemetry. Early driver registration alone does not prove that GPS
has a fix or that the modem has registered on a network. Record what actually
ran and the hardware state it produced before copying settings into iOS.

For each new subsystem, keep the same BOOT image builder, super log offset,
size guard, independent reboot watchdog and HackiOS restore image. Replace
only the static test helper, its firmware files and init script's bounded
telemetry. For GPS, record GNSS power, clocks, firmware load, serial or IPC
traffic and a real fix attempt. For cellular, record modem power and reset,
firmware load, IPC handshake, SIM state and network registration. Save each
run's image SHA256, raw log, exact trigger sequence and restore result; a
probe message or an open device node alone is not a functional pass.
