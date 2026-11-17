/*
 * SWEET C2: core power-down in idle for the Galaxy S9 (STARLTE), stage one of scion-kernel docs/ios27/CPU-IDLE.md.
 *
 * NOT RUN ON A PHONE. Built for STARLTE and FRANKEL; nothing here runs unless a boot-argument asks for it:
 *
 *   sweetc2=N        0 (default) off; 1 the Cortex-A55s 1-3; 2 cores 1-7 (the A55s and the four M3s). Core 0 never:
 *                    it is the core that wakes the others for their timers (below), and the boot cluster's.
 *   sweetc2cpu=N     only logical CPU N (one of those sweetc2 allows), for the first tests
 *   sweetc2max=N     at most N power-downs per core, then WFI again (0, the default: no limit)
 *   sweetc2dry=1     measure only: per core, how long each WFI was predicted (the next timer) and how long it lasted,
 *                    and how often the rule below would have powered the core down. Works with sweetc2=0.
 *   sweetc2delay=S   arm S seconds after every core has started (default 60)
 *   sweetc2log=S     print the counters every S seconds (default 30; 0: only kern.hakios.c2.stats)
 *   sweetc2flags=B   1: only idles with no timer at all (no wake-up from core 0 is ever needed); 2: none of the
 *                    Exynos PMU writes Samsung's kernel makes around c2 (diagnostic); 4: no history guard;
 *                    8: self-check only (arm, check every allowed core, print, never power one down)
 *
 * What a power-down is here (the facts and their sources are in CPU-IDLE.md): PSCI CPU_SUSPEND, function 0xC4000001,
 * power_state 0x10000 (the device tree's arm,psci-suspend-param for both clusters), entry point the physical address of
 * sweet_secondary_entry, the entry CPU_ON already uses. When the call does not return, the core comes back through
 * LowResetVectorBase, whose handler for an idle core is resume_idle_cpu (cpu_machine_idle_init), into
 * arm_init_idle_cpu() and sweet_c2_resume(), and Idle_load_context() returns to the idle thread that Idle_context()
 * saved before cpu_idle() ran. The architected timer of a powered-down core is assumed lost (the S9's device tree does
 * not mark it always-on, and Samsung's kernel uses the MCT for its per-core events): a core with a timer pending asks
 * core 0, by cross-call, to arm a timer_call that sends it an IPI an exit latency before that timer, and on its way
 * back it reprograms its own. Around the call it makes the writes Samsung's kernel makes for c2 (exynos-powermode.c
 * cpu_disable/cpu_enable -> pmucal_cpu_disable/enable): the core's CPU_INFORM hint and the interrupt-generator bits.
 *
 * The rule: the core is allowed (sweetc2, sweetc2cpu), the self-check passed, nothing holds c2 off, the next timer is
 * at least the cluster's min-residency away (2000 us A55, 3500 us M3) and the core's previous idle lasted as long.
 */
#include <arm64/sweet_c2.h>

#if SWEET_C2
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/timer_call.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/caches_internal.h>
#include <arm/rtclock.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>

#if defined(STARLTE)
#define C2_NAME "STARLTE C2"
#else
#define C2_NAME "SWEET C2"
#endif

#define C2_NCPU                 8
#define C2_PSCI_VERSION         0x84000000ULL
#define C2_PSCI_FEATURES        0x8400000AULL
#define C2_PSCI_CPU_SUSPEND     0xC4000001ULL   /* the device tree's /psci cpu_suspend */
#define C2_POWER_STATE          0x10000ULL      /* arm,psci-suspend-param of both idle states: StateType power-down */
#define C2_MARGIN_US            100             /* the IPI's own delivery, on top of the exit latency */
#define C2_MAX_FAILS            8               /* consecutive failed calls before a core gives up c2 */

#define C2_F_UNTIMED            1
#define C2_F_NOPMU              2
#define C2_F_NOHISTORY          4
#define C2_F_CHECKONLY          8

enum { C2_OUT = 0, C2_IN = 1 };
enum { C2_ENTER = 0, C2_R_OFF, C2_R_CPU, C2_R_CHECK, C2_R_BLOCK, C2_R_LIMIT, C2_R_SHORT, C2_R_HISTORY, C2_R_TIMED };

/* the latency table: the device tree's idle-states (exynos9810-starlte_eur_open_26.dts lines 4811-4833) */
struct c2_lat {
	const char *name;
	uint32_t entry_us, exit_us, residency_us;
	uint64_t exit_abs, residency_abs;
};
static struct c2_lat c2_lat[2] = {
	{ "A55", 70, 160, 2000, 0, 0 },         /* bootcl-cpu-sleep */
	{ "M3", 235, 220, 3500, 0, 0 },         /* nobootcl-cpu-sleep */
};

