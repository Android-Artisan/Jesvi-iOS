/*
 * The S20+'s (y2s, Exynos 990) loudspeakers: what iOS plays (/dev/redwood-audio, redwood_audio.c) through the
 * SoC's audio block (ABOX) to the two Cirrus Logic CS35L41 amplifiers. Boot-arg snd=1.
 *
 * Android drives ABOX through its DSP (the "Calliope" Cortex-A32, which Samsung's firmware runs): the AP's driver
 * sends it the stream's buffer by IPC and the firmware programs the DMA. Here the DSP stays in reset and the AP
 * programs the hardware itself, with the same registers Samsung's kernel names (sound/soc/samsung/abox:
 * abox_soc_3.h, abox_if.c, abox_cmpnt.c, abox_dma.c, which writes these DMA registers itself for its dump DMA):
 *
 *   power       pd-aud: PMU AUD_CONFIGURATION 0x1900 bit 0, AUD_STATUS 0x1904 (flexpmu_cal_local_exynos9830.h
 *               aud_on), then the DTZPC restore of exynos-pd for need_smc 0x18e10204, then the audio pads out of
 *               retention (PMU 0x1920 bit 11, abox.c ABOX_PAD_NORMAL)
 *   clocks      CMU_AUD 0x18c00000: PLL_AUD0 1179.648 MHz (M 181 P 4 S 0 K 31740, pll_aud0_rate_table);
 *               AUDIF /16 = 73.728 MHz (abox.c: 384000 * 96 * 2 for uaif_max_div 512); UAIF2's bit clock
 *               AUDIF /24 = 3.072 MHz = 48 kHz * 2 slots * 32 bits; the bus /6
 *   SysMMU      0x18e50000 (ABOX's): left off, so that the DMA's addresses are physical (logged)
 *   the path    RDMA3 (16-bit stereo from our buffer) -> SPUS chain 3 (no ASRC) -> SIFS0, the mixer (32-bit
 *               stereo) -> UAIF2 (ROUTE_CTRL0) -> pins gpb1-0..3 (aud-i2s2-bus): DSP_A, NB_NF, ABOX the master,
 *               2 slots of 32 bits. Each DMA and the mixer need a slice of ABOX's staging SRAM (SBANK), which
 *               the firmware hands out: given here (RDMA3 at 0, the mixer at 512, 512 bytes each)
 *   amplifiers  two CS35L41 on a bit-banged I2C bus (gpp4-2 SCL, gpp4-3 SDA): 0x41 the earpiece/top speaker
 *               (left, slot 0, reset gpb2-4), 0x40 the bottom speaker (right, slot 1, reset gpb2-5). Set up as
 *               Cirrus's driver does (cs35l41.c: cs35l41_init, otp_unpack, apply_pdata for this phone's device
 *               tree, set_dai_fmt, hw_params, set_sysclk from SCLK, main_amp_event) with the PCM straight to the
 *               DAC (ASPRX1 -> DAC_PCM1_SRC): the amplifiers' own DSP (Cirrus's speaker protection) is not loaded;
 *               the samples go through redwood_guard.c instead, and the amplifiers are kept under the chip's
 *               full level (snd_below, eighths of a dB; default 18 dB) with iOS's volume on top, in their
 *               digital volume
 *
 * One thread does everything after the bring-up: keeps the DMA buffer filled ahead of where the DMA reads (from
 * what iOS wrote, else silence, through the guard), powers the amplifiers up when there is sound and down after
 * three seconds without, follows iOS's volume, and looks at the amplifiers' fault bits twice a second (a fault:
 * both off for the rest of the run).
 *
 * Boot-args: snd=1, snd_below=N (amplifiers N/8 dB under the chip's full level), snd_gain=N (the guard's gain,
 * eighths of a dB, at most 12 dB), snd_tone=MS (a 1 kHz tone at -20 dBFS for MS ms once the amplifiers are up: a
 * self-test without iOS).
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_bbi2c.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <kern/kalloc.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_line(const char *line);
extern int redwood_wifi_dma_pool(uint64_t *phys, vm_offset_t *virt, size_t *size);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);   /* (redwood_image.c) */

/* ---- addresses ------------------------------------------------------------------------------------------------ */

#define PMU_PHYS                0x15860000U
#define PMU_AUD_CONFIGURATION   0x1900U
#define PMU_AUD_STATUS          0x1904U
#define PMU_ABOX_PAD            0x1920U
#define PMU_ABOX_PAD_NORMAL     (1U << 11)
#define AUD_NEED_SMC            0x18e10204U
#define SMC_PREPARE_PD_ONOFF    0x82000410U

#define CMU_AUD_PHYS            0x18c00000U
#define PLL_LOCKTIME_AUD0       0x0000U
#define PLL_CON0_AUD0           0x0100U         /* bit 4: PLL (1) or OSC; bit 7 busy */
#define PLL_CON3_AUD0           0x010cU         /* ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0 */
#define PLL_CON5_AUD0           0x0114U         /* K 15:0 */
#define MUX_UAIF2               0x1014U
#define DIV_AUDIF               0x1800U
#define MUX_AUD_CNT             0x1000U         /* SELECT bit 0: 0 DIV_CLK_AUD_AUDIF, 1 PLL_AUD1 (exynos9830 cmucal) */
#define DIV_AUD_CNT             0x180cU         /* DIVRATIO 8:0: the CNT clock, which paces the SIFS (abox "cnt") */
#define DIV_BUS                 0x1804U
#define DIV_BUSP                0x1808U
#define DIV_SCLK                0x1828U
#define DIV_UAIF2               0x1834U
#define CLK_BUSY                (1U << 16)
/* gates (cmucal-sfr.c, exynos9830): MANUAL 20 -> CG_VAL 21 decides; else ENABLE_AUTOMATIC_CLKGATING 28 (1: gated when idle) */
#define GATE_CG_VAL             (1U << 21)
#define GATE_MANUAL             (1U << 20)
#define GATE_AUTO               (1U << 28)
/* Q-channels: ENABLE 0 (1: the IP's handshake may stop the clock); with it 0, CLOCK_REQ 1 keeps the clock asked for */
#define QCH_ENABLE              (1U << 0)
#define QCH_CLOCK_REQ           (1U << 1)

#define SYSMMU_PHYS             0x18e50000U
#define MMU_CTRL                0x0000U
#define MMU_CFG                 0x0004U
#define MMU_STATUS              0x0008U
#define MMU_VERSION             0x0034U
#define MMU_CTRL_VM             0x8000U

#define ABOX_PHYS               0x18c50000U
#define ABOX_IP_INDEX           0x0000U
#define ABOX_VERSION            0x0004U
#define ABOX_SYSPOWER_CTRL      0x0010U
#define ABOX_SYSPOWER_STATUS    0x0014U
#define ABOX_DYN_CLOCK_OFF      0x0030U
#define ABOX_QCHANNEL_DISABLE   0x0038U         /* one bit per clock (abox.h enum qchannel): 1 ACLK, 2 + n BCLK_UAIFn */
#define QCH_ABOX_ACLK           1
#define QCH_ABOX_BCLK_UAIF(n)   (2 + (n))
#define ABOX_ROUTE_CTRL0        0x0040U         /* UAIFx's speaker source, 4 bits each: SIFSn + 1 */
#define ABOX_SPUS_CTRL_FC0      0x0200U         /* chains 0..3, 8 bits each: IN 6:5 (0 RDMA), OUT 4:1 (1 SIFS0), ASRC 0 */
#define ABOX_SPUS_CTRL1         0x0204U         /* MIXP_FORMAT 4:0 */
#define ABOX_SPUS_SBANK_RDMA(x) (0x0220U + 4 * (x))     /* SIZE 29:20, STR 13:4 */
#define ABOX_SPUS_SBANK_MIXP    0x0280U
#define ABOX_SIFS_CNT0          0x0290U
#define UAIF_ID                 2
#define ABOX_UAIF_CTRL0         (0x0500U + 0x10 * UAIF_ID)     /* MODE 2 (1 master), SPK_ENABLE 0 */
#define ABOX_UAIF_CTRL1         (0x0504U + 0x10 * UAIF_ID)
#define ABOX_UAIF_STATUS        (0x050cU + 0x10 * UAIF_ID)
static uint32_t snd_rdma = 3;           /* RDMA3 for the bypass; Calliope's firmware has a PCM task for RDMA0 only (run 6) */
#define RDMA_ID                 (snd_rdma)
#define RDMA(off)               (0x1000U + 0x100 * RDMA_ID + (off))
#define RDMA_CTRL0              0x00U           /* BURST_LEN 22:19, FUNC 18:16, FORMAT 12:8, ENABLE 0 */
#define RDMA_BUF_STR            0x08U
#define RDMA_BUF_END            0x0cU
#define RDMA_BUF_OFFSET         0x10U
#define RDMA_STR_POINT          0x14U
#define RDMA_VOL_FACTOR         0x18U
#define RDMA_STATUS             0x30U           /* PROGRESS 31, RBUF_OFFSET 27:12 (16 bytes), RBUF_CNT 11:0 */

#define ABOX_FORMAT(width, channels) (((channels) - 1) | (((width) == 16 ? 1U : (width) == 24 ? 2U : 3U) << 3))

#define PINCTRL_PERIC1_GPB1     0x100U
#define PINCTRL_PERIC1_GPB2     0x120U
#define PINCTRL_PERIC0_GPP4     0x080U

/* ---- CS35L41 (cs35l41-registers.h) --------------------------------------------------------------------------- */

#define L41_DEVID               0x00000000U
#define L41_REVID               0x00000004U
#define L41_OTPID               0x00000010U
#define L41_TEST_KEY_CTL        0x00000040U
#define L41_OTP_MEM0            0x00000400U
#define L41_PWR_CTRL1           0x00002014U
#define L41_PWR_CTRL2           0x00002018U
#define L41_PLL_CLK_CTRL        0x00002c04U
#define L41_GLOBAL_CLK_CTRL     0x00002c0cU
#define L41_BSTCVRT_PEAK_CUR    0x00003808U
#define L41_SP_ENABLES          0x00004800U
#define L41_SP_RATE_CTRL        0x00004804U
#define L41_SP_FORMAT           0x00004808U
#define L41_SP_HIZ_CTRL         0x0000480cU
#define L41_SP_FRAME_RX_SLOT    0x00004820U
#define L41_SP_RX_WL            0x00004840U
#define L41_DAC_PCM1_SRC        0x00004c00U
#define L41_AMP_DIG_VOL_CTRL    0x00006000U     /* volume 13:3, 1/8 dB two's complement; 0x400 mute */
#define L41_AMP_GAIN_CTRL       0x00006c04U
#define L41_IRQ1_STATUS1        0x00010010U
#define L41_IRQ1_STATUS4        0x0001001cU
#define L41_IRQ1_MASK1          0x00010110U

#define L41_CHIP_ID             0x35a40U
#define L41R_CHIP_ID            0x35b40U
#define L41_OTP_BOOT_DONE       0x02U
#define L41_PUP_DONE            0x01000000U
#define L41_PDN_DONE            0x00800000U
#define L41_FAULTS              0x800281c0U     /* amp short, temperature, boost short / DCM UVP / OVP */
#define L41_VOL_MUTE            0x400U
#define L41_SCLK_3072000        0x21U           /* cs35l41_pll_sysclk */
#define L41_FS_48000            0x03U
#define L41_BST_IPK_4100MA      (((4100 - 1600) / 50) + 0x10)   /* cirrus,boost-peak-milliamp of the phone's tree */

