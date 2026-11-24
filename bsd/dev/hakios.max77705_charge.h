/* SPDX-License-Identifier: MIT-0 */
/*
 * The Galaxy S9's charging rules: what the MAX77705's charger may be told, for each kind of charger and each battery
 * temperature. Plain C, no kernel: the iOS 27 fork's charger guard (bsd/dev/starlte_muic.c, sweetchg= and sweetafc=) asks
 * these functions, and tools/max77705_charge_test.c checks them on a computer (tools/max77705-charge-test.sh).
 * docs/ios27/CHARGING.md has the facts and where each number comes from.
 *
 * Every number is Samsung's own for this phone (exynos9810-starlte_eur_open_26.dts of its kernel: cable-info's current
 * groups, the battery node's temperature bands and timers; drivers/battery_v2/max77705_charger.c for the register
 * encodings; drivers/muic/max77705-muic*.c for the charger types and the AFC answer). Nothing here goes above them:
 *   - the input limit and the charge current of each charger type, at most Samsung's for that type;
 *   - charging stopped at 41.0 C and above or 5.0 C and below (Samsung's "swelling" block; stricter than its 50/0 C
 *     health limits, and without its reduced-float recharging there), back at 39.0 C and below and 10.0 C and above;
 *     700 mA at most at 15.0 C and below (back above it at 20.0 C);
 *   - a charging session's time limited (Samsung's software timers: its hardware one is switched off by its driver);
 *   - 9 V (Samsung Adaptive Fast Charging) only on a charger the MUIC calls a dedicated charger (DCP), with the battery
 *     between 15.0 C and 39.0 C, the charger chip's own thermistor under 44.0 C, the battery not full, the chip
 *     reporting no fault, and only after the charger answered the 9 V request with VBUS read in the 7.5-10.5 V band.
 *     On any doubt: 5 V and the standard limits, or less.
 * The float voltage (CNFG_04) is never written: it is only read, and charging is stopped if it reads above 4350 mV.
 * Nothing of the chip's non-volatile memory is touched by anything that uses this file.
 */
#ifndef HAKIOS_MAX77705_CHARGE_H
#define HAKIOS_MAX77705_CHARGE_H

#include <stdbool.h>
#include <stdint.h>

/* ---- the chip's registers (charger at 0x69, MUIC at 0x25) ------------------------------------------------------- */

#define CHG_REG_INT_OK          0xB2    /* bit 6 CHGIN_OK */
#define CHG_REG_DTLS_00         0xB3    /* [6:5] CHGIN_DTLS: 3 = VBUS valid, 2 = over-voltage */
#define CHG_REG_DTLS_01         0xB4    /* [3:0] CHG_DTLS, [6:4] BAT_DTLS */
#define CHG_REG_DTLS_02         0xB5    /* [3:0] BYP_DTLS */
#define CHG_REG_CNFG_00         0xB7    /* [3:0] mode (bit 0 CHG, bit 2 BUCK), bit 4 WDTEN */
#define CHG_REG_CNFG_01         0xB8    /* [2:0] FCHGTIME, [5:4] CHG_RSTRT (read only here) */
#define CHG_REG_CNFG_02         0xB9    /* [5:0] the charge current, 50 mA a step; [7:6] OTG_ILIM (kept) */
#define CHG_REG_CNFG_04         0xBB    /* [5:0] the float voltage (read only here) */
#define CHG_REG_CNFG_06         0xBD    /* [1:0] WDTCLR, [3:2] CHGPROT (11: unlocked) */
#define CHG_REG_CNFG_09         0xC0    /* [6:0] the input limit, 25 mA a step; bit 7 CHG_EN (kept) */

#define CHG_CNFG00_CHG          0x01u
#define CHG_CNFG00_BUCK         0x04u
#define CHG_CNFG00_WDTEN        0x10u
#define CHG_CNFG02_CC_MASK      0x3Fu
#define CHG_CNFG09_LIM_MASK     0x7Fu
#define CHG_CNFG06_WDTCLR_MASK  0x03u
#define CHG_CNFG06_WDTCLR       0x01u
#define CHG_CNFG06_CHGPROT_MASK 0x0Cu

