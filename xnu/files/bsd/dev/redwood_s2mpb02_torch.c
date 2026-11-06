/*
 * The Galaxy S20's (x1slte, SM-G980F) flashlight: the rear flash LED of the S2MPB02 (camera PMIC with a flash driver),
 * at I2C 0x59 on HSI2C_27 (gpp3-2 SCL, gpp3-3 SDA; bit-banged here, bsd/dev/redwood_bbi2c.c). From the phone's dtbo
 * (fragment@5, s2mpb02_pmic@59: torch table 3 5 5 6 6 8 8 8 10 10; torch1-gpio gpp4-4, flash1-gpio gpp4-0) and Samsung's
 * drivers/leds/leds-s2mpb02.c with include/linux/mfd/s2mpb02-regulator.h, include/linux/leds-s2mpb02.h (ExtremeXT):
 *
 *   0x12 FLED_CTRL1   bits 7:6 enable/mode: 00 off (or the GPIOs'), 10 flash by I2C, 11 torch by I2C; bit 3 LV_EN
 *   0x14 FLED_CUR1    bits 3:0 the torch's current, 20 mA a step; bits 7:4 the flash's (not used here)
 *   0x15 FLED_TIME1   bits 3:0 the torch's timer (left as the boot set it)
 * Samsung's "I2C turn way": CUR1's torch nibble, then CTRL1 7:6 = 11; off: CTRL1 7:6 = 00, the GPIOs low.
 *
 * iOS's side is the POCO's (bsd/dev/redwood_torch.c's contract): /dev/redwood-torch takes a level 0..1000, a read gives
 * what is lit. Control Center's button reaches it through mediaserverd (userland/src/inject_media.c); the loader tells
 * MobileGestalt there is a flash when torch=N is on the command line. Boot-args: torch=3 torch_chip=s2mpb02 (the POCO's
 * Qualcomm SPMI code is never run on this phone), torch_max_s (default 600).
 *
 * Limits: Samsung's own top step (10: 200 mA); on for torch_max_s at a time at most, then off until iOS asks anew; the
 * battery at 45 C or more (the MAX77705's gauge, bsd/dev/redwood_max77705.c): off. Every write read back; a write that
 * does not read back ends the light for the run. Off at the run's end and on a panic's way out.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>

#define BANK_GPP3               0x60U           /* PERIC0 */
#define PMIC_ADDR               0x59U
#define REG_ID                  0x00U
#define REG_FLED_CTRL1          0x12U
#define REG_FLED_CUR1           0x14U
#define REG_FLED_TIME1          0x15U
#define CTRL1_MODE_MASK         0xC0U
#define CTRL1_TORCH_ON          0xC0U
#define CTRL1_LV_EN             0x08U
#define HOTTEST_BATTERY_C       45

static const uint8_t torch_table[10] = { 3, 5, 5, 6, 6, 8, 8, 8, 10, 10 };     /* the dtbo's, 20 mA a step */

static struct redwood_bbi2c bus = { .name = "s2mpb02 bus gpp3-2/3", .scl = 2, .sda = 3, .half_us = 4 };
static volatile uint32_t wanted, shown;
static uint32_t max_secs = 600;
static volatile bool usable, lit, broken;

static bool
get(uint8_t reg, uint8_t *v)
{
	return redwood_bbi2c_read_reg(&bus, PMIC_ADDR, reg, v, 1);
}

/* only the two registers above, and only the bits named; read back */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t before = 0, after = 0, want;

	if (broken || (reg != REG_FLED_CTRL1 && reg != REG_FLED_CUR1) || !get(reg, &before)) {
		return false;
	}
	want = (uint8_t)((before & ~mask) | (value & mask));
	if (want == before) {
		return true;
	}
	if (!redwood_bbi2c_write_reg(&bus, PMIC_ADDR, reg, want) || !get(reg, &after) || after != want) {
		printf("torch: s2mpb02 %#04x %#04x -> %#04x read back %#04x: no more writes this run\n", reg, before, want, after);
		broken = true;
		return false;
	}
	return true;
}

