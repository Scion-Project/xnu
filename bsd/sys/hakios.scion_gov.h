/*
 * The scion CPU governor's control law (bsd/dev/hakios.scion_gov.c) and what a SoC gives it: its limits, as data.
 */
#ifndef _SYS_HAKIOS_SCION_GOV_H_
#define _SYS_HAKIOS_SCION_GOV_H_

#include <stdbool.h>
#include <stdint.h>

#define SCION_BIG_CORES         4

struct scion_soc {
	struct {
		uint32_t cores, khz;
	} big_limit[2];                 /* the fastest rate for at most this many big cores, few cores first */
	uint32_t big_floor, big_touch, big_frame, big_safe;     /* kHz */
	uint32_t lit_floor, lit_touch, lit_safe;
	uint32_t heavy_pct, idle_pct, lit_heavy_pct;            /* a big core's load that is heavy, is idle; a little core's that is heavy */
	uint32_t change_ms;             /* how long the load must ask before cores are parked */
	int32_t park_under_mc, unpark_at_mc;    /* the big zone, read each window: parked only below the first, back at the second */
};

struct scion_rate {
	uint32_t low_ms, peak;          /* how long the load has wanted less, and the most it wanted meanwhile */
};

struct scion_park {
	uint32_t asked_ms;
};

struct scion_pace {
	uint32_t floor, calm_ms;        /* the floor now (0: start over), how long only on-time frames have come at it */
};

enum scion_state { SCION_STATIC, SCION_TOUCHED, SCION_ANIMATING, SCION_STATES };

/* what the phone is doing now; quiet_ms since the last frame or finger */
enum scion_state scion_state(uint32_t fingers, bool moving, uint64_t quiet_ms, enum scion_state was);

/* a temperature to trust: read, plausible and recent */
bool scion_temp_ok(int32_t mc, uint64_t age_ms);

/* the rate for this window: levels fastest first, a rate from them, kHz */
uint32_t scion_rate(struct scion_rate *s, const uint32_t *levels, uint32_t n, uint32_t cur, uint32_t load, uint32_t floor,
    uint32_t ceiling, uint32_t window_ms);

/* the floor for this window from the frames since the last: levels fastest first, a kHz floor at or above low; a fresh s starts at start */
uint32_t scion_pace(struct scion_pace *s, const uint32_t *levels, uint32_t n, uint32_t late, uint32_t on_time, uint32_t low,
    uint32_t start, uint32_t window_ms);

/* how many big cores may run: load[] the big cores' loads, busiest first; mc the big zone now (a temperature scion_temp_ok() took) */
uint32_t scion_cores(struct scion_park *s, const struct scion_soc *soc, uint32_t cores, const uint32_t *load, uint32_t lit_load,
    uint32_t cur, int32_t mc, uint32_t window_ms);

/* the fastest rate for this many big cores */
uint32_t scion_limit(const struct scion_soc *soc, uint32_t cores);

#endif /* _SYS_HAKIOS_SCION_GOV_H_ */