#define MUIC_REG_USBC_STATUS1   0x06    /* [7:4] VBADC: VBUS in bands (below) */
#define MUIC_REG_BC_STATUS      0x08    /* bit 7 VBUSDet, [5:3] PrChgTyp, bit 2 DCDTmo, [1:0] ChgTyp */
#define MUIC_OP_CONTROL1_W      0x06    /* the switch: COM_OPEN 0x00, COM_USB 0x09 */
#define MUIC_OP_AFC_RESULT      0x21    /* COMMAND_AFC_RESULT_READ: the AFC request with its byte; the answer at 0x51 */
#define AFC_TX_9V               0x46    /* Samsung's byte for 9 V (max77705_muic_afc_hv_set) */
#define AFC_TX_5V               0x08    /* ... and for 5 V */

/* VBADC (USBC_STATUS1 [7:4], and the AFC answer's second byte): 2 = 4.5-5.5 V, 5 = 7.5-8.5, 6 = 8.5-9.5, 7 = 9.5-10.5 */
#define VBADC_5V_MAX            2u
#define VBADC_9V_MIN            5u
#define VBADC_9V_MAX            7u

/* ---- the encodings (max77705_charger.c's set_/get_ functions) ---------------------------------------------------- */

/* CNFG_09: (code + 1) x 25 mA, 100 mA at codes 0-3, 3200 mA at 0x7F */
static inline uint32_t
chg_input_ma_of(uint8_t code)
{
	code &= CHG_CNFG09_LIM_MASK;
	return code <= 3 ? 100u : code >= 0x7F ? 3200u : (uint32_t)(code + 1u) * 25u;
}

/* the code for at most ma (rounded down; never above ma, never under 100 mA's code) */
static inline uint8_t
chg_input_code(uint32_t ma)
{
	if (ma < 100u) {
		return 0;
	}
	if (ma > 3200u) {
		ma = 3200u;
	}
	return (uint8_t)(ma / 25u - 1u);
}

/* CNFG_02: code x 50 mA, 100 mA at codes 0-2 */
static inline uint32_t
chg_charge_ma_of(uint8_t code)
{
	code &= CHG_CNFG02_CC_MASK;
	return code <= 2 ? 100u : (uint32_t)code * 50u;
}

static inline uint8_t
chg_charge_code(uint32_t ma)
{
	if (ma < 150u) {
		return 0;               /* (100 mA: the least the chip does) */
	}
	if (ma > 3150u) {
		ma = 3150u;
	}
	return (uint8_t)(ma / 50u);
}

/* CNFG_04 [5:0]: 4000 + 50 mV a step to 0x04, then 4200 + 10 mV a step */
static inline uint32_t
chg_float_mv_of(uint8_t code)
{
	code &= 0x3F;
	return code <= 4 ? 4000u + code * 50u : 4200u + (code - 4u) * 10u;
}

/* ---- the charger types and Samsung's limits for each ------------------------------------------------------------- */

enum chg_type {
	CHG_NONE = 0,           /* no VBUS */
	CHG_SDP,                /* a computer's plain USB port (BC 1.2 SDP): Samsung's USB */
	CHG_CDP,                /* a computer's charging port (BC 1.2 CDP) */
	CHG_DCP,                /* a dedicated charger (DCP): Samsung's TA, at 5 V */
	CHG_AFC_9V,             /* a DCP that answered the 9 V request: Samsung's 9V_TA */
	CHG_UNKNOWN,            /* VBUS with anything else: DCD time-out, an Apple or other special charger, ChgTyp 0 */
	CHG_TYPES
};

struct chg_limit {
	uint32_t input_ma;
	uint32_t charge_ma;
	uint32_t session_s;     /* the charging time allowed in one session (Samsung's *_charging_total_time) */
	const char *name;
};

