/*
 * Galaxy S20 (x1slte, Exynos 990) camera bring-up.
 *
 * cam.s20 is a read-only pin inventory; cam.s20.id is a bounded rear-main
 * identity read with power, clock, pin and bus-state cleanup. This is separate
 * from the Qualcomm camera path.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_dbgport.h>
#include <sys/systm.h>
#include "redwood_camera_s20_3j1.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool redwood_s2mpb02_camera_read(uint8_t reg, uint8_t *value);
extern bool redwood_s2mpb02_camera_bits(uint8_t reg, uint8_t mask, uint8_t value);
extern int redwood_spmic_read(uint8_t type, uint8_t reg, uint8_t *value);

#define PINCTRL_PERIC1_GPC0       0x0a0U
#define PINCTRL_CMGP_GPM1         0x020U
#define PINCTRL_CMGP_GPM11        0x160U
#define PINCTRL_CMGP_GPM22        0x2c0U
#define PINCTRL_DRV               0x0cU
#define PINCTRL_PERIC0_PHYS       0x10430000U
#define PINCTRL_PERIC1_PHYS       0x10730000U
#define PINCTRL_CMGP_PHYS         0x15c30000U
#define PINCTRL_CMGP_GPM0         0x000U
#define CMU_TOP_PHYS              0x1a330000U
#define CMU_CSIS_PHYS             0x17000000U
#define PMU_PHYS                  0x15860000U
#define CSIS1_DPHY_PHYS           0x170d2b00U
#define CSIS1_DPHY_BIAS_PHYS      0x170d1000U
#define ISPPRE_SYSREG_PHYS        0x17020500U
#define SMC_PREPARE_PD_ONOFF      0x82000410ULL
#define CSIS_NEED_SMC             0x17010204ULL
#define CSIS1_PHYS                0x17040000U
#define CSIS1_DMA1_VC0_PHYS       0x170c2000U
#define CSIS1_DMA1_COMMON_PHYS    0x170c2400U
#define CMU_CIS_CLK0_MUX          0x1018U
#define CMU_CIS_CLK0_DIV          0x1814U
#define CMU_CIS_CLK0_GATE         0x201cU
#define CMU_CIS_CLK0_QCH          0x3004U
#define CMU_CIS_CLK1_MUX          0x101cU
#define CMU_CIS_CLK1_DIV          0x1818U
#define CMU_CIS_CLK1_GATE         0x2020U
#define CMU_CIS_CLK1_QCH          0x3008U
#define CAM_S2MPB03_ADDR          0x56U
#define CAM_S2MPB03_LDO2          0x04U /* VDDD_1.05V_CAM */
#define CAM_S2MPB03_LDO3          0x05U /* VDDIO_1.8V_CAM */
#define CAM_S2MPB03_LDO5          0x07U /* VDDA_2.8V_CAM */
#define CAM_S2MPB02_BUCK2         0x0dU
#define CAM_S2MPB02_LDO1          0x1eU /* VDDPHY_1.05V_CAM */
#define CAM_S2MPB02_LDO5          0x22U
#define CAM_S2MPB02_LDO6          0x23U
#define CAM_S2MPB02_LDO11         0x28U
#define CAM_S2MPB02_LDO12         0x29U /* front module VDDIO/analog rail per Samsung power_seq */
#define CAM_S2MPB02_LDO14         0x2bU /* VDDAF_COMMON_CAM */
#define CAM_S2MPB02_LDO16         0x2dU

static struct redwood_bbi2c cam_pmic03_bus = {
	.name = "S20 camera S2MPB03 gpm0-0/gpm1-0", .scl = 0, .sda = 0, .half_us = 4
};

static uint32_t
cam_s20_rd(volatile uint8_t *bank, uint32_t offset)
{
	return *(volatile uint32_t *)(bank + offset);
}

static void
cam_s20_wr(volatile uint8_t *bank, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(bank + offset) = value;
}

static bool
cam_s20_pmic03_read(uint8_t reg, uint8_t *value)
{
	return redwood_bbi2c_read_reg(&cam_pmic03_bus, CAM_S2MPB03_ADDR, reg, value, 1);
}

static bool
cam_s20_pmic03_bits(uint8_t reg, uint8_t before, uint8_t value)
{
	uint8_t after = 0, want = (uint8_t)((before & 0x7f) | (value & 0x80));
	if (reg != CAM_S2MPB03_LDO2 && reg != CAM_S2MPB03_LDO3 && reg != CAM_S2MPB03_LDO5) return false;
	if (want == before) return true;
	return redwood_bbi2c_write_reg(&cam_pmic03_bus, CAM_S2MPB03_ADDR, reg, want) &&
	    cam_s20_pmic03_read(reg, &after) && ((after ^ want) & 0x80) == 0;
}

static void
cam_s20_wait_ms(unsigned ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
cam_s20_smc4(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
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
 * One bounded rear-main identification attempt, callable only through root-only kern.redwood.poke.
 * The x1slte overlay routes S5K2LD@0x10 through HSI2C3/gpp0-6/7. Its camera rails span S2MPB03
 * (LDO2/3/5) and S2MPB02 (BUCK2/LDO1/LDO14). Only enable bits are changed; all originals are
 * restored even after a NACK or failed readback. No sensor configuration register is written.
 */
static void
cam_s20_identify(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	struct redwood_bbi2c sensor_bus = { .name = "S20 rear main HSI2C3 gpp0-6/7", .scl = 6, .sda = 7, .half_us = 4 };
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM22);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x4000);
	uint8_t p02[3] = { 0 }, p03[3] = { 0 }, id[2] = { 0 }, rev[2] = { 0 };
	uint32_t reset_con = cam_s20_rd(reset, REDWOOD_GPIO_CON), reset_dat = cam_s20_rd(reset, REDWOOD_GPIO_DAT);
	uint32_t reset_pud = cam_s20_rd(reset, REDWOOD_GPIO_PUD), mclk_con = cam_s20_rd(mclk, REDWOOD_GPIO_CON);
	uint32_t mclk_dat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT), mclk_pud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	uint32_t clock[4] = { 0 }, mask_reset = 0xfU, mask_mclk = 0xfU;
	char phase[40] = "S2MPB03 bus";
	char acked[96] = "";
	bool pmic03_ready = false, sensor_ready = false, clock_ready = false, rails02 = false, rails03 = false;
	bool ok = false, sensor_ack = false;
	uint32_t clock_offsets[4] = { CMU_CIS_CLK0_MUX, CMU_CIS_CLK0_DIV, CMU_CIS_CLK0_GATE, CMU_CIS_CLK0_QCH };
	unsigned i;

	if (cap == 0) return;
	if (cmu == NULL) {
		(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"result\":\"error\",\"phase\":\"CMU map\"}\n");
		return;
	}
	cam_pmic03_bus.bank = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM0);
	cam_pmic03_bus.sda_bank = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM1);
	pmic03_ready = true;
	if (!redwood_bbi2c_setup(&cam_pmic03_bus, false, out, cap)) goto cleanup;
	if (!cam_s20_pmic03_read(CAM_S2MPB03_LDO2, &p03[0]) || !cam_s20_pmic03_read(CAM_S2MPB03_LDO3, &p03[1]) ||
	    !cam_s20_pmic03_read(CAM_S2MPB03_LDO5, &p03[2])) goto cleanup;
	rails03 = true;
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &p02[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO1, &p02[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO14, &p02[2])) {
		(void)snprintf(phase, sizeof(phase), "S2MPB02 snapshot");
		goto cleanup;
	}
	rails02 = true;
	/* x1slte's per-module power sequence: shared 1.3 V first, reset low, then VDDIO/analog/PHY/core. */
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE);
	(void)snprintf(phase, sizeof(phase), "BUCK2 enable");
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0)) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "camera rails");
	if (!cam_s20_pmic03_bits(CAM_S2MPB03_LDO3, p03[1], 0x80)) goto cleanup;
	cam_s20_wait_ms(1);
	if (!cam_s20_pmic03_bits(CAM_S2MPB03_LDO5, p03[2], 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO1, 0x80, 0x80) ||
	    !cam_s20_pmic03_bits(CAM_S2MPB03_LDO2, p03[0], 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO14, 0x80, 0x80)) goto cleanup;
	cam_s20_wait_ms(2);
	(void)snprintf(phase, sizeof(phase), "sensor I2C pins");
	sensor_ready = true;
	sensor_bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	/* This bus's saved pin state carries a pull-down, unlike the DT's I2C pin state (no internal pull). */
	if (!redwood_bbi2c_setup(&sensor_bus, true, out, cap)) goto cleanup;
	/* CIS_CLK0: OSCCLK (26 MHz), divider 1; save and restore the mux, divider, QCH and gate. */
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu, clock_offsets[i]);
	cam_s20_wr(cmu, CMU_CIS_CLK0_MUX, clock[0] & ~1U);
	cam_s20_wr(cmu, CMU_CIS_CLK0_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu, CMU_CIS_CLK0_QCH, clock[3] | 3U);
	cam_s20_wr(cmu, CMU_CIS_CLK0_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 0, 0);
	redwood_gpio_func(mclk, 0, 2); /* DT sensor_mclk0_fn: gpc0-0 function 2 */
	redwood_gpio_pull(mclk, 0, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(2);
	(void)redwood_gpio_set(reset, 0, 1);
	cam_s20_wait_ms(10); /* Samsung's 3 ms minimum plus startup margin for a cold module */
	/* Bounded read-only address scan on the dedicated sensor bus. This also catches a shifted-address mistake. */
	(void)snprintf(phase, sizeof(phase), "S5K2LD address scan");
	for (i = 0x08; i < 0x78; i++) {
		if (redwood_bbi2c_xfer(&sensor_bus, (uint8_t)i, NULL, 0, NULL, 0)) {
			if (acked[0] != '\0') strlcat(acked, ",", sizeof(acked));
			{
				char address[8];
				(void)snprintf(address, sizeof(address), "%s0x%02x", acked[0] == '\0' ? "" : "", i);
				strlcat(acked, address, sizeof(acked));
			}
			if (i == 0x10) sensor_ack = true;
		}
	}
	if (!sensor_ack) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "S5K2LD model ID read");
	{
		uint8_t a[2] = { 0x00, 0x00 }, b[2] = { 0x00, 0x02 };
		ok = redwood_bbi2c_xfer(&sensor_bus, 0x10, a, sizeof(a), id, sizeof(id));
		if (ok) {
			(void)snprintf(phase, sizeof(phase), "S5K2LD revision read");
			ok = redwood_bbi2c_xfer(&sensor_bus, 0x10, b, sizeof(b), rev, sizeof(rev));
		}
	}