/* cs35l41-tables.c otp_map_2 (OTP IDs 2 and 3); ID 1 differs only in element 93, which it skips */
static const struct { uint32_t reg; uint8_t shift, size; } otp_map[99] = {
	{0x02030,0,4}, {0x02030,7,1}, {0x0208c,24,6}, {0x02090,14,4}, {0x02090,10,4}, {0x0300c,11,4}, {0x0394c,23,2}, {0x03950,0,7},
	{0x03950,8,7}, {0x03950,16,8}, {0x03950,24,8}, {0x03954,0,7}, {0x03954,8,7}, {0x03954,16,8}, {0x03954,24,8}, {0x03958,0,7},
	{0x03958,8,7}, {0x03958,16,8}, {0x03958,24,8}, {0x0395c,0,7}, {0x0395c,8,7}, {0x0395c,16,8}, {0x0395c,24,8}, {0x0416c,0,8},
	{0x04160,0,7}, {0x0416c,8,8}, {0x04160,16,10}, {0x0416c,16,12}, {0x0416c,28,1}, {0x04170,0,6}, {0x04170,6,1}, {0x04170,8,6},
	{0x04170,14,1}, {0x04170,16,9}, {0x04360,0,5}, {0x04360,6,9}, {0x04448,0,8}, {0x04448,8,8}, {0x04448,16,8}, {0x04448,24,8},
	{0x0444c,0,3}, {0x06e30,0,5}, {0x06e30,8,5}, {0x06e30,16,5}, {0x06e30,24,5}, {0x06e34,0,5}, {0x06e34,8,5}, {0x06e34,16,5},
	{0x06e34,24,5}, {0x06e38,0,5}, {0x06e38,8,5}, {0x06e38,16,5}, {0x06e38,24,5}, {0x06e3c,0,5}, {0x06e3c,8,5}, {0x06e3c,16,5},
	{0x06e3c,24,5}, {0x06e40,0,5}, {0x06e40,8,5}, {0x06e40,16,5}, {0x06e40,24,5}, {0x06e44,0,5}, {0x06e48,0,10}, {0x06e48,10,10},
	{0x06e48,20,10}, {0x06e4c,0,10}, {0x06e4c,10,10}, {0x06e4c,20,10}, {0x06e50,0,10}, {0x06e50,10,10}, {0x06e50,20,10}, {0x06e54,0,10},
	{0x06e54,10,10}, {0x06e54,20,10}, {0x06e58,0,10}, {0x06e58,10,10}, {0x06e58,20,10}, {0x06e5c,0,10}, {0x06e5c,10,10}, {0x06e5c,20,10},
	{0x06e60,0,10}, {0x06e60,10,10}, {0x06e60,20,10}, {0x06e64,0,10}, {0x07418,7,5}, {0x0741c,0,5}, {0x0741c,11,4}, {0x0741c,19,4},
	{0x07434,17,1}, {0x07434,18,7}, {0x07068,0,9}, {0x0410c,7,1}, {0x0400c,0,7}, {0x04000,11,1}, {0x17040,0,8}, {0x17040,8,8},
	{0x17040,16,8}, {0x17040,24,8}, {0x17044,0,24},
};
#define OTP_ID1_SKIP            93

/* ---- state ---------------------------------------------------------------------------------------------------- */

#define BUF_BYTES               32768U          /* the DMA's ring: 8192 frames, 171 ms */
#define BUF_FRAMES              (BUF_BYTES / 4)
#define PERIOD_BYTES            4096U
#define LEAD_FRAMES             2880U           /* written this far ahead of the DMA: 60 ms */
#define CHUNK                   1024U           /* the guard's most at once */
#define RING_FRAMES             16384U          /* iOS's samples waiting: 341 ms; a power of two */
#define SILENCE                 8
#define QUIET_OFF_US            3000000ull

static volatile uint8_t *pmu, *cmu, *mmu, *abox;
static volatile uint8_t *gpb1, *gpb2;
static uint64_t buf_phys;
static int16_t *buf;
static bool started, amps_ok, amps_on, faulted, use_status;
static uint32_t snd_below = 144, snd_gain = 12 * 8, snd_tone;
static uint32_t snd_calliope;           /* 1: boot Calliope only; 2: and play through it (stage 3) */

struct amp {
	const char *name;
	uint8_t addr;
	uint32_t reset_pin;
	uint32_t slot;
	bool present;
};
static struct amp amps[2] = {
	{ "left (top)", 0x41, 4, 0, false },
	{ "right (bottom)", 0x40, 5, 1, false },
};
static struct redwood_bbi2c amp_bus = { .name = "amp bus gpp4-2/3", .scl = 2, .sda = 3, .half_us = 3 };

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;
static volatile uint64_t last_sound_us;
/*
 * Quiet: the amplifiers off, no tone, no sound from iOS for QUIET_OFF_US (it keeps sending silence): the DMA's
 * whole ring is silence (it goes on reading it round and round) and the thread sleeps until iOS sends sound
 * (exynos_sink), a second at most, the silence meanwhile dropped -- instead
 * of feeding silence every 4 ms (some 1000 wake-ups a second on the big cores: the cpuidle wake record,
 * 27 October).
 */
static volatile bool snd_quiet;
static uint64_t quiet_spells, quiet_us;
static uint64_t ios_frames, fed_frames, silent_frames, underruns;

static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249, 3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638,
	1254, 848, 428, 0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249, -3277, -3249, -3165, -3028,
	-2838, -2600, -2317, -1995, -1639, -1254, -848, -428,
};

static void snd_log(const char *fmt, ...) __printflike(1, 2);
static void
snd_log(const char *fmt, ...)
{
	char line[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}

static inline uint32_t
rd(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(volatile uint8_t *base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The speaker path's clocks through CMU_AUD's gates and Q-channels, opened as Samsung's cal-if does it
 * (ra.c: ra_set_gate pass, ra_req_enable_qch). Without the DSP's driver nothing asks for them, and an
 * automatically gated BCLK_UAIF2 is a UAIF that never pulls a sample: RDMA3 stayed "in progress" with 0
 * frames read and UAIF2's status 0 (PLANS/08-audio.md). Each register is said before and after.
 */
static void
snd_clocks_on(void)
{
	static const struct { uint32_t off; const char *name; } gates[] = {
		{ 0x2010, "ABOX ACLK" }, { 0x2018, "ABOX BCLK_CNT" }, { 0x2028, "ABOX BCLK_UAIF2" },
		{ 0x2048, "ABOX SCLK" }, { 0x2058, "LHM_AXI_P" }, { 0x205c, "LHS_AXI_D" },
		{ 0x2070, "RSTNSYNC BUSP" }, { 0x2074, "RSTNSYNC BUS" }, { 0x2078, "RSTNSYNC CNT" },
		{ 0x2094, "RSTNSYNC SCLK" }, { 0x20a0, "RSTNSYNC UAIF2" },
	};
	static const struct { uint32_t off; const char *name; } qchs[] = {
		{ 0x301c, "QCH ACLK" }, { 0x3028, "QCH BCLK2" }, { 0x3044, "QCH CNT" }, { 0x3048, "QCH SCLK" },
	};
	char line[600];
	size_t at = 0;

	at += snprintf(line + at, sizeof(line) - at, "snd: CMU_AUD gates before -> after:");
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t was = rd(cmu, gates[i].off), now = was;

		if (was & GATE_MANUAL) {
			now |= GATE_CG_VAL;
		} else {
			now &= ~GATE_AUTO;
		}
		if (now != was) {
			wr(cmu, gates[i].off, now);
		}
		at += snprintf(line + at, sizeof(line) - at, " %s %#x->%#x;", gates[i].name, was, rd(cmu, gates[i].off));
	}
	snd_log("%s\n", line);
	at = 0;
	at += snprintf(line + at, sizeof(line) - at, "snd: CMU_AUD Q-channels before -> after:");
	for (unsigned i = 0; i < sizeof(qchs) / sizeof(qchs[0]); i++) {
		uint32_t was = rd(cmu, qchs[i].off);

		wr(cmu, qchs[i].off, (was & ~QCH_ENABLE) | QCH_CLOCK_REQ);
		at += snprintf(line + at, sizeof(line) - at, " %s %#x->%#x;", qchs[i].name, was, rd(cmu, qchs[i].off));
	}
	snd_log("%s\n", line);
}

static inline void
upd(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t value)
{
	wr(base, off, (rd(base, off) & ~mask) | (value & mask));
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static bool
wait_bits(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i < us; i += 10) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return (rd(base, off) & mask) == want;
}

static uint64_t
smc4(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * Samsung's SFR access through EL3 (include/linux/smc.h: SMC_CMD_REG -101, SFR_W class 1 << 30, SFR_R 3 << 30, the
 * address >> 2; arch/arm64/kernel/exynos-smc.S). ABOX's GIC on the Exynos 990 past rev 0 is secure (abox_util.c
 * is_secure_gic): abox_gic.c writes its distributor and CPU interface this way; plain writes are dropped.
 */
#define SMC_CMD_REG             0xFFFFFFFFFFFFFF9BULL
static uint64_t
sfr_smc_write(uint32_t addr, uint32_t value)
{
	return smc4(SMC_CMD_REG, (1U << 30) | (addr >> 2), value, 0);
}

static bool
sfr_smc_read(uint32_t addr, uint32_t *value)
{
	register uint64_t x0 __asm__("x0") = SMC_CMD_REG & 0xffffffffULL;
	register uint64_t x1 __asm__("x1") = (3U << 30) | (addr >> 2);
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	*value = (uint32_t)x2;
	return x0 == 0;
}

/* ---- the amplifiers ------------------------------------------------------------------------------------------- */

static bool
amp_write(struct amp *a, uint32_t reg, uint32_t value)
{
	uint8_t b[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		         (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };

	return redwood_bbi2c_xfer(&amp_bus, a->addr, b, sizeof(b), NULL, 0);
}

static bool
amp_read(struct amp *a, uint32_t reg, uint32_t *value)
{
	uint8_t r[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg }, v[4] = { 0 };
	bool ok = redwood_bbi2c_xfer(&amp_bus, a->addr, r, sizeof(r), v, sizeof(v));

	*value = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
	return ok;
}

static bool
amp_update(struct amp *a, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t old;

	return amp_read(a, reg, &old) && amp_write(a, reg, (old & ~mask) | (value & mask));
}

static bool
amp_unlock(struct amp *a, bool unlock)
{
	return unlock ? amp_write(a, L41_TEST_KEY_CTL, 0x5555) && amp_write(a, L41_TEST_KEY_CTL, 0xaaaa) :
	       amp_write(a, L41_TEST_KEY_CTL, 0xcccc) && amp_write(a, L41_TEST_KEY_CTL, 0x3333);
}

/* cs35l41_otp_unpack: the chip's trims, from its OTP into the registers they belong in */
static bool
amp_otp_unpack(struct amp *a)
{
	uint32_t mem[32], id = 0, bit = 16, word = 2;

	if (!amp_unlock(a, true) || !amp_read(a, L41_OTPID, &id)) {
		return false;
	}
	for (unsigned i = 0; i < 32; i++) {
		if (!amp_read(a, L41_OTP_MEM0 + 4 * i, &mem[i])) {
			return false;
		}
	}
	id &= 0xf;
	if ((mem[1] & 0xfffffffe) != 0x1500 || (mem[2] & 0xffff) != 0x300 || id < 1 || id > 3) {
		snd_log("snd: amp %s: OTP header %#x %#x, id %u: not unpacked\n", a->name, mem[1], mem[2], id);
		amp_unlock(a, false);
		return false;
	}
	for (unsigned i = 0; i < 99; i++) {
		uint32_t size = otp_map[i].size, value;

		if (bit + size - 1 >= 32) {
			value = (mem[word] >> bit) & ((1U << (32 - bit)) - 1);
			word++;
			value |= (mem[word] & ((1U << (bit + size - 32)) - 1)) << (32 - bit);
			bit += size - 32;
		} else {
			value = (mem[word] >> bit) & (size == 32 ? ~0U : (1U << size) - 1);
			bit += size;
		}
		if (bit == 32) {
			bit = 0;
			word++;
		}
		if (otp_map[i].reg != 0 && !(id == 1 && i == OTP_ID1_SKIP)) {
			uint32_t mask = (size == 32 ? ~0U : (1U << size) - 1) << otp_map[i].shift;

			if (!amp_update(a, otp_map[i].reg, mask, value << otp_map[i].shift)) {
				amp_unlock(a, false);
				return false;
			}
		}
	}
	return amp_unlock(a, false);
}

static const char *
amp_bad(struct amp *a, const char *what)
{
	snd_log("snd: amp %s (%#x): %s\n", a->name, a->addr, what);
	a->present = false;
	return NULL;
}

/* reset, identified, trimmed and set up for our stream; powered down (GLOBAL_EN 0) and muted */
static bool
amp_prepare(struct amp *a)
{
	uint32_t devid = 0, revid = 0, status = 0, fmt = 0, slot = 0, vol = 0, gain = 0, pwr2 = 0, pll = 0;
	unsigned i;

	if (!amp_read(a, L41_DEVID, &devid) || !amp_read(a, L41_REVID, &revid)) {
		amp_bad(a, "does not answer");
		return false;
	}
	if (devid != ((revid & 0xf) % 2 ? L41R_CHIP_ID : L41_CHIP_ID)) {
		snd_log("snd: amp %s: device ID %#x, revision %#x: not a CS35L41\n", a->name, devid, revid);
		a->present = false;
		return false;
	}
	amp_write(a, L41_IRQ1_MASK1, 0x7ffcfe3f);
	if (revid == 0xa0) {
		amp_unlock(a, true);
		amp_write(a, 0x3854, 0x05180240);
		amp_write(a, 0x7418, 0x9091a1c8);
		amp_write(a, 0x3014, 0x0200ee0e);
		amp_write(a, 0x381c, 0x00000051);
		amp_write(a, 0x0054, 0x00000004);
		amp_write(a, 0x10318, 0);
		amp_write(a, 0x10b18, 0);
		amp_unlock(a, false);
	}
	for (i = 0; i < 100; i++) {
		if (amp_read(a, L41_IRQ1_STATUS4, &status) && (status & L41_OTP_BOOT_DONE)) {
			break;
		}
		IOSleep(1);
	}
	if (!(status & L41_OTP_BOOT_DONE)) {
		amp_bad(a, "OTP_BOOT_DONE never came");
		return false;
	}
	if (!amp_otp_unpack(a)) {
		amp_bad(a, "the OTP trims could not be unpacked");
		return false;
	}
	/* the phone's tree (apply_pdata): ASP SDOUT hi-z when unused, the boost's peak current */
	amp_update(a, L41_SP_HIZ_CTRL, 0x3, 0x3);
	amp_update(a, L41_BSTCVRT_PEAK_CUR, 0x7f, L41_BST_IPK_4100MA);
	/* set_dai_fmt DSP_A | NB_NF | CBS_CFS: slave, ASP_FMT 0, no inversions; hw_params: 48 kHz, slots of 32, 24 bits */
	amp_update(a, L41_SP_FORMAT, 0xff000000 | 0x0700 | 0x40 | 0x10 | 0x04 | 0x01, 32U << 24);
	amp_update(a, L41_SP_RX_WL, 0x3f, 24);
	amp_update(a, L41_SP_FRAME_RX_SLOT, 0x3f3f, a->slot | (a->slot ^ 1) << 8);
	amp_update(a, L41_GLOBAL_CLK_CTRL, 0x1f, L41_FS_48000);
	/* set_sysclk from SCLK at 3.072 MHz */
	amp_update(a, L41_SP_RATE_CTRL, 0x3f, L41_SCLK_3072000);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0800, 0x0800);
	amp_update(a, L41_PLL_CLK_CTRL, 0x07e0, L41_SCLK_3072000 << 5);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0010, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0007, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0800, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0010, 0x0010);
	/* the PCM to the DAC, muted; ASPRX1 on */
	amp_write(a, L41_DAC_PCM1_SRC, 0x08);
	amp_update(a, L41_AMP_DIG_VOL_CTRL, 0x7ff << 3, L41_VOL_MUTE << 3);
	amp_update(a, L41_SP_ENABLES, 1U << 16, 1U << 16);

	amp_read(a, L41_SP_FORMAT, &fmt);
	amp_read(a, L41_SP_FRAME_RX_SLOT, &slot);
	amp_read(a, L41_AMP_DIG_VOL_CTRL, &vol);
	amp_read(a, L41_AMP_GAIN_CTRL, &gain);
	amp_read(a, L41_PWR_CTRL2, &pwr2);
	amp_read(a, L41_PLL_CLK_CTRL, &pll);
	snd_log("snd: amp %s (%#x): CS35L41 %#x rev %#x, trimmed; SP_FORMAT %#x RX_SLOT %#x PLL %#x VOL %#x GAIN %#x PWR2 %#x\n",
	    a->name, a->addr, devid, revid, fmt, slot, pll, vol, gain, pwr2);
	a->present = true;
	return true;
}

