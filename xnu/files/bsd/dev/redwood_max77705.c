/*
 * The Galaxy S20+'s (y2s, Exynos 990) battery: the MAX77705's fuel gauge and charger, read over
 * I2C bit-banged on the pins of HSI2C_7 (gpp1-6 SCL, gpp1-7 SDA: bsd/dev/redwood_bbi2c.c), and
 * handed to iOS as the power source (redwood_battery_report() -> iokit/Kernel/RedwoodBattery.cpp).
 *
 * READ ONLY. The MAX77705 charges by itself with what the bootloader set; nothing is ever written
 * to it here (a wrong charger setting could stop the charging, or worse).
 *
 * From Samsung's Exynos 990 kernel (drivers/mfd/max77705.c, drivers/battery_v2/max77705_*.c,
 * include/linux/mfd/max77705-private.h) and the phone's device tree (max77705@66 on hsi2c_7,
 * fuelgauge,fg_resistor = 2, battery,battery_full_capacity = 4500 mAh):
 *
 *   fuel gauge 0x36, 16-bit little-endian registers:
 *     0x00 STATUS     bit 3: no battery
 *     0x05 RepCap     1 mAh x fg_resistor / 2
 *     0x06 RepSOC     1/256 %
 *     0x08 Temp       1/256 C, signed (only meaningful if the gauge has a thermistor)
 *     0x09 VCell      78.125 uV
 *     0x0A Current    signed, 156.25 uA x fg_resistor (fg_resistor 2: 312.5 uA)
 *     0x0B AvgCurrent the same
 *     0x17 Cycles     (zeroed by Samsung's driver at every boot: not reported)
 *     0x35 FullCapRep as RepCap
 *     0xB3 VBYP       0.427246 mV (the input side: VBUS when a charger is in)
 *     0xD0 IIN        0.125 mA (input current)
 *   charger 0x69:
 *     0xB2 CHG_INT_OK bit 6 CHGIN_OK (wired power), bit 5 WCIN_OK (wireless), bit 2 BATP_OK
 *     0xB3 DETAILS_00 bits 6:5 CHGIN_DTLS (3: VBUS valid), bits 4:3 WCIN_DTLS (3: valid)
 *     0xB4 DETAILS_01 bits 3:0 CHG_DTLS: 0-2 charging, 3-4 done, 5-7 not charging, 8/A/B off
 *
 * Boot args: bms=1 (on), bms_secs=N (poll period, default 5), bms_trace=1 (every reading).
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/redwood_m2m.h>
#include <sys/systm.h>

#define BANK_GPP1               0x20U           /* PERIC0 */

#define FG_ADDR                 0x36U
#define CHG_ADDR                0x69U
#define MUIC_ADDR               0x25U           /* the USB-C/MUIC firmware */
#define MUIC_BC_STATUS          0x08U           /* bit 7 VBUS, bits 1:0 charger type: 0 none, 1 USB host, 2 charging host, 3 wall */

#define FG_STATUS               0x00U
#define FG_REPCAP               0x05U
#define FG_REPSOC               0x06U
#define FG_TEMP                 0x08U
#define FG_VCELL                0x09U
#define FG_CURRENT              0x0AU
#define FG_AVG_CURRENT          0x0BU
#define FG_CYCLES               0x17U
#define FG_DESIGNCAP            0x18U
#define FG_FULLCAP_REP          0x35U
#define FG_VBYP                 0xB3U
#define FG_IIN                  0xD0U

#define CHG_INT_OK              0xB2U
#define CHG_DETAILS_00          0xB3U
#define CHG_DETAILS_01          0xB4U
#define CHG_CNFG_00             0xB7U           /* bits 3:0 MODE (5: charger + buck) */
#define CHG_CNFG_02             0xB9U           /* bits 5:0 CHG_CC, fast-charge current, 50 mA steps */
#define CHG_CNFG_06             0xBDU           /* bits 3:2 CHGPROT (3: config registers unlocked) */
#define CHG_CNFG_09             0xC0U           /* bits 6:0 CHGIN_LIM, input limit, (n + 1) x 25 mA */

