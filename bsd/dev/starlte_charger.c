/* SPDX-License-Identifier: MIT-0 */
/*
 * iOS 27 on STARLTE (24 Nov 2026): the Galaxy S9's charger, watched and guarded. The MAX77705's charger (0x69) and MUIC
 * (0x25) on the bus of starlte_muic.c, and the battery's and the charger's thermistors on CMGP's ADC. The rules are
 * scion-kernel's drivers/power/max77705/hakios.max77705_charge.h (copied here as bsd/dev/hakios.max77705_charge.h,
 * tested on a computer by tools/max77705-charge-test.sh); docs/ios27/CHARGING.md has the facts.
 *
 *   sweetchg=0  nothing here (SweetBattery as before: the gauge alone, 25 C)
 *   sweetchg=1  READ ONLY: every second the charger, the MUIC, the gauge and both thermistors read; the charger type, the
 *               battery's temperature and the charger's own state handed to SweetBattery for iOS; what the guard WOULD do
 *               logged ("would"); the charger's registers 0xB2-0xC5 in the log at start and every 5 minutes.
 *   sweetchg=2  the guard: as 1, and the charger told (volatile registers only, each write read back):
 *                 CNFG_06 WDTCLR (the watchdog kicked every 10 s while it is on, as Samsung's driver does; without it the
 *                 charger stopped about 70 s after S-Boot on phone 2, DETAILS_01 0x3b),
 *                 CNFG_06 CHGPROT to 11 if it reads locked (Samsung's max77705_charger_unlock),
 *                 CNFG_00 bit 0 (charging on or off: the temperature bands, the session's time, full and recharge),
 *                 CNFG_09 [6:0] (the input limit) and CNFG_02 [5:0] (the charge current): Samsung's for the type, or less.
 *               A read-back that differs: charging off and nothing more written until the cable goes.
 *   sweetafc=1  (with sweetchg=2; OFF by default) the fast-charge path: on a DCP, inside the band, the MUIC's switch opened
 *               (Samsung's COM_OPEN for a TA) and Samsung's 9 V request sent (opcode 0x21 with 0x46); 9 V taken only when the
 *               answer is result 0 with VBUS in 7.5-10.5 V and USBC_STATUS1 says the same; then 1650/2150 mA. Checked every
 *               second: out of the band, VBUS out of 7.5-10.5 V, a fault or a differing read-back -> 500 mA, the 5 V request,
 *               the standard limits once VBUS reads 5 V; not asked again on that cable.
 *   sweetchg_drain=MIN  (a test, with sweetchg=2; off by default) for the first MIN minutes (30 at most) the input held at
 *               100 mA and charging off, so the phone runs from its battery and charging can then be seen from below
 *               full on a computer's port; given up at 50 % or 40 C. Lower limits only.
 * Never written: the float voltage, the timers, CNFG_01/03/04/05/07/08/10-12, the gauge, the PMIC, anything non-volatile.
 * The console: sysctl kern.hakios.chg_* (below).
 */
#include <stdbool.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.xnu.h>
#include "hakios.max77705_charge.h"

#define CHARGER         0x69
#define MUIC            0x25
#define GAUGE           0x36
#define OPCODE_WRITE    0x21
#define OPCODE_WRITE_END 0x41
#define OPCODE_READ     0x51
#define COM_OPEN        0x00

/* bsd/dev/starlte_muic.c: the bus, brought up and locked (NULL if it could not be), and let go */
extern struct hakios_hsi2c *starlte_muic_bus_take(void);
extern void starlte_muic_bus_give(void);

/* ---- the thermistors: CMGP's ADC (0x14230000), channel 4 the battery's, 3 the charger's (Samsung's adc-temp and
 * adc-chg-temp); five conversions, the highest and lowest dropped; the phone's table (battery,temp_table_adc/_data,
 * the same for both). The clock gates and the reset as Scion's hakios.max77705.c does it. */
#define ADC_PA          0x14230000UL
#define ADC_CON1        0x00
#define ADC_CON2        0x04
#define ADC_DAT         0x08
#define ADC_INT_EN      0x10
#define ADC_INT_ST      0x14
#define ADC_CH_BATT     4
#define ADC_CH_CHG      3
static const uint16_t temp_adc[] = { 275, 327, 385, 451, 535, 626, 737, 864, 1013, 1210, 1361, 1554, 1769, 2007, 2239,
	                             2476, 2702, 2924, 3116, 3294, 3446, 3564, 3676 };
