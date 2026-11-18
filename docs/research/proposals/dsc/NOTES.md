# DSC / DECON0 mres config — 1440x3200 <-> 1080x2400 (S6E3HAB hubble1, Exynos 990)

Sources are under `~/ios-port-research/git/ExtremeXT__android_kernel_samsung_exynos990/drivers/video/fbdev/exynos/`.
In this file `R` = `dpu30/cal_9830/decon_reg.c`, `H` = `dpu30/cal_9830/regs-decon.h`,
`P` = `panel/s6e3hab/s6e3hab_hubble1_a3_s0_panel.h`.

Files here:
- `redwood_dsc.h` / `redwood_dsc.c`: kernel-safe C (no floats, no libc).
- `test.c`: host test. Build with `clang -Wall -Wextra -std=c99 -pedantic -O2 -o test test.c redwood_dsc.c && ./test`.
- `test_output.txt`: output from the last run (**PASS**).

## Result

`redwood_dsc_pps()` matches Samsung's panel PPS byte for byte, all 128 bytes, in all three modes:
- 1440x3200: `hubble1_a3_s0_pps_table[0][S6E3HAB_SCALER_OFF]`, P:874
- 1080x2400: `[S6E3HAB_SCALER_x1_78]`, P:893
- 720x1600: P:912

The enum values `S6E3HAB_SCALER_OFF=0` and `x1_78=1` come from `panel/s6e3hab/s6e3hab.h:1320`.
The computed `width_per_enc` is 240 and 180, which matches the dtbo `enc_sw`.

## Formulas

The configuration is fixed: 8 bpc, 8.0 bpp, DSC 1.1, linebuf 9, block_pred and convert_rgb on. The constants are at R:759-767:
- rc_model_size = 8192
- initial_xmit_delay = 512
- initial_offset = 6144
- first_line_bpg_offset = 12
- initial_scale_value = 32 (Samsung notes this holds for slice_w >= 70)
- num_extra_mux_bits starts at 246

Variable values:

