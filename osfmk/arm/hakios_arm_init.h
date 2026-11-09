/*
 * What osfmk/arm/arm_init.c includes before arm_init(): the checkpoints of the bring-up (osfmk/arm64/loader_return.c), PAN on or off for the
 * whole system (the Galaxy S9's M3s have none), and the trap of EL0's cache maintenance by address (the M3s' instruction fetch).
 */
/* Bring-up checkpoints, see osfmk/arm64/loader_return.c */
extern void loader_checkpoint(uint64_t n);
extern void loader_early_log_init(void);

#if defined(__arm64__)
#include <arm64/psci_smp.h>
#endif

/*
 * HakiOS (patch 1509): PAN (ARMv8.1) on or off for the whole system. The Galaxy S9's Exynos M3s implement ARMv8.0: they
 * have no PAN, and every MSR/MRS of PSTATE.PAN is UNDEFINED on them -- arm_init_cpu()'s first instruction, and every
 * copyin/copyout and context switch after it. Off when the boot processor has no PAN, or with boot-arg pan=0. Off
 * means: SCTLR_EL1.SPAN set on every core that has PAN (so that taking an exception does not set PSTATE.PAN), PAN
 * cleared, and no PAN instruction executed anywhere (copyio.c, pcb.c, pmap.c, platform_tests.c). Decided once, on the
 * boot processor, before any other core starts: a thread that moves between an A55 and an M3 sees the same rule.
 */
uint32_t hakios_pan_off;

static bool
hakios_cpu_has_pan(void)
{
	return ((__builtin_arm_rsr64("S3_0_C0_C7_1") >> 20) & 0xf) != 0;      /* ID_AA64MMFR1_EL1.PAN */
}

static void
hakios_pan_apply(void)
{
	if (!hakios_pan_off) {
		__builtin_arm_wsr("pan", 1);
		return;
	}
	if (hakios_cpu_has_pan()) {
		__builtin_arm_wsr64("SCTLR_EL1", __builtin_arm_rsr64("SCTLR_EL1") | (1ULL << 23));    /* SPAN */
		__builtin_arm_isb(15);          /* ISB SY */
		__builtin_arm_wsr("pan", 0);
	}
}
/*
 * HakiOS (Galaxy S9, patch 1511): code that user space writes and then runs (JavaScriptCore's JIT, WebCore's CSS
 * selector compiler, any other JIT) made visible to the Exynos M3s' instruction fetch.
 *
 * iOS makes new code executable with sys_icache_invalidate() (libsystem_platform), which on 16.5 is DSB ISH, IC IVAU
 * for every 64-byte line, DSB ISH and ISB -- no DC CVAU -- and sys_dcache_flush() is a DSB alone. Apple's cores, like
 * the S9's Cortex-A55s (CLIDR_EL1 0x82000023: LoUU 0), unify instructions and data at their L1, so nothing has to be
 * cleaned for instruction fetch to see a store. The M3s do not (CLIDR_EL1 0x0b200123: LoUU 1): a line of new code
 * still dirty in an M3's L1 data cache is not what that M3 fetches, and it runs the stale bytes under it. On the
 * phone (3 November, cpus=6) every WebContent process died this way at its first style resolution: SIGSEGV, pc 0,
 * lr in the code the CSS selector compiler had just written (WebCore::Document::resolveStyle), hundreds in a row, and
 * Safari loaded no page. With the two A55s alone (cpus=2) it did not happen; CTR_EL0 is the same on both (0x84448004).
 *
 * So with an M3 running, SCTLR_EL1.UCI is cleared on every core: EL0's cache maintenance by VA (IC IVAU, DC CVAU,
 * DC CVAC, DC CIVAC, DC CVAP, DC CVADP) traps (ESR EC 0x18), and hakios_user_cache_op() in sleh.c does it for the
 * process with the clean the M3 needs: DC CVAC of the line (to the point of coherency, broadcast, which the clusters
 * honour: cohtest's steps 4 and 5), then the IC IVAU, through the kernel's alias of the page. (Linux does the same for
 * the Cortex-A53's errata 819472 and others: ARM64_WORKAROUND_CLEAN_CACHE.) A JIT pays one trap for each 64-byte line
 * it makes executable. Every core, not only the M3s: a thread may write code on an M3 and make it executable on an A55.
 *
 * Boot-arg ucache_trap: 0 never, 1 always; unset, when m3_prep is 3 or more (the M3s are started, so every core traps
 * from the first) or when a core itself needs the clean (hakios_core_needs_pou_clean(); then from that core on).
 * Applied on every core as it starts and as it wakes (start.s writes SCTLR_EL1_DEFAULT, UCI set, each time).
 */
uint32_t hakios_ucache_trap;
static int hakios_ucache_mode = -1;     /* 0 never, 1 always, 2 decided by m3_prep and the core's own registers */

/* Must this core's data cache be cleaned for its instruction fetch to see a store? Not with CTR_EL0.IDC, nor when
 * CLIDR_EL1's LoUU and LoUIS are 0 (as Linux reads them): the A55s, QEMU's Cortex-A76, Apple's cores. The M3s: yes. */
static bool
hakios_core_needs_pou_clean(void)
{
	uint64_t ctr = __builtin_arm_rsr64("CTR_EL0"), clidr = __builtin_arm_rsr64("CLIDR_EL1");

	if ((ctr >> 28) & 0x1) {
		return false;
	}
	return ((clidr >> 27) & 0x7) != 0 || ((clidr >> 21) & 0x7) != 0;
}

static void
hakios_ucache_apply(void)
{
	if (hakios_ucache_mode < 0) {
		uint32_t on = 0, m3 = 0;

		if (PE_parse_boot_argn("ucache_trap", &on, sizeof(on))) {
			hakios_ucache_mode = on ? 1 : 0;
		} else {
			hakios_ucache_mode = 2;
			if (PE_parse_boot_argn("m3_prep", &m3, sizeof(m3)) && m3 >= 3) {
				hakios_ucache_trap = 1;
			}
		}
		if (hakios_ucache_mode == 1) {
			hakios_ucache_trap = 1;
		}
	}
	if (hakios_ucache_mode == 2 && hakios_core_needs_pou_clean()) {
		hakios_ucache_trap = 1;
	}
	if (hakios_ucache_trap) {
		__builtin_arm_wsr64("SCTLR_EL1", __builtin_arm_rsr64("SCTLR_EL1") & ~(1ULL << 26));   /* UCI */
		__builtin_arm_isb(15);          /* ISB SY */
	}
}
extern void loader_log_cpu_facts(void);