cleanup:
	/* Stop clock and hold reset low before taking supplies away. */
	(void)redwood_gpio_set(reset, 0, 0);
	if (clock_ready) {
		(void)redwood_gpio_set(mclk, 0, 0);
		redwood_gpio_func(mclk, 0, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu, CMU_CIS_CLK0_GATE, clock[2]);
		cam_s20_wr(cmu, CMU_CIS_CLK0_QCH, clock[3]);
		cam_s20_wr(cmu, CMU_CIS_CLK0_DIV, clock[1]);
		cam_s20_wr(cmu, CMU_CIS_CLK0_MUX, clock[0]);
	}
	if (sensor_ready) {
		redwood_bbi2c_restore(&sensor_bus);
	}
	if (rails02)
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO14, 0x80, p02[2] & 0x80);
	if (rails03) (void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO2, p03[0], p03[0] & 0x80);
	if (rails02)
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO1, 0x80, p02[1] & 0x80);
	if (rails03) {
		(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO5, p03[2], p03[2] & 0x80);
		(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO3, p03[1], p03[1] & 0x80);
	}
	if (rails02)
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p02[0] & 0xc0);
	/* Use the GPIO helpers so register updates keep the shared pinctrl-bank lock. */
	redwood_gpio_restore(reset, 0, reset_con & mask_reset, reset_pud & mask_reset, reset_dat & 1U);
	redwood_gpio_restore(mclk, 0, mclk_con & mask_mclk, mclk_pud & mask_mclk, mclk_dat & 1U);
	if (pmic03_ready) redwood_bbi2c_restore(&cam_pmic03_bus);
	if (ok) {
		(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"address\":\"0x10\",\"result\":\"ok\",\"model_id\":\"0x%02x%02x\",\"revision\":\"0x%02x%02x\",\"state_restored\":true}\n",
		    id[0], id[1], rev[0], rev[1]);
	} else {
		(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"address\":\"0x10\",\"result\":\"error\",\"phase\":\"%s\",\"acked\":\"%s\",\"state_restored\":true}\n", phase, acked);
	}
}

/* Front SAK3J1 candidate. Uses only the x1slte overlay's VT rails, HSI2C1, reset and MCLK1. */
static void
cam_s20_front_identify(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	struct redwood_bbi2c sensor_bus = { .name = "S20 front SAK3J1 HSI2C1 gpp0-2/3", .scl = 2, .sda = 3, .half_us = 4 };
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x4000);
	uint8_t p[5] = { 0 }, id[2] = { 0 }, rev = 0;
	uint32_t rcon = cam_s20_rd(reset, REDWOOD_GPIO_CON), rdat = cam_s20_rd(reset, REDWOOD_GPIO_DAT);
	uint32_t rpud = cam_s20_rd(reset, REDWOOD_GPIO_PUD), mcon = cam_s20_rd(mclk, REDWOOD_GPIO_CON);
	uint32_t mdat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT), mpud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	uint32_t clock[4] = { 0 }, offsets[4] = { CMU_CIS_CLK1_MUX, CMU_CIS_CLK1_DIV, CMU_CIS_CLK1_GATE, CMU_CIS_CLK1_QCH };
	char phase[40] = "S2MPB02 snapshot", acked[96] = "";
	bool sensor_ready = false, clock_ready = false, rails = false, ok = false;
	unsigned i;
	if (cap == 0) return;
	if (cmu == NULL) { (void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"result\":\"error\",\"phase\":\"CMU map\"}\n"); return; }
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &p[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &p[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &p[2]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &p[3]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &p[4])) goto cleanup;
	rails = true;
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE);
	redwood_gpio_set(reset, 0, 0);
	redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	(void)snprintf(phase, sizeof(phase), "BUCK2 enable");
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0)) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "VT camera rails");
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, 0x80)) goto cleanup;
	cam_s20_wait_ms(1);
	(void)snprintf(phase, sizeof(phase), "sensor I2C pins");
	sensor_ready = true;
	sensor_bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	if (!redwood_bbi2c_setup(&sensor_bus, true, out, cap)) goto cleanup;
	/* CIS_CLK1 is the front module's gpc0-1 output in the Samsung x1slte overlay. */
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu, offsets[i]);
	cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0] & ~1U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3] | 3U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 1, 0);
	redwood_gpio_func(mclk, 1, 2);
	redwood_gpio_pull(mclk, 1, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(10); /* on_i2c delay in the vendor power table */
	(void)redwood_gpio_set(reset, 0, 1);
	cam_s20_wait_ms(9);  /* MCLK settle from the SAK3J1 vendor power table */
	(void)snprintf(phase, sizeof(phase), "SAK3J1 address scan");
	for (i = 0x08; i < 0x78; i++) {
		if (redwood_bbi2c_xfer(&sensor_bus, (uint8_t)i, NULL, 0, NULL, 0)) {
			if (acked[0] != '\0') strlcat(acked, ",", sizeof(acked));
			{
				char address[8];
				(void)snprintf(address, sizeof(address), "0x%02x", i);
				strlcat(acked, address, sizeof(acked));
			}
			if (i == 0x10) ok = true;
		}
	}
	if (!ok) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "SAK3J1 model/revision read");
	{
		uint8_t a[2] = { 0x00, 0x00 }, b[2] = { 0x00, 0x02 };
		ok = redwood_bbi2c_xfer(&sensor_bus, 0x10, a, sizeof(a), id, sizeof(id)) &&
		    redwood_bbi2c_xfer(&sensor_bus, 0x10, b, sizeof(b), &rev, sizeof(rev));
	}
cleanup:
	(void)redwood_gpio_set(reset, 0, 0);
	if (clock_ready) {
		(void)redwood_gpio_set(mclk, 1, 0);
		redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0]);
	}
	if (sensor_ready) redwood_bbi2c_restore(&sensor_bus);
	if (rails) {
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, p[4] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, p[3] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, p[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, p[1] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p[0] & 0xc0);
	}
	redwood_gpio_restore(reset, 0, rcon & 0xfU, rpud & 0xfU, rdat & 1U);
	redwood_gpio_restore(mclk, 1, mcon & (0xfU << 4), mpud & (0xfU << 4), mdat & 2U);
	if (ok) {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"address\":\"0x10\",\"result\":\"ok\",\"model_id\":\"0x%02x%02x\",\"revision\":\"0x%02x\",\"state_restored\":true}\n", id[0], id[1], rev);
	} else {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"address\":\"0x10\",\"result\":\"error\",\"phase\":\"%s\",\"acked\":\"%s\",\"state_restored\":true}\n", phase, acked);
	}
}

/* Read-only CSIS1/DMA1 baseline: whitelist-only registers from Samsung's v5.3 block map. */
static void
cam_s20_csis_inventory(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *csi = (volatile uint8_t *)ml_io_map(CSIS1_PHYS, 0x1000);
	volatile uint8_t *vc0 = (volatile uint8_t *)ml_io_map(CSIS1_DMA1_VC0_PHYS, 0x100);
	volatile uint8_t *common = (volatile uint8_t *)ml_io_map(CSIS1_DMA1_COMMON_PHYS, 0x100);
	if (cap == 0) return;
	if (csi == NULL || vc0 == NULL || common == NULL) {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"csis1-inventory\",\"result\":\"error\",\"phase\":\"map\"}\n");
		return;
	}
	(void)snprintf(out, cap,
	    "{\"camera\":\"SAK3J1\",\"stage\":\"csis1-inventory\",\"result\":\"ok\",\"write\":false,"
	    "\"csis1_version\":\"%#x\",\"cmn_ctrl\":\"%#x\",\"clk_ctrl\":\"%#x\",\"phy_status\":\"%#x\","
	    "\"frame_count_vc0\":\"%#x\",\"frame_count_vc1\":\"%#x\",\"irq_src0\":\"%#x\","
	    "\"dma_vc0_ctrl\":\"%#x\",\"dma_vc0_resol\":\"%#x\",\"dma_common_ctrl\":\"%#x\","
	    "\"dma_common_irq\":\"%#x\",\"dma_common_err\":\"%#x\"}\n",
	    cam_s20_rd(csi, 0x0000), cam_s20_rd(csi, 0x0004), cam_s20_rd(csi, 0x0008), cam_s20_rd(csi, 0x0700),
	    cam_s20_rd(csi, 0x0500), cam_s20_rd(csi, 0x0504), cam_s20_rd(csi, 0x0014),
	    cam_s20_rd(vc0, 0x0000), cam_s20_rd(vc0, 0x0004), cam_s20_rd(common, 0x0014),
	    cam_s20_rd(common, 0x0008), cam_s20_rd(common, 0x0018));
}

/*
 * Read CSIS clock/power state only after the documented CSIS power domain is on.
 * An earlier experiment read the DPHY while its domain/clocks were off and wedged the phone;
 * this command deliberately does not map or touch any DPHY registers or start the sensor.
 */
static void
cam_s20_csis_power_inventory(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL;
	uint32_t conf = 0, status = 0, initial_status = 0, after = 0, regs[8] = { 0 }, i;
	uint64_t smc = 0;
	bool initially_on, enabled_here = false, map_ok = false;
	if (cap == 0) return;
	if (pmu == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-power-inventory\",\"result\":\"error\",\"phase\":\"PMU map\"}\n");
		return;
	}
	conf = cam_s20_rd(pmu, 0x1d80);
	status = cam_s20_rd(pmu, 0x1d84);
	initial_status = status;
	initially_on = (status & 1U) != 0;
	if (!initially_on) {
		cam_s20_wr(pmu, 0x1d80, conf | 1U);
		for (i = 0; i < 100; i++) {
			status = cam_s20_rd(pmu, 0x1d84);
			if (status & 1U) break;
			cam_s20_wait_ms(1);
		}
		if (status & 1U) {
			enabled_here = true;
			smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
		}
	}
	if (status & 1U) cmu = (volatile uint8_t *)ml_io_map(CMU_CSIS_PHYS, 0x8000);
	map_ok = cmu != NULL;
	if (map_ok) {
		regs[0] = cam_s20_rd(cmu, 0x0600);
		regs[1] = cam_s20_rd(cmu, 0x0604);
		regs[2] = cam_s20_rd(cmu, 0x1800);
		regs[3] = cam_s20_rd(cmu, 0x3064);
		regs[4] = cam_s20_rd(cmu, 0x3078);
		regs[5] = cam_s20_rd(cmu, 0x3058);
		regs[6] = cam_s20_rd(cmu, 0x2018);
		regs[7] = cam_s20_rd(cmu, 0x2000);
	}
	if (enabled_here) {
		cam_s20_wr(pmu, 0x1d80, conf & ~1U);
		for (i = 0; i < 100; i++) {
			status = cam_s20_rd(pmu, 0x1d84);
			if (!(status & 1U)) break;
			cam_s20_wait_ms(1);
		}
	}
	after = cam_s20_rd(pmu, 0x1d84);
	if (map_ok) {
		(void)snprintf(out, cap,
		    "{\"stage\":\"csis-power-inventory\",\"result\":\"ok\",\"write_sensor\":false,\"phy_access\":false,"
		    "\"initial_domain_status\":\"%#x\",\"csis_bus_mux\":\"%#x\",\"csis_bus_mux_cfg\":\"%#x\","
		    "\"csis_bus_div\":\"%#x\",\"csis_qch\":\"%#x\",\"csis_dma_qch\":\"%#x\","
		    "\"csis_cmu_qch\":\"%#x\",\"csis_aclk1\":\"%#x\",\"csis_pclk\":\"%#x\","
		    "\"final_domain_status\":\"%#x\",\"powered_for_snapshot\":%s,\"smc_result\":\"%#llx\",\"state_restored\":%s}\n",
	    initial_status, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], after,
	    enabled_here ? "true" : "false", smc, (after & 1U) == (initial_status & 1U) ? "true" : "false");
	} else {
		(void)snprintf(out, cap,
		    "{\"stage\":\"csis-power-inventory\",\"result\":\"error\",\"phase\":\"CSIS domain or CMU map\","
		    "\"initial_conf\":\"%#x\",\"final_status\":\"%#x\",\"powered_for_snapshot\":%s,\"smc_result\":\"%#llx\"}\n",
		    conf, after, enabled_here ? "true" : "false", smc);
	}
}