| quantity | formula | source | 1440 | 1080 |
|---|---|---|---|---|
| dual_slice_en | (cnt==1 && slices==2) or (cnt==2 && slices==4) | R:627 | 0 | 0 |
| slice_mode_change | cnt==2 && slices==2 | R:645 | 1 | 1 |
| enc pic_width (width_eff) | xres/2 if 2 encoders (DSCC), else xres | R:785-789 | 720 | 540 |
| slice_width | width_eff, halved if dual_slice | R:791-795 | 720 | 540 |
| chunk_size | slice_width (8 bpp) | R:801 | 720 | 540 |
| num_extra_mux_bits | 246, decremented until (8·chunk·slice_h − n) % 48 == 0 | R:802-805 | 240 | 240 |
| groups_per_line | (slice_w+2)/3 | R:807 | 240 | 180 |
| final_offset | 8192 − ((512·128+8)>>4) + nemb | R:810 | 4336 | 4336 |
| final_scale | 8·8192/(8192−final_offset) | R:812 | 16 | 16 |
| nfl_bpg_offset | ceil(12·2048/(slice_h−1)) | R:814-818 | 631 | 631 |
| slice_bpg_offset | ceil(2048·(8192−6144+nemb)/(gpl·slice_h)) | R:820-825 | 489 | 651 |
| scale_increment_interval | 2048·final_offset / ((final_scale−9)·(nfl+slice_bpg)) | R:827 | 1132 | 989 |
| scale_decrement_interval | gpl/(32−8) | R:829 | 10 | 7 |
| initial_dec_delay (panel PPS) | ceil((8192−6144+512·8+gpl·12)/8) − 512 (VESA reference; Samsung's calc does not compute this) | — | 616 | 526 |
| initial_dec_delay (encoder reg) | hard-coded 0x1B4, because `VESA_SCR_V4` is never defined | R:917-923 | 436 | 436 |
| width_per_enc | slice_w/3, rounded up to even (forced even if slice_w%3), ×2 if dual_slice | R:831-874 | 240 | 180 |
| DSC_CONTROL3 | remainder = slice_w%3, or 3 when that is 0; grpcntline = (slice_w+2)/3 | R:715-736 | 0x30F0 | 0x30B4 |

PPS bytes 36-87 are the same in both modes (VESA 8bpc/8bpp defaults):
- flatness QP 3/12
- rc_edge_factor 6
- quant_incr_limit 11/11
- tgt_offset 3/3
- rc_buf_thresh {896 … 8064}>>6
- 15 rc_range words of the form min_qp<<11 | max_qp<<6 | bpg&0x3f

Bytes 88-127 are 0.

The panel PPS and the encoder registers differ in two places, and Samsung ships both differences:
- **pic_width:** the panel PPS carries the full width (1440/1080). Each encoder register gets half of it (720/540, R:785), because each of the two encoders compresses one slice column.
- **initial_dec_delay:** the panel PPS has 616/526. The encoder register always gets 0x1B4, which is the 720x1600 value. This is a decoder-only parameter, so the mismatch is harmless.

Samsung writes only PPS04-35, PPS58-59 and PPS76-87 into the encoder (R:899-952). The other encoder PPS words are never written, but in these modes they equal the reset/UEFI values (they are the mode-independent constants above).

## Register map

All offsets are relative to DECON0 at 0x19050000. The DSC encoders are not separate blocks: `dsc_write()` adds `DSC0_OFFSET` 0x4000 or `DSC1_OFFSET` 0x5000 to DECON0's `res.regs` (dpu30/decon.h:1532, H:466). Absolute addresses are DSC0 = 0x19054000 and DSC1 = 0x19055000. A read-only shadow copy of the active values is at +0x7000: DECON 0x7000.., DSC0 0xB000.., DSC1 0xC000.. (H:66-80, `SHADOW_OFFSET` H:666). Read the shadow on the phone to confirm what is latched.

Inside each DSC block, PPS byte n is at 0x20 + (n & ~3), big-endian within the word (H:525-645).

Write list in Samsung's order (`decon_reg_set_mres`, R:1955). `*` marks values that change between modes:

| # | off | register | mask | 1440x3200 | 1080x2400 |
|---|---|---|---|---|---|
| 0 | 0x0200 | BLENDER_BG_IMAGE_SIZE_0 (H<<16\|W) | full | 0x0C8005A0 | 0x09600438 * |
| 1 | 0x00A0 | SCALED_SIZE_CONTROL_0 (H<<16\|W) | full | 0x0C8005A0 | 0x09600438 * |
| 2 | 0x4000 | DSC0.CONTROL0 (byte-swap, flat_th=2, slice_mode_ch=1, dual=0) | full | 0x00000228 | 0x00000228 |
| 3 | 0x400C | DSC0.CONTROL3 | full | 0x000030F0 | 0x000030B4 * |
| 4 | 0x4024 | DSC0.PPS04_07 comp_cfg/bpp/pic_h | full | 0x30800C80 | 0x30800960 * |
| 5 | 0x4028 | DSC0.PPS08_11 enc_pic_w/slice_h | full | 0x02D00028 | 0x021C0028 * |
| 6 | 0x402C | DSC0.PPS12_15 slice_w/chunk | full | 0x02D002D0 | 0x021C021C * |
| 7 | 0x4030 | DSC0.PPS16_19 xmit/dec delay | full | 0x020001B4 | 0x020001B4 |
| 8 | 0x4034 | DSC0.PPS20_23 init_scale/scale_inc | full | 0x0020046C | 0x002003DD * |
| 9 | 0x4038 | DSC0.PPS24_27 scale_dec/fl_bpg | full | 0x000A000C | 0x0007000C * |
| 10 | 0x403C | DSC0.PPS28_31 nfl_bpg/slice_bpg | full | 0x027701E9 | 0x0277028B * |
| 11 | 0x4040 | DSC0.PPS32_35 init/final offset | full | 0x180010F0 | 0x180010F0 |
| 12 | 0x4058 | DSC0.PPS56_59 rc_range0 | 0x0000FFFF | 0x00000102 | 0x00000102 |
| 13-15 | 0x406C/70/74 | DSC0.PPS76-87 | full | 0x1AB62AF6 / 0x2B342B74 / 0x3B746BF4 | same |
| 16-29 | 0x5000.. | DSC1: same 14 writes, same values | | | |
| 30 | 0x0120 | OUTFIFO_SIZE_CONTROL_0 (yres<<16\|width_per_enc) | full | 0x0C8000F0 | 0x096000B4 * |
| 31 | 0x012C | OUTFIFO_TH_CONTROL_0 = 5 (1H) | 0x7 | 0x5 | 0x5 |
| 32 | 0x0124 | OUTFIFO_SIZE_CONTROL_1 (width_per_enc) | full | 0x000000F0 | 0x000000B4 * |
| 33 | 0x0128 | OUTFIFO_SIZE_CONTROL_2 (slice_h<<16\|enc_sw) | full | 0x002800F0 | 0x002800B4 * |

Sources for the rows: R:1270 (#0), R:362 (#1), R:715/899 (#2-29), R:405-430 and R:210-241 (#30-33).

Changing modes changes 22 of the 34 writes: 2 sizes, 7 per encoder (CONTROL3 plus 6 PPS words), and 3 OUTFIFO sizes.

Notes:
- `DSC_CONTROL0` is a full write (R:725). It clears the other bits: CG_EN bit1, DCG bits 19:17 and SW_RESET. Samsung does the same.
- There is no SW reset of the DSC on the mres path (`dsc_reg_init(..., swrst=0)`, R:1972).
- `DATA_PATH_CONTROL_2` (DSCC+ENC0+ENC1 → OUTFIFO0/1 → DSIMIF0) is not touched by mres. It was set at init and stays valid.

## Write order in Samsung's mres path (command mode, HW TE trigger)

`decon_update_regs()` in dpu30/decon_core.c:
1. `dpu_update_mres_lcd_info()` (decon_core.c:3146 → win_update.c:620/551-558) copies the new xres/yres/dsc slice_h/enc_sw/dec_sw/fps into lcd_info, then runs the BTS calc.
2. `__decon_update_regs()` (decon_core.c:2490):
   1. `decon_reg_wait_update_done_and_mask()` (decon_core.c:2504, R:1892) polls `SHADOW_REG_UPDATE_REQ` (0x0060) until it reads 0. This means the previous frame's shadow has been latched. It then **masks the HW trigger**: `HW_SW_TRIG_CONTROL` 0x0070, HW_TRIG_EN | HW_TRIG_MASK_DECON (bit4), R:1840.
   2. `dpu_set_mres_config()` (decon_core.c:2521 → win_update.c:735) runs these steps in order:
      1. `decon_reg_wait_idle_status_timeout(50 ms)` (win_update.c:747) polls `GLOBAL_CONTROL` bit5 IDLE_STATUS. This makes sure the previous frame has fully left DECON.
      2. `dpu_set_panel_mres()` (win_update.c:750) sends the **panel display_mode command set over DSI**: `hubble1_a3_s0_osc_*_display_mode_cmdtbl`, P:3004/3053, with CONFIG_SUPPORT_DSU and CONFIG_PANEL_DISPLAY_MODE both =y in extreme_x1s_defconfig. The order is:
         - key enable
         - backlight params
         - wait 1 vsync
         - DSC enable (type COMP, payload 0x01)
         - **PPS** (DSI PPS packet, the 128 bytes)
         - CASET 2A 0000 059F / 0437
         - PASET 2B 0000 0C7F / 095F
         - SCALER BA 01/02 26 08 08 F3 (P:933-955)
         - fps/osc sequence
         - gamma update
         - key disable
      3. `dsim_reg_set_mres()` (win_update.c:753, dpu30/cal_9830/dsim_reg.c:2599) writes:
         - cmd_lp_ref
         - threshold = enc_sw·slice_num (480 → 360)
         - vresol, hresol, porch
         - num_of_transfer = yres
         - DSC enable, num_of_slice, multi_slice, size_of_slice
      4. `decon_reg_set_mres()` (win_update.c:755, R:1955): the table above, in that order.
      5. `dpu_init_win_update()` updates software state only.
   3. `dpu_set_win_update_config()` (decon_core.c:2524) runs only if window update is enabled and a region is pending. For a full-screen region it writes the same BG/OUTFIFO/DSC values again (R:1925).
   4. DPP and window config, then `decon_reg_all_win_shadow_update_req()` sets 0x0060 bits[5:0] (decon_core.c:2588).
   5. `decon_reg_start()` (decon_core.c:2601, R:1716) does direct_on_off, then `update_req_global` (0x0060 bit31), waits for run status, then **unmasks the trigger**. The next TE latches the shadow and sends the first frame at the new size.
3. Back in `decon_update_regs`: wait for vsync and frame start, `decon_reg_wait_update_done_timeout`, then mask the trigger again (decon_core.c:3240-3275).

Variant with `CONFIG_EXYNOS_SET_ACTIVE_WITH_EMPTY_WINDOW` and no windows: `dpu_set_mres_config()` is called directly, followed by `EXYNOS_PANEL_IOC_SET_VREFRESH` (decon_core.c:3191). There is no frame start and no trigger unmask in that branch, so the new config takes effect on the next normal update.

## Preconditions for applying it safely (our port)

1. Command mode with the HW trigger **masked**: set 0x0070 bit4 together with HW_TRIG_EN.
2. `SHADOW_REG_UPDATE_REQ` (0x0060) == 0, meaning no pending shadow update.
3. DECON **idle**: `GLOBAL_CONTROL` (0x0000) bit5 == 1, meaning the frame transfer is finished.
4. Send the panel command set (with the new PPS) while idle, **before** the first frame at the new size. Samsung sends it before writing DSIM and DECON.
5. Write the DSIM registers, then the DECON/DSC list above.
6. Request a global shadow update: 0x0060 bit31, plus bits 5:0 for the windows. Any DPP/window still sized 1440x3200 must be reprogrammed to fit 1080x2400 in the same update, or the blender/DPP sizes will mismatch.
7. Unmask the trigger. Then wait until 0x0060 reads 0 (latched, 300 ms timeout in Samsung's code) before the next reconfiguration.

The register comment at H:86-90 says SHADOW_REG_UPDATE_REQ covers DECON 0x0000-0x011C and 0x0230+. OUTFIFO (0x120-0x12C) and BLENDER_BG (0x200) are outside that literal range. The DSC blocks do have shadows (0xB000/0xC000). Samsung relies on idle plus a masked trigger for everything, so treat all of these writes as "only while idle with the trigger masked".

## 2026-11-07 19:00 (B): fb_mode=2 run, the photo, what it means
- Boot line: `fb_mode=2 fb_mode_lpref=1` (usb_gadget_poll=1 dropped: the line was over 550 with it and the payload step
  refused silently in an earlier try). Log: `fb_mode: 1080x2400 at 60 Hz set (panel commands all sent); DSIM RESOL
  0x9600168 THRESHOLD 360 SLICE01 0x21c021c; 34 DECON registers`; `fb_scaler: on: window 5 reads channel 5`.
- Photo (`fbmode2-garble-20261107.jpg`): NOT iOS at all -- the loader's boot text ("...ing iOS" = "Starting iOS")
  squeezed and tiled three times across. The panel took the 1080x2400 commands (CASET/PASET/scaler) and shows its own
  GRAM with the new geometry; it never received a new frame. Screen on/off changes nothing.
- Counters: `fb: te: 292 copies ... frames sent 0 a second`; `fb_irq: 0 ended by its frame-start interrupt`; DECON
  global 0x133 trig 0x1001 te 0x5a205a2. => after the mres writes DECON never starts a frame (no TE-triggered
  frame start), i.e. the fault is the trigger/TE side of DECON (or DSIM refusing the stream), not DSC or the scaler.
- Next: compare DECON_TRIG_CONTROL / TE registers and DSIM_FIFOCTRL/INTSRC after the switch with Samsung's
  decon_reg_set_mres + dsim_reg_set_mres order (decon_reg_update_req, decon_reg_set_trigger(DECON_TRIG_ENABLE) after
  the shadow update?); dump DSIM INTSRC (underrun/overflow) and DECON INT pending right after the switch. Test
  fb_mode=2 again only with a dump of those registers in the log (no photo needed: "frames sent" says it).