#define C2_BUCKETS 7
static const uint32_t c2_bucket_us[C2_BUCKETS - 1] = { 1000, 2000, 3500, 10000, 100000, UINT32_MAX };

struct sweet_c2_cpu {
	/* the core's own */
	uint64_t entries, resumes, returns, fails, unexpected, gic_lost, bc_busy;
	uint64_t r_short, r_history, r_block, r_limit, r_timed;
	uint64_t asleep_abs, enter_abs, idle_start_abs, last_idle_abs;
	int64_t last_err;
	uint32_t consecutive_fails;
	uint32_t state;                 /* C2_OUT, C2_IN: written by the core, read by core 0 */
	uint64_t gen;                   /* bumped at every entry and exit: a wake-up of an old entry does nothing */
	uint64_t bc_deadline;
	uint8_t checked, refused, disabled, timed;
	uint32_t refuse_why;
	uint32_t events;
	/* sweetc2dry */
	uint64_t idles, idle_abs, would, would_abs, would_short;
	uint64_t pred_hist[C2_BUCKETS], act_hist[C2_BUCKETS];
	/* core 0's, on this core's behalf */
	uint64_t bc_arms, bc_fires;
	timer_call_data_t bc_call;
} __attribute__((aligned(64)));

static struct sweet_c2_cpu c2_cpu[C2_NCPU];

uint32_t sweet_c2_on;                   /* the hook in cpu_idle() is live (a mode was asked for) */
uint32_t sweet_c2_enable = 1;           /* kern.hakios.c2.enable */
static uint32_t c2_armed;               /* the self-check passed: power-downs may happen */
static uint32_t c2_dry, c2_mode, c2_flags, c2_max, c2_mask, c2_delay = 60, c2_log = 30;
static int32_t c2_only = -1;
static uint32_t c2_block_count;
static uint64_t c2_margin_abs;
static uint64_t c2_entry_pa, c2_resume_pa;
static int64_t c2_psci_version;

/* PA-only symbols: declared as data so that no pointer authentication is applied when their address is taken (cpu.c) */
extern uintptr_t sweet_secondary_entry;
extern uintptr_t resume_idle_cpu;
extern void pe_init_fiq(void);

/* the first events of each core, printed by the log thread */
struct c2_event {
	uint8_t cpu;
	char kind;              /* E entered, R back by reset, P PSCI returned 0, F PSCI failed, U reset not ours, X refused, G GIC lost, C checked */
	int64_t value;
};
#define C2_RING 64
static struct c2_event c2_ring[C2_RING];
static uint32_t c2_ring_n, c2_ring_printed;

static void
c2_event(unsigned int cpu, char kind, int64_t value)
{
	if (c2_cpu[cpu].events >= 8) {
		return;
	}
	c2_cpu[cpu].events++;
	uint32_t i = os_atomic_inc_orig(&c2_ring_n, relaxed);
	if (i < C2_RING) {
		c2_ring[i].cpu = (uint8_t)cpu;
		c2_ring[i].kind = kind;
		c2_ring[i].value = value;
	}
}

static inline unsigned int
c2_cluster(unsigned int cpu)
{
	return cpu < 4 ? 0 : 1;
}

static inline uint64_t
c2_us(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_USEC;
}

static unsigned int
c2_bucket(uint64_t abs, bool none)
{
	if (none) {
		return C2_BUCKETS - 1;
	}
	uint64_t us = c2_us(abs);
	for (unsigned int b = 0; b < C2_BUCKETS - 2; b++) {
		if (us < c2_bucket_us[b]) {
			return b;
		}
	}
	return C2_BUCKETS - 2;
}

/* SMCCC 1.0 by the board's conduit (STARLTE: SMC to Samsung's EL3; FRANKEL: HVC to its boot shim). x0 is the signed
 * PSCI result; x1-x17 are not kept. */
static int64_t
c2_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile (SWEET_PSCI_CONDUIT : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                "x14", "x15", "x16", "x17", "cc", "memory");
	return (int64_t)(int32_t)x0;
}