static const int16_t temp_dc[] = { 900, 850, 800, 750, 700, 650, 600, 550, 500, 450, 400, 350, 300, 250, 200, 150, 100,
	                           50, 0, -50, -100, -150, -200 };
static vm_offset_t adc;

static void
adc_up(struct hakios_hsi2c *bus)
{
	static const uint32_t gates[] = { 0x2018, 0x201C }, qch[] = { 0x3008, 0x300C };

	if (adc != 0 || bus->cmu == 0) {
		return;
	}
	adc = ml_io_map(ADC_PA, 0x1000);
	if (adc == 0) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		*(volatile uint32_t *)(bus->cmu + gates[i]) |= (1u << 20) | (1u << 21);
		*(volatile uint32_t *)(bus->cmu + qch[i]) |= 3;
	}
	*(volatile uint32_t *)(adc + ADC_CON1) = 1u << 2;
	*(volatile uint32_t *)(adc + ADC_CON1) = 1u << 1;
	*(volatile uint32_t *)(adc + ADC_CON2) = 6u << 4;
	*(volatile uint32_t *)(adc + ADC_INT_EN) = 1;
}

static int
adc_convert(unsigned ch)
{
	volatile uint32_t *r = (volatile uint32_t *)adc;

	r[ADC_CON2 / 4] = (r[ADC_CON2 / 4] & ~0xfu) | ch;
	r[ADC_CON1 / 4] |= 1;
	for (int i = 0; i < 10000; i++) {
		if (r[ADC_INT_ST / 4] & 1) {
			int v = (int)(r[ADC_DAT / 4] & 0xfff);

			r[ADC_INT_ST / 4] = 1;
			return v;
		}
		delay(1);
	}
	return -1;
}

/* tenths of a degree; false if the ADC did not answer */
static bool
thermistor(unsigned ch, int32_t *dc, int *raw_out)
{
	int s[5], lo = 0, hi = 0, sum = 0, raw;
	const unsigned n = sizeof(temp_adc) / sizeof(temp_adc[0]);

	if (adc == 0) {
		return false;
	}
	for (int i = 0; i < 5; i++) {
		if ((s[i] = adc_convert(ch)) < 0) {
			return false;
		}
		lo = s[i] < s[lo] ? i : lo;
		hi = s[i] > s[hi] ? i : hi;
	}
	for (int i = 0; i < 5; i++) {
		sum += (i == lo || i == hi) ? 0 : s[i];
	}
	raw = sum / (lo == hi ? 4 : 3);
	*raw_out = raw;
	if (raw <= temp_adc[0]) {
		*dc = temp_dc[0];
	} else if (raw >= temp_adc[n - 1]) {
		*dc = temp_dc[n - 1];
	} else {
		for (unsigned i = 1; i < n; i++) {
			if (raw <= temp_adc[i]) {
				*dc = temp_dc[i - 1] + (temp_dc[i] - temp_dc[i - 1]) * (raw - temp_adc[i - 1]) /
				    (temp_adc[i] - temp_adc[i - 1]);
				break;
			}
		}
	}
	return true;
}

/* ---- what the console and SweetBattery see -------------------------------------------------------------------------- */

