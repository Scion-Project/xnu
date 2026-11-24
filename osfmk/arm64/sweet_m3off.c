/*
 * SWEET M3OFF: an M3 of the Galaxy S9 (STARLTE) taken offline and started again (scion-kernel docs/ios27/BIG-CORES.md).
 *
 * Why. Samsung's kernel lets the M3s past 1794 MHz only with at most two of them online (the hotplug governor's
 * single_freq/dual_freq 2314000, triple_freq/quad_freq 1794000) and a core counts as online until it has been hot-unplugged:
 * taken out of the scheduler and powered off by PSCI CPU_OFF. An M3 parked in WFI or powered down in idle (sweet_c2.c) still
 * counts. This file is the hot-unplug: XNU's own processor offline path (processor_exit_from_user(): the threads moved off,
 * its timers to another core, its IPIs drained), then on the core itself what Samsung's hotplug-out does
 * (exynos-powermode.c exynos_hotplug_out_callback -> cpu_disable -> pmucal_cpu_disable: CPU_INFORM := C2, the core's "off"
 * sequence of flexpmu_cal_cpu_exynos9810.h) and PSCI CPU_OFF with the power-down state (arch/arm64/kernel/psci.c
 * cpu_psci_cpu_die: 0x10000; the S9's PSCI 0.1 numbers CPU_OFF 0x84000002 in its device tree). Then, as Samsung's CPU_DEAD
 * notifier, the PMU's CLUSTER1_CPUn_STATUS polled until it reads the core off, for up to 2 s. Started again by XNU's
 * processor start (cpu_start -> SweetCPU::startCPU: the M3 PMU defaults and CPU_ON, as at boot); on the core the "on"
 * sequence (exynos_hotplug_in_callback -> cpu_enable -> pmucal_cpu_enable: CPU_INFORM := 0) before it runs threads.
 * Samsung's kernel read for these facts (GPL), no line of it here. Core 4, the first M3, is never taken (Samsung's
 * fast_hp_cpus is "5-7").
 *
 * Failures end the boot visibly: a core the PMU still reads on 2 s after XNU had it offline, or one that does not come
 * back (XNU's own wait, cpu_boot_timeout_secs, panics), is a panic, and with sweetreboot (on by default) a panic writes
 * its log to the pmsg ring and resets into recovery.
 *
 * Off unless m3off= (1: the governor decides and logs only, nothing here runs; 2: this file runs). processor_exit=1 is
 * needed too (XNU's own switch for processor_exit_from_user). The governor (bsd/dev/hakios.exynos_cpufreq.c) is the only
 * caller of take/give.
 */
#include <arm64/sweet_m3off.h>

#if SWEET_M3OFF
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/caches_internal.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>

#define M3_PMU_PA               0x14060000u
#define M3_PMU_SIZE             0x8000u
#define M3_INTR_PA              0x14070000u
#define M3_INTR_SIZE            0x1000u
#define M3_CPU_INFORM(cpu)      (0x0860u + 4u * (cpu))          /* physical core p: A55 n = n, M3 n = 4 + n = the logical number */
#define M3_STATUS(cpu)          (0x2004u + 8u * ((cpu) - 4))    /* CLUSTER1_CPUn_STATUS [3:0]: 0xF on, 0 off */
#define M3_PMU_EVT_GRP2_EN      0x7f08u
#define M3_INTR_GRP1_STAT       0x0108u
#define M3_INTR_GRP1_CLEAR      0x010cu
#define M3_INTR_GRP2_ENABLE     0x0200u
#define M3_INTR_GRP2_STAT       0x0208u
#define M3_INTR_GRP2_CLEAR      0x020cu
#define M3_FIRST                4u
#define M3_LAST                 7u
#define PSCI_CPU_OFF_01         0x84000002ull                   /* the S9's device tree, psci { cpu_off } */
#define PSCI_POWER_DOWN         0x10000ull                      /* power_state: StateType power-down */
#define OFF_WAIT_MS             2000                            /* Samsung's CPU_DEAD wait */
#define EVENTS_LOGGED           40