#if defined(STARLTE)
/*
 * The Exynos 9810 PMU around c2. Samsung's c2 entry (exynos-powermode.c exynos_cpu_pm_enter -> cpu_disable ->
 * cal_cpu_disable -> pmucal_cpu_disable) writes the core's CPU_INFORM register with CPU_INFORM_C2 (1), then runs the
 * core's "off" sequence of flexpmu_cal_cpu_exynos9810.h (the !ACPM_FRAMEWORK half); its exit (exynos_cpu_pm_exit ->
 * pmucal_cpu_enable) writes CPU_INFORM 0 and runs the "on" sequence. For physical core p (A55 n: p = n, bit 4 + n;
 * M3 n: p = 4 + n, bit n), each a read-modify-write of one bit:
 *   off: INTR_GEN+0x200 (GRP2_INTR_BID_ENABLE) |= bit; if INTR_GEN+0x108 has bit: INTR_GEN+0x10c (GRP1_INTR_BID_CLEAR)
 *        |= bit; PMU+0x7f08 (PMU_EVENT_INTERRUPT_ENABLE_GRP2) |= bit
 *   on:  INTR_GEN+0x200 &= ~bit; if INTR_GEN+0x208 has bit: INTR_GEN+0x20c (GRP2_INTR_BID_CLEAR) |= bit;
 *        PMU+0x7f08 &= ~bit
 * Samsung holds a spin lock across these (pmucal_cpu_lock); so does this. What the bits do inside the PMU's
 * microcontroller is not documented anywhere this project has: they are reproduced, not understood.
 */
#define C2_PMU_PA               0x14060000u
#define C2_PMU_SIZE             0x8000u
#define C2_INTR_PA              0x14070000u
#define C2_INTR_SIZE            0x1000u
#define C2_PMU_CPU_INFORM(p)    (0x0860u + 4u * (p))
#define C2_PMU_STATUS(cpu)      ((cpu) < 4 ? 0x2024u + 8u * (cpu) : 0x2004u + 8u * ((cpu) - 4))
#define C2_PMU_EVT_GRP2_EN      0x7f08u
#define C2_INTR_GRP1_STAT       0x0108u
#define C2_INTR_GRP1_CLEAR      0x010cu
#define C2_INTR_GRP2_ENABLE     0x0200u
#define C2_INTR_GRP2_STAT       0x0208u
#define C2_INTR_GRP2_CLEAR      0x020cu

static vm_offset_t c2_pmu, c2_intr;
static uint32_t c2_pmu_lock;

static inline uint32_t
c2_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
c2_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline void
c2_set(vm_offset_t base, uint32_t off, uint32_t bit, bool on)
{
	uint32_t v = c2_rd(base, off);
	c2_wr(base, off, on ? (v | bit) : (v & ~bit));
}

static inline uint32_t
c2_bit(unsigned int cpu)
{
	return cpu < 4 ? 1u << (4 + cpu) : 1u << (cpu - 4);
}

static void
c2_pmu_take(void)
{
	while (!os_atomic_cmpxchg(&c2_pmu_lock, 0, 1, acquire)) {
		while (os_atomic_load(&c2_pmu_lock, relaxed) != 0) {
			__builtin_arm_yield();
		}
	}
}

static void
c2_pmu_give(void)
{
	os_atomic_store(&c2_pmu_lock, 0, release);
}

static void
c2_pmu_enter(unsigned int cpu)
{
	if (c2_flags & C2_F_NOPMU) {
		return;
	}
	uint32_t bit = c2_bit(cpu);
	c2_pmu_take();
	c2_wr(c2_pmu, C2_PMU_CPU_INFORM(cpu), 1);                     /* CPU_INFORM_C2 */
	c2_set(c2_intr, C2_INTR_GRP2_ENABLE, bit, true);
	if ((c2_rd(c2_intr, C2_INTR_GRP1_STAT) & bit) == bit) {
		c2_set(c2_intr, C2_INTR_GRP1_CLEAR, bit, true);
	}
	c2_set(c2_pmu, C2_PMU_EVT_GRP2_EN, bit, true);
	__builtin_arm_dsb(DSB_SY);
	c2_pmu_give();
}

static void
c2_pmu_exit(unsigned int cpu)
{
	if (c2_flags & C2_F_NOPMU) {
		return;
	}
	uint32_t bit = c2_bit(cpu);
	c2_pmu_take();
	c2_wr(c2_pmu, C2_PMU_CPU_INFORM(cpu), 0);
	c2_set(c2_intr, C2_INTR_GRP2_ENABLE, bit, false);
	if ((c2_rd(c2_intr, C2_INTR_GRP2_STAT) & bit) == bit) {
		c2_set(c2_intr, C2_INTR_GRP2_CLEAR, bit, true);
	}
	c2_set(c2_pmu, C2_PMU_EVT_GRP2_EN, bit, false);
	__builtin_arm_dsb(DSB_SY);
	c2_pmu_give();
}

extern uint32_t a5xelte_gic_cpu_lost(void);
#else
static inline void
c2_pmu_enter(__unused unsigned int cpu)
{
}

static inline void
c2_pmu_exit(__unused unsigned int cpu)
{
}
#endif /* STARLTE */