/* cable-info (current_group_12: USB; _3: CDP; default: TA; _4: 9V_TA; _1: TIMEOUT), the battery node's total times */
static const struct chg_limit chg_limits[CHG_TYPES] = {
	[CHG_NONE]    = { 0, 0, 0, "no charger" },
	[CHG_SDP]     = { 475, 550, 36000, "a computer's USB port (SDP)" },
	[CHG_CDP]     = { 1500, 1500, 18000, "a computer's charging port (CDP)" },
	[CHG_DCP]     = { 1800, 2100, 18000, "a wall charger at 5 V (DCP)" },
	[CHG_AFC_9V]  = { 1650, 2150, 10800, "a fast charger at 9 V (AFC)" },
	[CHG_UNKNOWN] = { 500, 500, 18000, "an unknown charger" },
};

#define CHG_FLOAT_MAX_MV        4350u   /* chg_float_voltage */
#define CHG_PRE_AFC_INPUT_MA    500u    /* pre_afc_input_current: the input limit while the voltage is asked for */
#define CHG_LOW_TEMP_CHARGE_MA  700u    /* swelling_low_temp_current */

/* BC_STATUS -> the type. ChgTyp 1 with DCDTmo, or ChgTyp 1/2 with a special PrChgTyp, is not a plain port (Samsung's
 * max77705_muic_check_chgtyp: DCD time-out, "unofficial TA"): unknown, the lowest limits. */
static inline enum chg_type
chg_type_of_bc(uint8_t bc)
{
	unsigned chgtyp = bc & 3u, dcdtmo = (bc >> 2) & 1u, prchgtyp = (bc >> 3) & 7u;

	if (!(bc & 0x80u)) {
		return CHG_NONE;
	}
	if (chgtyp == 1 && dcdtmo) {
		return CHG_UNKNOWN;
	}
	if ((chgtyp == 1 || chgtyp == 2) && prchgtyp >= 1 && prchgtyp <= 5) {
		return CHG_UNKNOWN;
	}
	switch (chgtyp) {
	case 1: return CHG_SDP;
	case 2: return CHG_CDP;
	case 3: return CHG_DCP;
	default: return CHG_UNKNOWN;
	}
}

/* ---- the temperature bands (tenths of a degree, as Samsung's device tree) --------------------------------------- */

#define T_STOP_HIGH_DC          410     /* swelling_high_temp_block */
#define T_RESUME_HIGH_DC        390     /* swelling_high_temp_recov */
#define T_STOP_LOW_DC           50      /* swelling_low_temp_block_2nd */
#define T_RESUME_LOW_DC         100     /* swelling_low_temp_recov_2nd */
#define T_REDUCE_DC             150     /* swelling_low_temp_block_1st: 700 mA at most */
#define T_UNREDUCE_DC           200     /* swelling_low_temp_recov_1st */
#define T_FAST_MIN_DC           150     /* 9 V only above the reduced band ... */
#define T_FAST_MAX_DC           390     /* ... and under the swelling recovery point */
#define T_CHG_FAST_MAX_DC       440     /* the charger's thermistor: chg_high_temp_recovery (46.0 C limits Samsung's 9 V) */
#define T_ALARM_DC              450     /* the brief's own: above 45 C every change is given up */
#define T_PLAUSIBLE_MIN_DC      (-300)
#define T_PLAUSIBLE_MAX_DC      900

/* charging recharged after a full charge (recharge_condition_vcell, _soc) */
#define RECHARGE_MV             4280u
#define RECHARGE_SOC            98u

/* ---- the decision ------------------------------------------------------------------------------------------------ */

/* what the guard read this time */
struct chg_in {
	bool read_ok;           /* every register below read */
	uint8_t bc;             /* MUIC BC_STATUS */
	uint8_t vbadc;          /* MUIC USBC_STATUS1 [7:4] */
	uint8_t dtls00, dtls01; /* charger DETAILS_00, _01 */
	uint8_t cnfg00, cnfg02, cnfg04, cnfg09;
	bool temp_ok;           /* the battery's thermistor read */
	int32_t temp_dc;        /* the battery, tenths of a degree */
	bool chg_temp_ok;       /* the charger's thermistor read */
	int32_t chg_temp_dc;
	uint32_t soc;           /* percent */
	uint32_t vcell_mv;
	uint32_t now_s;         /* seconds, any monotonic origin */
	bool afc_wanted;        /* sweetafc=1 */
};