#define CHG_INT_OK_WCIN         (1U << 5)
#define CHG_INT_OK_CHGIN        (1U << 6)
#define CHG_CHGIN_DTLS(d00)     (((d00) >> 5) & 3U)     /* max77705_get_vbus_state(): 3 = VBUS valid */
#define CHG_WCIN_DTLS(d00)      (((d00) >> 3) & 3U)

#define FG_RESISTOR             2               /* fuelgauge,fg_resistor */
#define DESIGN_MAH              4500            /* battery,battery_full_capacity */

/* Linux's POWER_SUPPLY_STATUS_*, which struct redwood_battery_state speaks */
#define PS_STATUS_CHARGING      1
#define PS_STATUS_DISCHARGING   2
#define PS_STATUS_NOT_CHARGING  3
#define PS_STATUS_FULL          4

static struct redwood_bbi2c bus = { .name = "max77705 bus gpp1-6/7", .scl = 6, .sda = 7, .half_us = 4 };
static uint32_t trace, wdt_kick, ilim;
static uint64_t wdt_kicks, wdt_expired_seen;

static bool
fg_word(uint8_t reg, uint16_t *value)
{
	uint8_t b[2];

	if (!redwood_bbi2c_read_reg(&bus, FG_ADDR, reg, b, 2)) {
		return false;
	}
	*value = (uint16_t)(b[0] | (b[1] << 8));
	return true;
}

static bool
chg_byte(uint8_t reg, uint8_t *value)
{
	return redwood_bbi2c_read_reg(&bus, CHG_ADDR, reg, value, 1);
}

/* signed 16-bit current register to uA */
static int32_t
current_ua(uint16_t raw)
{
	return (int32_t)(int16_t)raw * 15625 * FG_RESISTOR / 100;
}

static int
charge_status(bool external, uint8_t details01)
{
	if (!external) {
		return PS_STATUS_DISCHARGING;
	}
	switch (details01 & 0x0f) {
	case 0x0: case 0x1: case 0x2:
		return PS_STATUS_CHARGING;
	case 0x3: case 0x4:
		return PS_STATUS_FULL;
	case 0x8: case 0xA: case 0xB:
		return PS_STATUS_DISCHARGING;
	default:
		return PS_STATUS_NOT_CHARGING;
	}
}

static const char *
status_name(int status)
{
	switch (status) {
	case PS_STATUS_CHARGING: return "charging";
	case PS_STATUS_DISCHARGING: return "discharging";
	case PS_STATUS_FULL: return "full";
	default: return "not charging";
	}
}

/*
 * bms_ilim=1 (x1slte, 2026-11-06): the bootloader leaves the charger at 900 mA in (CNFG_09 0x23) and 900 mA
 * into the battery (CNFG_02 0x52), which is what iOS draws while awake: the battery gained nothing. As
 * Samsung's driver does once the cable is known (max77705_set_input_current / _set_charge_current, the
 * values from the phone's dtbo cable-info: TA 1650 mA in, CDP 1500 mA in), the input limit is raised
 * for a wall charger or a charging host. The charge current is kept at 1500 mA, below Samsung's 3150:
 * Samsung's software watches the battery's temperature and steps the current down, and this kernel does
 * not, so it stays at the bootloader's values outside 10..42 C. A USB host, no cable or a temperature
 * outside the range gets the bootloader's own values back. Every write is read back; on a mismatch the
 * feature switches itself off.
 */
static int16_t boot_c2 = -1, boot_c9 = -1;

