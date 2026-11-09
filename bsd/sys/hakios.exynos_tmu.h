/*
 * Ported to the Starbite iOS 27 (xnu-12377) STARLTE tree from HakiOS/Scion (Scion-Project/scion-kernel,
 * drivers/power/exynos/hakios.exynos_tmu.h, APSL), unchanged but for this note and its place (bsd/sys/).
 *
 * The Exynos 9810's TMU (its thermal management unit): the registers, the sensors and the arithmetic that turns a sensor's
 * code into degrees. Plain C, nothing of the kernel, so that tools/exynos_tmu_test.c checks the same arithmetic the driver
 * (hakios.exynos_tmu.c) runs. Only facts are taken from Samsung's kernel (GPL-2.0); none of its code is here.
 *
 * Sources (the S9's kernel fork, Scion-Project/android_kernel_samsung_exynos9810, lineage-24.0 at e5906667068e):
 *   drivers/thermal/samsung/exynos_tmu.c   the register map (69-187), the code-to-degrees arithmetic
 *                                           (code_to_temp_with_sensorinfo, 317-344, and its inverse 252-278), the trim
 *                                           read from TRIMINFO (exynos9810_tmu_initialize, 389-422), the start
 *                                           (exynos9810_tmu_control, 554-627; tmu_core_enable, 471-481), the trims of
 *                                           TMU_TOP that both blocks take (exynos_tmu_probe, 1581-1592), where each
 *                                           sensor's code is (all_temp_show, 1244-1255), the thresholds (432-460)
 *   include/soc/samsung/tmu.h:15-16        EXYNOS_MAX_TEMP 125, EXYNOS_MIN_TEMP 10 (the BSP's clamp)
 *   include/dt-bindings/thermal/thermal_exynos.h:22-24   calibration types: 0 one point, 1 two points
 *   arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts   the four TMU nodes (10597-10690) and thermal-zones
 *                                           (10696-11070); the same in Silicium's Resources/DTBs/starlte.dts (10519-10610,
 *                                           10618 on), the phone's own tree
 */
#ifndef _HAKIOS_EXYNOS_TMU_H_
#define _HAKIOS_EXYNOS_TMU_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/hakios.exynos_soc.h>
/* the two blocks: TMU_TOP (the BIG and LITTLE zones) and TMU_SUB (G3D and ISP); reg = <0 0x10070000 0x700> and 0x10074000 */
#define TMU_TOP_PA              ((uint32_t)EXYNOS_TMU_TOP_PA)
#define TMU_SUB_PA              ((uint32_t)EXYNOS_TMU_SUB_PA)
#define TMU_SIZE                0x700u

/* registers (exynos_tmu.c:69-187) */
#define TMU_TRIMINFO(s)         ((s) <= 7 ? (uint32_t)(s) * 4 : ((uint32_t)(s) - 8) * 4 + 0x400)       /* 69-73 */
#define TMU_CONTROL             0x20u           /* 74: [28:24] BUF_VREF_SEL, [15:13] TRIP_MODE, 12 THERM_TRIP_EN, [11:8] BUF_SLOPE_SEL, 0 CORE_EN */
#define TMU_CONTROL1            0x24u           /* 75: [19:16] NUM_PROBE, 10 LPI_MODE_EN */
#define TMU_STATUS              0x28u           /* 76 */
#define TMU_COUNTER_VALUE0      0x30u           /* 173: [15:0] EN_TEMP_SEN_OFF */
#define TMU_COUNTER_VALUE1      0x34u           /* 176: [31:16] CLK_SENSE_ON */
#define TMU_AVG_CON             0x38u           /* 162: [2:0] AVG_MODE, 4 EN_DEM */
#define TMU_TRIM0               0x3Cu           /* 146: [23:20] BGRI_TRIM, [15:12] VREF_TRIM, [11:8] VBEI_TRIM */
#define TMU_EMUL_CON            0x160u          /* 160: 0 the emulation on; its code at [15:7] */

#define TMU_CTRL_CORE_EN        (1u << 0)       /* 94 */
#define TMU_CTRL_TRIP_EN        (1u << 12)      /* 104 */
#define TMU_CTRL_TRIP_MODE(v)   (((v) >> 13) & 0x7u)            /* 102-103 */
#define TMU_CTRL_VREF_SHIFT     24              /* 90-91, mask 0x1f */
#define TMU_CTRL_SLOPE_SHIFT    8               /* 92-93, mask 0xf */
#define TMU_CON1_NUM_PROBE(v)   (((v) >> 16) & 0xfu)            /* 182-183 */
#define TMU_CON1_LPI            (1u << 10)      /* 184-185 */
#define TMU_AVG_MODE_MASK       0x7u            /* 165 */
#define TMU_AVG_DEM             (1u << 4)       /* 170-171 */
#define TMU_SENSE_VALUE         0x028Au         /* 180: EXYNOS_TMU_TEM1051X_SENSE_VALUE (the dt's tmu_sensor_type 8 is TEM1051X) */
#define TMU_EMUL_ENABLE         1u              /* 114 */