/* Per-core facts, checked once on the core itself (MIDR and MPIDR are the running core's). 0: all true. */
static uint32_t
c2_check_core(cpu_data_t *cdp, unsigned int cpu)
{
	uint32_t why = 0;
#if defined(STARLTE)
	uint64_t midr = __builtin_arm_rsr64("MIDR_EL1");
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	uint32_t impl = (uint32_t)(midr >> 24) & 0xff, part = (uint32_t)(midr >> 4) & 0xfff;
	if (cpu < 4) {
		/* Cortex-A55: Arm (0x41), part 0xD05; MPIDR Aff1 = core, MT set (SweetCPU.cpp, start.s) */
		if (impl != 0x41 || part != 0xd05 || (mpidr & 0x0100ffffULL) != (0x01000000ULL | ((uint64_t)cpu << 8)) ||
		    cdp->cpu_phys_id != (cpu << 8)) {
			why |= 1;
		}
	} else {
		/* Samsung M3: implementer 0x53, part 0x002 (MIDR 0x531f0020); MPIDR 0x100 + n, no MT; ADT reg 0x10100 + n */
		if (impl != 0x53 || part != 0x002 || (mpidr & 0x0100ffffULL) != (0x100ULL + cpu - 4) ||
		    cdp->cpu_phys_id != 0x10100u + cpu - 4) {
			why |= 1;
		}
	}
#endif
	if (cdp->cpu_reset_handler != c2_resume_pa) {
		why |= 2;
	}
#if defined(STARLTE)
	if ((c2_rd(c2_pmu, C2_PMU_STATUS(cpu)) & 0xf) != 0xf) {
		why |= 4;               /* the PMU does not show this core on: the register map is not what we think */
	}
	if (c2_rd(c2_pmu, C2_PMU_CPU_INFORM(cpu)) != 0) {
		why |= 8;               /* something else uses the core's hint */
	}
#endif
	return why;
}

/* No timer of any kind on this core: the only wake-up it needs is an IPI. (timer_resync_deadlines' three sources.) */
static bool
c2_untimed(cpu_data_t *cdp)
{
	rtclock_timer_t *t = &cdp->rtclock_timer;
	if (!t->has_expired && t->deadline != 0 && t->deadline != EndOfAllTime) {
		return false;
	}
	if (cdp->idle_timer_deadline != 0) {
		return false;
	}
	processor_t p = PERCPU_GET_RELATIVE(processor, cpu_data, cdp);
	return running_timers_deadline(p) == EndOfAllTime;
}

static int
c2_decide(cpu_data_t *cdp, struct sweet_c2_cpu *c, unsigned int cpu, uint64_t pred, bool untimed)
{
	if (!os_atomic_load(&c2_armed, acquire) || !os_atomic_load(&sweet_c2_enable, relaxed)) {
		return C2_R_OFF;
	}
	if (!(c2_mask & (1u << cpu)) || c->disabled) {
		return C2_R_CPU;
	}
	if (!c->checked) {
		c->refuse_why = c2_check_core(cdp, cpu);
		c->refused = c->refuse_why != 0;
		c->checked = 1;
		if (c->refused) {
			c2_event(cpu, 'X', c->refuse_why);
		}
	}
	if (c->refused) {
		return C2_R_CHECK;
	}
	if (c2_flags & C2_F_CHECKONLY) {
		if (c->events == 0) {
			c2_event(cpu, 'C', 0);  /* this core's self-check passed */
		}
		return C2_R_OFF;
	}
	if (os_atomic_load(&c2_block_count, relaxed) != 0) {
		c->r_block++;
		return C2_R_BLOCK;
	}
	if (c2_max != 0 && c->entries >= c2_max) {
		c->r_limit++;
		return C2_R_LIMIT;
	}
	if ((c2_flags & C2_F_UNTIMED) && !untimed) {
		c->r_timed++;
		return C2_R_TIMED;
	}
	const struct c2_lat *l = &c2_lat[c2_cluster(cpu)];
	if (pred < l->residency_abs) {
		c->r_short++;
		return C2_R_SHORT;
	}
	if (!(c2_flags & C2_F_NOHISTORY) && c->last_idle_abs < l->residency_abs) {
		c->r_history++;
		return C2_R_HISTORY;
	}
	return C2_ENTER;
}

/* core 0, cross-call from a core going down with a timer pending */
static void
c2_bc_arm(void *arg)
{
	unsigned int cpu = (unsigned int)((uintptr_t)arg - 1);
	struct sweet_c2_cpu *c = &c2_cpu[cpu];
	uint64_t gen = os_atomic_load(&c->gen, acquire);
	if (os_atomic_load(&c->state, acquire) == C2_IN) {
		c->bc_arms++;
		timer_call_enter1(&c->bc_call, (timer_call_param_t)gen, c->bc_deadline,
		    TIMER_CALL_SYS_CRITICAL | TIMER_CALL_LOCAL);
	}
}

