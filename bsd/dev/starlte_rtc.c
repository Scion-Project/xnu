/*
 * STARLTE (Galaxy S9) iOS 27 line: the calendar at start-up. This tree's platform expert answers 0 for the time of day, so
 * every boot starts on 1 January 1970 (the lock screen says "Wed Dec 31" in Pacific time) until timed hears from the
 * network. Two sources, both OFF by default:
 *   sweetepoch=SECONDS  a floor: the calendar starts no earlier than this (the time the image was made; mkimg writes it)
 *   rtc_pmic=1          read the S2MPS18 PMIC's real-time clock (block 2) through ACPM, READ ONLY, and use it when it is
 *                       valid and not behind sweetepoch=. Port of scion-kernel drivers/power/hakios.s2mps18_rtc.c (16.5 line),
 *                       with this tree's ACPM path (starlte_pmic_read, bsd/dev/starlte_touch.c).
 * Nothing is written to the PMIC. Log lines start with "STARLTE RTC:".
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <stdbool.h>

extern bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
#define HAKIOS_ACPM_PMIC_RTC 2

#define RTC_CTRL        0x00
#define RTC_UPDATE      0x02
#define RTC_CAPSEL      0x03
#define RTC_MSEC        0x04
#define RTC_SEC         0x05
#define RTC_MIN         0x06
#define RTC_HOUR        0x07
#define RTC_WEEK        0x08
#define RTC_DAY         0x09
#define RTC_MONTH       0x0A
#define RTC_YEAR        0x0B
#define RTC_REGS        12              /* 0x00 to 0x0B */

#define CTRL_BCD        0x01
#define CTRL_24H        0x02

static int rtc_say(const char *format, ...) __printflike(1, 2);

static int
rtc_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	return printf("STARLTE RTC: %s", line);
}

/* seconds since 1970 of a UTC date (year 1970 and on) */
static uint64_t
rtc_days_secs(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t min, uint32_t sec)
{
	static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	uint64_t days = (uint64_t)(year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400 +
	    before[month - 1] + (day - 1);
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	if (month > 2 && leap) {
		days++;
	}
	return days * 86400 + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
}

/* the date of seconds since 1970, for the log */
static void
rtc_date(uint64_t t, uint32_t *y, uint32_t *mo, uint32_t *d, uint32_t *h, uint32_t *mi, uint32_t *s)
{
	uint64_t days = t / 86400, rest = t % 86400;
	uint32_t year = 1970;

	for (;;) {
		uint32_t len = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;

		if (days < len) {
			break;
		}
		days -= len;
		year++;
	}
	*y = year;
	*mo = 1;
	for (; *mo < 12; (*mo)++) {
		uint64_t next = rtc_days_secs(year, *mo + 1, 1, 0, 0, 0) - rtc_days_secs(year, *mo, 1, 0, 0, 0);

		if (days < next / 86400) {
			break;
		}
		days -= next / 86400;
	}
	*d = (uint32_t)days + 1;
	*h = (uint32_t)(rest / 3600);
	*mi = (uint32_t)(rest / 60 % 60);
	*s = (uint32_t)(rest % 60);
}


/* seconds since 1970 for the platform expert's getGMTTimeOfDay(); 0 when neither boot-arg is given (the old answer) */
long starlte_rtc_boot_time(void);

long
starlte_rtc_boot_time(void)
{
	uint64_t epoch = 0;
	uint32_t on = 0, year, month, day, hour, min, sec, y, mo, d, h, mi, s;
	uint8_t r[RTC_REGS];
	uint64_t clock, now, up_ns;
	const char *why = NULL;
	static bool said;
	static long answer;

	(void)PE_parse_boot_argn("sweetepoch", &epoch, sizeof(epoch));
	if (!PE_parse_boot_argn("rtc_pmic", &on, sizeof(on)) || on == 0) {
		if (epoch != 0 && !said) {
			said = true;
			rtc_say("rtc_pmic off: the calendar starts at sweetepoch= %llu\n", epoch);
		}
		return (long)epoch;
	}
	if (said) {             /* asked again (wake, power-management logs): the first answer, no second read */
		return answer;
	}
	said = true;
	answer = (long)epoch;
	for (uint32_t i = 0; i < RTC_REGS; i++) {
		if (!starlte_pmic_read(HAKIOS_ACPM_PMIC_RTC, i, &r[i])) {
			rtc_say("reading register 0x%02x failed: not used; the calendar starts at sweetepoch= %llu\n", i, epoch);
			return (long)epoch;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &up_ns);
	rtc_say("CTRL 0x%02x UPDATE 0x%02x CAPSEL 0x%02x MSEC 0x%02x SEC 0x%02x MIN 0x%02x HOUR 0x%02x WEEK 0x%02x "
	    "DAY 0x%02x MONTH 0x%02x YEAR 0x%02x\n", r[RTC_CTRL], r[RTC_UPDATE], r[RTC_CAPSEL], r[RTC_MSEC], r[RTC_SEC],
	    r[RTC_MIN], r[RTC_HOUR], r[RTC_WEEK], r[RTC_DAY], r[RTC_MONTH], r[RTC_YEAR]);
	sec = r[RTC_SEC] & 0x7f;
	min = r[RTC_MIN] & 0x7f;
	hour = r[RTC_HOUR] & 0x1f;
	day = r[RTC_DAY] & 0x1f;
	month = r[RTC_MONTH] & 0x0f;
	year = 2000 + (r[RTC_YEAR] & 0x7f);
	if ((r[RTC_CTRL] & (CTRL_BCD | CTRL_24H)) != CTRL_24H) {
		why = "CTRL is not binary, 24-hour: not set up";
	} else if ((r[RTC_CAPSEL] & 0xf0) != 0xf0) {
		why = "CAPSEL[7:4] is not 0xF: the clock was reset";
	} else if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
		why = "a field out of range";
	}
	if (why != NULL) {
		rtc_say("not used (%s); the calendar starts at sweetepoch= %llu\n", why, epoch);
		return (long)epoch;
	}
	clock = rtc_days_secs(year, month, day, hour, min, sec);
	now = clock;
	if (up_ns >= 2 * NSEC_PER_SEC && up_ns <= 600 * NSEC_PER_SEC) {
		now += up_ns / NSEC_PER_SEC - 1;
	}
	rtc_say("the clock says %u-%02u-%02u %02u:%02u:%02u (%llu); uptime %llu s: %llu\n",
	    year, month, day, hour, min, sec, clock, up_ns / NSEC_PER_SEC, now);
	if (year < 2024 || year > 2099) {
		why = "the year is not 2024-2099: the clock was never set";
	} else if (epoch != 0 && now < epoch) {
		why = "it is behind sweetepoch=, the time the image was made";
	}
	if (why != NULL) {
		rtc_say("not used (%s); the calendar starts at sweetepoch= %llu\n", why, epoch);
		return (long)epoch;
	}
	rtc_date(now, &y, &mo, &d, &h, &mi, &s);
	rtc_say("using the clock: %u-%02u-%02u %02u:%02u:%02u UTC\n", y, mo, d, h, mi, s);
	answer = (long)now;
	return answer;
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