/* Reversible CSIS1 QCH gate check. Requires the CSIS power domain to already be on. */
void
cam_s20_csis_gate_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL;
	volatile uint8_t *csi = NULL;
	uint32_t pd_status, cmu_before = 0, cmu_during = 0, cmu_after = 0;
	uint32_t before = 0, during = 0, after = 0;
	bool cmu_changed = false, changed = false;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis1-gate\",\"result\":\"error\",\"phase\":\"CSIS power domain off\",\"write_sensor\":false}\n");
		return;
	}
	cmu = (volatile uint8_t *)ml_io_map(CMU_CSIS_PHYS, 0x8000);
	csi = (volatile uint8_t *)ml_io_map(CSIS1_PHYS, 0x1000);
	if (cmu == NULL || csi == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis1-gate\",\"result\":\"error\",\"phase\":\"map\",\"write_sensor\":false}\n");
		return;
	}
	cmu_before = cam_s20_rd(cmu, 0x3058);
	if (!(cmu_before & 1U)) {
		cam_s20_wr(cmu, 0x3058, cmu_before | 1U);
		cmu_changed = true;
	}
	cam_s20_wait_ms(2);
	cmu_during = cam_s20_rd(cmu, 0x3058);
	before = cam_s20_rd(cmu, 0x3064);
	if (!(before & 1U)) {
		cam_s20_wr(cmu, 0x3064, before | 1U);
		changed = true;
	}
	cam_s20_wait_ms(2);
	during = cam_s20_rd(cmu, 0x3064);
	(void)cam_s20_rd(csi, 0x0000); /* CSIS version: known safe read after its QCH is enabled. */
	if (changed) {
		cam_s20_wr(cmu, 0x3064, during & ~1U);
		cam_s20_wait_ms(2);
	}
	after = cam_s20_rd(cmu, 0x3064);
	if (cmu_changed) {
		cam_s20_wr(cmu, 0x3058, cmu_during & ~1U);
		cam_s20_wait_ms(2);
		cmu_after = cam_s20_rd(cmu, 0x3058);
	} else {
		cmu_after = cmu_before;
	}
	pd_status = cam_s20_rd(pmu, 0x1d84);
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis1-gate\",\"result\":\"ok\",\"write_sensor\":false,\"phy_access\":false,"
	    "\"cmu_qch_before\":\"%#x\",\"cmu_qch_enabled\":\"%#x\",\"cmu_qch_after\":\"%#x\","
	    "\"qch_before\":\"%#x\",\"qch_enabled\":\"%#x\",\"qch_after\":\"%#x\","
	    "\"domain_status\":\"%#x\",\"state_restored\":%s}\n",
	    cmu_before, cmu_during, cmu_after, before, during, after, pd_status,
	    (((after ^ before) & 1U) == 0 && ((cmu_after ^ cmu_before) & 1U) == 0) ? "true" : "false");
}

/* Read-only idle DPHY snapshot. Parent CSIS QCHs and PHY isolation are restored exactly. */
static void
cam_s20_csis_dphy_snapshot(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL, *dphy = NULL, *isppre = NULL;
	uint32_t qcmu = 0, qcmu_on = 0, qcmu_after = 0, qcsis = 0, qcsis_on = 0, qcsis_after = 0;
	uint32_t iso = 0, iso_after = 0, reset_sysreg = 0, sc[7] = { 0 }, lane0[6] = { 0 };
	static const uint32_t sc_offs[] = { 0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0030 };
	static const uint32_t lane_offs[] = { 0x0100, 0x0104, 0x0108, 0x010c, 0x0110, 0x0114 };
	bool cmu_changed = false, csis_changed = false, iso_changed = false, gates_ready, restored;
	unsigned i;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-snapshot\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	cmu = (volatile uint8_t *)ml_io_map(CMU_CSIS_PHYS, 0x8000);
	dphy = (volatile uint8_t *)ml_io_map(CSIS1_DPHY_PHYS, 0x500);
	isppre = (volatile uint8_t *)ml_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (cmu == NULL || dphy == NULL || isppre == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-snapshot\",\"result\":\"error\",\"phase\":\"map\"}\n");
		return;
	}
	qcmu = cam_s20_rd(cmu, 0x3058);
	qcsis = cam_s20_rd(cmu, 0x3064);
	iso = cam_s20_rd(pmu, 0x0730);
	if (!(qcmu & 1U)) { cam_s20_wr(cmu, 0x3058, qcmu | 1U); cmu_changed = true; }
	cam_s20_wait_ms(2);
	qcmu_on = cam_s20_rd(cmu, 0x3058);
	if (!(qcsis & 1U)) { cam_s20_wr(cmu, 0x3064, qcsis | 1U); csis_changed = true; }
	cam_s20_wait_ms(2);
	qcsis_on = cam_s20_rd(cmu, 0x3064);
	gates_ready = (qcmu_on & 1U) != 0 && (qcsis_on & 1U) != 0;
	if (gates_ready) {
		if (!(iso & 1U)) { cam_s20_wr(pmu, 0x0730, iso | 1U); iso_changed = true; }
		cam_s20_wait_ms(1);
		reset_sysreg = cam_s20_rd(isppre, 0);
		for (i = 0; i < sizeof(sc_offs) / sizeof(sc_offs[0]); i++) sc[i] = cam_s20_rd(dphy, sc_offs[i]);
		for (i = 0; i < sizeof(lane_offs) / sizeof(lane_offs[0]); i++) lane0[i] = cam_s20_rd(dphy, lane_offs[i]);
	}
	if (iso_changed) cam_s20_wr(pmu, 0x0730, iso);
	iso_after = cam_s20_rd(pmu, 0x0730);
	if (csis_changed) cam_s20_wr(cmu, 0x3064, qcsis);
	qcsis_after = cam_s20_rd(cmu, 0x3064);
	if (cmu_changed) cam_s20_wr(cmu, 0x3058, qcmu);
	qcmu_after = cam_s20_rd(cmu, 0x3058);
	restored = ((iso_after ^ iso) & 1U) == 0 && ((qcsis_after ^ qcsis) & 1U) == 0 &&
	    ((qcmu_after ^ qcmu) & 1U) == 0;
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-dphy-snapshot\",\"result\":\"%s\",\"phase\":\"%s\",\"write_sensor\":false,\"write_dphy\":false,"
	    "\"cmu_qch_before\":\"%#x\",\"cmu_qch_on\":\"%#x\",\"cmu_qch_after\":\"%#x\","
	    "\"csis_qch_before\":\"%#x\",\"csis_qch_on\":\"%#x\",\"csis_qch_after\":\"%#x\","
	    "\"isolation_before\":\"%#x\",\"isolation_after\":\"%#x\",\"reset_sysreg\":\"%#x\","
	    "\"sc_gnr_con0\":\"%#x\",\"sc_gnr_con1\":\"%#x\",\"sc_ana_con0\":\"%#x\",\"sc_ana_con1\":\"%#x\","
	    "\"sc_ana_con2\":\"%#x\",\"sc_ana_con3\":\"%#x\",\"sc_time_con0\":\"%#x\","
	    "\"lane0_gnr_con0\":\"%#x\",\"lane0_gnr_con1\":\"%#x\",\"lane0_ana_con0\":\"%#x\","
	    "\"lane0_ana_con1\":\"%#x\",\"lane0_ana_con2\":\"%#x\",\"lane0_ana_con3\":\"%#x\",\"state_restored\":%s}\n",
	    gates_ready ? "ok" : "error", gates_ready ? "readback" : "QCH enable",
	    qcmu, qcmu_on, qcmu_after, qcsis, qcsis_on, qcsis_after, iso, iso_after, reset_sysreg,
	    sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6],
	    lane0[0], lane0[1], lane0[2], lane0[3], lane0[4], lane0[5], restored ? "true" : "false");
}

/* Restore the documented CSIS power-domain TZPC access through EL3; read reset sysreg only. */
static void
cam_s20_csis_secure_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *sysreg;
	uint32_t status;
	uint64_t smc;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-secure\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	/* exynos9830.dts: pd-csis need_smc=0x17010204; Linux calls this on domain-on. */
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	sysreg = (volatile uint8_t *)ml_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (sysreg == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-secure\",\"result\":\"error\",\"phase\":\"sysreg map\",\"smc\":\"%#llx\"}\n", smc);
		return;
	}
	status = cam_s20_rd(sysreg, 0);
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-secure\",\"result\":\"ok\",\"smc\":\"%#llx\",\"domain_status\":\"%#x\","
	    "\"reset_sysreg_read\":\"%#x\",\"wrote_registers\":false}\n",
	    smc, cam_s20_rd(pmu, 0x1d84), status);
}

/* One-register reset-bit toggle and restore after the Samsung CSIS DTZPC restore; lanes stay disabled. */
static void
cam_s20_csis_reset_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *sysreg;
	uint32_t before, requested, during, after;
	uint64_t smc;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-reset\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	sysreg = (volatile uint8_t *)ml_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (sysreg == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-reset\",\"result\":\"error\",\"phase\":\"sysreg map\",\"smc\":\"%#llx\"}\n", smc);
		return;
	}
	before = cam_s20_rd(sysreg, 0);
	requested = before ^ (1U << 3); /* verify both reset values, then restore; lanes and clocks remain disabled */
	cam_s20_wr(sysreg, 0, requested);
	during = cam_s20_rd(sysreg, 0);
	cam_s20_wr(sysreg, 0, before);
	after = cam_s20_rd(sysreg, 0);
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-reset\",\"result\":\"%s\",\"smc\":\"%#llx\","
	    "\"before\":\"%#x\",\"requested\":\"%#x\",\"during\":\"%#x\",\"after\":\"%#x\","
	    "\"restored\":%s,\"dphy_written\":false,\"sensor_streamed\":false}\n",
	    during == requested && after == before ? "ok" : "error", smc, before, requested, during, after,
	    after == before ? "true" : "false");
}