uint32_t sweet_m3off_mode;
static vm_offset_t m3_pmu, m3_intr;
static uint32_t m3_lock;
static uint32_t m3_off_mask;            /* bit cpu: taken offline by this file and read off by the PMU, not started again */
static uint32_t m3_going_mask;          /* bit cpu: between the request and its end (either way) */
static int32_t m3_dying_rc[MAX_CPUS];   /* CPU_OFF's answer on a core it came back from (it should never) */
static uint32_t m3_events;
static struct {
	uint32_t takes, gives, fails;
	uint64_t take_us_last, take_us_max, give_us_last, give_us_max;
} m3s[MAX_CPUS];

static inline uint32_t
m3_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
m3_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline void
m3_set(vm_offset_t base, uint32_t off, uint32_t bit, bool on)
{
	uint32_t v = m3_rd(base, off);
	m3_wr(base, off, on ? (v | bit) : (v & ~bit));
}

static void
m3_take_lock(void)
{
	while (!os_atomic_cmpxchg(&m3_lock, 0, 1, acquire)) {
		while (os_atomic_load(&m3_lock, relaxed) != 0) {
			__builtin_arm_yield();
		}
	}
}

static void
m3_give_lock(void)
{
	os_atomic_store(&m3_lock, 0, release);
}

static bool
m3_is_m3(unsigned int cpu)
{
	const ml_topology_info_t *topo = ml_get_topology_info();

	return cpu >= M3_FIRST && cpu <= M3_LAST && cpu < topo->num_cpus && (topo->cpus[cpu].phys_id & 0x10000) != 0;
}

static uint32_t
m3_status(unsigned int cpu)
{
	return m3_pmu != 0 && m3_is_m3(cpu) ? m3_rd(m3_pmu, M3_STATUS(cpu)) & 0xf : 0xf;
}

static uint64_t
m3_us(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_USEC;
}