/* core 0, at that core's timer less its exit latency: wake it with an IPI */
static void
c2_bc_fire(timer_call_param_t p0, timer_call_param_t p1)
{
	unsigned int cpu = (unsigned int)(uintptr_t)p0;
	struct sweet_c2_cpu *c = &c2_cpu[cpu];
	if (os_atomic_load(&c->state, acquire) == C2_IN && os_atomic_load(&c->gen, acquire) == (uint64_t)p1) {
		c->bc_fires++;
		cpu_signal(cpu_datap((int)cpu), SIGPnop, NULL, NULL);
	}
}

/* Returns true when the core slept (PSCI returned); false when nothing was done and the caller should WFI. */
static bool
c2_enter(cpu_data_t *cdp, struct sweet_c2_cpu *c, unsigned int cpu, uint64_t now, bool untimed)
{
	const struct c2_lat *l = &c2_lat[c2_cluster(cpu)];
	if (!untimed) {
		uint64_t lead = l->exit_abs + c2_margin_abs;
		uint64_t pop = cdp->rtcPop;
		c->bc_deadline = pop > now + lead ? pop - lead : now;
	}
	c->timed = !untimed;
	c->enter_abs = now;
	os_atomic_inc(&c->gen, release);
	os_atomic_store(&c->state, C2_IN, release);
	if (!untimed) {
		cpu_data_t *keeper = cpu_datap(0);
		if ((os_atomic_load(&keeper->cpu_signal, relaxed) & (SIGPxcall | SIGPdisabled)) != 0 ||
		    cpu_signal(keeper, SIGPxcall, (void *)c2_bc_arm, (void *)(uintptr_t)(cpu + 1)) != KERN_SUCCESS) {
			os_atomic_store(&c->state, C2_OUT, release);
			os_atomic_inc(&c->gen, release);
			c->bc_busy++;
			return false;
		}
	}
	c2_pmu_enter(cpu);
	/* The reset path reads this core's cpu_data with the MMU and caches off (reset_vector: cpu_phys_id,
	 * cpu_reset_handler; start_cpu: the stack tops): it must be in memory, not only in a cache. */
	CleanPoC_DcacheRegion_Force((vm_offset_t)cdp, sizeof(*cdp));
	c->entries++;
	if (c->entries <= 4) {
		c2_event(cpu, 'E', (int64_t)(untimed ? -1 : (int64_t)c2_us(cdp->rtcPop - now)));
	}
	__builtin_arm_dsb(DSB_SY);

	int64_t rc = c2_psci(C2_PSCI_CPU_SUSPEND, C2_POWER_STATE, c2_entry_pa, cpu);

	/* Returned: the core did not lose power (a wake-up was already pending), or the firmware refused. */
	uint64_t t = mach_absolute_time();
	c2_pmu_exit(cpu);
	os_atomic_store(&c->state, C2_OUT, release);
	os_atomic_inc(&c->gen, release);
	if (!untimed) {
		timer_call_cancel(&c->bc_call);
	}
	c->asleep_abs += t - now;
	if (rc == 0) {
		c->returns++;
		c->consecutive_fails = 0;
		c2_event(cpu, 'P', (int64_t)c2_us(t - now));
	} else {
		c->fails++;
		c->last_err = rc;
		c2_event(cpu, 'F', rc);
		if (++c->consecutive_fails >= C2_MAX_FAILS) {
			c->disabled = 1;
		}
	}
	return true;
}

void
sweet_c2_idle(cpu_data_t *cdp)
{
	unsigned int cpu = cdp->cpu_number;
	if (cpu >= C2_NCPU) {
		__builtin_arm_wfi();
		return;
	}
	struct sweet_c2_cpu *c = &c2_cpu[cpu];
	uint64_t now = mach_absolute_time();
	bool untimed = c2_untimed(cdp);
	uint64_t pop = cdp->rtcPop;
	uint64_t pred = untimed ? UINT64_MAX : (pop > now ? pop - now : 0);
	const struct c2_lat *l = &c2_lat[c2_cluster(cpu)];
	bool would = cpu != 0 && pred >= l->residency_abs && c->last_idle_abs >= l->residency_abs;

	c->idle_start_abs = now;
	if (c2_dry) {
		c->idles++;
		c->pred_hist[c2_bucket(pred, untimed)]++;
		if (would) {
			c->would++;
		}
	}

	bool slept = false;
	if (c2_decide(cdp, c, cpu, pred, untimed) == C2_ENTER) {
		slept = c2_enter(cdp, c, cpu, now, untimed);     /* does not come back here if the core lost power */
	}
	if (!slept) {
		__builtin_arm_wfi();
	}

	uint64_t d = mach_absolute_time() - now;
	c->last_idle_abs = d;
	if (c2_dry) {
		c->idle_abs += d;
		c->act_hist[c2_bucket(d, false)]++;
		if (would) {
			c->would_abs += d;
			if (d < l->residency_abs) {
				c->would_short++;
			}
		}
	}
}