static void
cam_s20_update_bits(volatile uint8_t *bank, uint32_t offset, uint32_t mask, uint32_t value)
{
	uint32_t old = cam_s20_rd(bank, offset);
	cam_s20_wr(bank, offset, (old & ~mask) | (value & mask));
}

/* Samsung Exynos990 CONFIG_PABLO_V8_1_0 selects PHY 0x0503.0000, D-PHY.
 * Port of phy-exynos-mipi.c::__set_phy_cfg_0503_0000_dcphy for SAK3J1:
 * 4 lanes (zero-based 3), 1807 Mbps, settle 9, skew delay 3. */
static void
cam_s20_dphy_3j1_config(volatile uint8_t *dphy, volatile uint8_t *bias)
{
    static const uint32_t lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
    unsigned i;

    cam_s20_wr(dphy, 0x0000, 0);
    for (i = 0; i < 4; i++) cam_s20_wr(dphy, lane_base[i], 0);
    delay_for_interval(200, NSEC_PER_USEC);
    cam_s20_wr(bias, 0x0000, 0x10);
    cam_s20_wr(bias, 0x0004, 0x110);
    cam_s20_wr(bias, 0x0008, 0x3223);

    /* Exact __set_phy_cfg_0503_0000_dcphy() recipe, mode D/type D (CSI B/D/F). */
    cam_s20_wr(dphy, 0x0004, 0x1450);
    cam_s20_wr(dphy, 0x0008, 0x8);
    /* Samsung's loop updates 0x0010 on its first iteration. */
    cam_s20_wr(dphy, 0x0010, 0x2 | (3U << 8));
    cam_s20_wr(dphy, 0x0014, 0x8680);
    cam_s20_wr(dphy, 0x0018, 0x4000);
    cam_s20_wr(dphy, 0x0030, 0x301);
    cam_s20_wr(dphy, 0x0000, 1); /* clock enable last */

    for (i = 0; i < 4; i++) {
        uint32_t b = lane_base[i];
        cam_s20_wr(dphy, b + 0x0004, 0x1450);
        cam_s20_wr(dphy, b + 0x0010, 0x2);
        if (i < 3) cam_s20_update_bits(dphy, b + 0x0010, 3U << 8, 3U << 8);
        cam_s20_wr(dphy, b + 0x0014, 0x8680);
        cam_s20_wr(dphy, b + 0x0018, 0x4000);
        cam_s20_update_bits(dphy, b + 0x0030, 0xffU, 9);
		cam_s20_update_bits(dphy, b + 0x0030, 1U << 8, 0); /* settle_clk_sel=0 at 1807 Mbps */
        cam_s20_wr(dphy, b + 0x0034, 3);
		cam_s20_update_bits(dphy, b + 0x0040, 1U, 1U); /* skew_cal_en=1 at >=1500 Mbps */
        cam_s20_wr(dphy, b + 0x0050, 0x81a);
        cam_s20_wr(dphy, b, 1); /* data enable last */
    }
	/* __set_phy_cfg_0503_0000_dcphy() settles for 200 us after lane enable. */
	delay_for_interval(200, NSEC_PER_USEC);
}

/* Configure, inspect and exactly restore the idle DPHY; no sensor or CSIS core writes. */
void
cam_s20_csis_dphy_config_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	static const uint32_t sc_offs[] = { 0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0030 };
	static const uint32_t lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
	static const uint32_t lane_regs[] = { 0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0030, 0x0034, 0x0040, 0x0050 };
	static const uint32_t bias_offs[] = { 0x0000, 0x0004, 0x0008, 0x0010 };
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL, *dphy = NULL, *bias = NULL, *isppre = NULL;
	uint32_t sc_saved[8] = { 0 }, lane_saved[4][11] = { { 0 } }, bias_saved[3] = { 0 };
	uint32_t qcmu = 0, qcsis = 0, iso = 0, reset = 0, qcmu_after = 0, qcsis_after = 0, iso_after = 0, reset_after = 0;
	uint32_t verify = 0, lane0_cfg = 0, lane3_cfg = 0, settle_cfg = 0, i, j;
	uint8_t ldo5 = 0, ldo6 = 0;
	int ldo5_status, ldo6_status;
	uint64_t smc;
	bool cmu_changed = false, csis_changed = false, iso_changed = false, saved = false;
	bool configured = false, state_ok = false;
	char phase[32] = "map";
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	ldo5_status = redwood_spmic_read(1, 0x2e, &ldo5);
	ldo6_status = redwood_spmic_read(1, 0x2f, &ldo6);
	if (ldo5_status != 0 || ldo6_status != 0 || (ldo5 & 0xc0) != 0xc0 || (ldo6 & 0xc0) != 0xc0) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"PHY rails off\",\"ldo5s\":\"%#x\",\"ldo6s\":\"%#x\"}\n", ldo5, ldo6);
		return;
	}
	cmu = (volatile uint8_t *)ml_io_map(CMU_CSIS_PHYS, 0x8000);
	dphy = (volatile uint8_t *)ml_io_map(CSIS1_DPHY_PHYS, 0x500);
	bias = (volatile uint8_t *)ml_io_map(CSIS1_DPHY_BIAS_PHYS, 0x100);
	isppre = (volatile uint8_t *)ml_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (cmu == NULL || dphy == NULL || bias == NULL || isppre == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"map\"}\n");
		return;
	}
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	if (smc != 0) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"CSIS DTZPC\",\"smc\":\"%#llx\"}\n", smc);
		return;
	}
	qcmu = cam_s20_rd(cmu, 0x3058); qcsis = cam_s20_rd(cmu, 0x3064); iso = cam_s20_rd(pmu, 0x0730);
	if (!(qcmu & 1U)) { cam_s20_update_bits(cmu, 0x3058, 1U, 1U); cmu_changed = true; }
	cam_s20_wait_ms(2);
	if (!(qcsis & 1U)) { cam_s20_update_bits(cmu, 0x3064, 1U, 1U); csis_changed = true; }
	cam_s20_wait_ms(2);
	if (!(cam_s20_rd(cmu, 0x3058) & 1U) || !(cam_s20_rd(cmu, 0x3064) & 1U)) {
		(void)snprintf(phase, sizeof(phase), "QCH enable"); goto cleanup;
	}
	if (!(iso & 1U)) { cam_s20_update_bits(pmu, 0x0730, 1U, 1U); iso_changed = true; }
	cam_s20_wait_ms(1);
	reset = cam_s20_rd(isppre, 0);
	for (i = 0; i < 8; i++) sc_saved[i] = cam_s20_rd(dphy, sc_offs[i]);
	for (i = 0; i < 4; i++) for (j = 0; j < 11; j++) lane_saved[i][j] = cam_s20_rd(dphy, lane_base[i] + lane_regs[j]);
	for (i = 0; i < 3; i++) bias_saved[i] = cam_s20_rd(bias, bias_offs[i]);
	saved = true;
	(void)snprintf(phase, sizeof(phase), "configure");
	cam_s20_wr(isppre, 0, reset & ~(1U << 3)); /* Samsung PHY driver asserts reset around PHY set */
	cam_s20_dphy_3j1_config(dphy, bias);
	cam_s20_wr(isppre, 0, reset | (1U << 3));
	cam_s20_wait_ms(1);
	lane0_cfg = cam_s20_rd(dphy, 0x0100); lane3_cfg = cam_s20_rd(dphy, 0x0400);
	settle_cfg = cam_s20_rd(dphy, 0x0130) & 0xffU;
    /* Enable is bit 0; high status bits read back as 1 on this live PHY. */
    configured = (cam_s20_rd(dphy, 0x0000) & 1U) != 0 && (lane0_cfg & 1U) != 0 &&
        (lane3_cfg & 1U) != 0 && settle_cfg == 9U;
	if (configured) (void)snprintf(phase, sizeof(phase), "configured");
	else (void)snprintf(phase, sizeof(phase), "readback");
cleanup:
	if (saved) {
		cam_s20_wr(isppre, 0, reset & ~(1U << 3));
		for (i = 4; i > 0; i--) for (j = 11; j > 0; j--) cam_s20_wr(dphy, lane_base[i - 1] + lane_regs[j - 1], lane_saved[i - 1][j - 1]);
		for (i = 8; i > 0; i--) cam_s20_wr(dphy, sc_offs[i - 1], sc_saved[i - 1]);
		for (i = 3; i > 0; i--) cam_s20_wr(bias, bias_offs[i - 1], bias_saved[i - 1]);
		cam_s20_wr(isppre, 0, reset);
		for (i = 0; i < 8; i++) if (cam_s20_rd(dphy, sc_offs[i]) != sc_saved[i]) verify++;
		for (i = 0; i < 4; i++) for (j = 0; j < 11; j++) if (cam_s20_rd(dphy, lane_base[i] + lane_regs[j]) != lane_saved[i][j]) verify++;
		for (i = 0; i < 3; i++) if (cam_s20_rd(bias, bias_offs[i]) != bias_saved[i]) verify++;
		reset_after = cam_s20_rd(isppre, 0);
	}
	if (iso_changed) cam_s20_update_bits(pmu, 0x0730, 1U, iso & 1U);
	iso_after = cam_s20_rd(pmu, 0x0730);
	if (csis_changed) cam_s20_update_bits(cmu, 0x3064, 1U, qcsis & 1U);
	qcsis_after = cam_s20_rd(cmu, 0x3064);
	if (cmu_changed) cam_s20_update_bits(cmu, 0x3058, 1U, qcmu & 1U);
	qcmu_after = cam_s20_rd(cmu, 0x3058);
	state_ok = saved && verify == 0 && reset_after == reset && ((iso_after ^ iso) & 1U) == 0 &&
	    ((qcsis_after ^ qcsis) & 1U) == 0 && ((qcmu_after ^ qcmu) & 1U) == 0;
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-dphy-config\",\"result\":\"%s\",\"phase\":\"%s\",\"sensor_streamed\":false,"
	    "\"clock_enabled\":%s,\"lane0_enable\":\"%#x\",\"lane3_enable\":\"%#x\",\"settle\":\"%#x\","
	    "\"restore_mismatches\":%u,\"state_restored\":%s,\"smc\":\"%#llx\"}\n",
	    configured && state_ok ? "ok" : "error", phase, saved ? "true" : "false",
	    lane0_cfg, lane3_cfg, settle_cfg,
	    verify, state_ok ? "true" : "false", smc);
}

/* Read the two S2MPS22 rails required by the Exynos990 CSIS PHY. This is
 * intentionally read-only: no DPHY test is safe until the prior disconnect is
 * understood and these rails' baseline is known. */