/* what the guard keeps from one time to the next */
struct chg_state {
	enum chg_type type;     /* the type in force: CHG_AFC_9V only after a good answer */
	bool stopped_hot, stopped_cold, reduced;        /* the bands' hysteresis */
	bool full;              /* the charger said done; charging again at the recharge condition */
	bool expired;           /* the session's time ran out: nothing until the cable goes */
	bool afc_failed;        /* the 9 V request went wrong once: not again until the cable goes */
	bool latched_off;       /* a fault (a read-back that differed, the float above 4350 mV, an implausible
	                         * temperature, 45 C): charging off, nothing written but that, until the cable goes */
	uint32_t session_start_s, charging_s, last_s;
	uint32_t dcp_since_s;   /* when the DCP was first seen (9 V asked 1 s after: Samsung waits 500 ms) */
	bool dcp_seen;
};

enum chg_why {
	WHY_OK = 0,             /* charging allowed at the type's limits */
	WHY_NO_VBUS,
	WHY_READ_FAILED,
	WHY_NO_TEMP,
	WHY_HOT,
	WHY_COLD,
	WHY_FULL,
	WHY_EXPIRED,
	WHY_FAULT,
	WHY_ALARM,
	WHY_FLOAT_HIGH,
};

/* what to do now */
struct chg_out {
	bool enable;            /* CNFG_00 bit 0 */
	uint32_t input_ma;      /* CNFG_09: what it is to be set to (0: leave it) */
	uint32_t charge_ma;     /* CNFG_02: ditto */
	bool ask_9v;            /* send the 9 V request now */
	bool ask_5v;            /* send the 5 V request now (back from 9 V) */
	bool rearm;             /* the charger's watchdog expired (CHG_DTLS 0xB) with charging wanted: kick it, and charging
	                         * off and on again, as Samsung's max77705_get_charging_health does */
	enum chg_why why;
};

static inline const char *
chg_why_name(enum chg_why why)
{
	static const char *const names[] = { "charging allowed", "no VBUS", "a read failed", "no battery temperature",
		                            "too hot", "too cold", "full", "the session's time ran out", "a fault",
		                            "45 C or more", "the float voltage reads above 4350 mV" };

	return (unsigned)why < sizeof(names) / sizeof(names[0]) ? names[why] : "?";
}

static inline bool
chg_temp_plausible(int32_t dc)
{
	return dc >= T_PLAUSIBLE_MIN_DC && dc <= T_PLAUSIBLE_MAX_DC;
}

/* CHG_DTLS: 0-2 charging (pre-qualification, fast CC, fast CV), 3 top-off, 4 done, 6 the fast-charge timer's fault, 8 off
 * (no valid input, or charging off), 0xA off at the junction's thermal shutdown, 0xB off because the charger's watchdog
 * expired (Samsung's driver counts 8, 0xA and 0xB as discharging, and re-arms the charger on 8 and 0xB) */
#define CHG_DTLS_DONE           0x4u
#define CHG_DTLS_WDT_EXPIRED    0xBu

/* the chip says something is wrong: VBUS over-voltage, the battery not OK, the timer's fault, thermal shutdown */
static inline bool
chg_chip_fault(const struct chg_in *in)
{
	unsigned chgin = (in->dtls00 >> 5) & 3u, bat = (in->dtls01 >> 4) & 7u, chg = in->dtls01 & 0xFu;

	if (chgin == 2) {
		return true;            /* CHGIN > OVLO */
	}
	if (bat == 0 || bat == 2 || bat == 5) {
		return true;            /* no battery, battery dead, battery over-voltage */
	}
	return chg == 0x6 || chg == 0xA;
}

/* the 9 V conditions, apart from the type */
static inline bool
chg_fast_band(const struct chg_in *in)
{
	return in->temp_ok && in->chg_temp_ok && in->temp_dc > T_FAST_MIN_DC && in->temp_dc <= T_FAST_MAX_DC &&
	       in->chg_temp_dc < T_CHG_FAST_MAX_DC && in->soc < RECHARGE_SOC;
}