void
sweet_c2_resume(cpu_data_t *cdp)
{
	unsigned int cpu = cdp->cpu_number;
	uint64_t now = mach_absolute_time();
	if (cpu >= C2_NCPU) {
		return;
	}
	struct sweet_c2_cpu *c = &c2_cpu[cpu];
	bool ours = os_atomic_load(&c->state, acquire) == C2_IN;

	if (ours) {
		c2_pmu_exit(cpu);
		os_atomic_store(&c->state, C2_OUT, release);
		os_atomic_inc(&c->gen, release);
		if (c->timed) {
			timer_call_cancel(&c->bc_call);
		}
		c->resumes++;
		c->consecutive_fails = 0;
		c->asleep_abs += now - c->enter_abs;
		c->last_idle_abs = now - c->idle_start_abs;
		if (c2_dry) {
			const struct c2_lat *l = &c2_lat[c2_cluster(cpu)];
			c->idle_abs += c->last_idle_abs;
			c->act_hist[c2_bucket(c->last_idle_abs, false)]++;
			/* an entry was a "would" by construction unless sweetc2flags lifted the history guard */
			if (c->last_idle_abs < l->residency_abs) {
				c->would_short++;
			}
			c->would_abs += c->last_idle_abs;
		}
		c2_event(cpu, 'R', (int64_t)c2_us(now - c->enter_abs));
	} else {
		c->unexpected++;
		c2_event(cpu, 'U', 0);
	}

	/* The GIC: on STARLTE the GIC-400's CPU interface is outside the core, so its state should have survived;
	 * count the resumes where it did not, then set it up as a cold start does (pe_init_fiq is per-core and
	 * idempotent). On FRANKEL (GICv3) the CPU interface is the core's system registers and is always lost. */
#if defined(STARLTE)
	uint32_t lost = a5xelte_gic_cpu_lost();
	if (lost != 0) {
		c->gic_lost++;
		c2_event(cpu, 'G', lost);
	}
#endif
	pe_init_fiq();

	/* The timer: fiq_context_init() has enabled CNTV again with an unknown compare value; program the pending
	 * deadline (rtcPop) as cpu_idle() does after its WFI. A deadline already past fires at once. */
	ClearIdlePop(FALSE);
}

void
sweet_c2_block(int on)
{
	if (on) {
		os_atomic_inc(&c2_block_count, relaxed);
	} else {
		os_atomic_dec(&c2_block_count, relaxed);
	}
}

size_t
sweet_c2_format(char *buf, size_t len)
{
	size_t n = 0;
	n += (size_t)snprintf(buf + n, len - n, "mode %u mask 0x%x flags %u max %u armed %u enable %u block %u dry %u psci %lld\n",
	    c2_mode, c2_mask, c2_flags, c2_max, c2_armed, sweet_c2_enable, c2_block_count, c2_dry, c2_psci_version);
	for (unsigned int cpu = 0; cpu < C2_NCPU && n < len; cpu++) {
		struct sweet_c2_cpu *c = &c2_cpu[cpu];
		if (c->entries + c->unexpected + c->idles + (uint64_t)c->refused == 0) {
			continue;
		}
		n += (size_t)snprintf(buf + n, len - n,
		    "cpu%u %s: in %llu back-by-reset %llu psci-returned %llu failed %llu (last %lld)%s, asleep %llu ms, "
		    "core0 armed %llu fired %llu busy %llu, gic-lost %llu, unexpected %llu, refused%s%s short %llu history %llu "
		    "timed %llu blocked %llu limit %llu\n",
		    cpu, c2_lat[c2_cluster(cpu)].name, c->entries, c->resumes, c->returns, c->fails, c->last_err,
		    c->disabled ? " DISABLED" : "", c2_us(c->asleep_abs) / 1000, c->bc_arms, c->bc_fires, c->bc_busy,
		    c->gic_lost, c->unexpected, c->refused ? " BY SELF-CHECK" : "", c->refused ? (c->refuse_why & 1 ? " (ids)" :
		    c->refuse_why & 2 ? " (reset handler)" : c->refuse_why & 4 ? " (pmu status)" : " (cpu inform)") : "",
		    c->r_short, c->r_history, c->r_timed, c->r_block, c->r_limit);
		if (c2_dry && n < len) {
			const uint64_t *p = c->pred_hist, *a = c->act_hist;
			n += (size_t)snprintf(buf + n, len - n,
			    "cpu%u dry: idle %llu x %llu ms; next timer <1/1-2/2-3.5/3.5-10/10-100/100+ ms/none: %llu %llu %llu %llu "
			    "%llu %llu %llu; lasted: %llu %llu %llu %llu %llu %llu; would %llu (%llu ms), of which woke early %llu\n",
			    cpu, c->idles, c2_us(c->idle_abs) / 1000, p[0], p[1], p[2], p[3], p[4], p[5], p[6],
			    a[0], a[1], a[2], a[3], a[4], a[5], c->would, c2_us(c->would_abs) / 1000, c->would_short);
		}
	}
	return n < len ? n : len;
}