static void
cam_s20_csis_rails(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	uint8_t ldo5 = 0, ldo6 = 0;
	int r5, r6;
	if (cap == 0) return;
	r5 = redwood_spmic_read(1, 0x2e, &ldo5); /* S2MPS22 LDO5S, 0.85 V */
	r6 = redwood_spmic_read(1, 0x2f, &ldo6); /* S2MPS22 LDO6S, 1.8 V */
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-phy-rails\",\"result\":\"%s\",\"ldo5s\":\"%#x\",\"ldo5s_status\":%d,"
	    "\"ldo6s\":\"%#x\",\"ldo6s_status\":%d,\"writes\":false}\n",
	    r5 == 0 && r6 == 0 ? "ok" : "error", ldo5, r5, ldo6, r6);
}

#define CAM_3J1_BURST_ADDR 0xf0000001U
#define CAM_3J1_BURST_DATA 0xf0000002U
#define CAM_3J1_DELAY      0xf0000003U

static bool
cam_3j1_write16(struct redwood_bbi2c *bus, uint16_t reg, uint16_t value)
{
	uint8_t bytes[4] = { reg >> 8, reg, value >> 8, value };
	return redwood_bbi2c_xfer(bus, 0x10, bytes, sizeof(bytes), NULL, 0);
}

static bool
cam_3j1_write8(struct redwood_bbi2c *bus, uint16_t reg, uint8_t value)
{
	uint8_t bytes[3] = { reg >> 8, reg, value };
	return redwood_bbi2c_xfer(bus, 0x10, bytes, sizeof(bytes), NULL, 0);
}

static bool
cam_3j1_read16(struct redwood_bbi2c *bus, uint16_t reg, uint16_t *value)
{
	uint8_t address[2] = { reg >> 8, reg }, data[2] = { 0 };
	if (value == NULL || !redwood_bbi2c_xfer(bus, 0x10, address, sizeof(address), data, sizeof(data))) return false;
	*value = (uint16_t)((data[0] << 8) | data[1]);
	return true;
}

static bool
cam_3j1_read8(struct redwood_bbi2c *bus, uint16_t reg, uint8_t *value)
{
	uint8_t address[2] = { reg >> 8, reg }, data = 0;
	if (value == NULL || !redwood_bbi2c_xfer(bus, 0x10, address, sizeof(address), &data, sizeof(data))) return false;
	*value = data;
	return true;
}

static bool
cam_3j1_flush_burst(struct redwood_bbi2c *bus, uint8_t *bytes, uint32_t words)
{
	return words == 0 || redwood_bbi2c_xfer(bus, 0x10, bytes, 2 + 2 * words, NULL, 0);
}

/* Execute Samsung's three-word setfile records. Preserve its 200-word global burst. */
static bool
cam_3j1_apply(struct redwood_bbi2c *bus, const uint32_t *table, uint32_t count)
{
	uint8_t burst[402]; /* Samsung's global microcode is one 200-word I2C transfer. */
	uint32_t i, nburst = 0;
	bool in_burst = false;
	/* Samsung sensor_cis_set_registers() waits 3 ms before every setfile. */
	cam_s20_wait_ms(3);

	for (i = 0; i + 2 < count; i += 3) {
		uint32_t reg = table[i], value = table[i + 1], width = table[i + 2];
		if (reg == CAM_3J1_BURST_ADDR) {
			if (in_burst && !cam_3j1_flush_burst(bus, burst, nburst)) return false;
			burst[0] = value >> 8;
			burst[1] = value;
			nburst = 0;
			in_burst = true;
		} else if (reg == CAM_3J1_BURST_DATA && in_burst) {
			if (nburst == 200) {
				if (!cam_3j1_flush_burst(bus, burst, nburst)) return false;
				nburst = 0;
			}
			burst[2 + 2 * nburst] = value >> 8;
			burst[3 + 2 * nburst] = value;
			nburst++;
		} else {
			if (in_burst) {
				if (!cam_3j1_flush_burst(bus, burst, nburst)) return false;
				in_burst = false;
				nburst = 0;
			}
			if (reg == CAM_3J1_DELAY) {
				delay_for_interval(value, NSEC_PER_USEC);
			} else if (width == 2) {
				if (reg > 0xffff || value > 0xffff || !cam_3j1_write16(bus, (uint16_t)reg, (uint16_t)value)) return false;
			} else {
				return false;
			}
		}
	}
	return !in_burst || cam_3j1_flush_burst(bus, burst, nburst);
}

/* Apply the vendor global table and a binned 30 fps mode, verify output dimensions, then power back down. */
static void
cam_s20_front_set_mode(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	struct redwood_bbi2c bus = { .name = "SAK3J1 HSI2C1 gpp0-2/3", .scl = 2, .sda = 3, .half_us = 4 };
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x4000);
	uint8_t p[5] = { 0 };
	uint32_t rcon = cam_s20_rd(reset, REDWOOD_GPIO_CON), rdat = cam_s20_rd(reset, REDWOOD_GPIO_DAT);
	uint32_t rpud = cam_s20_rd(reset, REDWOOD_GPIO_PUD), mcon = cam_s20_rd(mclk, REDWOOD_GPIO_CON);
	uint32_t mdat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT), mpud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	uint32_t clock[4] = { 0 }, offsets[4] = { CMU_CIS_CLK1_MUX, CMU_CIS_CLK1_DIV, CMU_CIS_CLK1_GATE, CMU_CIS_CLK1_QCH };
	uint8_t stream = 0;
	uint16_t id = 0, width = 0, height = 0, frame_length = 0;
	char phase[48] = "snapshot";
	bool rails = false, bus_ready = false, sensor_present = false, clock_ready = false, configured = false;
	unsigned i;
	if (cap == 0) return;
	if (cmu == NULL) { (void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"result\":\"error\",\"phase\":\"CMU map\"}\n"); return; }
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &p[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &p[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &p[2]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &p[3]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &p[4])) goto cleanup;
	rails = true;
	(void)snprintf(phase, sizeof(phase), "BUCK2 and VT rails");
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE);
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO12, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, 0x80)) goto cleanup;
	cam_s20_wait_ms(1);
	(void)snprintf(phase, sizeof(phase), "I2C bus setup");
	bus_ready = true;
	bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	if (!redwood_bbi2c_setup(&bus, true, out, cap)) goto cleanup;
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu, offsets[i]);
	cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0] & ~1U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3] | 3U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 1, 0);
	redwood_gpio_func(mclk, 1, 2);
	redwood_gpio_pull(mclk, 1, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(10);
	(void)redwood_gpio_set(reset, 0, 1);
	cam_s20_wait_ms(9);
	(void)snprintf(phase, sizeof(phase), "SAK3J1 ID check");
	if (!cam_3j1_read16(&bus, 0x0000, &id) || id != 0x30a1) goto cleanup;
	sensor_present = true;
	(void)snprintf(phase, sizeof(phase), "Samsung global setfile");
	if (!cam_3j1_apply(&bus, redwood_3j1_global, REDWOOD_3J1_GLOBAL_WORDS)) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "1824x1368 mode setfile");
	if (!cam_3j1_apply(&bus, redwood_3j1_1824x1368_30fps, REDWOOD_3J1_1824X1368_30FPS_WORDS)) goto cleanup;
	if (!cam_3j1_read16(&bus, 0x034c, &width) || !cam_3j1_read16(&bus, 0x034e, &height) ||
	    !cam_3j1_read16(&bus, 0x0340, &frame_length) || !cam_3j1_read8(&bus, 0x0100, &stream)) {
		(void)snprintf(phase, sizeof(phase), "mode readback");
		goto cleanup;
	}
	configured = width == 1824 && height == 1368 && stream == 0;
	if (!configured) (void)snprintf(phase, sizeof(phase), "mode verification");
cleanup:
	if (sensor_present) (void)cam_3j1_write16(&bus, 0x0100, 0);
	(void)redwood_gpio_set(reset, 0, 0);
	if (clock_ready) {
		(void)redwood_gpio_set(mclk, 1, 0);
		redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0]);
	}
	if (bus_ready) redwood_bbi2c_restore(&bus);
	if (rails) {
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, p[4] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, p[3] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, p[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, p[1] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p[0] & 0xc0);
	}
	redwood_gpio_restore(reset, 0, rcon & 0xfU, rpud & 0xfU, rdat & 1U);
	redwood_gpio_restore(mclk, 1, mcon & (0xfU << 4), mpud & (0xfU << 4), mdat & 2U);
	if (configured) {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"mode-setup\",\"result\":\"ok\",\"width\":%u,\"height\":%u,\"frame_length\":%u,\"stream_on\":false,\"state_restored\":true}\n",
		    width, height, frame_length);
	} else {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"mode-setup\",\"result\":\"error\",\"phase\":\"%s\",\"id\":\"0x%04x\",\"width\":%u,\"height\":%u,\"stream\":%u,\"state_restored\":true}\n",
		    phase, id, width, height, (unsigned)stream);
	}
}

/* One bounded optical-path proof: SAK3J1 -> Exynos990 DPHY -> CSIS1 frame counter.
 * No DMA or userspace buffers are enabled here. Every sensor/receiver setting is restored. */
