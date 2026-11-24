/*
 * SWEET M3OFF: an M3 (a big core of the Galaxy S9's Exynos 9810) taken offline and started again, the way Samsung's
 * hotplug governor does before it lets the other M3s run past 1794 MHz. Off unless the boot-argument m3off is given.
 * scion-kernel docs/ios27/BIG-CORES.md.
 */
#ifndef _ARM64_SWEET_M3OFF_H_
#define _ARM64_SWEET_M3OFF_H_

#include <pexpert/arm64/board_config.h>

#if defined(STARLTE)
#define SWEET_M3OFF 1
#else
#define SWEET_M3OFF 0
#endif

#if SWEET_M3OFF && !defined(__ASSEMBLER__)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

__BEGIN_DECLS
/* m3off=: 0 (default) nothing; 1 decide and log only (no core is touched); 2 take M3s offline and start them again */
extern uint32_t sweet_m3off_mode;

/* SweetCPU::startAll(), once every core runs: reads the boot-arguments, maps the PMU */
void sweet_m3off_init(void);

/* SweetCPU::quiesceCPU(), on the core going offline (cpu_sleep(), interrupts masked): the PMU's "off" writes, then
 * PSCI CPU_OFF; never returns */
void sweet_m3off_dying(void) __dead2;

/* SweetCPU::initCPU(false), on a core started again: the PMU's "on" writes; false if this file did not take it down */
bool sweet_m3off_back(unsigned int cpu);

/* SweetCPU::startCPU(): this core may be started again (this file took it offline and the PMU reads it off) */
bool sweet_m3off_may_start(unsigned int cpu);

/* thread context, never the core itself: offline (0 when it is, PMU-checked) and online again (0 when it runs). Each
 * panics (and so resets into recovery, sweetreboot) when a core does not go or does not come back. */
int sweet_m3off_take(unsigned int cpu);
int sweet_m3off_give(unsigned int cpu);

/* the M3s this file has taken offline and the PMU reads off now (0 to 3; cpu 4 is never taken) */
uint32_t sweet_m3off_count(void);
/* this logical core is offline or on its way (not to be measured, bound to, or counted) */
bool sweet_m3off_is_off(unsigned int cpu);
/* the counters as text; returns the length written */
size_t sweet_m3off_format(char *buf, size_t len);
__END_DECLS
#endif

#endif /* _ARM64_SWEET_M3OFF_H_ */