static uint32_t amps_volume_reg = L41_VOL_MUTE;        /* as amp_prepare() leaves them */

static void
amps_volume(uint32_t below, bool muted)
{
	uint32_t reg = (muted || below >= 816) ? L41_VOL_MUTE : (uint32_t)(-(int32_t)below) & 0x7ff;

	if (reg == amps_volume_reg) {
		return;
	}
	amps_volume_reg = reg;
	for (unsigned i = 0; i < 2; i++) {
		if (amps[i].present) {
			amp_update(&amps[i], L41_AMP_DIG_VOL_CTRL, 0x7ff << 3, reg << 3);
		}
	}
}

/* main_amp_event POST_PMU: the power-up patch, GLOBAL_EN, PUP_DONE */
static bool
amps_up(void)
{
	bool ok = true;

	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t status = 0;
		unsigned t;

		if (!a->present) {
			continue;
		}
		amp_write(a, L41_TEST_KEY_CTL, 0x55);
		amp_write(a, L41_TEST_KEY_CTL, 0xaa);
		amp_write(a, 0x2084, 0x002f1aa0);
		amp_write(a, L41_TEST_KEY_CTL, 0xcc);
		amp_write(a, L41_TEST_KEY_CTL, 0x33);
		amp_update(a, L41_PWR_CTRL1, 1, 1);
		for (t = 0; t < 20; t++) {
			if (amp_read(a, L41_IRQ1_STATUS1, &status) && (status & L41_PUP_DONE)) {
				break;
			}
			IOSleep(1);
		}
		amp_write(a, L41_IRQ1_STATUS1, L41_PUP_DONE);
		if (!(status & L41_PUP_DONE) || (status & L41_FAULTS)) {
			snd_log("snd: amp %s: power-up %s (IRQ1_STATUS1 %#x)\n", a->name,
			    (status & L41_FAULTS) ? "FAULT" : "not done", status);
			ok = false;
		}
	}
	return ok;
}

static void
amps_down(void)
{
	amps_volume(0, true);
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t status = 0;

		if (!a->present) {
			continue;
		}
		amp_update(a, L41_PWR_CTRL1, 1, 0);
		for (unsigned t = 0; t < 20; t++) {
			if (amp_read(a, L41_IRQ1_STATUS1, &status) && (status & L41_PDN_DONE)) {
				break;
			}
			IOSleep(1);
		}
		amp_write(a, L41_IRQ1_STATUS1, L41_PDN_DONE);
		amp_write(a, L41_TEST_KEY_CTL, 0x55);
		amp_write(a, L41_TEST_KEY_CTL, 0xaa);
		amp_write(a, 0x2084, 0x002f1aa3);
		amp_write(a, L41_TEST_KEY_CTL, 0xcc);
		amp_write(a, L41_TEST_KEY_CTL, 0x33);
	}
}

static uint32_t
amps_faults(void)
{
	uint32_t faults = 0;

	for (unsigned i = 0; i < 2; i++) {
		uint32_t status = 0;

		if (amps[i].present && amp_read(&amps[i], L41_IRQ1_STATUS1, &status)) {
			faults |= status & L41_FAULTS;
		}
	}
	return faults;
}

static bool
amps_bring_up(void)
{
	char report[200];
	bool any = false;

	gpb2 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB2);
	amp_bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, PINCTRL_PERIC0_GPP4);
	for (unsigned i = 0; i < 2; i++) {
		redwood_gpio_set(gpb2, amps[i].reset_pin, 0);
	}
	IOSleep(2);
	for (unsigned i = 0; i < 2; i++) {
		redwood_gpio_set(gpb2, amps[i].reset_pin, 1);
	}
	IOSleep(3);
	if (!redwood_bbi2c_setup(&amp_bus, true, report, sizeof(report))) {
		snd_log("snd: %s: the bus is not free\n", report);
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		any |= amp_prepare(&amps[i]);
	}
	return any;
}

/* ---- ABOX ----------------------------------------------------------------------------------------------------- */

static bool
aud_power_on(void)
{
	bool was_on = (rd(pmu, PMU_AUD_STATUS) & 1) != 0;
	uint64_t ret;

	if (!was_on) {
		upd(pmu, PMU_AUD_CONFIGURATION, 1, 1);
		if (!wait_bits(pmu, PMU_AUD_STATUS, 1, 1, 10000)) {
			snd_log("snd: pd-aud does not power on (status %#x)\n", rd(pmu, PMU_AUD_STATUS));
			return false;
		}
	}
	ret = smc4(SMC_PREPARE_PD_ONOFF, 1, AUD_NEED_SMC, 2);
	upd(pmu, PMU_ABOX_PAD, PMU_ABOX_PAD_NORMAL, PMU_ABOX_PAD_NORMAL);
	snd_log("snd: pd-aud %s; DTZPC restore answers %#llx; pads %#x\n", was_on ? "was on" : "powered on", ret,
	    rd(pmu, PMU_ABOX_PAD));
	return true;
}

static void
set_div(uint32_t off, uint32_t mask, uint32_t value)
{
	upd(cmu, off, mask, value);
	wait_bits(cmu, off, CLK_BUSY, 0, 1000);
}

static bool
aud_clocks(void)
{
	uint32_t con3;
	uint32_t found_con3 = rd(cmu, PLL_CON3_AUD0);
	uint32_t found_con5 = rd(cmu, PLL_CON5_AUD0);
	uint32_t fp = (found_con3 >> 8) & 0x3fU, fs = found_con3 & 7U;
	uint64_t fm = (found_con3 >> 16) & 0x3ffU, fk = found_con5 & 0xffffU;
	bool locked = (found_con3 & (1U << 29)) != 0;
	/*
	 * The rate, not the fields: S-Boot locks the same 1179.648 MHz with a different decomposition
	 * (M 45 P 1 K 24319 where this driver writes M 181 P 4 K 31740), so comparing M/P/K called a
	 * working PLL "wrong" and reset it.
	 */
	uint64_t found_rate = (fp == 0 || fs > 6) ? 0 :
	    (26000000ULL * ((fm << 16) + fk)) / ((uint64_t)fp << (16 + fs));
	bool ours = locked && found_rate > 1179648000ULL - 118000ULL && found_rate < 1179648000ULL + 118000ULL;

	snd_log("snd: CMU_AUD as found: LOCKTIME %#x CON0 %#x CON1 %#x CON2 %#x CON3 %#x CON4 %#x CON5 %#x (%s%s)\n",
	    rd(cmu, PLL_LOCKTIME_AUD0), rd(cmu, PLL_CON0_AUD0), rd(cmu, 0x104), rd(cmu, 0x108),
	    found_con3, rd(cmu, 0x110), found_con5, locked ? "locked" : "not locked",
	    ours ? ", the rate we want" : "");

	/*
	 * S-Boot plays its own sounds, so it may have left the audio PLL running. Keep it when it is
	 * locked at the rate this driver wants: resetting a working PLL is what failed before.
	 */
	if (locked && ours) {
		snd_log("snd: PLL_AUD0 was already locked at our rate: leaving it alone\n");
	} else {
		/* PLL_AUD0 off its output while it is set (ra_enable_pll / ra_pll_set_pmsk) */
		upd(cmu, PLL_CON0_AUD0, 1U << 4, 0);
		wait_bits(cmu, PLL_CON0_AUD0, 1U << 7, 0, 1000);
		upd(cmu, PLL_CON3_AUD0, 1U << 31, 0);
		wr(cmu, PLL_LOCKTIME_AUD0, 4 * 500);
		upd(cmu, PLL_CON5_AUD0, 0xffff, 31740);
		con3 = rd(cmu, PLL_CON3_AUD0) & ~((0x3ffU << 16) | (0x3fU << 8) | 7U);
		wr(cmu, PLL_CON3_AUD0, con3 | 181U << 16 | 4U << 8 | 0U | 1U << 4 | 1U << 31);
		if (!wait_bits(cmu, PLL_CON3_AUD0, 1U << 29, 1U << 29, 10000)) {
			snd_log("snd: PLL_AUD0 does not lock: CON0 %#x CON1 %#x CON2 %#x CON3 %#x CON4 %#x CON5 %#x\n",
			    rd(cmu, PLL_CON0_AUD0), rd(cmu, 0x104), rd(cmu, 0x108), rd(cmu, PLL_CON3_AUD0),
			    rd(cmu, 0x110), rd(cmu, PLL_CON5_AUD0));
			return false;
		}
		upd(cmu, PLL_CON0_AUD0, 1U << 4, 1U << 4);
		wait_bits(cmu, PLL_CON0_AUD0, 1U << 7, 0, 1000);
	}

	set_div(DIV_BUS, 0x7, 5);               /* 196.6 MHz */
	set_div(DIV_BUSP, 0x3, 2);              /* 65.5 MHz */
	set_div(DIV_AUDIF, 0x1ff, 15);          /* 73.728 MHz */
	set_div(DIV_SCLK, 0x1ff, 0);            /* ABOX's SCLK: 73.728 MHz */
	snd_clocks_on();
	upd(cmu, MUX_UAIF2, 0x3, 0);            /* UAIF2 from AUDIF */
	wait_bits(cmu, MUX_UAIF2, CLK_BUSY, 0, 1000);
	set_div(DIV_UAIF2, 0x1ff, 23);          /* 3.072 MHz */
	snd_log("snd: CMU_AUD: PLL_AUD0 CON0 %#x CON3 %#x CON5 %#x; BUS %#x BUSP %#x AUDIF %#x SCLK %#x UAIF2 mux %#x div %#x\n",
	    rd(cmu, PLL_CON0_AUD0), rd(cmu, PLL_CON3_AUD0), rd(cmu, PLL_CON5_AUD0), rd(cmu, DIV_BUS), rd(cmu, DIV_BUSP),
	    rd(cmu, DIV_AUDIF), rd(cmu, DIV_SCLK), rd(cmu, MUX_UAIF2), rd(cmu, DIV_UAIF2));
	return true;
}