static int chg_mode;                    /* sweetchg= */
static int chg_afc;                     /* sweetafc= */
static int chg_drain_min;               /* sweetchg_drain= (a test): minutes on the battery first */
static uint32_t drain_until_s;
static int k_type = -1, k_why = -1, k_temp_dc = -2730, k_chg_temp_dc = -2730, k_input_ma = -1, k_charge_ma = -1;
static int k_enable = -1, k_vbadc = -1, k_afc_asked, k_afc_good, k_afc_drops, k_writes, k_readback_bad, k_kicks;
static int k_bc = -1, k_dtls01 = -1, k_float_mv = -1, k_soc = -1, k_vcell = -1, k_current_ma;

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_mode, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &chg_mode, 0, "sweetchg=: 0 off, 1 read only, 2 the guard");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_afc, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &chg_afc, 0, "sweetafc=: the 9 V path allowed");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_type, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_type, 0,
    "the charger: 0 none, 1 SDP, 2 CDP, 3 DCP at 5 V, 4 AFC at 9 V, 5 unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_why, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_why, 0,
    "0 charging allowed, 1 no VBUS, 2 read failed, 3 no temperature, 4 hot, 5 cold, 6 full, 7 time, 8 fault, 9 45 C, 10 float");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_temp_dc, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_temp_dc, 0, "the battery, tenths of a degree");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_chgtemp_dc, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_chg_temp_dc, 0, "the charger's thermistor");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_input_ma, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_input_ma, 0, "CNFG_09 as read, mA");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_charge_ma, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_charge_ma, 0, "CNFG_02 as read, mA");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_enable, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_enable, 0, "CNFG_00 bit 0 as read");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_vbadc, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_vbadc, 0, "VBUS band: 2 = 4.5-5.5 V, 5-7 = 7.5-10.5 V");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_afc_asked, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_afc_asked, 0, "9 V requests sent");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_afc_good, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_afc_good, 0, "9 V sessions begun");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_afc_drops, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_afc_drops, 0, "9 V given up (5 V asked)");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_writes, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_writes, 0, "charger register writes");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_readback_bad, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_readback_bad, 0, "read-backs that differed");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_kicks, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_kicks, 0, "watchdog kicks");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_bc, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_bc, 0, "MUIC BC_STATUS");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_dtls01, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_dtls01, 0, "charger DETAILS_01");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_float_mv, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_float_mv, 0, "CNFG_04 as read, mV");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_soc, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_soc, 0, "the gauge's RepSOC, percent");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_current_ma, 0,
    "the gauge's Current, mA, above 0 while charging");
SYSCTL_INT(_kern_hakios, OID_AUTO, chg_vcell_mv, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &k_vcell, 0, "the gauge's VCELL, mV");

/* for SweetBattery.cpp (sweetchg >= 1) */
struct starlte_chg_report {
	bool valid;
	int type;                       /* enum chg_type */
	bool external, charging, full;
	int32_t temp_centi;             /* the battery, hundredths of a degree (iOS's unit) */
	uint32_t input_ma, vbus_mv;
	const char *type_name;
};
int starlte_chg_report(struct starlte_chg_report *r);

static struct starlte_chg_report report;

int
starlte_chg_report(struct starlte_chg_report *r)
{
	if (chg_mode < 1 || !report.valid) {
		return -1;
	}
	*r = report;
	return 0;
}

/* ---- the chip -------------------------------------------------------------------------------------------------------- */

static uint32_t
rd8(struct hakios_hsi2c *bus, uint8_t addr, uint8_t reg, uint8_t *v)
{
	return hakios_hsi2c_read_reg(bus, addr, reg, v, 1);
}

static uint32_t
rd16(struct hakios_hsi2c *bus, uint8_t addr, uint8_t reg, uint16_t *v)
{
	uint8_t b[2] = { 0, 0 };
	uint32_t e = hakios_hsi2c_read_reg(bus, addr, reg, b, 2);

	*v = (uint16_t)(b[0] | (b[1] << 8));
	return e;
}

/* a charger register: only the bits of mask changed, the rest as read; read back. true when it reads as written. */
static bool
chg_write_bits(struct hakios_hsi2c *bus, uint8_t reg, uint8_t mask, uint8_t value, const char *what)
{
	uint8_t now = 0, back = 0, b[2];
	bool ok;

	if (rd8(bus, CHARGER, reg, &now) != 0) {
		printf("STARLTE CHG: %s: %#x not read: not written\n", what, reg);
		return false;
	}
	b[0] = reg;
	b[1] = (uint8_t)((now & ~mask) | (value & mask));
	if (b[1] == now) {
		return true;
	}
	ok = hakios_hsi2c_message(bus, CHARGER, false, b, 2, true) == 0 && rd8(bus, CHARGER, reg, &back) == 0 &&
	    chg_readback_ok(b[1], back, mask);
	k_writes++;
	printf("STARLTE CHG: %s: %#x %#x -> %#x, read back %#x%s\n", what, reg, now, b[1], back, ok ? "" : " DIFFERS");
	if (!ok) {
		k_readback_bad++;
	}
	return ok;
}