/* TRIMINFO fields */
#define TMU_TRIM_CAL_TYPE(v)    (((v) >> 23) & 0x1u)            /* 132-133, of TRIMINFO(0): the block's calibration */
#define TMU_TRIM_ERROR1(v)      ((v) & 0x1ffu)                  /* 134-135, 399: the code at 25 degrees */
#define TMU_TRIM_ERROR2(v)      (((v) >> 9) & 0x1ffu)           /* 136, 400: the code at 85 degrees */
#define TMU_TRIM_FIELD(v)       (((v) >> 18) & 0x1fu)           /* 139-147: VREF_SEL (TRIMINFO0, 5 bits) */
#define TMU_TRIM_FIELD4(v)      (((v) >> 18) & 0xfu)            /* SLOPE_SEL (TRIMINFO1); BGRI/VREF/VBEI (TRIMINFO3/4/5) */
#define TMU_TRIM_AVG_MODE(v)    (((v) >> 18) & 0x7u)            /* 163-165, 576: of TRIMINFO(2) */

/* the current codes, 9 bits each (77-82; all_temp_show 1244-1255): 0 and 1 in 0x40, 2-4 in 0x44, 5-7 in 0x48 */
static inline uint32_t
tmu_temp_reg(unsigned sensor)
{
	if (sensor < 2) {
		return 0x40u;
	}
	if (sensor < 8) {
		return 0x40u + ((sensor - 2) / 3 + 1) * 4;
	}
	return sensor < 10 ? 0x440u : 0x444u + ((sensor - 10) / 3) * 4;       /* 80-82: 8-9, 10-12, 13-15 */
}

static inline unsigned
tmu_temp_shift(unsigned sensor)
{
	if (sensor < 2) {
		return 9 * sensor;
	}
	if (sensor < 8) {
		return 9 * ((sensor - 2) % 3);
	}
	return sensor < 10 ? 9 * (sensor - 8) : 9 * ((sensor - 10) % 3);
}

/* the rising thresholds of a sensor: RISE7 in [24:16] of this word, RISE6 in [8:0]; the falling ones 0x10 on (432-460) */
static inline uint32_t
tmu_thd_reg(unsigned sensor)
{
	if (sensor == 0) {
		return 0x50u;                           /* 116, 436 */
	}
	if (sensor < 8) {
		return 0x170u + (sensor - 1) * 0x20;    /* 117, 438 */
	}
	return 0x450u + (sensor - 8) * 0x20;            /* 118, 440 */
}

/* a sensor's interrupt enables (523-528): bit 7 RISE7 ... bit 0 RISE0, the falling ones 16 up */
static inline uint32_t
tmu_inten_reg(unsigned sensor)
{
	if (sensor < 5) {
		return 0x110u + 0x10 * sensor;
	}
	if (sensor < 8) {
		return 0x310u + 0x10 * (sensor - 5);
	}
	return 0x650u + 0x10 * (sensor - 8);
}

/* the calibration (the dt's samsung,tmu_* of all four nodes) */
#define TMU_FIRST_POINT         25              /* tmu_first_point_trim */
#define TMU_SECOND_POINT        85              /* tmu_second_point_trim */
#define TMU_EFUSE_VALUE         55              /* tmu_efuse_value: error1 when the fuse holds 0 (411-412) */
#define TMU_BSP_MIN             10              /* tmu.h: what the BSP clamps to */
#define TMU_BSP_MAX             125

struct tmu_cal {
	uint16_t        error1;         /* the code at 25 degrees */
	uint16_t        error2;         /* the code at 85 degrees (two-point only) */
	uint8_t         two_point;      /* TRIMINFO(0) bit 23 */
	uint8_t         fell_back;      /* 1: error1 from the dt (the fuse read 0); 2: two-point not usable, one-point used */
};

/*
 * A sensor's calibration from its TRIMINFO word and the block's type, as exynos9810_tmu_initialize() (389-422) takes it,
 * with one difference: where the BSP would divide by error2 - error1 <= 0 (a two-point block whose second fuse is empty:
 * its fallback, the dt's efuse value 55 >> 9, is 0), this uses one-point arithmetic and says so (fell_back 2).
 */