static uint32_t snd_sifs_cnt;           /* snd_sifs_cnt=V: SIFS0's count as given (0: computed as Samsung's driver does) */

/*
 * The SIFS's count (ABOX_SPUS_CTRL_SIFS_CNT, 0x290 + 4n, 23:0): how many CNT-clock cycles per sample period it takes
 * its next sample from the chain. Samsung's driver (abox_cmpnt.c set_cnt_val, sifsx_cnt_val) writes
 *   round(cnt_hz * n / (channels * rate)) - 1,  n = 2 * (32 / physical width)
 * for every stream; left at 0 nothing paces the SIFS, UAIF2 is never fed (STATUS 0) and RDMA3 waits with PROGRESS set
 * and 0 frames read -- what this phone did (2026-11-06). Here SIFS0 (the mixer) runs 32-bit stereo at 48 kHz.
 */
static uint32_t
sifs_count(void)
{
	uint32_t mux = rd(cmu, MUX_AUD_CNT) & 1, div = rd(cmu, DIV_AUD_CNT) & 0x1ff;
	uint32_t audif_div = rd(cmu, DIV_AUDIF) & 0x1ff;
	uint64_t audif_hz = 1179648000ULL / (audif_div + 1), cnt_hz, n = 2 * (32 / 32), d = 2 * 48000ULL, v;

	if (mux != 0) {
		/* PLL_AUD1's rate is not known here: the CNT clock is moved to AUDIF, whose rate is */
		wr(cmu, MUX_AUD_CNT, rd(cmu, MUX_AUD_CNT) & ~1U);
		IOSleep(1);
		mux = rd(cmu, MUX_AUD_CNT) & 1;
	}
	cnt_hz = audif_hz / (div + 1);
	v = (cnt_hz * n + d / 2) / d - 1;
	snd_log("snd: SIFS count: CNT clock mux %u div %u -> %llu Hz (AUDIF %llu Hz); 32-bit stereo 48 kHz: count %llu%s\n",
	    mux, div, cnt_hz, audif_hz, v, snd_sifs_cnt ? " (overridden by snd_sifs_cnt)" : "");
	return snd_sifs_cnt ? snd_sifs_cnt : (uint32_t)(v & 0xffffff);
}

static uint32_t snd_sblimit;            /* snd_sblimit=V: RDMA3 SBANK_LIMIT (+0x20, v3 field 13:4), as calliope sets it */
static uint32_t snd_burst = 4;          /* snd_burst=N: RDMA3 CTRL0 BURST_LEN (22:19); Scion's S9 runs 8 */
static uint32_t snd_bufupd;             /* snd_bufupd=1: RDMA3 CTRL0 BUF_UPDATE (bit 5), as calliope sets it on the S9 */
static uint32_t snd_sifs1;              /* snd_sifs1=1: chain 3 out to SIFS1 and UAIF2 from it, not SIFS0 (the mixer) */
static void
abox_route(void)
{
	uint32_t ctrl0, ctrl1;

	snd_log("snd: ABOX IP %#x version %#x; SysMMU CTRL %#x CTRL_VM %#x CFG %#x STATUS %#x VERSION %#x\n",
	    rd(abox, ABOX_IP_INDEX), rd(abox, ABOX_VERSION), rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM), rd(mmu, MMU_CFG),
	    rd(mmu, MMU_STATUS), rd(mmu, MMU_VERSION));
	/*
	 * Translating when any of bits 0-2 is set: Linux's CTRL_ENABLE is 0x5 and CTRL_BLOCK 0x7, and the boot leaves 0x4
	 * (bit 2 alone: translating) -- read on the S20 (x1slte) and on the S9 (Scion's hakios.exynos_abox.c, step 7). The
	 * old test of bit 0 alone took 0x4 for off: RDMA3's physical ring was then an IOVA nothing mapped, and the DMA
	 * read 0 frames.
	 */
	if (snd_calliope < 2 && ((rd(mmu, MMU_CTRL) & 0x7) || (rd(mmu, MMU_CTRL_VM) & 1))) {
		wr(mmu, MMU_CTRL_VM, 0);
		wr(mmu, MMU_CTRL, 0);
		snd_log("snd: SysMMU was on: off (CTRL %#x CTRL_VM %#x)\n", rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM));
	}

	wr(abox, ABOX_SYSPOWER_CTRL, 1);        /* abox_request_dram_on */
	wait_bits(abox, ABOX_SYSPOWER_STATUS, 1, 1, 10000);

	/* RDMA3 -> chain 3 -> SIFS0 (the mixer, 32-bit stereo) -> UAIF2 */
	/* OUT 4:1 is SIFSn + 1: 1 the mixer (SIFS0), 2 SIFS1 */
	upd(abox, ABOX_SPUS_CTRL_FC0, 0xffU << (8 * RDMA_ID), (snd_sifs1 ? 0x04U : 0x02U) << (8 * RDMA_ID));
	upd(abox, ABOX_SPUS_CTRL1, 0x1f, ABOX_FORMAT(32, 2));
	if (rd(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID)) == 0) {
		wr(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID), (512U << 16) | 0x000);
	}
	if (rd(abox, ABOX_SPUS_SBANK_MIXP) == 0) {
		wr(abox, ABOX_SPUS_SBANK_MIXP, (512U << 16) | 0x200);
	}
	wr(abox, ABOX_SIFS_CNT0, sifs_count());
	if (snd_sifs1) {
		wr(abox, ABOX_SIFS_CNT0 + 4, sifs_count());     /* SIFS1's count */
	}
	upd(abox, ABOX_ROUTE_CTRL0, 0xfU << (4 * UAIF_ID), (snd_sifs1 ? 2U : 1U) << (4 * UAIF_ID));

	/*
	 * UAIF2's bit clock past the ABOX's own Q-channel: Samsung's driver disables it when the interface's BCLK
	 * widget powers up (abox_if.c, abox_if_bclk_event -> abox_disable_qchannel(ABOX_BCLK_UAIF0 + id, 1)), and
	 * with nothing asking for the clock the UAIF never pulls a sample -- RDMA3 "in progress", 0 frames read,
	 * with CMU_AUD's gates and Q-channels all open (run 30). The bus clock (ACLK) as well.
	 */
	{
		uint32_t qch = rd(abox, ABOX_QCHANNEL_DISABLE), dyn = rd(abox, ABOX_DYN_CLOCK_OFF);

		wr(abox, ABOX_QCHANNEL_DISABLE, qch | (1U << QCH_ABOX_BCLK_UAIF(UAIF_ID)) | (1U << QCH_ABOX_ACLK));
		snd_log("snd: ABOX Q-channels disabled %#x -> %#x (BCLK_UAIF%u, ACLK); dynamic clock off %#x\n", qch,
		    rd(abox, ABOX_QCHANNEL_DISABLE), UAIF_ID, dyn);
	}

	/* UAIF2: set_fmt DSP_A | NB_NF | CBS_CFS, hw_params 32 bits x 2 */
	ctrl0 = rd(abox, ABOX_UAIF_CTRL0);
	/*
	 * bit 2 = MODE (1: master, it makes the BCLK), bit 0 = SPK_ENABLE: Samsung sets the latter
	 * from abox_uaif_mute_stream on unmute, and with it clear the mixer has no consumer, so the
	 * RDMA never advances (it sat at 0 with the enable bit set).
	 */
	ctrl0 = (ctrl0 & ~(1U << 2 | 1U)) | 1U << 2 | 1U;
	wr(abox, ABOX_UAIF_CTRL0, ctrl0);
	ctrl1 = ABOX_FORMAT(32, 2) << 24 | 1U << 23 | 1U << 22 | 0U << 21 | (2U - 1) << 18 | 31U << 12 | 31U << 6 | 31U;
	wr(abox, ABOX_UAIF_CTRL1, ctrl1);

	gpb1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB1);
	for (uint32_t pin = 0; pin < 4; pin++) {
		redwood_gpio_pull(gpb1, pin, REDWOOD_GPIO_PULL_NONE);
		redwood_gpio_func(gpb1, pin, 2);
	}

	if (snd_calliope >= 2) {
		snd_log("snd: calliope: routing and UAIF2 set by the AP; RDMA3 is Calliope's (ROUTE0 %#x FC0 %#x UAIF2 %#x %#x)\n",
		    rd(abox, ABOX_ROUTE_CTRL0), rd(abox, ABOX_SPUS_CTRL_FC0), rd(abox, ABOX_UAIF_CTRL0), rd(abox, ABOX_UAIF_CTRL1));
		return;
	}
	/* RDMA3: our ring, 16-bit stereo */
	wr(abox, RDMA(RDMA_CTRL0), ABOX_FORMAT(16, 2) << 8 | (snd_burst & 0xfU) << 19 | (snd_bufupd ? 1U << 5 : 0));
	if (snd_sblimit) {
		wr(abox, RDMA(0x20), snd_sblimit);
	}
	snd_log("snd: x1slte knobs: snd_burst=%u snd_bufupd=%u snd_sifs1=%u snd_sblimit=%#x (reads %#x)\n", snd_burst, snd_bufupd,
	    snd_sifs1, snd_sblimit, rd(abox, RDMA(0x20)));
	wr(abox, RDMA(RDMA_BUF_STR), (uint32_t)buf_phys);
	wr(abox, RDMA(RDMA_BUF_END), (uint32_t)buf_phys + BUF_BYTES);
	wr(abox, RDMA(RDMA_BUF_OFFSET), PERIOD_BYTES);
	wr(abox, RDMA(RDMA_STR_POINT), (uint32_t)buf_phys);
	if ((rd(abox, RDMA(RDMA_VOL_FACTOR)) & 0xffffff) == 0) {
		wr(abox, RDMA(RDMA_VOL_FACTOR), 0x800000);
	}
	/*
	 * Start it: CTRL0 bit 0 is ABOX_RDMA_ENABLE (abox_soc_2.h), and Samsung's PCM trigger
	 * normally sets it from the DSP's mailbox. Written last, so the DMA never runs over a
	 * buffer that is not set up yet.
	 */
	upd(abox, RDMA(RDMA_CTRL0), 1U, 1U);
	snd_log("snd: RDMA3 enabled: CTRL0 %#x STATUS %#x\n", rd(abox, RDMA(RDMA_CTRL0)),
	    rd(abox, RDMA(RDMA_STATUS)));
	snd_log("snd: ABOX: SYSPOWER %#x/%#x ROUTE0 %#x FC0 %#x SPUS1 %#x SBANK %#x/%#x UAIF2 %#x %#x RDMA3 %#x %#x-%#x vol %#x\n",
	    rd(abox, ABOX_SYSPOWER_CTRL), rd(abox, ABOX_SYSPOWER_STATUS), rd(abox, ABOX_ROUTE_CTRL0),
	    rd(abox, ABOX_SPUS_CTRL_FC0), rd(abox, ABOX_SPUS_CTRL1), rd(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID)),
	    rd(abox, ABOX_SPUS_SBANK_MIXP), rd(abox, ABOX_UAIF_CTRL0), rd(abox, ABOX_UAIF_CTRL1), rd(abox, RDMA(RDMA_CTRL0)),
	    rd(abox, RDMA(RDMA_BUF_STR)), rd(abox, RDMA(RDMA_BUF_END)), rd(abox, RDMA(RDMA_VOL_FACTOR)));
}

/* where the DMA reads, in frames into the ring (abox_rdma_pointer) */
static uint32_t
dma_position(uint32_t *raw)
{
	uint32_t st = rd(abox, RDMA(RDMA_STATUS)), bytes = (((st >> 12) & 0xffff) << 4) + (st & 0xfff);

	if (raw != NULL) {
		*raw = st;
	}
	return (bytes % BUF_BYTES) / 4;
}

static void
abox_start(void)
{
	uint32_t raw[8], pos[8];
	uint64_t t0, t[8];

	bzero(buf, BUF_BYTES);
	__asm__ volatile ("dsb sy" ::: "memory");
	upd(abox, ABOX_UAIF_CTRL0, 1, 1);
	upd(abox, RDMA(RDMA_CTRL0), 1, 1);
	t0 = now_us();
	for (unsigned i = 0; i < 8; i++) {
		IOSleep(5);
		pos[i] = dma_position(&raw[i]);
		t[i] = now_us() - t0;
	}
	snd_log("snd: started; RDMA3 STATUS %#x@%llu %#x@%llu %#x@%llu %#x@%llu ... %#x@%llu us (frames %u %u %u %u .. %u); "
	    "UAIF2 STATUS %#x\n", raw[0], t[0], raw[1], t[1], raw[2], t[2], raw[3], t[3], raw[7], t[7], pos[0], pos[1], pos[2],
	    pos[3], pos[7], rd(abox, ABOX_UAIF_STATUS));
}

