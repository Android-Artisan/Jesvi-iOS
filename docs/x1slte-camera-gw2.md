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

The board uses CSIS2 C-PHY3, PHY170d1b00, WDMA3/VC0170c4000 and mux17020420=2.
A 5x+ application route will require sensor capture, calibration/exposure, focus,
measured field-of-view alignment and cropped high-resolution data. S20 hardware
is specified as3x hybrid; do not advertise5x optical or30x quality without tests.

Live counter test passed:2432x1824,11changes, verified stop/cleanup. GW2 must
read width/height using separate16-bit reads; combined4-byte read was invalid.
Controls read back correctly: frame3608/line17680, exposure3000/gain64. Receiver
initialization refused before DMA arm, cleanup passed. Auditing stock mux code
found mux_reg=[address,byte_length], not[address,value]: write CSI channel2,
not length4. Added exact failed-MMIO report and corrected route in next build.
Front/main/UW capture regression36frames each passed after the refused probe.

RAW2432x1824 now PASSED: no CSI/DMA errors, guards intact, exact controls,
shutdown/restoration verified. Frame was exported and a real scene was visible.
WDMA3 address slots17..32 read0xdeadc0de; configure only16 implemented slots.
VC1 carries608x904 RAW10 PDAF data and needs valid CSI dimensions even when not
DMA-captured; configuring it removed WRONG_CFG. Full crop4864x3648 size readback
passed but control gate caught a timing conversion error:0x142e is5166 lines,
not5294. Corrected to5166/12304 (1918MHz pixel clock) before stream test.

Next candidate adds mode6 continuous4864x3648, distinctive sensor4 metadata,
finite exposure limits, frame-geometry guard, and GW2 AK737x0x0f on HSI2C0, per
stock DT (primary remains0x0c). Native selector test passed on actual CameraUI
for0.5/1/3 buttons and10x drag with no graph zoom forwarding; front delegate
restoration still passed. GW2 backend/UI are opt-in by redwood-gw2-enable.
3x is a provisional test entry, not a claimed stock switching threshold.
Samsung public documentation confirms3x hybrid/up to30x digital only:
https://www.samsung.com/uk/support/mobile-devices/check-out-the-new-camera-functions-of-the-galaxy-s20-plus-s20-ultra/
This prototype caps the control at16x and uses stock central crop, nominal2x
base pending field-of-view calibration. It does not implement5x optical zoom.