/* a new cable (or none): everything of the last one forgotten */
static inline void
chg_state_reset(struct chg_state *s, uint32_t now_s)
{
	*s = (struct chg_state){ .type = CHG_NONE, .session_start_s = now_s, .last_s = now_s };
}

/*
 * The guard's one step: from what was read and what it remembers, what the charger is to be told. Every limit given is at
 * most Samsung's for the type in force; charging is off unless every condition holds.
 */
static inline struct chg_out
chg_step(struct chg_state *s, const struct chg_in *in)
{
	struct chg_out o = { .enable = false, .input_ma = 0, .charge_ma = 0, .why = WHY_OK };
	enum chg_type seen;
	const struct chg_limit *lim;
	uint32_t dt;

	if (!in->read_ok) {
		/* nothing known: off is asked, nothing else (the caller writes only CNFG_00 when it could read it) */
		o.why = WHY_READ_FAILED;
		return o;
	}
	seen = chg_type_of_bc(in->bc);
	if (seen == CHG_NONE) {
		chg_state_reset(s, in->now_s);
		o.why = WHY_NO_VBUS;
		return o;
	}
	/* the type in force: a 9 V session stays one while the MUIC still says DCP; anything else starts again */
	if (s->type == CHG_NONE || (s->type != seen && !(s->type == CHG_AFC_9V && seen == CHG_DCP))) {
		bool was_9v = s->type == CHG_AFC_9V;

		chg_state_reset(s, in->now_s);
		s->type = seen;
		if (was_9v) {
			s->afc_failed = true;   /* (the MUIC changed its mind under 9 V: no new request on this cable) */
		}
	}
	dt = in->now_s >= s->last_s ? in->now_s - s->last_s : 0;
	s->last_s = in->now_s;

	/* the faults: off and latched until the cable goes */
	if (s->latched_off) {
		o.why = WHY_FAULT;
		goto drop_9v;
	}
	if (chg_float_mv_of(in->cnfg04) > CHG_FLOAT_MAX_MV) {
		s->latched_off = true;
		o.why = WHY_FLOAT_HIGH;
		goto drop_9v;
	}
	if (!in->temp_ok || !chg_temp_plausible(in->temp_dc)) {
		o.why = WHY_NO_TEMP;
		goto drop_9v;
	}
	if (in->temp_dc >= T_ALARM_DC) {
		s->latched_off = true;
		o.why = WHY_ALARM;
		goto drop_9v;
	}
	if (chg_chip_fault(in)) {
		s->latched_off = true;
		o.why = WHY_FAULT;
		goto drop_9v;
	}

	/* the bands, with Samsung's hysteresis */
	if (in->temp_dc >= T_STOP_HIGH_DC) {
		s->stopped_hot = true;
	} else if (in->temp_dc <= T_RESUME_HIGH_DC) {
		s->stopped_hot = false;
	}
	if (in->temp_dc <= T_STOP_LOW_DC) {
		s->stopped_cold = true;
	} else if (in->temp_dc >= T_RESUME_LOW_DC) {
		s->stopped_cold = false;
	}
	if (in->temp_dc <= T_REDUCE_DC) {
		s->reduced = true;
	} else if (in->temp_dc >= T_UNREDUCE_DC) {
		s->reduced = false;
	}

	/* full, and charging again at Samsung's recharge condition */
	if ((in->dtls01 & 0xFu) == CHG_DTLS_DONE) {
		s->full = true;
	} else if (s->full && (in->vcell_mv < RECHARGE_MV || in->soc < RECHARGE_SOC)) {
		s->full = false;
		s->session_start_s = in->now_s;
		s->charging_s = 0;
	}

	/* the session's time: counted while charging is allowed and the chip charges */
	if ((in->cnfg00 & CHG_CNFG00_CHG) && (in->dtls01 & 0xFu) <= 0x3) {
		s->charging_s += dt;
	}
	lim = &chg_limits[s->type];
	if (s->charging_s >= lim->session_s) {
		s->expired = true;
	}

	if (s->stopped_hot) {
		o.why = WHY_HOT;
		goto drop_9v;
	}
	if (s->stopped_cold) {
		o.why = WHY_COLD;
		goto drop_9v;
	}
	if (s->expired) {
		o.why = WHY_EXPIRED;
		goto drop_9v;
	}

	/* 9 V: kept only inside its band; asked for once a DCP has been seen for a second, inside it */
	if (s->type == CHG_AFC_9V && (!chg_fast_band(in) || in->vbadc < VBADC_9V_MIN || in->vbadc > VBADC_9V_MAX ||
	    s->reduced || s->full)) {
		s->type = CHG_DCP;
		s->afc_failed = true;
		o.ask_5v = true;
	}
	if (s->type == CHG_DCP && !s->dcp_seen) {
		s->dcp_seen = true;
		s->dcp_since_s = in->now_s;
	}
	if (s->type == CHG_DCP && in->afc_wanted && !s->afc_failed && !s->full && !s->reduced && chg_fast_band(in) &&
	    in->vbadc <= VBADC_5V_MAX && in->now_s - s->dcp_since_s >= 1) {
		o.ask_9v = true;
	}

	lim = &chg_limits[s->type];
	o.enable = !s->full;
	o.why = s->full ? WHY_FULL : WHY_OK;
	o.rearm = o.enable && (in->dtls01 & 0xFu) == CHG_DTLS_WDT_EXPIRED;
	o.input_ma = lim->input_ma;
	o.charge_ma = lim->charge_ma;
	if (s->reduced && o.charge_ma > CHG_LOW_TEMP_CHARGE_MA) {
		o.charge_ma = CHG_LOW_TEMP_CHARGE_MA;
	}
	/* VBUS above 5.5 V without a 9 V session in force (a request on its way, or a charger that went up by itself):
	 * the input held at the pre-AFC limit, which is under every type's */
	if (s->type != CHG_AFC_9V && in->vbadc > VBADC_5V_MAX) {
		o.input_ma = CHG_PRE_AFC_INPUT_MA;
	}
	if (o.ask_9v) {
		o.input_ma = CHG_PRE_AFC_INPUT_MA;
	}
	return o;

drop_9v:
	/* charging off; a 9 V session asked back to 5 V, the input held at the pre-AFC limit meanwhile */
	if (s->type == CHG_AFC_9V || in->vbadc > VBADC_5V_MAX) {
		if (s->type == CHG_AFC_9V) {
			s->type = CHG_DCP;
			o.ask_5v = true;
		}
		s->afc_failed = true;
		o.input_ma = CHG_PRE_AFC_INPUT_MA;
	}
	return o;
}