/* ---- iOS's samples -------------------------------------------------------------------------------------------- */

/* from the writer's thread, under redwood_audio.c's lock: never waits */
static uint32_t
exynos_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t l = frames[2 * i], r = frames[2 * i + 1];

		ring[2 * ((head + i) % RING_FRAMES)] = l;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = r;
		sound |= l > SILENCE || l < -SILENCE || r > SILENCE || r < -SILENCE;
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	if (sound) {
		last_sound_us = now_us();
	}
	if (sound && snd_quiet) {
		thread_wakeup((event_t)&ring_head);     /* the thread is waiting for something to play */
	}
	return taken;
}

static uint32_t tone_left, tone_phase;

/* `n' frames (at most CHUNK) at frame `at' of the DMA's ring: from iOS (or the tone), the rest silence, through the guard */
static void
fill(uint64_t at, uint32_t n)
{
	static int16_t chunk[CHUNK * 2];
	uint32_t tail = ring_tail, have = ring_head - tail, take = have < n ? have : n, i = 0;

	if (tone_left != 0) {
		for (; i < n && tone_left != 0; i++, tone_left--) {
			chunk[2 * i] = chunk[2 * i + 1] = tone[tone_phase];
			tone_phase = (tone_phase + 1) % 48;
		}
		take = 0;
	} else {
		OSMemoryBarrier();
		for (; i < take; i++) {
			chunk[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
			chunk[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
		}
		OSMemoryBarrier();
		ring_tail = tail + take;
	}
	fed_frames += i;
	silent_frames += n - i;
	bzero(chunk + 2 * i, (n - i) * 4);
	redwood_guard_process(chunk, n);
	if (take != 0 && redwood_audio_recording_fed()) {
		redwood_audio_record_fed(chunk, take);
	}
	memcpy(buf + 2 * (at % BUF_FRAMES), chunk, n * 4);
}

/* ---- the thread ----------------------------------------------------------------------------------------------- */

/* ---- Calliope (snd_calliope=1, B 2026-11-06): Samsung's DSP firmware, stage 1 -- load, map, start, see it live ------ */
/*
 * proposals/sound-calliope/NOTES.md. On the S20 Linux never programs an RDMA; Calliope does (abox_rdma.c only sends IPC),
 * and the bypass (above) stays idle on ABOX v3. Samsung's order (abox.c abox_enable): ABOX's GIC (abox_gic_init_gic),
 * the CPU clock, the timer (abox_start_timer), the firmware (abox_core_download_firmware), DRAM on (SYSPOWER), the cores'
 * PMU power then enable. Calliope sees its DRAM at IOVA 0x80000000 through ABOX's SysMMU (abox.c: IOVA_DRAM_FIRMWARE ->
 * abox_rmem, DRAM_FIRMWARE_SIZE 0x2A00000, and 0x10000000 one to one), so the SysMMU is turned ON here with a first-level
 * table of 1 MB sections (exynos-iommu.h: mk_lv1ent_sect(pa) = pa >> 4 | 2; FLPT base by page number, the VM register
 * set when CAPA1 says VCR). Facts: exynos9830.dts abox@18c50000, abox-core@18c55000/@18c55080, abox-gic@18cf0000,
 * abox-rmem 0xF8000000 (free in UEFI's map: f2195000-100000000 type 7, read 2026-11-06).
 */
#define CAL_SRAM_PHYS           0x18d00000U
#define CAL_SRAM_SIZE           0x78c00U
#define CAL_DRAM_PHYS           0xf8000000ULL
#define CAL_DRAM_SIZE           0x2a00000U
#define CAL_IOVA_DRAM           0x80000000U
#define CAL_IOVA_SLOG           0xE0000000U     /* IOVA_SILENT_LOG */
#define CAL_SLOG_PHYS           0xF2200000ULL
#define CAL_SLOG_SIZE           0x2800000U
#define CAL_IOVA_DUMP           0xD0000000U     /* IOVA_DUMP_BUFFER */
#define CAL_DUMP_PHYS           0xFAA00000ULL
#define CAL_DUMP_SIZE           0x5600000U
#define CAL_IOVA_VSS            0xA0000000U     /* IOVA_VSS_FIRMWARE; VSS_PARAMETER +16 MB, VSS_PCI +32 MB, doorbell +48 MB */
#define CAL_IOVA_PCI            0xA2000000U     /* IOVA_VSS_PCI */
#define CAL_PCI_PHYS            0xF4A00000ULL   /* Samsung's abox-pci-rmem is 0xF4200000, under our silent log: moved up */
#define CAL_PCI_SIZE            0x800000U
#define CAL_VSS_SPARE_PHYS      0xF5200000ULL   /* one cleared MB behind every other voice-call section */
static volatile uint8_t *cal_slog;              /* Calliope's own log, mapped for us to read */
#define CAL_GPR_PHYS            0x18c55000U
#define CAL_GICD_PHYS           0x18cf1000U
#define CAL_GICC_PHYS           0x18cf2000U
#define CAL_TIMER_PHYS          0x18ce0000U
#define CAL_IPC_RX_OFF          0x868000U       /* samsung,ipc-rx-area <1 0x868000 0x18000>: DRAM */
#define CAL_IPC_TX_OFF          0x850000U       /* samsung,ipc-tx-area <1 0x850000 0x18000>: DRAM */
#define CAL_IPC_AREA            0x18000U
/* abox_msg.h: struct abox_msg_queue = cmd queue (idx_s, idx_e, 128 cmds of 56 bytes), then data queue (idx_s, idx_e, len, bytes) */
#define CAL_Q_CMD_ELEM          8U
#define CAL_Q_CMD_SIZE          56U
#define CAL_Q_CMD_LEN           128U
#define CAL_Q_DATA              (8U + CAL_Q_CMD_LEN * CAL_Q_CMD_SIZE)
#define CAL_Q_DATA_ELEM         (CAL_Q_DATA + 12U)
#define CAL_HNDSHK_OFF          0x2f2e0U        /* samsung,handshake-area <0 0x2f2e0 0x20>: SRAM */
#define MMU_CAPA0_V7            0x870U
#define MMU_CAPA1_V7            0x874U

/*
 * A firmware file from the system image's /fw once iOS's root is mounted (A, 2026-11-06): Calliope's 3 MB do not fit
 * in the small image beside everything else (its slot is 14.4 MB), so userland/build.sh puts them in the system image
 * (on super), as redwood_adsp.c's root_file does for the Qualcomm DSP. Returns kalloc'd memory (*owned set) or NULL.
 */
static uint8_t *
calliope_root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > 64 * 1024 * 1024) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool
calliope_copy(volatile uint8_t *to, uint32_t room, const char *name, uint32_t offset)
{
	uint64_t size = 0;
	const uint8_t *fw = redwood_image_file("fw", name, &size, NULL);
	uint8_t *owned = NULL;

	if (fw == NULL) {
		fw = owned = calliope_root_file(name, &size);
	}
	if (fw == NULL || size == 0 || offset + size > room) {
		snd_log("snd: calliope: %s: %s\n", name, fw == NULL ? "neither in the small image's fw/ nor in /fw" : "does not fit");
		if (owned != NULL) {
			kfree_data(owned, size);
		}
		return false;
	}
	for (uint64_t i = 0; i + 4 <= size; i += 4) {
		uint32_t w;

		memcpy(&w, fw + i, 4);
		*(volatile uint32_t *)(to + offset + i) = w;
	}
	for (uint64_t i = size & ~3ULL; i < size; i++) {
		to[offset + i] = fw[i];
	}
	snd_log("snd: calliope: %s (%llu bytes) at +%#x%s\n", name, size, offset, owned ? " (from the system image)" : "");
	if (owned != NULL) {
		kfree_data(owned, size);
	}
	return true;
}


/*
 * Stage 2: read Calliope's messages to the AP the way abox_msg_recv does (the AP owns the rx queue's idx_s) and say what
 * each one is (ABOX_IPC_MSG: ipcid, task_id, then for IPC_SYSTEM msgtype, param1..3). BOOT_DONE's param3 is the
 * firmware's version, 4 characters ("Calliope is ready to sing").
 */
static uint32_t cal_msgs, cal_pointers;
static volatile uint8_t *cal_dram, *cal_gicd, *cal_pt, *cal_gpr, *cal_timer;
static uint32_t cal_inv;
static volatile bool cal_booted;
static bool cal_sgi_smc, cal_sgi_tested;
static volatile uint32_t cal_logbuf;   /* ABOX_REPORT_LOG's address (IOVA in the DRAM firmware) */
static volatile uint32_t cal_pointer;   /* the last PCM_PLTDAI_POINTER Calliope sent for RDMA3 */

/* Calliope's own log (struct ABOX_LOG_BUFFER: index_writer, index_reader, size, text), what is new, line by line */
static void
calliope_log(void)
{
	volatile uint8_t *lb;
	uint32_t wi, ri, size;
	char line[160];
	unsigned n = 0, lines = 0;

	if (cal_dram == NULL || cal_logbuf == 0) {
		return;
	}
	lb = cal_dram + (cal_logbuf - 0x80000000U);
	wi = *(volatile uint32_t *)(lb + 0);
	ri = *(volatile uint32_t *)(lb + 4);
	size = *(volatile uint32_t *)(lb + 8);
	if (size == 0 || size > 0x400000 || wi >= size || ri >= size) {
		return;
	}
	while (ri != wi && lines < 60) {
		char c = (char)lb[12 + ri];

		ri = (ri + 1) % size;
		if (c == '\n' || n + 1 >= sizeof(line)) {
			line[n] = 0;
			if (n > 0) {
				snd_log("snd: calliope says: %s\n", line);
				lines++;
			}
			n = 0;
		} else if (c >= 0x20 && c < 0x7f) {
			line[n++] = c;
		}
	}
	*(volatile uint32_t *)(lb + 4) = ri;
}

static void
calliope_drain(volatile uint8_t *dram)
{
	volatile uint8_t *rx = dram + CAL_IPC_RX_OFF;
	uint32_t data_len = *(volatile uint32_t *)(rx + CAL_Q_DATA + 8);

	for (int n = 0; n < 32; n++) {
		int32_t s = *(volatile int32_t *)(rx + 0), e = *(volatile int32_t *)(rx + 4);
		volatile uint8_t *cmd, *d;
		uint32_t idx, size, w[6];

		if (s == e || s < 0 || s >= (int32_t)CAL_Q_CMD_LEN || data_len == 0 || data_len > CAL_IPC_AREA) {
			return;
		}
		cmd = rx + CAL_Q_CMD_ELEM + (uint32_t)s * CAL_Q_CMD_SIZE;
		idx = *(volatile uint32_t *)(cmd + 20);
		if (idx >= data_len) {
			snd_log("snd: calliope: message %u: data index %#x is beyond the queue (%#x): reading stops\n", cal_msgs, idx,
			    data_len);
			*(volatile int32_t *)(rx + 0) = e;
			return;
		}
		d = rx + CAL_Q_DATA_ELEM + idx;
		size = *(volatile uint32_t *)d;
		for (int i = 0; i < 6; i++) {
			w[i] = (size >= 4U * (i + 1)) ? *(volatile uint32_t *)(d + 4 + 4 * i) : 0;
		}
		cal_msgs++;
		if (w[0] == 2 && w[2] == 18) {                 /* IPC_PCMPLAYBACK, PCM_PLTDAI_POINTER: a period played */
			cal_pointers++;
			cal_pointer = w[4];
		} else if (w[0] == 1 && w[2] == 0x10 && w[3] == 0 && w[4] >= 0x80000000U && w[4] < 0x80000000U + CAL_DRAM_SIZE) {
			cal_logbuf = w[4];
			snd_log("snd: calliope: its log buffer at %#x\n", w[4]);
		} else if (w[0] == 1 && w[2] == 3) {
			cal_booted = true;
			snd_log("snd: calliope: Calliope is ready to sing (version %c%c%c%c), message %u\n", (char)(w[5] >> 24),
			    (char)(w[5] >> 16), (char)(w[5] >> 8), (char)w[5], cal_msgs);
		} else if (cal_msgs <= 80) {
			snd_log("snd: calliope: message %u: id %d cmd %d, %u bytes: ipc %u task %d type %#x p %#x %#x %#x\n", cal_msgs,
			    *(volatile int32_t *)cmd, *(volatile int32_t *)(cmd + 4), size, w[0], (int)w[1], w[2], w[3], w[4], w[5]);
		}
		*(volatile int32_t *)(rx + 0) = (s + 1) % (int32_t)CAL_Q_CMD_LEN;
		*(volatile int32_t *)(rx + CAL_Q_DATA + 0) = (int32_t)((idx + size + 4) % data_len);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void
calliope_boot(void)
{
	volatile uint8_t *sram, *dram, *gpr, *gicd, *gicc, *timer, *pt;
	uint64_t pool_phys = 0, pool_size = 0, pt_phys;
	vm_offset_t pool_virt = 0;
	uint32_t capa0, capa1 = 0, flpt, inv, rx0;
	bool vcr = false;
	static const struct { uint32_t off; const char *name; } gates[] = {
		{ 0x2040, "CCLK_CA32" }, { 0x2084, "RSTNSYNC CPU_ACLK" }, { 0x2088, "RSTNSYNC CPU" }, { 0x208c, "RSTNSYNC CPU_PCLKDBG" },
		{ 0x207c, "CPU0 SW_RESET" }, { 0x2080, "CPU1 SW_RESET" },
	};

	sram = (volatile uint8_t *)ml_io_map(CAL_SRAM_PHYS, CAL_SRAM_SIZE);
	dram = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)CAL_DRAM_PHYS, CAL_DRAM_SIZE, VM_WIMG_WCOMB);
	gpr = (volatile uint8_t *)ml_io_map(CAL_GPR_PHYS, 0x1000);
	gicd = (volatile uint8_t *)ml_io_map(CAL_GICD_PHYS, 0x1000);
	gicc = (volatile uint8_t *)ml_io_map(CAL_GICC_PHYS, 0x1000);
	timer = (volatile uint8_t *)ml_io_map(CAL_TIMER_PHYS, 0x1000);
	if (sram == NULL || dram == NULL || gpr == NULL || gicd == NULL || gicc == NULL || timer == NULL ||
	    redwood_wifi_dma_pool(&pool_phys, &pool_virt, &pool_size) < 0 || pool_size < (8U << 20)) {
		snd_log("snd: calliope: cannot map (or no DMA pool for the SysMMU table)\n");
		return;
	}
	/* the CPU's clocks (Samsung: clk_cpu, abox_request_cpu_gear): gates passed, the CPU Q-channel asked for */
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t was = rd(cmu, gates[i].off), now = (was & GATE_MANUAL) ? (was | GATE_CG_VAL) : (was & ~GATE_AUTO);

		if (now != was) {
			wr(cmu, gates[i].off, now);
		}
	}
	wr(cmu, 0x3000, (rd(cmu, 0x3000) & ~QCH_ENABLE) | QCH_CLOCK_REQ);
	snd_log("snd: calliope: CPU clock mux %u (0 PLL divider, 1 CMU_TOP), ACLK div %#x, CA32 gate %#x, QCH CPU %#x\n",
	    rd(cmu, 0x1004) & 1, rd(cmu, 0x1810), rd(cmu, 0x2040), rd(cmu, 0x3000));
	/* ABOX's GIC and timer, as abox_enable */
	wr(gicc, 0x04, 0xff);                   /* GIC_CPU_PRIMASK */
	wr(gicd, 0x000, 0x3);                   /* GIC_DIST_CTRL */
	wr(gicc, 0x000, 0x3);                   /* GIC_CPU_CTRL */
	/* the secure GIC (A, 2026-11-06: stage 3's messages sat unread): the same three writes through EL3, as abox_gic.c */
	{
		uint64_t r1 = sfr_smc_write(CAL_GICC_PHYS + 0x04, 0xff), r2 = sfr_smc_write(CAL_GICD_PHYS + 0x000, 0x3),
		    r3 = sfr_smc_write(CAL_GICC_PHYS + 0x000, 0x3);
		uint32_t dctl = 0xfafafafa, cctl = 0xfafafafa;
		bool ok1 = sfr_smc_read(CAL_GICD_PHYS, &dctl), ok2 = sfr_smc_read(CAL_GICC_PHYS, &cctl);

		snd_log("snd: calliope: GIC by SMC: writes %lld %lld %lld; GICD_CTLR %#x (%s, plain %#x) GICC_CTLR %#x (%s)\n",
		    (long long)r1, (long long)r2, (long long)r3, dctl, ok1 ? "read" : "unreadable", rd(gicd, 0), cctl,
		    ok2 ? "read" : "unreadable");
	}
	wr(timer, 0x600 + 0x20 * 0, 1);         /* ABOX_TIMER_CTRL0(0): START */
	wr(timer, 0x600 + 0x20 * 2, 1);         /* ABOX_TIMER_CTRL0(2) */
	/* the firmware (abox_core_download_firmware): SRAM and DRAM cleared, the four images at the dts offsets */
	for (uint32_t i = 0; i < CAL_SRAM_SIZE; i += 4) {
		*(volatile uint32_t *)(sram + i) = 0;
	}
	for (uint32_t i = 0; i < CAL_DRAM_SIZE; i += 4) {
		*(volatile uint32_t *)(dram + i) = 0;
	}
	if (!calliope_copy(sram, CAL_SRAM_SIZE, "calliope_sram.bin", 0) || !calliope_copy(dram, CAL_DRAM_SIZE, "calliope_dram.bin", 0) ||
	    !calliope_copy(sram, CAL_SRAM_SIZE, "calliope_sram_2.bin", 0x32000) ||
	    !calliope_copy(dram, CAL_DRAM_SIZE, "calliope_dram_2.bin", 0x280000)) {
		return;
	}
	*(volatile uint32_t *)(sram + CAL_HNDSHK_OFF) = 0x656b6177;   /* hndshk_tag->suspend_wait_flag = "wake" */
	__asm__ volatile ("dsb sy" ::: "memory");
	/* the SysMMU on, IOVA 0x80000000 -> abox_rmem: a 16 KB first-level table 64 KB below the sound ring in the pool */
	pt_phys = pool_phys + pool_size - 0x30000;
	pt = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)pt_phys, 0x4000, VM_WIMG_WCOMB);
	if (pt == NULL) {
		snd_log("snd: calliope: cannot map the SysMMU table\n");
		return;
	}
	for (uint32_t i = 0; i < 4096; i++) {
		*(volatile uint32_t *)(pt + 4 * i) = 0;
	}
	for (uint32_t mb = 0; mb < CAL_DRAM_SIZE >> 20; mb++) {
		*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_DRAM >> 20) + mb)) = (uint32_t)((CAL_DRAM_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
	}
	*(volatile uint32_t *)(pt + 4 * (0x10000000U >> 20)) = (0x10000000U >> 4) | 2;
	/*
	 * A, 2026-11-06: Calliope's first access beyond its firmware faulted at 0xE0803000 (SysMMU INT 0x2): its silent log,
	 * IOVA_SILENT_LOG 0xE0000000 (abox.h:55), Samsung's reserved "exynos,abox_slog" of 0x2800000 (exynos9830.dts:2640;
	 * abox_dbg.c:139 maps it). Also its dump buffer, IOVA_DUMP_BUFFER 0xD0000000, "exynos,abox_dbg" 0x5600000
	 * (abox_dbg.c:392). Both in the low bank's free run after the framebuffer (UEFI's map: f2195000-100000000 free):
	 * the log at 0xF2200000 (1 MB aligned, cleared), the dump buffer from 0xFAA00000 to the 4 GB line, right after
	 * abox_rmem -- the same hole Samsung's sizes fill exactly.
	 */
	{
		volatile uint8_t *slog = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)CAL_SLOG_PHYS, CAL_SLOG_SIZE,
		    VM_WIMG_WCOMB);

		if (slog != NULL) {
			for (uint32_t i = 0; i < CAL_SLOG_SIZE; i += 4) {
				*(volatile uint32_t *)(slog + i) = 0;
			}
			cal_slog = slog;
		}
		for (uint32_t mb = 0; mb < CAL_SLOG_SIZE >> 20; mb++) {
			*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_SLOG >> 20) + mb)) = (uint32_t)((CAL_SLOG_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
		}
		for (uint32_t mb = 0; mb < CAL_DUMP_SIZE >> 20; mb++) {
			*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_DUMP >> 20) + mb)) = (uint32_t)((CAL_DUMP_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
		}
		/*
		 * A, 2026-11-06: next fault 0xA2000002, IOVA_VSS_PCI (abox.h:52; abox_pci.c:140 maps abox-pci-rmem, 8 MB,
		 * cleared). The voice-call regions (VSS firmware/parameter in the modem's shared memory, the PCI doorbell) have
		 * no modem behind them here: their 64 MB of IOVA all point at one cleared spare MB, the PCI 8 MB at its own.
		 */
		{
			volatile uint8_t *pci = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)CAL_PCI_PHYS,
			    CAL_PCI_SIZE + (1U << 20), VM_WIMG_WCOMB);

			if (pci != NULL) {
				for (uint32_t i = 0; i < CAL_PCI_SIZE + (1U << 20); i += 4) {
					*(volatile uint32_t *)(pci + i) = 0;
				}
			}
			for (uint32_t mb = 0; mb < 64; mb++) {
				*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_VSS >> 20) + mb)) = (uint32_t)(CAL_VSS_SPARE_PHYS >> 4) | 2;
			}
			for (uint32_t mb = 0; mb < CAL_PCI_SIZE >> 20; mb++) {
				*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_PCI >> 20) + mb)) = (uint32_t)((CAL_PCI_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
			}
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	capa0 = rd(mmu, MMU_CAPA0_V7);
	if ((capa0 >> 11) & 1) {
		capa1 = rd(mmu, MMU_CAPA1_V7);
		vcr = (capa1 >> 14) & 1;
	}
	flpt = vcr ? 0x800c : 0x000c;
	inv = vcr ? 0x8010 : 0x0010;
	wr(mmu, MMU_CTRL, 0x7);                 /* CTRL_BLOCK while it is set up */
	wr(mmu, MMU_CFG, 0);
	if (vcr) {
		wr(mmu, 0x8004, 0);             /* REG_MMU_CFG_VM */
	}
	wr(mmu, flpt, (uint32_t)(pt_phys >> 12));
	wr(mmu, inv, 1);                        /* all TLBs invalidated */
	wr(mmu, MMU_CTRL, 0x5);                 /* CTRL_ENABLE */
	if (vcr) {
		wr(mmu, MMU_CTRL_VM, 0x5 | 0x8);        /* CTRL_ENABLE | CTRL_FAULT_STALL_MODE */
	}
	snd_log("snd: calliope: SysMMU on: CAPA0 %#x CAPA1 %#x (VCR %u), table at %#llx, CTRL %#x CTRL_VM %#x FLPT %#x\n", capa0, capa1,
	    vcr, pt_phys, rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM), rd(mmu, flpt));
	/* DRAM on, then the cores: power, then enable (abox_core_power, abox_core_enable) */
	wr(abox, ABOX_SYSPOWER_CTRL, 1);
	wait_bits(abox, ABOX_SYSPOWER_STATUS, 1, 1, 10000);
	rx0 = *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF);
	/*
	 * A, 2026-11-06, stage 2: Samsung's abox_msg_init sets up both message queues before the DSP runs (empty, and the
	 * data queue's length = the area less the headers). Without it the first run's queue length was 0.
	 */
	for (uint32_t q = 0; q < 2; q++) {
		volatile uint8_t *a = dram + (q ? CAL_IPC_RX_OFF : CAL_IPC_TX_OFF);

		*(volatile int32_t *)(a + 0) = 0;
		*(volatile int32_t *)(a + 4) = 0;
		*(volatile int32_t *)(a + CAL_Q_DATA + 0) = 0;
		*(volatile int32_t *)(a + CAL_Q_DATA + 4) = 0;
		*(volatile uint32_t *)(a + CAL_Q_DATA + 8) = CAL_IPC_AREA - CAL_Q_DATA_ELEM;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	upd(pmu, 0x3180, 1, 1);
	upd(pmu, 0x3200, 1, 1);
	IOSleep(2);
	upd(pmu, 0x31a0, 1, 1);
	upd(pmu, 0x3220, 1, 1);
	snd_log("snd: calliope: cores powered %#x/%#x, enabled %#x/%#x; IPC rx word 0 before: %#x\n", rd(pmu, 0x3180), rd(pmu, 0x3200),
	    rd(pmu, 0x31a0), rd(pmu, 0x3220), rx0);
	cal_dram = dram;
	cal_gpr = gpr;
	cal_timer = timer;
	cal_gicd = gicd;
	cal_pt = pt;
	cal_inv = inv;
	/* signs of life for 30 s: its messages as they come (every 20 ms); every 500 ms for the first 5 s the GPR, a fault */
	/* (snd_calliope=2: on to playback 1 s after its boot-done message; 10 s at most) */
	for (int tick = 1; tick <= 1500; tick++) {
		int t = tick / 25 - 1;

		IOSleep(20);
		calliope_drain(dram);
		if (snd_calliope >= 2 && ((cal_booted && tick >= 50) || tick >= 500)) {
			break;
		}
		if (tick % 25 != 0 || t >= 10) {
			continue;
		}
		snd_log("snd: calliope: +%d ms: GPR0 %08x %08x %08x %08x .. pc? %08x %08x; rx %08x %08x %08x %08x; SysMMU INT %#x FAULT_VA %#x\n",
		    (t + 1) * 500, rd(gpr, 0x0), rd(gpr, 0x4), rd(gpr, 0x8), rd(gpr, 0xc), rd(gpr, 0x3c), rd(gpr, 0x40),
		    *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF), *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF + 4),
		    *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF + 8), *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF + 12),
		    rd(mmu, 0x060), rd(mmu, vcr ? 0x1000 : 0x0070));
		/* Calliope's own words: the printable text in its silent log so far (the first 4 KB with something in them) */
		if (cal_slog != NULL) {
			char text[200];
			unsigned n = 0;

			for (uint32_t i = 0; i < 0x100000 && n + 1 < sizeof(text); i++) {
				char c = (char)cal_slog[i];

				if (c >= 0x20 && c < 0x7f) {
					text[n++] = c;
				} else if (c == '\n' && n > 0 && text[n - 1] != '|') {
					text[n++] = '|';
				}
			}
			text[n] = 0;
			if (n > 0) {
				snd_log("snd: calliope: its log: %s\n", text);
			}
		}
	}
}