/* a MUIC opcode with its data, through OPCODE_WRITE and OPCODE_WRITE_END (Samsung's max77705_usbc_opcode_write) */
static uint32_t
muic_opcode(struct hakios_hsi2c *bus, uint8_t op, uint8_t data)
{
	uint8_t w1[3] = { OPCODE_WRITE, op, data }, w2[2] = { OPCODE_WRITE_END, 0 };
	uint32_t e = hakios_hsi2c_message(bus, MUIC, false, w1, 3, true);

	return e ? e : hakios_hsi2c_message(bus, MUIC, false, w2, 2, true);
}

static void
regs_say(struct hakios_hsi2c *bus, const char *when)
{
	char line[3 * 20 + 1];
	size_t at = 0;
	uint8_t v = 0, bc = 0, s1 = 0, s2 = 0;

	for (uint8_t reg = 0xB2; reg <= 0xC5; reg++) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, rd8(bus, CHARGER, reg, &v) == 0 ? " %02x" : " --", v);
	}
	(void)rd8(bus, MUIC, MUIC_REG_BC_STATUS, &bc);
	(void)rd8(bus, MUIC, MUIC_REG_USBC_STATUS1, &s1);
	(void)rd8(bus, MUIC, 0x07, &s2);
	printf("STARLTE CHG: registers %s: charger 0xb2-0xc5:%s; MUIC BC_STATUS %#x USBC_STATUS1 %#x SYSMSG %#x\n", when, line,
	    bc, s1, s2);
}

/* ---- the guard --------------------------------------------------------------------------------------------------------- */

static thread_call_t chg_call;
static struct chg_state st;

static unsigned ticks;
static enum chg_why last_why = (enum chg_why)-1;
static enum chg_type last_type = (enum chg_type)-1;
static bool last_enable;

static uint32_t
now_s(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_SEC);
}

/* 9 V asked for: the switch opened, the request sent, the answer awaited (3 s); true for a good one */
static bool
afc_ask(struct hakios_hsi2c *bus, uint8_t tx)
{
	uint8_t a[3] = { 0, 0, 0 }, s1 = 0;
	uint32_t e;

	(void)muic_opcode(bus, MUIC_OP_CONTROL1_W, COM_OPEN);
	delay(50000);
	e = muic_opcode(bus, MUIC_OP_AFC_RESULT, tx);
	printf("STARLTE CHG: AFC: %s request sent (%#x): %s\n", tx == AFC_TX_9V ? "9 V" : "5 V", tx, e ? "FAILED" : "ok");
	if (e != 0) {
		return false;
	}
	for (int i = 0; i < 60; i++) {
		/* (the bus let go meanwhile is not needed: the guard is its only user once the switch's two minutes are over,
		 * and the switch's tick takes the same lock) */
		delay(50000);
		if (hakios_hsi2c_read_reg(bus, MUIC, OPCODE_READ, a, 3) == 0 && a[0] == MUIC_OP_AFC_RESULT) {
			break;
		}
	}
	(void)rd8(bus, MUIC, MUIC_REG_USBC_STATUS1, &s1);
	printf("STARLTE CHG: AFC: answer opcode %#x result %u VBADC %u; USBC_STATUS1 VBADC %u\n", a[0], a[1], a[2], s1 >> 4);
	return tx == AFC_TX_9V && chg_afc_answer_9v(a, (uint8_t)(s1 >> 4));
}