static bool
dark(void)
{
	bool ok = put_bits(REG_FLED_CTRL1, CTRL1_MODE_MASK, 0);

	lit = !ok && lit;
	return ok;
}

static bool
light(uint32_t permille)
{
	unsigned i = (permille * 9 + 500) / 1000;
	uint8_t step = torch_table[i > 9 ? 9 : i];

	if (!put_bits(REG_FLED_CUR1, 0x0F, step) || !put_bits(REG_FLED_CTRL1, CTRL1_MODE_MASK, CTRL1_TORCH_ON)) {
		(void)dark();
		return false;
	}
	lit = true;
	return true;
}

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		wanted = value > 1000 ? 1000 : value;
	}
	return 0;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	char line[192];
	uint8_t id = 0, c1 = 0, cur = 0, t1 = 0;
	uint32_t applied = 0, times = 0;
	uint64_t since = 0;
	bool hold_off = false;
	int major;

	bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP3);
	if (!redwood_bbi2c_setup(&bus, false, line, sizeof(line)) &&
	    !redwood_bbi2c_setup(&bus, true, line, sizeof(line))) {
		printf("torch: %s: the bus is not usable\n", line);
		redwood_bbi2c_restore(&bus);
		return;
	}
	if (!get(REG_ID, &id) || !get(REG_FLED_CTRL1, &c1) || !get(REG_FLED_CUR1, &cur) || !get(REG_FLED_TIME1, &t1)) {
		printf("torch: %s; the S2MPB02 does not answer at %#x\n", line, PMIC_ADDR);
		redwood_bbi2c_restore(&bus);
		return;
	}
	printf("torch: %s; S2MPB02 id %#04x, FLED_CTRL1 %#04x CUR1 %#04x TIME1 %#04x\n", line, id, c1, cur, t1);
	/* Samsung's setup: low-voltage mode off, the light off */
	if (!put_bits(REG_FLED_CTRL1, CTRL1_LV_EN | CTRL1_MODE_MASK, 0)) {
		return;
	}
	usable = true;
	major = cdevsw_add(-1, &torch_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-torch") == NULL) {
		printf("torch: /dev/redwood-torch cannot be made\n");
		return;
	}
	printf("torch: the S20's flashlight is iOS's (/dev/redwood-torch): at most 200 mA, %u s at a time\n", max_secs);
	while (!broken) {
		uint32_t want = wanted;
		uint64_t now = now_ms();
		int temp = redwood_battery_temperature();

		if (want == 0) {
			hold_off = false;
		}
		if (hold_off || (want != 0 && temp >= HOTTEST_BATTERY_C)) {
			want = 0;
		}
		if (want != applied) {
			if (!(want == 0 ? dark() : light(want))) {
				break;
			}
			if (applied == 0) {
				since = now;
				times++;
			}
			printf("torch: %s (iOS asked for %u of 1000; battery %d C)\n", want ? "on" : "off", wanted, temp);
			applied = want;
			shown = want;
		}
		if (applied != 0 && now - since >= (uint64_t)max_secs * 1000) {
			printf("torch: on for %u s, the limit: off until iOS asks again\n", max_secs);
			hold_off = true;
			continue;
		}
		delay_for_interval(30, NSEC_PER_MSEC);
	}
	(void)dark();
	shown = 0;
	printf("torch: given up on (%u times lit)\n", times);
}

void redwood_s2mpb02_torch_init(void);

void
redwood_s2mpb02_torch_init(void)
{
	thread_t thread;

	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 5 ? 5 : max_secs > 3600 ? 3600 : max_secs;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* the run's end, a panic's way out: dark */
void redwood_s2mpb02_torch_quiesce(void);

void
redwood_s2mpb02_torch_quiesce(void)
{
	if (usable && lit) {
		broken = false;
		printf("torch: at the run's end: the light %s\n", dark() ? "switched off" : "COULD NOT BE SWITCHED OFF");
	}
}
