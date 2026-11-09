/*
 * The Exynos 9810's always-on power processor (APM) and its ACPM firmware, asked from the kernel: drivers/core/
 * hakios.exynos_acpm.c. What the PMIC's registers and the clocks' DVFS rates go through on this SoC. Polled; one
 * request at a time (a lock inside); thread context only.
 */
#ifndef _SYS_HAKIOS_EXYNOS_ACPM_H_
#define _SYS_HAKIOS_EXYNOS_ACPM_H_

#include <stdbool.h>
#include <stdint.h>

#define HAKIOS_ACPM_CH_DVFS     0       /* set and get a domain's rate (where Samsung's kernel sends them; see the .c) */
#define HAKIOS_ACPM_CH_PMIC     2       /* the S2MPS18's registers, over SPEEDY */
#define HAKIOS_ACPM_CH_ACPM     4       /* the firmware itself (the S9's acpm node's acpm-ipc-channel): its STOP */

#define HAKIOS_ACPM_PMIC_TOP    0       /* the S2MPS18's blocks ("type") */
#define HAKIOS_ACPM_PMIC_PMIC   1
#define HAKIOS_ACPM_PMIC_RTC    2

/* Find the firmware's channel table and say what it is (`say`: a driver's log); false: no ACPM here. Once is enough. */
bool hakios_acpm_init(int (*say)(const char *, ...));
/* One request on a channel: four words out, the reply's four back in their place. 0, or an errno. */
int hakios_acpm_request(uint32_t channel, uint32_t cmd[4]);
/*
 * The PMIC: read a register, write one, or update the bits of `mask` to `value`. 0, or an errno (EIO: SPEEDY failed).
 * A caller writes only registers it names in an allow-list of its own, and reads each write back.
 */
int hakios_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *value);
int hakios_acpm_pmic_write(uint32_t type, uint32_t reg, uint8_t value);
int hakios_acpm_pmic_update(uint32_t type, uint32_t reg, uint8_t value, uint8_t mask);
/* A DVFS domain's rate (its index: the low bits of the CAL id, 4 for G3D, 9 for AUD), in kHz. */
int hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz);
int hakios_acpm_dvfs_set(uint32_t index, uint32_t khz);
/*
 * A DVFS domain's levels as the APM holds them (its "fvmap"), read only: rates in kHz and the voltages the APM applies
 * with them, the fastest first, at most `most`. How many were read; 0 when there is no such table (said with `say`).
 */
uint32_t hakios_acpm_dvfs_table(uint32_t index, uint32_t *khz, uint32_t *uv, uint32_t most, int (*say)(const char *, ...));
/*
 * The Galaxy S9's CPU frequency governor (drivers/power/exynos/hakios.exynos_cpufreq.c): started with cpufreq_gov=1, in place of
 * the fixed cpufreq_lit= and cpufreq_big= rates. True when it runs (it then owns both clusters' rates).
 */
bool hakios_exynos_cpufreq_gov_start(void);
/*
 * The governor's boosts: a finger event of the touch screen (hakios.s6sy761.c) and a frame given to the display
 * (MiniIOMobileFramebuffer.cpp). Any context that may call thread_wakeup: a stamp, a count, the governor's thread woken;
 * nothing waited for. Nothing when the governor does not run (and stubs without CONFIG_HAKIOS_EXYNOS_CPUFREQ).
 */
void hakios_exynos_cpufreq_touch(void);
/* how many fingers are down now, after a change: the touch screen's thread, as above */
void hakios_exynos_cpufreq_fingers(uint32_t down);
void hakios_exynos_cpufreq_frame(void);
/*
 * Ask the APM to stop before the phone is powered off (Samsung's acpm_enter_wfi(), from exynos_acpm_reboot()): the firmware
 * goes to WFI and no longer answers. Only on the way to PS_HOLD (drivers/power/exynos/hakios.exynos_poweroff.c). 0, or an errno.
 */
int hakios_acpm_stop(void);

#endif /* _SYS_HAKIOS_EXYNOS_ACPM_H_ */