static void
chg_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	struct hakios_hsi2c *bus = starlte_muic_bus_take();
	struct chg_in in = { .read_ok = false };
	struct chg_out o;
	uint8_t cnfg06 = 0, dtls02 = 0, s1 = 0;
	uint16_t soc = 0, vcell = 0, cur = 0;
	int raw_b = -1, raw_c = -1;
	bool guard = chg_mode >= 2;
	uint64_t deadline;

	if (bus == NULL) {
		goto again;
	}
	adc_up(bus);
	in.read_ok = rd8(bus, MUIC, MUIC_REG_BC_STATUS, &in.bc) == 0 && rd8(bus, MUIC, MUIC_REG_USBC_STATUS1, &s1) == 0 &&
	    rd8(bus, CHARGER, CHG_REG_DTLS_00, &in.dtls00) == 0 && rd8(bus, CHARGER, CHG_REG_DTLS_01, &in.dtls01) == 0 &&
	    rd8(bus, CHARGER, CHG_REG_DTLS_02, &dtls02) == 0 && rd8(bus, CHARGER, CHG_REG_CNFG_00, &in.cnfg00) == 0 &&
	    rd8(bus, CHARGER, CHG_REG_CNFG_02, &in.cnfg02) == 0 && rd8(bus, CHARGER, CHG_REG_CNFG_04, &in.cnfg04) == 0 &&
	    rd8(bus, CHARGER, CHG_REG_CNFG_06, &cnfg06) == 0 && rd8(bus, CHARGER, CHG_REG_CNFG_09, &in.cnfg09) == 0 &&
	    rd16(bus, GAUGE, 0x06, &soc) == 0 && rd16(bus, GAUGE, 0x09, &vcell) == 0;
	in.vbadc = (uint8_t)(s1 >> 4);
	in.temp_ok = thermistor(ADC_CH_BATT, &in.temp_dc, &raw_b);
	in.chg_temp_ok = thermistor(ADC_CH_CHG, &in.chg_temp_dc, &raw_c);
	(void)rd16(bus, GAUGE, 0x0A, &cur);     /* Current: 312.5 uA a count, above 0 while charging (for the log) */
	k_current_ma = (int)((int64_t)(int16_t)cur * 3125 / 10000);
	in.soc = soc / 256u;
	in.vcell_mv = (uint32_t)((uint64_t)vcell * 78125u / 1000000u);
	in.now_s = now_s();
	in.afc_wanted = chg_afc != 0 && guard;
	if (ticks == 0) {
		regs_say(bus, "at start");
		printf("STARLTE CHG: sweetchg=%d sweetafc=%d: %s; battery %d.%d C (ADC %d), charger %d.%d C (ADC %d)\n", chg_mode,
		    chg_afc, guard ? "the guard (volatile charger registers, read back)" : "read only", in.temp_dc / 10,
		    (in.temp_dc < 0 ? -in.temp_dc : in.temp_dc) % 10, raw_b, in.chg_temp_dc / 10,
		    (in.chg_temp_dc < 0 ? -in.chg_temp_dc : in.chg_temp_dc) % 10, raw_c);
	} else if (ticks % 300 == 0) {
		regs_say(bus, "every 5 minutes");
	}

	o = chg_step(&st, &in);
	if (guard && drain_until_s != 0) {
		if (in.now_s < drain_until_s && in.read_ok && in.soc > 50 && in.temp_ok && in.temp_dc < 400 &&
		    o.why != WHY_FAULT && !st.latched_off) {
			o.enable = false;
			o.input_ma = 100;
			o.ask_9v = false;
			if (ticks % 60 == 0) {
				printf("STARLTE CHG: drain (sweetchg_drain=%d): %u s left, %u %% %u mV\n", chg_drain_min,
				    drain_until_s - in.now_s, in.soc, in.vcell_mv);
			}
		} else {
			printf("STARLTE CHG: drain over (%s): %u %% %u mV, battery %d.%d C; the guard's limits from now\n",
			    in.now_s >= drain_until_s ? "its time" : "50 %, 40 C or a fault", in.soc, in.vcell_mv, in.temp_dc / 10,
			    (in.temp_dc < 0 ? -in.temp_dc : in.temp_dc) % 10);
			drain_until_s = 0;
		}
	}

	if (guard && in.read_ok) {
		bool ok = true;

		/* the watchdog kicked while it is on (every 10 s, as Samsung's monitor does; it expired about 70 s after S-Boot
		 * on phone 2 without it), and at once when it has expired; CHGPROT unlocked if it reads locked */
		if ((in.cnfg00 & CHG_CNFG00_WDTEN) && (ticks % 10 == 0 || o.rearm)) {
			uint8_t b[2] = { CHG_REG_CNFG_06, (uint8_t)((cnfg06 & ~CHG_CNFG06_WDTCLR_MASK) | CHG_CNFG06_WDTCLR) };

			if (hakios_hsi2c_message(bus, CHARGER, false, b, 2, true) == 0) {
				if (k_kicks++ == 0) {
					printf("STARLTE CHG: watchdog: CNFG_00 %#x (WDTEN on): CNFG_06 %#x written %#x (kicked)\n", in.cnfg00,
					    cnfg06, b[1]);
				}
			}
		}
		if ((cnfg06 & CHG_CNFG06_CHGPROT_MASK) != CHG_CNFG06_CHGPROT_MASK) {
			ok = chg_write_bits(bus, CHG_REG_CNFG_06, CHG_CNFG06_CHGPROT_MASK, CHG_CNFG06_CHGPROT_MASK, "CHGPROT unlocked");
		}
		/* lower first, then on: the input before a request, 5 V asked after the input is down */
		if (ok && o.input_ma != 0) {
			uint8_t code = chg_input_code(o.input_ma);

			if ((in.cnfg09 & CHG_CNFG09_LIM_MASK) != code) {
				ok = chg_write_bits(bus, CHG_REG_CNFG_09, CHG_CNFG09_LIM_MASK, code, "input limit");
			}
		}
		if (ok && o.charge_ma != 0) {
			uint8_t code = chg_charge_code(o.charge_ma);

			if ((in.cnfg02 & CHG_CNFG02_CC_MASK) != code) {
				ok = chg_write_bits(bus, CHG_REG_CNFG_02, CHG_CNFG02_CC_MASK, code, "charge current");
			}
		}
		if (o.ask_5v) {
			k_afc_drops++;
			(void)afc_ask(bus, AFC_TX_5V);
		}
		if (!ok) {
			/* a read-back that differs: off, latched until the cable goes */
			st.latched_off = true;
			o.enable = false;
			o.ask_9v = false;
			if (st.type == CHG_AFC_9V) {
				st.type = CHG_DCP;
				st.afc_failed = true;
				k_afc_drops++;
				(void)afc_ask(bus, AFC_TX_5V);
			}
		}
		/* the watchdog expired with charging wanted: off and on again (Samsung's max77705_get_charging_health) */
		if (o.rearm && o.enable && !st.latched_off && (in.cnfg00 & CHG_CNFG00_BUCK) && (in.cnfg00 & CHG_CNFG00_CHG)) {
			static unsigned rearms;

			if (rearms++ < 5) {
				printf("STARLTE CHG: the charger's watchdog had expired (DETAILS_01 %#x): kicked, charging off and on\n",
				    in.dtls01);
			}
			if (!chg_write_bits(bus, CHG_REG_CNFG_00, CHG_CNFG00_CHG, 0, "re-arm: charging off")) {
				st.latched_off = true;
			}
			in.cnfg00 &= (uint8_t)~CHG_CNFG00_CHG;
		}
		/* charging on or off (CNFG_00 bit 0 only; the buck, which powers the phone from the cable, never touched) */
		if (((in.cnfg00 & CHG_CNFG00_CHG) != 0) != o.enable && (in.cnfg00 & CHG_CNFG00_BUCK) && !(o.enable && st.latched_off)) {
			if (!chg_write_bits(bus, CHG_REG_CNFG_00, CHG_CNFG00_CHG, o.enable ? CHG_CNFG00_CHG : 0,
			    o.enable ? "charging on" : "charging off")) {
				st.latched_off = true;
			}
		}
		if (ok && o.ask_9v && !st.latched_off) {
			bool good;

			k_afc_asked++;
			good = afc_ask(bus, AFC_TX_9V);
			chg_afc_result(&st, good);
			if (good) {
				k_afc_good++;
				printf("STARLTE CHG: AFC: 9 V; the input and charge current go to %u/%u mA at the next second\n",
				    chg_limits[CHG_AFC_9V].input_ma, chg_limits[CHG_AFC_9V].charge_ma);
			} else {
				printf("STARLTE CHG: AFC: no 9 V; 5 V limits, not asked again on this cable\n");
				(void)afc_ask(bus, AFC_TX_5V);
			}
		}
	}

	/* what was read, for the console and for iOS */
	k_type = st.type;
	k_why = o.why;
	k_temp_dc = in.temp_ok ? in.temp_dc : -2730;
	k_chg_temp_dc = in.chg_temp_ok ? in.chg_temp_dc : -2730;
	k_input_ma = (int)chg_input_ma_of(in.cnfg09);
	k_charge_ma = (int)chg_charge_ma_of(in.cnfg02);
	k_enable = in.cnfg00 & CHG_CNFG00_CHG;
	k_vbadc = in.vbadc;
	k_bc = in.bc;
	k_dtls01 = in.dtls01;
	k_float_mv = (int)chg_float_mv_of(in.cnfg04);
	k_soc = (int)in.soc;
	k_vcell = (int)in.vcell_mv;
	if (in.read_ok) {
		unsigned d = in.dtls01 & 0xFu;

		report.type = st.type;
		report.type_name = chg_limits[st.type].name;
		report.external = chg_type_of_bc(in.bc) != CHG_NONE;
		report.charging = report.external && (in.cnfg00 & CHG_CNFG00_CHG) && d <= 0x2;
		report.full = report.external && (d == 0x3 || d == 0x4 || st.full);
		report.temp_centi = in.temp_ok ? in.temp_dc * 10 : 2500;
		report.input_ma = chg_input_ma_of(in.cnfg09);
		report.vbus_mv = in.vbadc >= VBADC_9V_MIN ? 9000u : 5000u;
		report.valid = in.temp_ok;
	}

	if (o.why != last_why || st.type != last_type || o.enable != last_enable || ticks % 60 == 0) {
		printf("STARLTE CHG: %s%s; %s; charging %s; input %u mA charge %u mA (as read %u/%u, CNFG_00 %#x); "
		    "battery %d.%d C, charger %d.%d C; %u %% %u mV %d mA; DTLS %#x/%#x/%#x; BC %#x VBADC %u; float %u mV%s\n",
		    guard ? "" : "would: ", drain_until_s != 0 ? "the drain test" : chg_why_name(o.why), chg_limits[st.type].name,
		    o.enable ? "on" : "off",
		    o.input_ma, o.charge_ma, chg_input_ma_of(in.cnfg09), chg_charge_ma_of(in.cnfg02), in.cnfg00,
		    in.temp_dc / 10, (in.temp_dc < 0 ? -in.temp_dc : in.temp_dc) % 10, in.chg_temp_dc / 10,
		    (in.chg_temp_dc < 0 ? -in.chg_temp_dc : in.chg_temp_dc) % 10, in.soc, in.vcell_mv, k_current_ma, in.dtls00, in.dtls01,
		    dtls02, in.bc, in.vbadc, chg_float_mv_of(in.cnfg04), in.read_ok ? "" : " (A READ FAILED)");
		last_why = o.why;
		last_type = st.type;
		last_enable = o.enable;
	}
	starlte_muic_bus_give();