static inline struct tmu_cal
tmu_cal_of(uint32_t triminfo, uint32_t triminfo0)
{
	struct tmu_cal c = { 0, 0, 0, 0 };

	c.two_point = (uint8_t)TMU_TRIM_CAL_TYPE(triminfo0);
	c.error1 = (uint16_t)TMU_TRIM_ERROR1(triminfo);
	if (c.error1 == 0) {
		c.error1 = TMU_EFUSE_VALUE & 0x1ff;
		c.fell_back = 1;
	}
	if (c.two_point) {
		c.error2 = (uint16_t)TMU_TRIM_ERROR2(triminfo);
		if (c.error2 == 0) {
			c.error2 = (TMU_EFUSE_VALUE >> 9) & 0x1ff;
		}
		if (c.error2 <= c.error1) {
			c.two_point = 0;
			c.fell_back = 2;
		}
	}
	return c;
}

/*
 * A code in millidegrees, unclamped (code_to_temp_with_sensorinfo, 322-335, worked in millidegrees: the BSP's integer
 * degrees are this divided by 1000 and truncated toward zero).
 */
static inline int32_t
tmu_code_to_mc(const struct tmu_cal *c, uint32_t code)
{
	int32_t d = (int32_t)code - (int32_t)c->error1;

	if (c->two_point) {
		return (int32_t)((int64_t)d * (TMU_SECOND_POINT - TMU_FIRST_POINT) * 1000 / ((int32_t)c->error2 - (int32_t)c->error1)) +
		       TMU_FIRST_POINT * 1000;
	}
	return (d + TMU_FIRST_POINT) * 1000;
}

/* the BSP's own result for a code: integer degrees, clamped to 10-125 (317-344) */
static inline int32_t
tmu_code_to_bsp_c(const struct tmu_cal *c, uint32_t code)
{
	int32_t d = (int32_t)code - (int32_t)c->error1, t;

	if (c->two_point) {
		t = d * (TMU_SECOND_POINT - TMU_FIRST_POINT) / ((int32_t)c->error2 - (int32_t)c->error1) + TMU_FIRST_POINT;
	} else {
		t = d + TMU_FIRST_POINT;
	}
	return t > TMU_BSP_MAX ? TMU_BSP_MAX : (t < TMU_BSP_MIN ? TMU_BSP_MIN : t);
}

/* degrees to a code (temp_to_code_with_sensorinfo, 252-278): to decode a threshold the boot left, and for the test */
static inline int32_t
tmu_c_to_code(const struct tmu_cal *c, int32_t temp)
{
	temp = temp > TMU_BSP_MAX ? TMU_BSP_MAX : (temp < TMU_BSP_MIN ? TMU_BSP_MIN : temp);
	if (c->two_point) {
		return (temp - TMU_FIRST_POINT) * ((int32_t)c->error2 - (int32_t)c->error1) / (TMU_SECOND_POINT - TMU_FIRST_POINT) +
		       (int32_t)c->error1;
	}
	return temp + (int32_t)c->error1 - TMU_FIRST_POINT;
}

/* a reading believed: a code that is neither all zeros nor all ones, and -40 to 160 degrees */
static inline bool
tmu_reading_ok(uint32_t code, int32_t mc)
{
	return code != 0 && code != 0x1ff && mc >= -40000 && mc <= 160000;
}

/*
 * The zones: which sensors of which block (the dt's "sensors" masks: BIG 94 = 1,2,3,4,6 and LITTLE 32 = 5 of TMU_TOP; G3D 2
 * = 1 and ISP 4 = 2 of TMU_SUB), and two of each zone's trips from thermal-zones: where Android starts to cool it (BIG
 * big-control-temp 83, the power allocator's target, and hotplug_out_threshold 96 takes the big cores away; LITTLE
 * little-alert1 76; G3D gpu-control-temp 88; ISP isp-alert1 91), and its "hot" trip, 115 for all four, the one
 * exynos9810_tmu_initialize() writes as RISE7.
 */
enum { TMU_BLOCK_TOP = 0, TMU_BLOCK_SUB = 1 };

static const struct tmu_zone {
	const char *    name;
	uint8_t         block;
	uint16_t        sensors;        /* the dt's mask */
	int32_t         android_cools_mc;
	int32_t         android_hot_mc;
} tmu_zones[] = {
	{ "big", TMU_BLOCK_TOP, 0x5e, 83000, 115000 },
	{ "little", TMU_BLOCK_TOP, 0x20, 76000, 115000 },
	{ "g3d", TMU_BLOCK_SUB, 0x02, 88000, 115000 },
	{ "isp", TMU_BLOCK_SUB, 0x04, 91000, 115000 },
};
#define TMU_NZONES      (sizeof(tmu_zones) / sizeof(tmu_zones[0]))
#define TMU_ANDROID_HOT_MC      115000

#endif /* _HAKIOS_EXYNOS_TMU_H_ */