/* The global facts stage one depends on. 0: all true; otherwise a reason, printed. */
static const char *
c2_check_system(void)
{
	const ml_topology_info_t *topo = ml_get_topology_info();
	if (topo->boot_cpu == NULL || topo->boot_cpu->cpu_id != 0 || topo->boot_cpu->phys_id != 0) {
		return "the boot core is not logical 0 / MPIDR 0 (core 0 is the one that wakes the others)";
	}
	if (topo->num_cpus < 2 || topo->num_cpus > C2_NCPU) {
		return "fewer than two cores, or more than eight";
	}
	for (unsigned int i = 0; i < topo->num_cpus; i++) {
		cpu_data_t *cdp = cpu_datap((int)i);
		if (cdp == NULL || (os_atomic_load(&cdp->cpu_signal, relaxed) & SIGPdisabled)) {
			return "a core is not running (cpus= did not start it, or it is not up yet)";
		}
	}
	c2_entry_pa = ml_static_vtop((vm_offset_t)&sweet_secondary_entry);
	c2_resume_pa = ml_static_vtop((vm_offset_t)&resume_idle_cpu);
	if (c2_entry_pa == 0 || c2_resume_pa == 0) {
		return "no physical address for sweet_secondary_entry or resume_idle_cpu";
	}
	if ((__builtin_arm_rsr64("CurrentEL") >> 2) != 1) {
		return "not at EL1";
	}
	/* PSCI 0.1 (the S9's device tree: function numbers from /psci, no PSCI_VERSION) answers NOT_SUPPORTED (-1);
	 * 0.2 and later answer their version. From 1.0, CPU_SUSPEND must be there with the original power_state format,
	 * in which 0x10000 is StateType = power-down. */
	c2_psci_version = c2_psci(C2_PSCI_VERSION, 0, 0, 0);
	if (c2_psci_version >= 0x10000) {
		int64_t f = c2_psci(C2_PSCI_FEATURES, C2_PSCI_CPU_SUSPEND, 0, 0);
		if (f < 0) {
			return "PSCI_FEATURES says CPU_SUSPEND is not supported";
		}
		if (f & 2) {
			return "PSCI uses the extended power_state format: 0x10000 would not be a power-down";
		}
	} else if (c2_psci_version != -1 && c2_psci_version < 2) {
		return "PSCI_VERSION answered something that is neither a version nor NOT_SUPPORTED";
	}
#if defined(FRANKEL)
	if (c2_psci_version < 2) {
		return "FRANKEL's boot shim answers PSCI_VERSION; it did not";
	}
#endif
#if defined(STARLTE)
	c2_pmu = ml_io_map(C2_PMU_PA, C2_PMU_SIZE);
	c2_intr = ml_io_map(C2_INTR_PA, C2_INTR_SIZE);
	if (c2_pmu == 0 || c2_intr == 0) {
		return "cannot map the PMU";
	}
	for (unsigned int i = 0; i < topo->num_cpus; i++) {
		if ((c2_rd(c2_pmu, C2_PMU_STATUS(i)) & 0xf) != 0xf) {
			return "the PMU does not show every running core on (CLUSTERn_CPUm_STATUS)";
		}
	}
#endif
	return NULL;
}

static void
c2_print(void)
{
	static char buf[2048];
	size_t n = sweet_c2_format(buf, sizeof(buf));
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
	char *line = buf;
	while (*line) {
		char *e = line;
		while (*e && *e != '\n') {
			e++;
		}
		char was = *e;
		*e = 0;
		printf(C2_NAME ": %s\n", line);
		if (!was) {
			break;
		}
		line = e + 1;
	}
	uint32_t total = os_atomic_load(&c2_ring_n, relaxed);
	if (total > C2_RING) {
		total = C2_RING;
	}
	for (; c2_ring_printed < total; c2_ring_printed++) {
		struct c2_event *ev = &c2_ring[c2_ring_printed];
		printf(C2_NAME ": event cpu%u %c %lld\n", ev->cpu, ev->kind, ev->value);
	}
}