static void
cam_s20_front_stream_probe(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	static const uint32_t phy_sc_offs[] = { 0x0000, 0x0004, 0x0008, 0x0010, 0x0014, 0x0018, 0x0030 };
	static const uint32_t phy_lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
	static const uint32_t phy_lane_offs[] = { 0x0000, 0x0004, 0x0010, 0x0014, 0x0018, 0x0030, 0x0034, 0x0040, 0x0050 };
	static const uint32_t phy_bias_offs[] = { 0x0000, 0x0004, 0x0008 };
	static const uint32_t csi_offs[] = { 0x0004, 0x0008, 0x000c, 0x0040, 0x0044, 0x0050, 0x0054, 0x0600, 0x0690, 0x0704 };
	struct redwood_bbi2c bus = { .name = "SAK3J1 HSI2C1 gpp0-2/3", .scl = 2, .sda = 3, .half_us = 4 };
	volatile uint8_t *pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu_top = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x4000);
	volatile uint8_t *cmu = (volatile uint8_t *)ml_io_map(CMU_CSIS_PHYS, 0x8000);
	volatile uint8_t *csi = (volatile uint8_t *)ml_io_map(CSIS1_PHYS, 0x1000);
	volatile uint8_t *dphy = (volatile uint8_t *)ml_io_map(CSIS1_DPHY_PHYS, 0x500);
	volatile uint8_t *bias = (volatile uint8_t *)ml_io_map(CSIS1_DPHY_BIAS_PHYS, 0x100);
	volatile uint8_t *isppre = (volatile uint8_t *)ml_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	uint8_t rails[5] = { 0 }, stream = 0, sensor_fc_before = 0, sensor_fc_after = 0;
	uint32_t phy_sc[7] = { 0 }, phy_lane[4][9] = { { 0 } }, phy_bias[3] = { 0 }, csi_saved[10] = { 0 };
	uint32_t clock[4] = { 0 }, clock_offs[4] = { CMU_CIS_CLK1_MUX, CMU_CIS_CLK1_DIV, CMU_CIS_CLK1_GATE, CMU_CIS_CLK1_QCH };
	uint32_t rcon, rdat, rpud, mcon, mdat, mpud;
	uint32_t reset_sys = 0;
	uint32_t mask0_saved = 0, mask1_saved = 0;
	uint32_t cmu_qch = 0, csi_qch = 0, dma_qch = 0, top_qch = 0, iso = 0;
	uint32_t before = 0, after = 0, irq = 0, fs_irq = 0, fe_irq = 0, phy_status = 0;
	uint32_t phy_status_or = 0, phy_status_and = ~0U, phy_active_samples = 0, irq_seen = 0;
	uint32_t cmn_live = 0, phy_live = 0, config_live = 0, config1_live = 0, resol_live = 0;
	uint32_t shadow1_live = 0, lrte_live = 0, dphy_clk = 0, lane0_live = 0, lane3_live = 0, i, j;
	uint32_t phy_sc_time1 = 0, phy_sd0_ana6 = 0, phy_sd0_ana7 = 0;
	uint32_t phy_sc_ana2 = 0, phy_sd0_ana2 = 0, phy_sd3_ana2 = 0;
	uint32_t phy_sd0_settle = 0, phy_sd0_deskew = 0, phy_bias0 = 0, phy_bias1 = 0, phy_bias2 = 0;
	uint32_t cmu_qch_live = 0, csi_qch_live = 0, dma_qch_live = 0, top_qch_live = 0;
	uint32_t mclk_mux_live = 0, mclk_div_live = 0, mclk_gate_live = 0, mclk_qch_live = 0;
	uint32_t csis_bus_mux_live = 0, pmu_iso_live = 0, phy_reset_live = 0, irq_mask_live = 0;
	uint8_t rails_live[5] = { 0 }, pattern_live = 0;
	uint16_t sid = 0, sensor_fmt = 0, sensor_lanes = 0, sensor_mipi_pll = 0;
	uint16_t sensor_frame_length = 0, sensor_line_length = 0, sensor_crop_x = 0;
	uint16_t sensor_crop_end_x = 0, sensor_scaler = 0, sensor_binning = 0, sensor_pll2 = 0;
	uint16_t sensor_code[5] = { 0 };
	static const uint16_t sensor_code_addrs[5] = { 0x918c, 0x918e, 0x9190, 0x9192, 0x9256 };
	static const uint16_t diag_regs[] = { 0x0850, 0x0b02, 0x0b04, 0x0b06, 0x0116, 0x0118,
	    0x0812, 0x0814, 0x080e, 0x0810, 0x0816, 0x6214, 0x6244, 0x6b5e, 0x6b60,
	    0x0a7a, 0x0a7c, 0x0a76, 0x0202, 0x0204, 0x020e, 0x0104 };
	uint16_t diag_vals[sizeof(diag_regs) / sizeof(diag_regs[0])] = { 0 };
	char phase[40] = "map";
	bool rails_saved = false, pins_saved = false, bus_ready = false, clock_ready = false, sensor_on = false;
	bool phy_saved = false, csi_saved_ok = false, irq_masks_saved = false, qch_changed = false, dma_qch_changed = false, top_qch_changed = false;
	bool iso_changed = false, result = false;
	bool pd_mode = n > 0 && (v[0] == 1 || v[0] == 2 || v[0] == 3 || v[0] == 4);
	bool stock_gain = n > 0 && (v[0] == 2 || v[0] == 4);
	bool test_pattern = n > 0 && v[0] == 3;
	uint64_t smc = 0;
	if (cap == 0) return;
	if (n > 0 && v[0] == 4) bus.half_us = 2; /* 250 kHz software I2C, closer to Samsung HSI2C. */
	if (pmu == NULL || cmu_top == NULL || cmu == NULL || csi == NULL || dphy == NULL || bias == NULL || isppre == NULL) goto done;
	if (!(cam_s20_rd(pmu, 0x1d84) & 1U)) { strlcpy(phase, "CSIS domain off", sizeof(phase)); goto done; }
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	if (smc != 0) { strlcpy(phase, "CSIS secure access", sizeof(phase)); goto done; }
	cmu_qch = cam_s20_rd(cmu, 0x3058); csi_qch = cam_s20_rd(cmu, 0x3064);
	dma_qch = cam_s20_rd(cmu, 0x3078); top_qch = cam_s20_rd(cmu, 0x307c); iso = cam_s20_rd(pmu, 0x0730);
	if (!(cmu_qch & 1U)) { cam_s20_update_bits(cmu, 0x3058, 1U, 1U); qch_changed = true; }
	/* Samsung exynos9830_is_sensor_iclk_cfg enables the shared WDMA and PDP-top clocks too. */
	if (!(dma_qch & 1U)) { cam_s20_update_bits(cmu, 0x3078, 1U, 1U); dma_qch_changed = true; }
	if (!(top_qch & 1U)) { cam_s20_update_bits(cmu, 0x307c, 1U, 1U); top_qch_changed = true; }
	if (!(csi_qch & 1U)) { cam_s20_update_bits(cmu, 0x3064, 1U, 1U); qch_changed = true; }
	if (!(iso & 1U)) { cam_s20_update_bits(pmu, 0x0730, 1U, 1U); iso_changed = true; }
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &rails[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &rails[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &rails[2]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &rails[3]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &rails[4])) { strlcpy(phase, "sensor rail snapshot", sizeof(phase)); goto cleanup; }
	rails_saved = true;
	rcon = cam_s20_rd(reset, REDWOOD_GPIO_CON); rdat = cam_s20_rd(reset, REDWOOD_GPIO_DAT); rpud = cam_s20_rd(reset, REDWOOD_GPIO_PUD);
	mcon = cam_s20_rd(mclk, REDWOOD_GPIO_CON); mdat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT); mpud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	/* x1slte SAK3J1 DTS: BUCK2, LDO16/11/6/5; no GPC0-5 route-select rail/step. */
	pins_saved = true;
	(void)redwood_gpio_set(reset, 0, 0); redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE); redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	strlcpy(phase, "sensor rails", sizeof(phase));
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, 0x80)) goto cleanup;
	bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	if (!redwood_bbi2c_setup(&bus, true, out, cap)) { strlcpy(phase, "sensor I2C setup", sizeof(phase)); goto cleanup; }
	bus_ready = true;
	/* Samsung order: on_i2c (10 ms), reset high, MCLK pin/clock on (9 ms). */
	cam_s20_wait_ms(10);
	(void)redwood_gpio_set(reset, 0, 1);
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu_top, clock_offs[i]);
	cam_s20_wr(cmu_top, CMU_CIS_CLK1_MUX, clock[0] & ~1U); cam_s20_wr(cmu_top, CMU_CIS_CLK1_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu_top, CMU_CIS_CLK1_QCH, clock[3] | 3U); cam_s20_wr(cmu_top, CMU_CIS_CLK1_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 1, 0); redwood_gpio_func(mclk, 1, 2); redwood_gpio_pull(mclk, 1, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(9);
	strlcpy(phase, "sensor ID", sizeof(phase));
	if (!cam_3j1_read16(&bus, 0x0000, &sid) || sid != 0x30a1) goto cleanup;
	strlcpy(phase, "sensor mode", sizeof(phase));
	if (!cam_3j1_apply(&bus, redwood_3j1_global, REDWOOD_3J1_GLOBAL_WORDS) ||
	    !(pd_mode ? cam_3j1_apply(&bus, redwood_3j1_1824x1368_30fps_pd, REDWOOD_3J1_1824X1368_30FPS_PD_WORDS) :
	        cam_3j1_apply(&bus, redwood_3j1_1824x1368_30fps, REDWOOD_3J1_1824X1368_30FPS_WORDS))) goto cleanup;
	/* Mirror sensor_3j1_cis_mode_change() and cis_set_size(): EMB off, 2x2 binning crop. */
	if (!cam_3j1_write8(&bus, 0x0118, 0)) goto cleanup;
	/* cis_set_size() selects the sensor's 0x2000 register page before crop writes. */
	if (!cam_3j1_write16(&bus, 0x6028, 0x2000)) goto cleanup;
	if (!pd_mode && (
	    !cam_3j1_write16(&bus, 0x0344, 164) || !cam_3j1_write16(&bus, 0x0346, 0) ||
	    !cam_3j1_write16(&bus, 0x0348, 3811) || !cam_3j1_write16(&bus, 0x034a, 2735) ||
	    !cam_3j1_write16(&bus, 0x034c, 1824) || !cam_3j1_write16(&bus, 0x034e, 1368) ||
	    !cam_3j1_write16(&bus, 0x0380, 1) || !cam_3j1_write16(&bus, 0x0382, 3) ||
	    !cam_3j1_write16(&bus, 0x0384, 1) || !cam_3j1_write16(&bus, 0x0386, 3) ||
	    !cam_3j1_write8(&bus, 0x0900, 1) || !cam_3j1_write8(&bus, 0x0901, 0x22) ||
	    !cam_3j1_write16(&bus, 0x0400, 0) || !cam_3j1_write16(&bus, 0x0404, 0x10))) goto cleanup;
	/* Samsung's mode-change path applies analog/digital gains before stream.
	 * Option 2 tests those exact clamp values while retaining mode 8. */
	if (stock_gain && (!cam_3j1_write16(&bus, 0x0204, 0x0200) ||
	    !cam_3j1_write16(&bus, 0x31e8, 0) ||
	    !cam_3j1_write16(&bus, 0x020e, 0x1000))) goto cleanup;
	if (test_pattern && !cam_3j1_write8(&bus, 0x0601, 1)) goto cleanup;
	for (i = 0; i < 7; i++) phy_sc[i] = cam_s20_rd(dphy, phy_sc_offs[i]);
	for (i = 0; i < 4; i++) for (j = 0; j < 9; j++) phy_lane[i][j] = cam_s20_rd(dphy, phy_lane_base[i] + phy_lane_offs[j]);
	for (i = 0; i < 3; i++) phy_bias[i] = cam_s20_rd(bias, phy_bias_offs[i]);
	phy_saved = true;
	for (i = 0; i < 10; i++) csi_saved[i] = cam_s20_rd(csi, csi_offs[i]);
	csi_saved_ok = true; reset_sys = cam_s20_rd(isppre, 0);
	mask0_saved = cam_s20_rd(csi, 0x0010); mask1_saved = cam_s20_rd(csi, 0x0018);
	irq_masks_saved = true;
	strlcpy(phase, "receiver setup", sizeof(phase));
	/* Samsung runs csi_hw_reset() during csi_init, before phy_set(). */
	cam_s20_wr(csi, 0x0004, csi_saved[0] | (1U << 1));
	for (i = 0; i < 10 && (cam_s20_rd(csi, 0x0004) & (1U << 1)); i++) delay_for_interval(10, NSEC_PER_USEC);
	if (cam_s20_rd(csi, 0x0004) & (1U << 1)) { strlcpy(phase, "CSIS reset timeout", sizeof(phase)); goto cleanup; }
	cam_s20_wr(isppre, 0, reset_sys & ~(1U << 3)); cam_s20_dphy_3j1_config(dphy, bias); cam_s20_wr(isppre, 0, reset_sys | (1U << 3));
	/* Stock CSIS1 debug dump before sensor stream: CMN_CTRL=0x4b01.
	 * The 1807 Mbps SAK3J1 mode uses deskew level 2 and VC interleave mode 2;
	 * omitting either leaves the receiver in a different mode from Samsung's. */
	cam_s20_wr(csi, 0x0004,
	    (csi_saved[0] & ~((7U << 13) | (3U << 10) | (3U << 8) | (1U << 21) | 3U)) |
	    (2U << 13) | (2U << 10) | (3U << 8));
	cam_s20_wr(csi, 0x0008, csi_saved[1] | (1U << 4)); /* stock CLK_CTRL=0x10 */
	cam_s20_wr(csi, 0x0040, (0x2bU << 2) | (2U << 12));
	cam_s20_wr(csi, 0x0044, (1368U << 16) | 1824U);
	/* x1slte mode 8 sends VC1 RAW10 auxiliary rows, 1824x342. */
	cam_s20_wr(csi, 0x0050, (1U << 16) | (0x2bU << 2) | (2U << 12));
	cam_s20_wr(csi, 0x0054, (342U << 16) | 1824U);
	cam_s20_wr(csi, 0x0600, csi_saved[7] & ~(1U << 31)); /* LRTE_DISABLE */
	cam_s20_wr(csi, 0x0690, csi_saved[8] | (1U << 23)); /* CSIS_CTRL_PIXEL_ALIGN_MODE=1 */
	cam_s20_wr(csi, 0x000c, 0xfU); /* update all VC shadow registers */
	cam_s20_wr(csi, 0x0704, (csi_saved[9] & ~0x1fU) | 0x1fU);
	/* Samsung request_irq() enables link frame/error sources before CSI_EN. The
	 * iOS GIC leaves SPI 215 masked; poll the raw source register here. */
	cam_s20_wr(csi, 0x0010, 0x00fff07fU);
	cam_s20_wr(csi, 0x0018, 0x0000007fU);
	cam_s20_wr(csi, 0x0004, cam_s20_rd(csi, 0x0004) | 1U); /* CSI_EN; DPHY is already configured */
	cam_s20_wait_ms(2);
	strlcpy(phase, "sensor stream", sizeof(phase));
	/* Samsung stream_on(): choose the BINNING default MIPI table before releasing hold, then wait 8 ms. */
	if (!cam_3j1_write16(&bus, 0x0136, 0x1a00) || !cam_3j1_write16(&bus, 0x0300, 7) ||
	    !cam_3j1_write16(&bus, 0x0304, 4) || !cam_3j1_write16(&bus, 0x0306, 0xd9) ||
	    !cam_3j1_write16(&bus, 0x030c, 0) || !cam_3j1_write16(&bus, 0x030e, 4) ||
	    !cam_3j1_write16(&bus, 0x0310, 0x116) || !cam_3j1_write16(&bus, 0x0312, 1)) goto cleanup;
	/* SAK3J1's Samsung stream-on sequence configures dual-sync before 0x0100. */
	if (!cam_3j1_write8(&bus, 0x0104, 0)) goto cleanup;
	cam_s20_wait_ms(8);
	if (!cam_3j1_write16(&bus, 0x0a70, 0x0001) || !cam_3j1_write16(&bus, 0x0a72, 0x0100) ||
	    !cam_3j1_write8(&bus, 0x0100, 1)) goto cleanup;
	sensor_on = true;
	if (!cam_3j1_write8(&bus, 0x021e, 0)) goto cleanup; /* non-WDR mode */
	if (!cam_3j1_read8(&bus, 0x0100, &stream) || stream != 1) goto cleanup;
	(void)cam_3j1_read8(&bus, 0x0005, &sensor_fc_before);
	/* CSIS v5.3 FRM_CNT_CH0 is 0x0500 (v5.2 used 0x0100). */
	before = cam_s20_rd(csi, 0x0500);
	for (i = 0; i < 500; i++) {
		uint32_t status = cam_s20_rd(csi, 0x0700);
		phy_status_or |= status;
		phy_status_and &= status;
		if ((status & 0xf1U) != 0xf1U) phy_active_samples++;
		irq_seen |= cam_s20_rd(csi, 0x0014);
		cam_s20_wait_ms(1);
	}
	after = cam_s20_rd(csi, 0x0500);
	(void)cam_3j1_read8(&bus, 0x0005, &sensor_fc_after);
	irq = cam_s20_rd(csi, 0x0014); fs_irq = cam_s20_rd(csi, 0x0024); fe_irq = cam_s20_rd(csi, 0x002c);
	cmn_live = cam_s20_rd(csi, 0x0004); phy_live = cam_s20_rd(csi, 0x0704);
	config_live = cam_s20_rd(csi, 0x0040); config1_live = cam_s20_rd(csi, 0x0050);
	shadow1_live = cam_s20_rd(csi, 0x0250); lrte_live = cam_s20_rd(csi, 0x0600);
	resol_live = cam_s20_rd(csi, 0x0044); phy_status = cam_s20_rd(csi, 0x0700);
	dphy_clk = cam_s20_rd(dphy, 0); lane0_live = cam_s20_rd(dphy, 0x100); lane3_live = cam_s20_rd(dphy, 0x400);
	phy_sc_time1 = cam_s20_rd(dphy, 0x34);
	phy_sd0_ana6 = cam_s20_rd(dphy, 0x120); phy_sd0_ana7 = cam_s20_rd(dphy, 0x124);
	phy_sc_ana2 = cam_s20_rd(dphy, 0x10); phy_sd0_ana2 = cam_s20_rd(dphy, 0x110);
	phy_sd3_ana2 = cam_s20_rd(dphy, 0x410); phy_sd0_settle = cam_s20_rd(dphy, 0x130);
	phy_sd0_deskew = cam_s20_rd(dphy, 0x140);
	phy_bias0 = cam_s20_rd(bias, 0); phy_bias1 = cam_s20_rd(bias, 4); phy_bias2 = cam_s20_rd(bias, 8);
	cmu_qch_live = cam_s20_rd(cmu, 0x3058); csi_qch_live = cam_s20_rd(cmu, 0x3064);
	dma_qch_live = cam_s20_rd(cmu, 0x3078); top_qch_live = cam_s20_rd(cmu, 0x307c);
	(void)cam_3j1_read16(&bus, 0x0112, &sensor_fmt); (void)cam_3j1_read16(&bus, 0x0114, &sensor_lanes);
	(void)cam_3j1_read16(&bus, 0x0306, &sensor_mipi_pll);
	(void)cam_3j1_read16(&bus, 0x0310, &sensor_pll2);
	(void)cam_3j1_read16(&bus, 0x0340, &sensor_frame_length);
	(void)cam_3j1_read16(&bus, 0x0342, &sensor_line_length);
	(void)cam_3j1_read16(&bus, 0x0344, &sensor_crop_x);
	(void)cam_3j1_read16(&bus, 0x0348, &sensor_crop_end_x);
	(void)cam_3j1_read16(&bus, 0x0404, &sensor_scaler);
	(void)cam_3j1_read16(&bus, 0x0900, &sensor_binning);
	(void)cam_3j1_read8(&bus, 0x0601, &pattern_live);
	for (i = 0; i < sizeof(diag_regs) / sizeof(diag_regs[0]); i++)
		(void)cam_3j1_read16(&bus, diag_regs[i], &diag_vals[i]);
	/* Samsung sensors use 0x602C/0x602E for indirect reads; 0x6028/0x602A
	 * select the write address and yielded unrelated alternating values. */
	if (cam_3j1_write16(&bus, 0x602c, 0x2000)) {
		for (i = 0; i < 5; i++) {
			if (cam_3j1_write16(&bus, 0x602e, sensor_code_addrs[i]))
				(void)cam_3j1_read16(&bus, 0x6f12, &sensor_code[i]);
		}
	}
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &rails_live[0]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &rails_live[1]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &rails_live[2]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &rails_live[3]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &rails_live[4]);
	mclk_mux_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_MUX);
	mclk_div_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_DIV);
	mclk_gate_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_GATE);
	mclk_qch_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_QCH);
	csis_bus_mux_live = cam_s20_rd(cmu, 0x0600);
	pmu_iso_live = cam_s20_rd(pmu, 0x0730);
	phy_reset_live = cam_s20_rd(isppre, 0);
	irq_mask_live = cam_s20_rd(csi, 0x0010);
	result = after != before;
	strlcpy(phase, result ? "frames observed" : "no CSIS frames", sizeof(phase));