/*
 * Stage 3 (snd_calliope=2): the AP's side of abox_msg_send + abox_ipc_send: the message (ABOX_IPC_MSG, 768 bytes) into
 * the tx data queue, its command (id = ipcid, cmd = msgtype, args) into the tx command queue, then SGI 15 to Calliope
 * through ABOX's GIC (abox_gic_generate_interrupt: GICD_SGIR = 1 << 16 | 15).
 */
#define CAL_MSG_BYTES           768U
#define CAL_IOVA_RDMA3          (0x91000000U + 0x100000U * RDMA_ID)     /* IOVA_RDMA_BUFFER(n) (abox.h:39) */

static bool
calliope_send(uint32_t ipcid, uint32_t msgtype, const uint32_t *p, unsigned np)
{
	static uint32_t m[CAL_MSG_BYTES / 4];
	volatile uint8_t *tx = cal_dram + CAL_IPC_TX_OFF, *c, *d;
	uint32_t len = *(volatile uint32_t *)(tx + CAL_Q_DATA + 8), size = 4 + CAL_MSG_BYTES, e, sidx, blank;
	int32_t cs, ce;

	bzero(m, sizeof(m));
	m[0] = ipcid;
	m[1] = (ipcid == 2 || ipcid == 3) ? (np > 0 ? p[0] : 0) : 0;  /* task_id = the channel for PCM messages */
	m[2] = msgtype;
	for (unsigned i = 0; i < np && i < 64; i++) {
		m[3 + i] = p[i];
	}
	for (int w = 0; ; w++) {
		e = *(volatile uint32_t *)(tx + CAL_Q_DATA + 4);
		sidx = *(volatile uint32_t *)(tx + CAL_Q_DATA + 0);
		if (sidx > e) {
			blank = sidx - e;
		} else {
			blank = len - e;
			if (blank < size) {
				e = 0;
				blank = sidx;
			}
		}
		cs = *(volatile int32_t *)(tx + 0);
		ce = *(volatile int32_t *)(tx + 4);
		if (blank >= size && cs != (ce + 1) % (int32_t)CAL_Q_CMD_LEN) {
			break;
		}
		if (w >= 100) {
			snd_log("snd: calliope: send %u/%u: the queue stays full (data s %u e %u, cmd s %d e %d)\n", ipcid, msgtype,
			    sidx, e, cs, ce);
			return false;
		}
		IOSleep(1);
	}
	d = tx + CAL_Q_DATA_ELEM + e;
	*(volatile uint32_t *)d = CAL_MSG_BYTES;
	for (uint32_t i = 0; i < CAL_MSG_BYTES / 4; i++) {
		*(volatile uint32_t *)(d + 4 + 4 * i) = m[i];
	}
	*(volatile uint32_t *)(tx + CAL_Q_DATA + 4) = (e + size) % len;
	c = tx + CAL_Q_CMD_ELEM + (uint32_t)ce * CAL_Q_CMD_SIZE;
	for (uint32_t i = 0; i < CAL_Q_CMD_SIZE; i += 4) {
		*(volatile uint32_t *)(c + i) = 0;
	}
	*(volatile uint32_t *)(c + 0) = ipcid;
	*(volatile uint32_t *)(c + 4) = msgtype;
	*(volatile uint32_t *)(c + 8) = np > 0 ? p[0] : 0;
	if (ipcid == 1 || ipcid == 0xA) {
		*(volatile uint32_t *)(c + 12) = np > 1 ? p[1] : 0;
		*(volatile uint32_t *)(c + 16) = np > 2 ? p[2] : 0;
	}
	*(volatile uint32_t *)(c + 20) = e;
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile int32_t *)(tx + 4) = (ce + 1) % (int32_t)CAL_Q_CMD_LEN;
	__asm__ volatile ("dsb sy" ::: "memory");
	/* run 4: a plain GICD_SGIR write is never taken (the GIC is secure); run 3: through EL3 the first one was */
	{
		uint64_t r = sfr_smc_write(CAL_GICD_PHYS + 0xf00, (1U << 16) | 15U);

		if (r != 0) {
			snd_log("snd: calliope: the SGI through EL3 answers %lld\n", (long long)r);
		}
	}
	/*
	 * A, run 3: the EL3 SGI (after a plain one not taken in 50 ms) was never taken; the first message was, late. Plain
	 * only now, and how long each took (up to 500 ms) is logged.
	 */
	{
		int i;

		for (i = 0; i < 500 && *(volatile int32_t *)(tx + 0) == cs; i++) {
			IOSleep(1);
		}
		snd_log("snd: calliope: sent %u/%u (%d): %s after %d ms\n", ipcid, msgtype, np > 0 ? (int)p[0] : -1,
		    *(volatile int32_t *)(tx + 0) != cs ? "taken" : "NOT taken", i);
	}
	(void)cal_sgi_smc;
	(void)cal_sgi_tested;
	return true;
}