void
sweet_m3off_init(void)
{
	uint32_t mode = 0;

	if (!PE_parse_boot_argn("m3off", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (ml_get_topology_info()->num_cpus != 8 || !m3_is_m3(M3_LAST)) {
		printf("SWEET M3OFF: m3off=%u: not 8 cores with the M3s at 4-7: off\n", mode);
		return;
	}
	m3_pmu = ml_io_map(M3_PMU_PA, M3_PMU_SIZE);
	m3_intr = ml_io_map(M3_INTR_PA, M3_INTR_SIZE);
	if (m3_pmu == 0 || m3_intr == 0) {
		printf("SWEET M3OFF: m3off=%u: cannot map the PMU: off\n", mode);
		return;
	}
	if (mode >= 2 && !enable_processor_exit) {
		printf("SWEET M3OFF: m3off=%u needs processor_exit=1 (XNU's processor_exit_from_user): decisions only\n", mode);
		mode = 1;
	}
	printf("SWEET M3OFF: m3off=%u (%s); the M3s' power (CLUSTER1_CPUn_STATUS) %x %x %x %x; cores 5-7 may go offline, 4 never\n",
	    mode, mode >= 2 ? "cores taken offline by CPU_OFF and started again" : "the governor decides and logs, no core touched",
	    m3_status(4), m3_status(5), m3_status(6), m3_status(7));
	os_atomic_store(&sweet_m3off_mode, MIN(mode, 2u), release);
}

void
sweet_m3off_dying(void)
{
	cpu_data_t *cdp = getCpuDatap();
	unsigned int cpu = (unsigned int)cdp->cpu_number;
	uint32_t bit;

	if (os_atomic_load(&sweet_m3off_mode, acquire) < 2 || !m3_is_m3(cpu) || cpu == M3_FIRST ||
	    (os_atomic_load(&m3_going_mask, acquire) & (1u << cpu)) == 0) {
		panic("SWEET M3OFF: cpu %u asked to halt, not by m3off (mode %u, going %#x)", cpu, sweet_m3off_mode, m3_going_mask);
	}
	bit = 1u << (cpu - M3_FIRST);
	/* Samsung's cpu_disable() on the dying core: the C2 hint in its CPU_INFORM, then its "off" sequence */
	m3_take_lock();
	m3_wr(m3_pmu, M3_CPU_INFORM(cpu), 1);
	m3_set(m3_intr, M3_INTR_GRP2_ENABLE, bit, true);
	if ((m3_rd(m3_intr, M3_INTR_GRP1_STAT) & bit) == bit) {
		m3_set(m3_intr, M3_INTR_GRP1_CLEAR, bit, true);
	}
	m3_set(m3_pmu, M3_PMU_EVT_GRP2_EN, bit, true);
	__builtin_arm_dsb(DSB_SY);
	m3_give_lock();
	/* cpu_exit_wait() on the other side waits for this; cpu_start() cleans cpu_data to PoC again before CPU_ON */
	cdp->cpu_sleep_token = ARM_CPU_ON_SLEEP_PATH;
	CleanPoC_DcacheRegion_Force((vm_offset_t)cdp, sizeof(*cdp));
	__builtin_arm_dsb(DSB_SY);
	{
		register uint64_t x0 __asm__("x0") = PSCI_CPU_OFF_01;
		register uint64_t x1 __asm__("x1") = PSCI_POWER_DOWN;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;
		__asm__ volatile (SWEET_PSCI_CONDUIT : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                        "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                        "x14", "x15", "x16", "x17", "cc", "memory");
		/* CPU_OFF does not return when it works: the PMU poll of sweet_m3off_take() fails and says why */
		m3_dying_rc[cpu] = (int32_t)x0;
	}
	__builtin_arm_dsb(DSB_SY);
	for (;;) {
		__builtin_arm_wfi();
	}
}

bool
sweet_m3off_back(unsigned int cpu)
{
	uint32_t bit;

	if (!m3_is_m3(cpu) || m3_pmu == 0 ||
	    ((os_atomic_load(&m3_off_mask, acquire) | os_atomic_load(&m3_going_mask, acquire)) & (1u << cpu)) == 0) {
		return false;
	}
	bit = 1u << (cpu - M3_FIRST);
	/* Samsung's cpu_enable() on the core coming up: CPU_INFORM cleared, then its "on" sequence */
	m3_take_lock();
	m3_wr(m3_pmu, M3_CPU_INFORM(cpu), 0);
	m3_set(m3_intr, M3_INTR_GRP2_ENABLE, bit, false);
	if ((m3_rd(m3_intr, M3_INTR_GRP2_STAT) & bit) == bit) {
		m3_set(m3_intr, M3_INTR_GRP2_CLEAR, bit, true);
	}
	m3_set(m3_pmu, M3_PMU_EVT_GRP2_EN, bit, false);
	__builtin_arm_dsb(DSB_SY);
	m3_give_lock();
	return true;
}

bool
sweet_m3off_may_start(unsigned int cpu)
{
	return os_atomic_load(&sweet_m3off_mode, acquire) >= 2 && m3_is_m3(cpu) &&
	       ((os_atomic_load(&m3_off_mask, acquire) & os_atomic_load(&m3_going_mask, acquire)) & (1u << cpu)) != 0 &&
	       m3_status(cpu) == 0;
}

static void
m3_event(unsigned int cpu, const char *what, uint64_t us, kern_return_t kr)
{
	if (os_atomic_inc_orig(&m3_events, relaxed) < EVENTS_LOGGED || kr != KERN_SUCCESS) {
		printf("SWEET M3OFF: cpu %u %s in %llu us (kr %d); M3 power %x %x %x %x; offline %#x\n", cpu, what, us, kr,
		    m3_status(4), m3_status(5), m3_status(6), m3_status(7), os_atomic_load(&m3_off_mask, relaxed));
	}
}

int
sweet_m3off_take(unsigned int cpu)
{
	processor_t p;
	kern_return_t kr;
	uint64_t t0, waited = 0;

	if (os_atomic_load(&sweet_m3off_mode, acquire) < 2 || !m3_is_m3(cpu) || cpu == M3_FIRST) {
		return KERN_INVALID_ARGUMENT;
	}
	if ((os_atomic_load(&m3_off_mask, acquire) & (1u << cpu)) != 0) {
		return KERN_SUCCESS;
	}
	p = cpu_to_processor((int)cpu);
	if (p == PROCESSOR_NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	os_atomic_or(&m3_going_mask, 1u << cpu, release);
	t0 = mach_absolute_time();
	kr = processor_exit_from_user(p);       /* returns once XNU has it offline (cpu_exit_wait) */
	if (kr != KERN_SUCCESS) {
		os_atomic_andnot(&m3_going_mask, 1u << cpu, release);
		m3s[cpu].fails++;
		m3_event(cpu, "offline REFUSED by XNU", m3_us(mach_absolute_time() - t0), kr);
		return kr;
	}
	while (m3_status(cpu) != 0) {
		if (waited++ >= OFF_WAIT_MS * 10) {
			panic("SWEET M3OFF: cpu %u offline in XNU but the PMU still reads it on (%x) %u ms later; CPU_OFF answered %d",
			    cpu, m3_status(cpu), OFF_WAIT_MS, m3_dying_rc[cpu]);
		}
		delay(100);
	}
	os_atomic_or(&m3_off_mask, 1u << cpu, release);
	os_atomic_andnot(&m3_going_mask, 1u << cpu, release);
	m3s[cpu].takes++;
	m3s[cpu].take_us_last = m3_us(mach_absolute_time() - t0);
	m3s[cpu].take_us_max = MAX(m3s[cpu].take_us_max, m3s[cpu].take_us_last);
	m3_event(cpu, "OFFLINE (XNU offline, CPU_OFF, PMU reads off)", m3s[cpu].take_us_last, kr);
	return KERN_SUCCESS;
}

int
sweet_m3off_give(unsigned int cpu)
{
	processor_t p;
	kern_return_t kr;
	uint64_t t0, waited = 0;

	if (!m3_is_m3(cpu) || (os_atomic_load(&m3_off_mask, acquire) & (1u << cpu)) == 0) {
		return KERN_SUCCESS;
	}
	p = cpu_to_processor((int)cpu);
	os_atomic_or(&m3_going_mask, 1u << cpu, release);
	t0 = mach_absolute_time();
	kr = processor_start_from_user(p);      /* returns once it runs (processor_wait_for_start panics if it does not) */
	if (kr != KERN_SUCCESS) {
		os_atomic_andnot(&m3_going_mask, 1u << cpu, release);
		m3s[cpu].fails++;
		m3_event(cpu, "start REFUSED by XNU (still offline)", m3_us(mach_absolute_time() - t0), kr);
		return kr;
	}
	while (m3_status(cpu) != 0xf) {
		if (waited++ >= 1000) {
			panic("SWEET M3OFF: cpu %u runs again in XNU but the PMU reads it %x", cpu, m3_status(cpu));
		}
		delay(100);
	}
	os_atomic_andnot(&m3_off_mask, 1u << cpu, release);
	os_atomic_andnot(&m3_going_mask, 1u << cpu, release);
	m3s[cpu].gives++;
	m3s[cpu].give_us_last = m3_us(mach_absolute_time() - t0);
	m3s[cpu].give_us_max = MAX(m3s[cpu].give_us_max, m3s[cpu].give_us_last);
	m3_event(cpu, "BACK (CPU_ON, running)", m3s[cpu].give_us_last, kr);
	return KERN_SUCCESS;
}

uint32_t
sweet_m3off_count(void)
{
	uint32_t mask = os_atomic_load(&m3_off_mask, acquire), n = 0;

	for (unsigned int cpu = M3_FIRST + 1; cpu <= M3_LAST; cpu++) {
		if ((mask & (1u << cpu)) != 0 && m3_status(cpu) == 0) {
			n++;
		}
	}
	return n;
}

bool
sweet_m3off_is_off(unsigned int cpu)
{
	return cpu < 32 && ((os_atomic_load(&m3_off_mask, acquire) | os_atomic_load(&m3_going_mask, acquire)) & (1u << cpu)) != 0;
}

size_t
sweet_m3off_format(char *buf, size_t len)
{
	size_t at = 0;

	at += (size_t)scnprintf(buf + at, len - at, "mode %u offline %#x power %x %x %x %x", sweet_m3off_mode, m3_off_mask,
	    m3_status(4), m3_status(5), m3_status(6), m3_status(7));
	for (unsigned int cpu = M3_FIRST + 1; cpu <= M3_LAST && at < len; cpu++) {
		at += (size_t)scnprintf(buf + at, len - at, " | cpu%u off %u (last %llu us, max %llu) on %u (last %llu us, max %llu) "
		    "fails %u", cpu, m3s[cpu].takes, m3s[cpu].take_us_last, m3s[cpu].take_us_max, m3s[cpu].gives,
		    m3s[cpu].give_us_last, m3s[cpu].give_us_max, m3s[cpu].fails);
	}
	return at;
}
#endif /* SWEET_M3OFF */