again:
	ticks++;
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(chg_call, deadline);
}

/* the 9 V path on a DCP wants the switch open: starlte_muic.c's tick leaves it so */
bool starlte_chg_afc_owns_switch(uint8_t bc);
bool
starlte_chg_afc_owns_switch(uint8_t bc)
{
	return chg_mode >= 2 && chg_afc != 0 && chg_type_of_bc(bc) == CHG_DCP;
}

__startup_func
static void
starlte_chg_init(void)
{
	uint64_t deadline;

	(void)PE_parse_boot_argn("sweetchg", &chg_mode, sizeof(chg_mode));
	(void)PE_parse_boot_argn("sweetafc", &chg_afc, sizeof(chg_afc));
	if (chg_mode <= 0) {
		return;
	}
	if (chg_mode > 2) {
		chg_mode = 2;
	}
	(void)PE_parse_boot_argn("sweetchg_drain", &chg_drain_min, sizeof(chg_drain_min));
	if (chg_mode == 2 && chg_drain_min > 0) {
		drain_until_s = now_s() + 60u * (uint32_t)(chg_drain_min > 30 ? 30 : chg_drain_min);
	}
	chg_state_reset(&st, 0);

	chg_call = thread_call_allocate_with_priority(chg_tick, NULL, THREAD_CALL_PRIORITY_KERNEL);
	/* the first look 5 s after this (the switch's code brings the bus up first) */
	clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(chg_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, starlte_chg_init);