static void
charger_limits(bool external, int32_t temp_hundredths)
{
	uint8_t bc = 0, c2 = 0, c9 = 0, want2, want9, r2 = 0, r9 = 0;
	unsigned type, in_ma = 0;
	static int last_in = -1;

	if (!ilim || !chg_byte(CHG_CNFG_02, &c2) || !chg_byte(CHG_CNFG_09, &c9)) {
		return;
	}
	if (boot_c2 < 0) {
		boot_c2 = c2;
		boot_c9 = c9;
	}
	if (!redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_BC_STATUS, &bc, 1)) {
		bc = 0;                 /* unknown: treated as no charger */
	}
	type = bc & 3U;
	if (external && (bc & 0x80U) && temp_hundredths >= 1000 && temp_hundredths <= 4200) {
		in_ma = type == 3 ? 1650 : type == 2 ? 1500 : 0;
	}
	if (in_ma) {
		want9 = (uint8_t)((c9 & 0x80U) | ((in_ma / 25 - 1) & 0x7fU));
		want2 = (uint8_t)((c2 & ~0x3fU) | ((1500 / 50) & 0x3fU));
	} else {
		want9 = (uint8_t)boot_c9;
		want2 = (uint8_t)boot_c2;
	}
	if (want9 == c9 && want2 == c2) {
		return;
	}
	if (!redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_09, want9) ||
	    !redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_02, want2) ||
	    !chg_byte(CHG_CNFG_09, &r9) || !chg_byte(CHG_CNFG_02, &r2) || r9 != want9 || r2 != want2) {
		m2m_klog('W', "DEV", "bms: charger limits did not take (CNFG_09 %#x -> %#x reads %#x, CNFG_02 %#x -> %#x reads "
		    "%#x): bms_ilim switched off, bootloader values put back", c9, want9, r9, c2, want2, r2);
		ilim = 0;
		redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_09, (uint8_t)boot_c9);
		redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_02, (uint8_t)boot_c2);
		return;
	}
	if ((int)in_ma != last_in) {
		m2m_klog('I', "DEV", "bms: charger type %u (BC_STATUS %#x), %d.%02d C: input limit %u mA, charge %u mA "
		    "(CNFG_09 %#x, CNFG_02 %#x)", type, bc, temp_hundredths / 100, temp_hundredths % 100,
		    ((r9 & 0x7fU) + 1) * 25, (r2 & 0x3fU) * 50, r9, r2);
		last_in = (int)in_ma;
	}
}