cleanup:
	if (sensor_on || stream) (void)cam_3j1_write8(&bus, 0x0100, 0);
	if (irq_masks_saved) { cam_s20_wr(csi, 0x0010, mask0_saved); cam_s20_wr(csi, 0x0018, mask1_saved); }
	if (csi_saved_ok) cam_s20_wr(csi, 0x0004, cam_s20_rd(csi, 0x0004) & ~1U);
	if (csi_saved_ok) for (i = 10; i > 0; i--) cam_s20_wr(csi, csi_offs[i - 1], csi_saved[i - 1]);
	if (phy_saved) {
		cam_s20_wr(dphy, 0, 0);
		for (i = 4; i > 0; i--) for (j = 9; j > 0; j--) cam_s20_wr(dphy, phy_lane_base[i - 1] + phy_lane_offs[j - 1], phy_lane[i - 1][j - 1]);
		for (i = 7; i > 0; i--) cam_s20_wr(dphy, phy_sc_offs[i - 1], phy_sc[i - 1]);
		for (i = 3; i > 0; i--) cam_s20_wr(bias, phy_bias_offs[i - 1], phy_bias[i - 1]);
		cam_s20_wr(isppre, 0, reset_sys);
	}
	if (clock_ready) { (void)redwood_gpio_set(mclk, 1, 0); redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu_top, CMU_CIS_CLK1_GATE, clock[2]); cam_s20_wr(cmu_top, CMU_CIS_CLK1_QCH, clock[3]);
		cam_s20_wr(cmu_top, CMU_CIS_CLK1_DIV, clock[1]); cam_s20_wr(cmu_top, CMU_CIS_CLK1_MUX, clock[0]); }
	if (pins_saved) (void)redwood_gpio_set(reset, 0, 0);
	if (bus_ready) redwood_bbi2c_restore(&bus);
	if (rails_saved) { (void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, rails[4] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, rails[3] & 0x80); (void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, rails[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, rails[1] & 0x80); (void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, rails[0] & 0xc0); }
	if (pins_saved) {
		redwood_gpio_restore(reset, 0, rcon & 0xfU, rpud & 0xfU, rdat & 1U);
		redwood_gpio_restore(mclk, 1, mcon & (0xfU << 4), mpud & (0xfU << 4), mdat & 2U);
	}
	if (iso_changed) cam_s20_update_bits(pmu, 0x0730, 1U, iso & 1U);
	if (qch_changed) { cam_s20_update_bits(cmu, 0x3064, 1U, csi_qch & 1U); cam_s20_update_bits(cmu, 0x3058, 1U, cmu_qch & 1U); }
	if (top_qch_changed) cam_s20_update_bits(cmu, 0x307c, 1U, top_qch & 1U);
	if (dma_qch_changed) cam_s20_update_bits(cmu, 0x3078, 1U, dma_qch & 1U);
done:
	(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"bounded-CSIS-stream\",\"result\":\"%s\",\"phase\":\"%s\",\"sensor_stream\":%u,\"sensor_frame_counter_before\":%u,\"sensor_frame_counter_after\":%u,\"sensor_raw_format\":\"%#x\",\"sensor_lane_mode\":\"%#x\",\"sensor_mipi_pll\":\"%#x\",\"sensor_pll2\":\"%#x\",\"sensor_frame_length\":\"%#x\",\"sensor_line_length\":\"%#x\",\"sensor_crop_x\":\"%#x\",\"sensor_crop_end_x\":\"%#x\",\"sensor_scaler\":\"%#x\",\"sensor_binning\":\"%#x\",\"sensor_code\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"pattern\":%u,\"rails\":[%u,%u,%u,%u,%u],\"mclk\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"bus_mux\":\"%#x\",\"pmu_iso\":\"%#x\",\"phy_reset\":\"%#x\",\"irq_mask\":\"%#x\",\"frames_before\":%u,\"frames_after\":%u,\"irq_src0\":\"%#x\",\"irq_seen\":\"%#x\",\"frame_start_irq\":\"%#x\",\"frame_end_irq\":\"%#x\",\"csis_cmn\":\"%#x\",\"csis_phy_ctrl\":\"%#x\",\"csis_config\":\"%#x\",\"csis_config1\":\"%#x\",\"csis_shadow1\":\"%#x\",\"csis_lrte\":\"%#x\",\"csis_resolution\":\"%#x\",\"phy_status\":\"%#x\",\"phy_status_or\":\"%#x\",\"phy_status_and\":\"%#x\",\"phy_active_samples\":%u,\"dphy_clock\":\"%#x\",\"dphy_lane0\":\"%#x\",\"dphy_lane3\":\"%#x\",\"phy_sc_time1\":\"%#x\",\"phy_sd0_ana6\":\"%#x\",\"phy_sd0_ana7\":\"%#x\",\"phy_sc_ana2\":\"%#x\",\"phy_sd0_ana2\":\"%#x\",\"phy_sd3_ana2\":\"%#x\",\"phy_sd0_settle\":\"%#x\",\"phy_sd0_deskew\":\"%#x\",\"phy_bias0\":\"%#x\",\"phy_bias1\":\"%#x\",\"phy_bias2\":\"%#x\",\"qch_csi_bus\":\"%#x\",\"qch_csis1\":\"%#x\",\"qch_wdma\":\"%#x\",\"qch_pdp_top\":\"%#x\",\"state_restored\":true}\n",
	    result ? "ok" : "error", phase, stream, sensor_fc_before, sensor_fc_after, sensor_fmt, sensor_lanes, sensor_mipi_pll, sensor_pll2, sensor_frame_length, sensor_line_length, sensor_crop_x, sensor_crop_end_x, sensor_scaler, sensor_binning, sensor_code[0], sensor_code[1], sensor_code[2], sensor_code[3], sensor_code[4], pattern_live, rails_live[0], rails_live[1], rails_live[2], rails_live[3], rails_live[4], mclk_mux_live, mclk_div_live, mclk_gate_live, mclk_qch_live, csis_bus_mux_live, pmu_iso_live, phy_reset_live, irq_mask_live, before, after, irq, irq_seen, fs_irq, fe_irq, cmn_live, phy_live, config_live, config1_live, shadow1_live, lrte_live, resol_live, phy_status, phy_status_or, phy_status_and, phy_active_samples, dphy_clk, lane0_live, lane3_live, phy_sc_time1, phy_sd0_ana6, phy_sd0_ana7, phy_sc_ana2, phy_sd0_ana2, phy_sd3_ana2, phy_sd0_settle, phy_sd0_deskew, phy_bias0, phy_bias1, phy_bias2, cmu_qch_live, csi_qch_live, dma_qch_live, top_qch_live);
	if (sensor_on) {
		size_t used = strlen(out);
		for (i = 0; i < sizeof(diag_regs) / sizeof(diag_regs[0]) && used < cap; i++) {
			int written = snprintf(out + used, cap - used, "%04x=%04x%s", diag_regs[i], diag_vals[i],
			    i + 1 == sizeof(diag_regs) / sizeof(diag_regs[0]) ? "\n" : " ");
			if (written < 0) break;
			used += (size_t)written < cap - used ? (size_t)written : cap - used;
		}
	}
}