/* The answer to the 9 V request (OPCODE_READ: the opcode, the result, VBADC): true only for result 0 with VBUS in the
 * 7.5-10.5 V bands, read again from USBC_STATUS1 as well (Samsung: AFC_CHARGER_9V for those bands). */
static inline bool
chg_afc_answer_9v(const uint8_t answer[3], uint8_t vbadc_now)
{
	return answer[0] == MUIC_OP_AFC_RESULT && answer[1] == 0 && answer[2] >= VBADC_9V_MIN && answer[2] <= VBADC_9V_MAX &&
	       vbadc_now >= VBADC_9V_MIN && vbadc_now <= VBADC_9V_MAX;
}

/* After the answer: the state's type moves to 9 V only with a good one; a bad one is remembered for the cable. */
static inline void
chg_afc_result(struct chg_state *s, bool good)
{
	if (s->type != CHG_DCP) {
		return;
	}
	if (good) {
		s->type = CHG_AFC_9V;
	} else {
		s->afc_failed = true;
	}
}

/* A written register read back: the bits written must read as written (the guard's watchdog). */
static inline bool
chg_readback_ok(uint8_t wrote, uint8_t read, uint8_t mask)
{
	return (wrote & mask) == (read & mask);
}

#endif /* HAKIOS_MAX77705_CHARGE_H */
