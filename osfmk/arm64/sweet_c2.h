/*
 * SWEET C2: core power-down in idle ("c2", PSCI CPU_SUSPEND with a power-down parameter) for the Galaxy S9
 * (STARLTE), and the same generic part on FRANKEL for QEMU. Off unless the boot-argument sweetc2 or sweetc2dry is given;
 * without either, cpu_idle() runs exactly the WFI it ran before. NOT run on a phone. scion-kernel docs/ios27/CPU-IDLE.md.
 */
#ifndef _ARM64_SWEET_C2_H_
#define _ARM64_SWEET_C2_H_

#include <pexpert/arm64/board_config.h>

#if defined(STARLTE) || defined(FRANKEL)
#define SWEET_C2 1
#else
#define SWEET_C2 0
#endif

#if SWEET_C2 && !defined(__ASSEMBLER__)
#include <stdint.h>
#include <stddef.h>

struct cpu_data;

/* Set once by sweet_c2_start() when a mode is asked for; read by cpu_idle() before its WFI. */
extern uint32_t sweet_c2_on;

/* cpu_idle(), interrupts masked, in place of its WFI: either WFI, or the power-down (which does not return to here
 * when the core lost power: the core comes back through the reset vector and arm_init_idle_cpu()). */
void sweet_c2_idle(struct cpu_data *cdp);

/* arm_init_idle_cpu(), on the core that came back through the reset vector, before cpu_idle_exit(TRUE). */
void sweet_c2_resume(struct cpu_data *cdp);

/* SweetCPU::startAll(), once every core has been started: reads the boot-arguments, and if one asks for it starts
 * the thread that arms c2 after sweetc2delay seconds and prints the counters. */
void sweet_c2_start(void);

/* A driver that cannot wait an exit latency (160 to 220 us) for an interrupt holds c2 off while it needs that. */
void sweet_c2_block(int on);

/* The counters as text (kern.hakios.c2.stats); returns the length written. */
size_t sweet_c2_format(char *buf, size_t len);

/* kern.hakios.c2.enable: 1 lets an armed c2 run, 0 holds every core in WFI. */
extern uint32_t sweet_c2_enable;
#endif

#endif /* _ARM64_SWEET_C2_H_ */