static bool
poll_once(bool say)
{
	struct redwood_battery_state b;
	uint16_t status, repcap, repsoc, temp, vcell, cur, avg, cycles, fullcap, designcap, vbyp, iin;
	uint8_t int_ok, d00, d01;
	bool external;
	int32_t temp_hundredths;

	if (!fg_word(FG_REPSOC, &repsoc) || !fg_word(FG_VCELL, &vcell) || !fg_word(FG_CURRENT, &cur) ||
	    !fg_word(FG_AVG_CURRENT, &avg) || !fg_word(FG_STATUS, &status) || !fg_word(FG_REPCAP, &repcap) ||
	    !fg_word(FG_FULLCAP_REP, &fullcap) || !fg_word(FG_DESIGNCAP, &designcap) || !fg_word(FG_CYCLES, &cycles) ||
	    !fg_word(FG_TEMP, &temp) || !fg_word(FG_VBYP, &vbyp) || !fg_word(FG_IIN, &iin)) {
		return false;
	}
	if (!chg_byte(CHG_INT_OK, &int_ok) || !chg_byte(CHG_DETAILS_00, &d00) || !chg_byte(CHG_DETAILS_01, &d01)) {
		return false;
	}
	/*
	 * The charger's watchdog (x1slte, 2026-11-06): Samsung's kernel enables it (CNFG_00 WDTEN) and kicks it
	 * from its charger driver (max77705_chg_set_wdtmr_kick: CNFG_06 WDTCLR = 01). Nobody kicks it under iOS,
	 * so about a minute after boot CHG_DTLS reads 0xB, "WDT expired": the charger stops charging and the
	 * phone runs from the cable while the battery drains. bms_wdt=1 kicks it at every poll, as Samsung's
	 * kernel does: a read-modify-write of WDTCLR (bits 1:0) only, nothing about currents or voltages.
	 */
	if (wdt_kick) {
		uint8_t c6 = 0, after = 0;

		if ((d01 & 0x0fU) == 0x0bU && wdt_expired_seen++ < 5) {
			m2m_klog('W', "DEV", "bms: the charger's watchdog had expired (CHG_DTLS 0xB): kicking it");
		}
		if (chg_byte(CHG_CNFG_06, &c6) &&
		    redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_06, (uint8_t)((c6 & ~0x03U) | 0x01U))) {
			if (wdt_kicks++ == 0) {
				chg_byte(CHG_CNFG_06, &after);
				m2m_klog('I', "DEV", "bms: watchdog kicked: CNFG_06 %#x -> wrote %#x, reads %#x", c6,
				    (c6 & ~0x03U) | 0x01U, after);
			}
		}
	}

	/* a cable or a pad: its OK bit and its input valid, as Samsung's driver reads it (max77705_get_vbus_state) */
	external = ((int_ok & CHG_INT_OK_CHGIN) && CHG_CHGIN_DTLS(d00) == 3) ||
	    ((int_ok & CHG_INT_OK_WCIN) && CHG_WCIN_DTLS(d00) == 3);

	/* the gauge's temperature is only real with a thermistor on it (Samsung reads one on the AP's ADC) */
	temp_hundredths = (int32_t)(int16_t)temp * 100 / 256;
	if (temp_hundredths < -2000 || temp_hundredths > 7000) {
		temp_hundredths = 2500;
	}

	charger_limits(external, temp_hundredths);

	bzero(&b, sizeof(b));
	b.size = sizeof(b);
	b.percent_hundredths = (int32_t)((uint32_t)repsoc * 100 / 256);
	if (b.percent_hundredths > 10000) {
		b.percent_hundredths = 10000;
	}
	b.voltage_uv = (int32_t)((uint64_t)vcell * 78125 / 1000);      /* (64-bit: 4.3 V overflows 32) */
	b.current_ua = current_ua(avg);
	b.temperature_hundredths = temp_hundredths;
	b.present = (status & (1U << 3)) == 0;
	b.external = external;
	b.status = charge_status(external, d01);
	b.charge_uah = (int32_t)repcap * FG_RESISTOR / 2 * 1000;
	b.full_uah = (int32_t)fullcap * FG_RESISTOR / 2 * 1000;
	b.design_uah = DESIGN_MAH * 1000;
	b.cycles = 0;                           /* Samsung's driver zeroes the gauge's count: the real one is kept in its EFS */
	b.usb_type = 0;                         /* unknown: the type is the MUIC's (0x25) to say */
	if (external) {
		b.usb_voltage_uv = (int32_t)((uint64_t)vbyp * 427246 / 1000);
		b.usb_current_ua = (int32_t)iin * 125;
	}
	redwood_battery_report(&b);
	{
		static int last_external = -1, last_status = -1;

		if (b.external != last_external || b.status != last_status) {
			m2m_klog('I', "DEV", "bms: %s, %s at %d.%02d %%: INT_OK %#x DETAILS_00 %#x DETAILS_01 %#x, input %d mV %d mA",
			    external ? "on a charger" : "no charger", status_name(b.status), b.percent_hundredths / 100,
			    b.percent_hundredths % 100, int_ok, d00, d01, (int)((uint64_t)vbyp * 427246 / 1000000),
			    (int)iin * 125 / 1000);
			last_external = b.external;
			last_status = b.status;
		}
	}

	if (say) {
		/* read only: what the bootloader left the charger at (x1slte: input 879 mA at 4.8 V does not keep up) */
		uint8_t c0 = 0, c2 = 0, c6 = 0, c9 = 0;

		if (chg_byte(CHG_CNFG_00, &c0) && chg_byte(CHG_CNFG_02, &c2) && chg_byte(CHG_CNFG_06, &c6) &&
		    chg_byte(CHG_CNFG_09, &c9)) {
			m2m_klog('I', "DEV", "bms: charger config: CNFG_00 %#x (mode %u), CNFG_02 %#x (charge %u mA), CNFG_06 %#x "
			    "(prot %u), CNFG_09 %#x (input limit %u mA); battery %d mA, input %d mV %d mA",
			    c0, c0 & 0xfU, c2, (c2 & 0x3fU) * 50, c6, (c6 >> 2) & 3U, c9, ((c9 & 0x7fU) + 1) * 25,
			    b.current_ua / 1000, b.usb_voltage_uv / 1000, b.usb_current_ua / 1000);
		} else {
			m2m_klog('W', "DEV", "bms: charger config registers did not answer");
		}
	}
	if (say || trace) {
		printf("bms: %d.%02d %% %d mV %d mA (now %d), %d.%02d C, %d/%d mAh (design %d, gauge %u), %u cycles, "
		    "%s%s, input %d mV %d mA; INT_OK %#x DTLS %#x %#x\n",
		    b.percent_hundredths / 100, b.percent_hundredths % 100, b.voltage_uv / 1000, b.current_ua / 1000,
		    current_ua(cur) / 1000, temp_hundredths / 100, temp_hundredths % 100 < 0 ? -(temp_hundredths % 100) :
		    temp_hundredths % 100, b.charge_uah / 1000, b.full_uah / 1000, DESIGN_MAH,
		    designcap * FG_RESISTOR / 2, b.cycles, b.present ? "" : "NO BATTERY, ",
		    status_name(b.status), b.usb_voltage_uv / 1000, b.usb_current_ua / 1000, int_ok, d00, d01);
	}
	return true;
}