/* CSIS/DPHY streaming probe removed after 2026-11-08 receiver-path kernel panic. */

static size_t
cam_s20_pin_snapshot(char *out, size_t cap, size_t used, const char *resource,
	    uint32_t block, uint32_t bank_offset, uint32_t phys, const char *pin, unsigned index)
{
	volatile uint8_t *bank = redwood_gpio_bank(block, bank_offset);
	uint32_t con = cam_s20_rd(bank, REDWOOD_GPIO_CON);
	uint32_t dat = cam_s20_rd(bank, REDWOOD_GPIO_DAT);
	uint32_t pud = cam_s20_rd(bank, REDWOOD_GPIO_PUD);
	uint32_t drv = cam_s20_rd(bank, PINCTRL_DRV);
	uint32_t cfg = (con >> (index * 4)) & 0xf;
	uint32_t val = (dat >> index) & 1;
	uint32_t pull = (pud >> (index * 4)) & 0xf;
	uint32_t drive = (drv >> (index * 4)) & 0xf;

	if (used < cap) {
		used += (size_t)snprintf(out + used, cap - used,
		    "{\"resource\":\"%s\",\"pin\":\"%s\",\"phys\":\"%#x\",\"index\":%u,"
		    "\"function\":%u,\"value\":%u,\"pull\":%u,\"drive\":%u,"
		    "\"con_raw\":\"%#x\",\"dat_raw\":\"%#x\",\"pud_raw\":\"%#x\","
		    "\"drv_raw\":\"%#x\",\"write\":false}\n", resource, pin, phys, index,
		    cfg, val, pull, drive, con, dat, pud, drv);
	}
	return used;
}

static void
cam_s20_inventory(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	size_t used;

	if (cap == 0) {
		return;
	}
	used = (size_t)snprintf(out, cap, "camera-s20: read-only pin inventory; no camera power or clock changes\n");
	/* x1slte fragment@16 targets hsi2c_3 and contains is-2ld@10:
	 * HSI2C3 at 0x10580000 uses gpp0-6/7. */
	used = cam_s20_pin_snapshot(out, cap, used, "main.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-6", 6);
	used = cam_s20_pin_snapshot(out, cap, used, "main.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-7", 7);
	/* x1slte fragment@17 targets hsi2c_1 and contains is-3j1@10:
	 * HSI2C1 at 0x10560000 uses gpp0-2/3. HSI2C4/gpp1-0/1 is the display-PMIC bus;
	 * HSI2C45/gpm14/15 is for rear tele 2LA. */
	used = cam_s20_pin_snapshot(out, cap, used, "front.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-2", 2);
	used = cam_s20_pin_snapshot(out, cap, used, "front.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-3", 3);
	used = cam_s20_pin_snapshot(out, cap, used, "main.mclk", PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0,
	    0x107300a0U, "gpc0-0", 0);
	used = cam_s20_pin_snapshot(out, cap, used, "front.mclk", PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0,
	    0x107300a0U, "gpc0-1", 1);
	used = cam_s20_pin_snapshot(out, cap, used, "front.reset", PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11,
	    0x15c30160U, "gpm11-0", 0);
	(void)cam_s20_pin_snapshot(out, cap, used, "main.reset", PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM22,
	    0x15c302c0U, "gpm22-0", 0);
}

void
redwood_exynos_camera_s20_init(void)
{
	/* printf is not included in the phone's HTTP /log stream. Return snapshots
	 * over the existing root-only kern.redwood.poke interface instead. */
	redwood_dbgport_add("cam.s20", ": read-only S20 camera pin inventory; no writes", cam_s20_inventory);
	redwood_dbgport_add("cam.s20.id", ": one-shot rear S5K2LD model/revision read; camera rail enable bits and pins are restored", cam_s20_identify);
	redwood_dbgport_add("cam.s20.front.id", ": one-shot front SAK3J1 model/revision read; camera rail enable bits and pins are restored", cam_s20_front_identify);
	redwood_dbgport_add("cam.s20.csis1", ": read-only CSIS1 and DMA1 version/status/counter snapshot", cam_s20_csis_inventory);
	redwood_dbgport_add("cam.s20.csis.power", ": bounded CSIS power-domain and clock inventory; no DPHY or sensor access", cam_s20_csis_power_inventory);
	redwood_dbgport_add("cam.s20.csis.dphy", ": idle read-only DPHY snapshot after enabling CSIS CMU/CSIS1 QCH; no sensor or DPHY writes", cam_s20_csis_dphy_snapshot);
	redwood_dbgport_add("cam.s20.csis.secure", ": EL3 CSIS DTZPC restore only, then read-only PHY reset sysreg check", cam_s20_csis_secure_probe);
	redwood_dbgport_add("cam.s20.csis.reset", ": EL3 restore then one reset-sysreg write/read/restore; no DPHY or sensor writes", cam_s20_csis_reset_probe);
	redwood_dbgport_add("cam.s20.csis.rails", ": read-only S2MPS22 LDO5S/LDO6S state required by Exynos990 CSIS PHY", cam_s20_csis_rails);
	redwood_dbgport_add("cam.s20.csis.dphy.config", ": bounded Exynos990 PHY 0x0503.0000 setup/readback/restore after rail check; no sensor streaming", cam_s20_csis_dphy_config_probe);
	redwood_dbgport_add("cam.s20.front.mode", ": apply Samsung SAK3J1 global and 1824x1368/30 non-PDAF setfiles, verify registers, power down", cam_s20_front_set_mode);
	redwood_dbgport_add("cam.s20.front.stream", ": bounded SAK3J1 to CSIS1 frame-counter test, 500 ms, no DMA; restores sensor/receiver state", cam_s20_front_stream_probe);
}