/* its answer: what it sent in the next `ms' milliseconds, and how far it took our queue */
static void
calliope_settle(const char *what, unsigned ms)
{
	volatile uint8_t *tx = cal_dram + CAL_IPC_TX_OFF;

	for (unsigned i = 0; i < ms / 10; i++) {
		IOSleep(10);
		calliope_drain(cal_dram);
	}
	calliope_log();
	snd_log("snd: calliope: %s: tx cmd s %d e %d; RDMA3 CTRL0 %#x STATUS %#x BUF %#x-%#x; UAIF2 STATUS %#x; GPR %08x %08x pc? %08x %08x\n",
	    what, *(volatile int32_t *)(tx + 0), *(volatile int32_t *)(tx + 4), rd(abox, RDMA(RDMA_CTRL0)), rd(abox, RDMA(RDMA_STATUS)),
	    rd(abox, RDMA(RDMA_BUF_STR)), rd(abox, RDMA(RDMA_BUF_END)), rd(abox, ABOX_UAIF_STATUS),
	    cal_gpr ? rd(cal_gpr, 0x0) : 0, cal_gpr ? rd(cal_gpr, 0x4) : 0, cal_gpr ? rd(cal_gpr, 0x3c) : 0,
	    cal_gpr ? rd(cal_gpr, 0x40) : 0);
	/* abox_soc_3.h: ABOX_CA32_CORE0_R(x) 0x5000 + 4x, PC = R(31) at 0x7c; core 1 at 0x5080; status 0x5100 */
	if (cal_gpr != NULL) {
		for (int pass = 0; pass < 3; pass++) {
			snd_log("snd: calliope: %s: core0 pc %08x r28-30 %08x %08x %08x r0-3 %08x %08x %08x %08x; core1 pc %08x; status %08x\n",
			    what, rd(cal_gpr, 0x7c), rd(cal_gpr, 0x70), rd(cal_gpr, 0x74), rd(cal_gpr, 0x78), rd(cal_gpr, 0x0),
			    rd(cal_gpr, 0x4), rd(cal_gpr, 0x8), rd(cal_gpr, 0xc), rd(cal_gpr, 0x80 + 0x7c), rd(cal_gpr, 0x100));
			IOSleep(3);
		}
	}
	{
		/* ABOX's GIC as EL3 reads it: groups, enables, pending, active for IRQs 0-127 */
		static const uint32_t offs[] = { 0x080, 0x084, 0x088, 0x08c, 0x100, 0x104, 0x108, 0x10c, 0x200, 0x204, 0x208, 0x20c,
			                         0x300, 0x304, 0x308, 0x30c };
		uint32_t v[16];

		for (int i = 0; i < 16; i++) {
			if (!sfr_smc_read(CAL_GICD_PHYS + offs[i], &v[i])) {
				v[i] = 0xdeadbeef;
			}
		}
		snd_log("snd: calliope: %s: GIC group %08x %08x %08x %08x en %08x %08x %08x %08x pend %08x %08x %08x %08x "
		    "act %08x %08x %08x %08x\n", what, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
		    v[12], v[13], v[14], v[15]);
	}
	if (rd(mmu, 0x060) != 0) {
		snd_log("snd: calliope: %s: SysMMU fault: INT %#x FAULT_VA %#x %#x (INFO %#x)\n", what, rd(mmu, 0x060), rd(mmu, 0x1000),
		    rd(mmu, 0x0070), rd(mmu, 0x1004));
	}
}