static void
bms_thread(__unused void *parameter, __unused wait_result_t result)
{
	char line[192];
	uint32_t secs = 5, failures = 0;
	uint64_t polls = 0;

	PE_parse_boot_argn("bms_secs", &secs, sizeof(secs));
	PE_parse_boot_argn("bms_trace", &trace, sizeof(trace));
	PE_parse_boot_argn("bms_wdt", &wdt_kick, sizeof(wdt_kick));
	PE_parse_boot_argn("bms_ilim", &ilim, sizeof(ilim));
	if (secs < 1 || secs > 600) {
		secs = 5;
	}
	bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP1);
	if (!redwood_bbi2c_setup(&bus, false, line, sizeof(line))) {
		printf("bms: %s: the bus is not usable, giving up\n", line);
		redwood_bbi2c_restore(&bus);
		return;
	}
	printf("bms: %s\n", line);

	for (;; polls++) {
		/* a line at the start, then once a minute */
		bool say = polls == 0 || (polls * secs) % 60 < secs;

		if (poll_once(say)) {
			if (failures) {
				m2m_klog('I', "DEV", "bms: the MAX77705 answers again after %u failed reads", failures);
			}
			failures = 0;
		} else {
			/*
			 * x1slte (2026-11-06): the first read at boot worked, then never again (the percentage froze at
			 * its boot value). The touch driver bit-bangs its PMIC bus on this same bank at the same time,
			 * and a transfer cut short leaves the gauge holding SDA low. Set the bus up again (DAT preset to
			 * 0, up to 9 clocks to free a stuck slave, a stop) and read again at once.
			 */
			bool ok = redwood_bbi2c_setup(&bus, false, line, sizeof(line)) && poll_once(say);

			if (failures++ < 5 || failures % 60 == 0) {
				m2m_klog(ok ? 'I' : 'W', "DEV", "bms: no answer from the MAX77705 (%u, %u stretched clocks given up on "
				    "so far); bus set up again: %s: %s", failures, bus.stretch_timeouts, line,
				    ok ? "answers again" : "still no answer");
			}
			if (ok) {
				failures = 0;
			}
		}
		delay_for_interval(secs, NSEC_PER_SEC);
	}
}

void
redwood_max77705_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bms", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(bms_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
