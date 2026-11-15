# GW2 third-rear bring-up

Isolated branch feat/s20-gw2-telephoto from accepted f03fd343. Preserve front,
primary and ultra-wide behavior and permanent root. Existing session camera-test
reboot authorization continues. Stable full recovery/boot rollback remains in
workspace build/camera-final/deploy. No new Camera lens until RAW is verified.

Hardware GW2 ID0972/A300. First diagnostic loads stock setA TNP firmware/global,
single-sync and2432x1824@30 mode; verifies output dimensions before stream-on;
checks12 frame-counter samples; verifies stream-off and restores both PMIC maps,
GPIO and MCLK. No receiver or autofocus/OIS writes. No EEPROM calibration is
invented: diagnostic uses stock uncalibrated-module PDXTC-disable fallback.
Image tuning/calibration remain unqualified. Firmware comes from Samsung's GPL2
Exynos990 kernel table, with source hash recorded in generated header. Checked
executor validates before writing, chunks indirect firmware transfers128words,
and fails immediately on any I2C error. Host test checks exact firmware byte order
and every transfer failure, plus invalid table refusal.

The board uses CSIS2 C-PHY3, PHY170d1b00, WDMA3/VC0170c4000 and mux17020420=4.
A 5x+ application route will require sensor capture, calibration/exposure, focus,
measured field-of-view alignment and cropped high-resolution data. S20 hardware
is specified as3x hybrid; do not advertise5x optical or30x quality without tests.