/* abox_rdma.c's order: open, set_buffer + hw_params, prepare, trigger -- for RDMA3, our ring behind IOVA_RDMA_BUFFER(3) */
static bool
calliope_play(void)
{
	uint32_t ch = RDMA_ID, iova = CAL_IOVA_RDMA3 + (uint32_t)(buf_phys & 0xfffffU);
	uint32_t a[4];

	if (cal_dram == NULL || !cal_booted) {
		snd_log("snd: calliope: not booted: no playback\n");
		return false;
	}
	*(volatile uint32_t *)(cal_pt + 4 * (CAL_IOVA_RDMA3 >> 20)) = (uint32_t)((buf_phys & ~0xfffffULL) >> 4) | 2;
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(mmu, cal_inv, 1);
	bzero(buf, BUF_BYTES);
	__asm__ volatile ("dsb sy" ::: "memory");
	/*
	 * ABOX_RESUME as abox_boot_done_work -> abox_cpu_pm_ipc: bundle.param_u64[0] = the AP's clock in ns,
	 * [1] = ABOX timer 1 (26 MHz) in ns (atime * 500 / 13). Off with snd_cal_resume=0.
	 */
	a[0] = 1;
	PE_parse_boot_argn("snd_cal_resume", a, sizeof(a[0]));
	if (a[0] != 0) {
		uint64_t kt, at = 0;
		uint32_t r[7] = { 0, 0, 0, 0, 0, 0, 0 };

		absolutetime_to_nanoseconds(mach_absolute_time(), &kt);
		if (cal_timer != NULL) {
			at = ((uint64_t)rd(cal_timer, 0x600 + 0x20 + 0x14) << 32) | rd(cal_timer, 0x600 + 0x20 + 0x10);
			at = at * 500 / 13;
		}
		r[3] = (uint32_t)kt; r[4] = (uint32_t)(kt >> 32); r[5] = (uint32_t)at; r[6] = (uint32_t)(at >> 32);
		(void)calliope_send(1, 2, r, 7);        /* ABOX_RESUME */
		calliope_settle("resume", 100);
	}
	calliope_log();
	/* the mixer's side, as abox_cmpnt.c's controls do: SIFS0 48 kHz, 32-bit stereo (IPC_ABOX_CONFIG) */
	/* (run 5: SET_SIFS0_RATE hung its IPC thread at "Changing MIXER sample rate": only with snd_cal_cfg=1) */
	if (PE_parse_boot_argn("snd_cal_cfg", a, sizeof(a[0])) && a[0] != 0) {
		a[0] = 48000;
		(void)calliope_send(0xA, 1, a, 1);      /* SET_SIFS0_RATE */
		a[0] = 32; a[1] = 2;
		(void)calliope_send(0xA, 0x10, a, 2);   /* SET_SIFS0_FORMAT */
		calliope_settle("sifs0", 50);
	}
	a[0] = ch;
	(void)calliope_send(2, 11, a, 1);               /* PCM_PLTDAI_OPEN */
	calliope_settle("open", 100);
	a[0] = ch; a[1] = iova; a[2] = PERIOD_BYTES; a[3] = BUF_BYTES / PERIOD_BYTES;
	(void)calliope_send(2, 20, a, 4);               /* PCM_SET_BUFFER: phyaddr, period bytes, periods */
	calliope_settle("set_buffer", 50);
	{
		uint32_t h[5] = { ch, 48000, 16, 2, 0 };

		(void)calliope_send(2, 14, h, 5);       /* PCM_PLTDAI_HW_PARAMS: rate, width, channels, packed */
	}
	calliope_settle("hw_params", 100);
	a[0] = ch;
	(void)calliope_send(2, 16, a, 1);               /* PCM_PLTDAI_PREPARE */
	calliope_settle("prepare", 100);
	a[0] = ch; a[1] = 1;
	(void)calliope_send(2, 17, a, 2);               /* PCM_PLTDAI_TRIGGER 1 */
	calliope_settle("trigger", 200);
	snd_log("snd: calliope: playback asked for (ring IOVA %#x = %#llx); %u pointer messages so far\n", iova, buf_phys,
	    cal_pointers);
	return true;
}

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t written, consumed, base, t0, last_look = 0, last_vol = 0, checked_at = 0;
	uint64_t status_consumed = 0, quiet_since = 0;
	uint32_t last_pos, generation = 0, below = 0;
	bool muted = false, decided = false;

	if (!aud_power_on() || !aud_clocks()) {
		return;
	}
	if (snd_calliope != 0) {
		/*
		 * The firmware is in the system image's /fw, which is the root only once init has switched to iOS's volume (the
		 * small image is the root before): wait until the file is there (120 s at most), unless the small image has it.
		 */
		{
			uint64_t sz = 0;
			unsigned waited = 0;
			bool there = redwood_image_file("fw", "calliope_dram_2.bin", &sz, NULL) != NULL;

			while (!there && waited++ < 240) {
				vnode_t vn = NULLVP;

				if (rootvnode != NULLVP && vnode_lookup("/fw/calliope_dram_2.bin", 0, &vn, vfs_context_kernel()) == 0) {
					vnode_put(vn);
					there = true;
					break;
				}
				delay_for_interval(500, NSEC_PER_MSEC);
			}
			snd_log("snd: calliope: the firmware is %s (after %u ms)\n", there ? "there" : "NOT there after 120 s",
			    waited * 500);
		}
		calliope_boot();                /* stage 1 of Samsung's way: the DSP firmware; no bypass playback this run */
		if (snd_calliope < 2) {
			return;
		}
	}
	abox_route();
	amps_ok = amps_bring_up();      /* (their setup needs no clock; their power-up does) */
	if (snd_calliope >= 2) {
		if (!calliope_play()) {
			return;
		}
	} else {
		abox_start();
	}
	/* frames are counted on from where the DMA is now, in the ring's terms: ring index = count % BUF_FRAMES */
	t0 = now_us();
	last_pos = dma_position(NULL);
	base = written = last_pos;
	redwood_guard_configure(true, snd_gain);
	redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
	redwood_audio_set_sink("the S20+'s speakers (ABOX UAIF2, CS35L41 x2)", exynos_sink);
	if (snd_tone != 0) {
		tone_left = snd_tone * 48;
		last_sound_us = now_us();
	}
	snd_log("snd: running (amplifiers %s, %u/8 dB under the chip's full level, guard gain %u/8 dB%s)\n",
	    amps_ok ? "ready" : "MISSING", snd_below, snd_gain, snd_tone ? ", the test tone first" : "");

	for (;;) {
		uint64_t now = now_us(), by_time = (now - t0) * REDWOOD_AUDIO_RATE / 1000000;
		uint32_t pos = dma_position(NULL), delta = (pos + BUF_FRAMES - last_pos) % BUF_FRAMES;

		last_pos = pos;
		status_consumed += delta;
		if (cal_dram != NULL) {
			static uint64_t cal_log_at;

			calliope_drain(cal_dram);
			if (now - cal_log_at >= 500000) {
				cal_log_at = now;
				calliope_log();
			}
		}
		/* after a quarter second: does the DMA's own count agree with the clock? (it should, within a period) */
		if (!decided && now - t0 >= 250000) {
			int64_t diff = (int64_t)status_consumed - (int64_t)by_time;

			decided = true;
			use_status = diff < 1024 && diff > -1024 && status_consumed != 0;
			snd_log("snd: after %llu us the DMA read %llu frames, the clock says %llu: following %s\n", now - t0,
			    status_consumed, by_time, use_status ? "the DMA" : "the clock");
		}
		consumed = base + (use_status ? status_consumed : by_time);
		if (written < consumed) {
			underruns++;
			written = consumed + 64;
		}
		while (written < consumed + LEAD_FRAMES) {
			uint32_t n = (uint32_t)(consumed + LEAD_FRAMES - written), room = BUF_FRAMES - (uint32_t)(written % BUF_FRAMES);

			n = n < CHUNK ? n : CHUNK;
			n = n < room ? n : room;
			fill(written, n);
			written += n;
		}
		__asm__ volatile ("dsb sy" ::: "memory");

		/* the amplifiers: on with sound, off after three quiet seconds; iOS's volume; their faults */
		if (amps_ok && !faulted && now - last_vol >= 100000) {
			bool sound = last_sound_us != 0 && now - last_sound_us < QUIET_OFF_US;
			uint32_t b = 0, g = 0;
			bool m = false;

			last_vol = now;
			if (redwood_audio_volume(&b, &m, &g) && g != generation) {
				generation = g;
				below = b;
				muted = m;
			}
			if (sound && !muted && !amps_on) {
				amps_on = amps_up();
				if (!amps_on) {
					faulted = true;
					amps_down();
				}
				quiet_since = 0;
				snd_log("snd: amplifiers %s (iOS volume %u/8 dB under)\n", amps_on ? "on" : "FAILED", below);
			} else if (amps_on && (!sound || muted)) {
				if (quiet_since == 0) {
					quiet_since = now;
				}
				if (muted || now - quiet_since >= 100000) {
					amps_down();
					amps_on = false;
					redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
					snd_log("snd: amplifiers off (%llu frames from iOS, %llu fed, %u underruns)\n", ios_frames, fed_frames,
					    (uint32_t)underruns);
				}
			} else if (sound) {
				quiet_since = 0;
			}
			/* (quiet_since is not touched here: clearing it on every pass kept the amplifiers on for good, 27 October) */
			if (amps_on) {
				uint32_t total = snd_below + (tone_left != 0 ? 0 : below);

				amps_volume(total, false);
				redwood_guard_attenuation(total);
				if (now - checked_at >= 500000) {
					uint32_t f = amps_faults();

					checked_at = now;
					if (f != 0) {
						snd_log("snd: AMPLIFIER FAULT %#x: both off for the rest of the run\n", f);
						amps_down();
						amps_on = false;
						faulted = true;
						redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
					}
				}
			}
		}
		if (now - last_look >= 60000000) {
			last_look = now;
			snd_log("snd: %llu frames from iOS, %llu fed, %llu silent, %u underruns; DMA at %u (%s); quiet %llu times, "
			    "%llu ms in all\n", ios_frames, fed_frames, silent_frames, (uint32_t)underruns, pos,
			    use_status ? "followed" : "by the clock", quiet_spells, quiet_us / 1000);
		}
		if (decided && !amps_on && tone_left == 0 &&
		    (last_sound_us == 0 || now - last_sound_us >= QUIET_OFF_US)) {
			uint64_t began = now_us(), heard = last_sound_us;

			/* the whole ring silence, then asleep until iOS sends sound (it goes on sending silence: dropped) */
			bzero(buf, BUF_BYTES);
			__asm__ volatile ("dsb sy" ::: "memory");
			snd_quiet = true;
			quiet_spells++;
			while (last_sound_us == heard && now_us() - began < 60000000) {
				ring_tail = ring_head;
				assert_wait_timeout((event_t)&ring_head, THREAD_UNINT, 1000, NSEC_PER_MSEC);
				if (last_sound_us != heard) {
					clear_wait(current_thread(), THREAD_AWAKENED);
					break;
				}
				thread_block(THREAD_CONTINUE_NULL);
			}
			snd_quiet = false;
			now = now_us();
			quiet_us += now - began;
			/* on from where the DMA is now: the count again such that count % BUF_FRAMES is its ring index */
			pos = dma_position(NULL);
			base = written + (pos + BUF_FRAMES - (uint32_t)(written % BUF_FRAMES)) % BUF_FRAMES;
			written = base;
			last_pos = pos;
			status_consumed = 0;
			t0 = now;
			continue;
		}
		IOSleep(4);
	}
}

/* ---- entry points --------------------------------------------------------------------------------------------- */

void
redwood_exynos_sound_init(void)
{
	uint32_t on = 0;
	uint64_t pool_phys;
	vm_offset_t pool_virt;
	size_t pool_size;
	thread_t thread;

	if (!PE_parse_boot_argn("snd", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("snd_below", &snd_below, sizeof(snd_below));
	/* x1slte experiments, after Scion's S9 ABOX (hakios.exynos_abox.c: RDMA0 CTRL0 0x400921 there, SIFS1 not the mixer) */
	PE_parse_boot_argn("snd_burst", &snd_burst, sizeof(snd_burst));
	PE_parse_boot_argn("snd_bufupd", &snd_bufupd, sizeof(snd_bufupd));
	PE_parse_boot_argn("snd_sifs1", &snd_sifs1, sizeof(snd_sifs1));
	PE_parse_boot_argn("snd_sifs_cnt", &snd_sifs_cnt, sizeof(snd_sifs_cnt));
	PE_parse_boot_argn("snd_sblimit", &snd_sblimit, sizeof(snd_sblimit));
	PE_parse_boot_argn("snd_gain", &snd_gain, sizeof(snd_gain));
	PE_parse_boot_argn("snd_tone", &snd_tone, sizeof(snd_tone));
	PE_parse_boot_argn("snd_calliope", &snd_calliope, sizeof(snd_calliope));
	if (snd_calliope >= 2) {
		snd_rdma = 0;
	}
	if (snd_calliope >= 2 && !PE_parse_boot_argn("snd_tone", &snd_tone, sizeof(snd_tone))) {
		snd_tone = 2000;         /* stage 3's own test: 2 s of 1 kHz once the amplifiers are up */
	}
	if (snd_gain > REDWOOD_GUARD_MAX_GAIN) {
		snd_gain = REDWOOD_GUARD_MAX_GAIN;
	}
	if (snd_tone > 3000) {
		snd_tone = 3000;
	}
	/* the DMA's ring: the last 64 KB of the loader's pool below 4 GB (the Wi-Fi driver only uses its first 4 MB) */
	if (redwood_wifi_dma_pool(&pool_phys, &pool_virt, &pool_size) < 0 || pool_size < (8U << 20)) {
		snd_log("snd: no DMA pool from the loader: no sound\n");
		return;
	}
	buf_phys = pool_phys + pool_size - 0x10000;
	buf = (int16_t *)ml_io_map_unmappable((vm_offset_t)buf_phys, 0x10000, VM_WIMG_WCOMB);
	pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);        /* (to 0x3224: the Calliope cores' power, snd_calliope) */
	cmu = (volatile uint8_t *)ml_io_map(CMU_AUD_PHYS, 0x4000);
	mmu = (volatile uint8_t *)ml_io_map(SYSMMU_PHYS, 0x9000);
	abox = (volatile uint8_t *)ml_io_map(ABOX_PHYS, 0x2000);
	if (buf == NULL || pmu == NULL || cmu == NULL || mmu == NULL || abox == NULL) {
		snd_log("snd: cannot map\n");
		return;
	}
	snd_log("snd=1: the speakers through ABOX; DMA ring at %#llx\n", buf_phys);
	started = true;
	if (kernel_thread_start_priority(sound_thread, NULL, 90, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		started = false;
	}
}

/* at the run's end and on a panic's way out: the amplifiers into reset at once, the DMA stopped (its memory is reused) */
void
redwood_exynos_sound_quiesce(void)
{
	if (!started) {
		return;
	}
	if (gpb2 != NULL) {
		for (unsigned i = 0; i < 2; i++) {
			redwood_gpio_set(gpb2, amps[i].reset_pin, 0);
		}
	}
	if (abox != NULL && (rd(pmu, PMU_AUD_STATUS) & 1)) {
		upd(abox, RDMA(RDMA_CTRL0), 1, 0);
		upd(abox, ABOX_UAIF_CTRL0, 1, 0);
	}
	started = false;
}
