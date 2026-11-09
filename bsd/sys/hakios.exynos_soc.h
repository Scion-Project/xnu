/*
 * The numbers of the Exynos this kernel is built for: EXYNOS_* from socs/<soc>/include/hakios.<soc>_soc.h, chosen by CONFIG_SOC
 * (tools/hakios-config.py writes HAKIOS_SOC_HEADER into hakios.drivers.h; xnu/apply.sh lays the header into sys/).
 *
 * With SOC_RUNTIME the same names read the table hakios_soc_select() picked at boot (hakios.soc_runtime.h).
 *
 * Exynos drivers are also built into kernels for other SoCs, where they check the SoC's base at run time and do nothing;
 * those kernels take the 9810's numbers to compile.
 */
#ifndef _SYS_HAKIOS_EXYNOS_SOC_H_
#define _SYS_HAKIOS_EXYNOS_SOC_H_

#include "../dev/hakios.drivers.h"

#if HAKIOS_SOC_RUNTIME && !defined(HAKIOS_SOC_FIXED)
#include <sys/hakios.soc_runtime.h>
#elif HAKIOS_SOC_RUNTIME
/* a driver of the 9810's phones alone: its numbers stay compile time, and its check of the SoC's base fails on any other chip */
#include <sys/hakios.exynos9810_soc.h>
int hakios_soc_is_9810(void);
#undef EXYNOS_SOC_BASE_PA
#define EXYNOS_SOC_BASE_PA      (hakios_soc_is_9810() ? 0x10000000UL : ~0UL)
#elif defined(HAKIOS_SOC_HEADER)
#include HAKIOS_SOC_HEADER
#else
#include <sys/hakios.exynos9810_soc.h>
#endif

#ifndef HAKIOS_SOC_HAS
#define HAKIOS_SOC_HAS(name)    1
#endif

#endif /* _SYS_HAKIOS_EXYNOS_SOC_H_ */