static void
c2_sleep(uint32_t secs)
{
	assert_wait_timeout((event_t)&c2_delay, THREAD_UNINT, secs, NSEC_PER_SEC);
	thread_block(THREAD_CONTINUE_NULL);
}

__attribute__((noreturn)) static void
c2_thread(__unused void *arg, __unused wait_result_t wr)
{
	if (c2_mode != 0) {
		c2_sleep(c2_delay);
		const char *bad = c2_check_system();
		if (bad != NULL) {
			printf(C2_NAME ": NOT armed: %s\n", bad);
		} else {
			for (unsigned int i = 0; i < C2_NCPU; i++) {
				timer_call_setup(&c2_cpu[i].bc_call, c2_bc_fire, (timer_call_param_t)(uintptr_t)i);
			}
			/* every core's cpu_data, once, to memory (its reset handler was set to resume_idle_cpu after the
			 * cold start had cleaned it); each entry cleans its own again */
			for (unsigned int i = 0; i < ml_get_topology_info()->num_cpus; i++) {
				CleanPoC_DcacheRegion_Force((vm_offset_t)cpu_datap((int)i), sizeof(cpu_data_t));
			}
			__builtin_arm_dsb(DSB_SY);
			os_atomic_store(&c2_armed, 1, release);
			printf(C2_NAME ": armed: sweetc2=%u mask 0x%x flags %u max %u; PSCI_VERSION %lld; CPU_SUSPEND 0x%llx "
			    "power_state 0x%llx entry 0x%llx (resume_idle_cpu 0x%llx); A55 %u/%u/%u us, M3 %u/%u/%u us "
			    "(entry/exit/residency). NOT RUN ON A PHONE BEFORE THIS BUILD.\n",
			    c2_mode, c2_mask, c2_flags, c2_max, c2_psci_version, C2_PSCI_CPU_SUSPEND, C2_POWER_STATE,
			    c2_entry_pa, c2_resume_pa, c2_lat[0].entry_us, c2_lat[0].exit_us, c2_lat[0].residency_us,
			    c2_lat[1].entry_us, c2_lat[1].exit_us, c2_lat[1].residency_us);
		}
	}
	if (c2_log == 0) {
		thread_terminate(current_thread());
	}
	for (;;) {
		c2_sleep(c2_log);
		c2_print();
	}
}

void
sweet_c2_start(void)
{
	uint32_t v;
	if (PE_parse_boot_argn("sweetc2", &v, sizeof(v))) {
		c2_mode = v;
	}
	if (PE_parse_boot_argn("sweetc2dry", &v, sizeof(v))) {
		c2_dry = v != 0;
	}
	if (c2_mode == 0 && !c2_dry) {
		return;                 /* the default: nothing below runs, cpu_idle() is unchanged */
	}
	int32_t only;
	if (PE_parse_boot_argn("sweetc2cpu", &only, sizeof(only))) {
		c2_only = only;
	}
	if (PE_parse_boot_argn("sweetc2max", &v, sizeof(v))) {
		c2_max = v;
	}
	if (PE_parse_boot_argn("sweetc2delay", &v, sizeof(v))) {
		c2_delay = v;
	}
	if (PE_parse_boot_argn("sweetc2log", &v, sizeof(v))) {
		c2_log = v;
	}
	if (PE_parse_boot_argn("sweetc2flags", &v, sizeof(v))) {
		c2_flags = v;
	}
	c2_mask = c2_mode == 1 ? 0x0eu : c2_mode == 2 ? 0xfeu : 0;
	if (c2_only >= 0) {
		c2_mask &= (c2_only < C2_NCPU) ? (1u << c2_only) : 0;
	}
	for (unsigned int i = 0; i < 2; i++) {
		nanoseconds_to_absolutetime((uint64_t)c2_lat[i].exit_us * NSEC_PER_USEC, &c2_lat[i].exit_abs);
		nanoseconds_to_absolutetime((uint64_t)c2_lat[i].residency_us * NSEC_PER_USEC, &c2_lat[i].residency_abs);
	}
	nanoseconds_to_absolutetime((uint64_t)C2_MARGIN_US * NSEC_PER_USEC, &c2_margin_abs);
	printf(C2_NAME ": sweetc2=%u (mask 0x%x) sweetc2dry=%u flags %u max %u; %s in %u s. NOT RUN ON A PHONE BEFORE THIS BUILD.\n",
	    c2_mode, c2_mask, c2_dry, c2_flags, c2_max, c2_mode ? "self-check and arming" : "measuring only", c2_delay);
	os_atomic_store(&sweet_c2_on, 1, release);

	thread_t thread;
	if (kernel_thread_start(c2_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* SWEET_C2 */
