/*
 * Ported to the Starbite iOS 27 (xnu-12377) STARLTE tree from HakiOS/Scion (Scion-Project/scion-kernel,
 * drivers/power/exynos/hakios.exynos_cpufreq.c, APSL), keeping Scion's notes below. What differs here:
 *   - ACPM: no client of its own. Requests go through bsd/dev/hakios.exynos_acpm_dvfs.c onto starlte_touch.c's ACPM client
 *     (one lock, one sequence numbering on the APM's mailbox); see that file.
 *   - Started from IOKitBSDInit (beside starlte_touch_probe) only with cpufreq_gov=1; the thread waits STARTUP_MS before
 *     its first ACPM request (after the touch thread has powered the panel), and starlte_touch.c does not set its fixed
 *     starcpu= rates when cpufreq_gov=1. Without cpufreq_gov=1 nothing here runs and the fixed rates stay as before.
 *   - No UEFI loader and no loader core: S-Boot boots a shim that jumps to XNU and the scheduler has every core it is
 *     given. So the A55s' floor is their min-freq (455000) by default, not 1053000 (cpufreq_gov_lit_min= sets another).
 *   - The cores: logical 0-3 are the A55s (phys id 0x000-0x300, the core in Aff1), 4-7 the M3s (0x10100-0x10103,
 *     SweetCPU.cpp): a core's cluster is phys id bits 23:16 here, not 15:8. With cpus=4 only the A55s run: the M3s'
 *     cluster has no core online and is left alone after the first request (its ceiling, as the fixed rates did).
 *   - The TMU driver is bsd/dev/hakios.exynos_tmu.c (Scion's steps 1-2 only, tmu=1 or 2; no step 3). Without tmu= no
 *     temperatures: the M3s stay at or below 1794000 (Scion's own rule without them), the thermal ceilings never move,
 *     and the scion policies (2-4) run at their safe rates.
 *   - No loader log: the lines go to printf (the console and the USB console's log).
 *   - The touch boost from starlte_touch.c, the frame boost from SweetFramebuffer.cpp's present (AppleCLCD::present).
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/*
 * The Galaxy S9's CPU frequency governor: the A55s' (cpucl0) and the M3s' (cpucl1) rates moved with the load, inside
 * the limits Samsung's own kernel keeps, and lowered when the TMU says the cores are hot. Only with cpufreq_gov=1; without
 * it the fixed rates of cpufreq_lit= and cpufreq_big= stay (drivers/soc/exynos/hakios.exynos_acpm.c), as before.
 *
 * How a rate is set. A DVFS request to the APM's ACPM firmware names a domain (2 cpucl0, 3 cpucl1) and a rate in kHz,
 * nothing else (hakios_acpm_dvfs_set; Samsung's cal_dfs_set_rate -> exynos_acpm_set_rate sends the same three words).
 * The voltage is the APM's: it keeps a table of rate and voltage per level ("fvmap", the ASV voltages already in it) and
 * applies the voltage with the rate. Samsung's kernel never sends a voltage for the CPU clusters; it reads the table
 * once (cal-if/fvmap.c) and adds a margin to it only when its command line asks. Neither does this file: it reads the
 * table, logs it, asks only for rates that are in it, and reads each rate back.
 *
 * The levels. From the APM's table when it can be read (hakios_acpm_dvfs_table), else the device tree's (the dm
 * constraints' mif-perf tables of Samsung's tree and the phone's own, which list every level of each cluster):
 *   cpucl0  1950000 1794000 1690000 1456000 1248000 1053000 949000 832000 715000 598000 455000
 *   cpucl1  2886000 2860000 2704000 2652000 2496000 2314000 2106000 2002000 1924000 1794000 1690000 1586000 1469000
 *           1261000 1170000 1066000 962000 858000 741000 650000 598000
 *
 * The limits, as Samsung's kernel keeps them on this phone:
 *   cpucl0  min-freq 455000, max-freq 1950000 (Samsung's tree; the phone's own has no max-freq: the CAL's, the table's top)
 *   cpucl1  min-freq 650000; max-freq 2314000 and policy-max 1794000 in Samsung's tree, max-freq 1794000 in the phone's
 *           own; and by how many M3s are online (the hotplug governor's single/dual/triple/quad_freq, of which the
 *           driver takes the triple's for three): 1 or 2 online 2314000, 3 or 4 online 1794000
 * So stock Android on this phone never runs the M3s above 1794000 with all four online, and above 2314000 never at all
 * (2704000, the S9's advertised top, and 2886000 are in the table and nowhere allowed). The ceiling here: for the M3s
 * the online count's limit, and by default no more than 1794000 in any case (the phone's tree's max-freq;
 * cpufreq_gov_big_max= and gov_policy=3 held there too unless ocp_guard=4's rule allows more); for the A55s 1794000 by default
 * (cpufreq_gov_lit_max= up to 1950000). The floors: the M3s' min-freq; the A55s' 1053000 by default, not 455000, because
 * core 0, which runs the loader's UFS service, is an A55 the scheduler does not see: its load is not counted here.
 *
 * The heat, as Samsung's thermal zones treat it: BIG cooled from 83 C (big-control-temp, the power allocator, polled
 * every 50 ms once there) and its cores taken away at 96 (hotplug_out_threshold, back at 91); LITTLE's first frequency
 * trip at 76 (little-alert1); "hot" at 115 for both. The frequencies of each trip are in the bootloader's ECT, not in the
 * source, so this file has its own, in the same places: every second, a zone at its control point (83 BIG, 76 LITTLE)
 * or above loses one level of its cluster's ceiling, and gets one back once 5 degrees under it (the trips' hysteresis);
 * at 96 or above the cluster goes to its lowest level at once (the slowest at or above min-freq: never under what
 * Samsung's kernel uses). The temperatures are the TMU driver's, asked of it directly (hakios_exynos_tmu_zone_mc(): its
 * last reading of the BIG and LITTLE zones, with tmu=1 or more). They were read through kern.hakios.temp.big_mc and
 * little_mc (kernel_sysctlbyname) at first, which on the phone gave nothing ("temp big - lit - C" all through main-1102
 * while the TMU logged 90 C and more); the sysctl's answer is still logged once, beside the direct reading. Without
 * temperatures (no tmu=, or not read yet) the M3s stay at or below 1794000 whatever the online count. tmu=3 ends the run
 * at 110 C as before; this changes nothing there.
 *
 * The load. Every cpufreq_gov_ms (40 ms by default, 20 to 200) each core's idle time from the scheduler
 * (ml_cpu_idle_time, as cpuload= reads it); a cluster's load is its busiest core's share of the window it was not idle.
 * The rate wanted is the one at which that core would be 60% busy (the current rate times load / 60; gov_target=), rounded
 * up to a level. Up at once, and a core 90% busy or more (its real need unknown: it had no time left to show it) goes at
 * least to the cluster's touch-boost rate in one step (Android's interactive governor's hispeed_freq); down only after
 * five windows in a row want less (about 200 ms; gov_down=). It was 75% and three windows: on the phone the
 * M3s sat at 650 MHz with 20 to 40% load, and the display's presenter took 4.2 ms to copy a frame instead of 1.6 ms.
 *
 * The boosts. What the load cannot see coming, raised before the load shows it:
 *   touch  every finger event of the touch screen (hakios.s6sy761.c, its polling thread, hakios_exynos_cpufreq_touch())
 *          holds the M3s at 1469000 or more and the A55s at 1690000 or more until gov_boost_ms (300) after the last one;
 *   frames while the display's presenter is given frames (MiniIOMobileFramebuffer.cpp present(),
 *          hakios_exynos_cpufreq_frame()), a frame in the last 100 ms: the M3s at 1261000 or more (gov_frame_big=).
 * The caller only stamps the time, counts, and wakes this file's thread (thread_wakeup) when no boost of its kind was
 * running; it never asks the APM itself, never waits, never takes a lock. The thread, woken between its windows, raises
 * whichever cluster is under its boost at once, then goes back to waiting out the window; the load is measured over the
 * whole window as before. A boost is a floor only: it is clamped by everything that clamps the load's rate -- the
 * ceiling, the online count's limit, 1794000 without temperatures, the thermal ceiling, the lowest level at 96 C -- and
 * when it ends the rate comes down as the load's would (the down windows).
 * A core that has never been idle
 * since start is taken as not started (a core the kernel was not given, or not yet: the loader's core 0 until its
 * handover with core0=1): it counts neither for the load nor as online. Every change is read back from the APM (hakios_acpm_dvfs_get); a rate read back above the ceiling is logged
 * and the next level down asked for. The APM of the S9 answers FREQ_GET with 0 for both CPU domains (main-1102 and the
 * fixed-rate runs before it: "dvfs: the M3s (cpucl1): 0 kHz (0) -> asked 2106000 kHz (0) -> now 0 kHz (0)"; Samsung's
 * own cpufreq driver reads its rate from the CAL's cache, never from the APM). Taken as a rate, that 0 stopped the
 * governor dead: decide() skips a cluster whose rate is 0, so nothing after the first request was ever asked for, the
 * thermal ceilings included. A 0 is therefore "not said": the rate asked for is the rate, the APM's answer to the
 * request itself is logged (its four words, for the first changes), and the rate the cores really run at is measured
 * once a log line (measure_mhz(): a chain of dependent additions, one a cycle on both the A55 and the M3, timed on a
 * core of each cluster with interrupts off for a fraction of a millisecond).
 *
 * The log: at start the table, the limits and the rates found; then once a second (cpufreq_gov_log=SECONDS; 0: none)
 *   cpufreq_gov: 12 s: lit 1248 MHz load 31% (avg 22%, 2 online, 1794 max, measured 1251) | big 1586 MHz load 64% (avg
 *       58%, 4 online, 1794 max, measured 1590) | temp big 52.1 lit 48.0 C | 9 changes | boost: 41 touch, 58 frames, 2
 *       raised, touch on
 * (a rate is "asked" when the APM does not say it, "?" measured when no core of the cluster could be measured; the boost
 * part: the touch events and frames of that second, how many times a boost raised a cluster, which boosts hold now --
 * "touch", "frames", both, or "off" -- so boosts are said once a second at most, never one line each)
 * to the loader's log (loader_log_line), which printf does not reach.
 *
 * The scion governor (gov_policy=3; hakios.scion_gov.c has its control law, EXYNOS_SCION_GOV in the SoC's table its numbers):
 * what usb-1104.log said (a second per line, 12,000 seconds of use) is that with a finger moving the M3s are at 1794000
 * already, 93% of the seconds, the busiest core 78% busy there, and that heat never moved that ceiling (78 C at most). So what
 * is left is the first moments of a gesture and what stock allows above 1794000: 2314000 with at most two M3s running. Here
 * the other two are parked (no longer recommended to the scheduler, in WFI: stock powers them off, which this kernel cannot do
 * yet, so their leakage stays and the temperature has the last word). gov_policy=2 is the same law without parking.
 * gov_policy=4 is 2 with the frames setting the M3s' touch floor (scion_pace()): the display's presenter is the one place that
 * sees a frame miss its refresh, and iOS does not report its frames' deadlines here (kernel PR 210). A frame over LATE_MS after the
 * last is late; content that moves at 30 frames a second under a finger reads as late too and takes the floor to the ceiling, for as long as the finger stays.
 * A finger coming down (hakios_exynos_cpufreq_fingers()) puts the M3s at their ceiling for 120 ms; the touch floor holds while
 * a finger is down, however still, at most 5 s after its last event. The frame floor is for frames under 50 ms apart: an idle
 * screen presents ten a second, and policy 1 held the M3s at 1261000 for them. The window is 20 ms while a boost holds.
 *
 * The numbers to compare the policies by, under any of them (gov_stat=0: none), a line after any second with something to say,
 * counted since the line before:
 *   (the log's prefix, then) stat 12 s: policy 2 | big ms 310 40 90 560 0, lit ms 900 100 | sat big 3 lit 0 of 41 | rev big 1 lit 0 | sum big
 *       140% lit 6% | raise 1 avg 410 us max 410 | top 1 avg 2 ms max 2, 0 missed | acpm 6 avg 180 us max 310 | fingers 420 ms | few 0 ms | cooled 0
 * "big ms": time below 1261 MHz, below 1469, below 1794, at 1794, above it; "lit ms": at 1053 MHz or below, above. "sat": windows
 * with a core 90% busy or more while the cluster was below its ceiling, of all windows (the governor behind the load).
 * "rev": rates raised within 300 ms of falling. "sum": the cores' busy shares added up (a thread moving among the cores shows
 * less on the busiest than it needs). "raise": from a gesture's first event to the M3s at its floor; "top": to their ceiling
 * ("missed": not within a second). "acpm": time in each change's requests. "fingers": time with a finger down. "few": with M3s parked. "cooled": thermal ceiling moves.
 *
 * Status: the load governor run on the phone (ACPM takes the requests, the measured MHz match them, the temperatures are
 * read), with the touch and frame boosts, the 60% target and the five down windows (kernel PR 158): the S9's default,
 * cpufreq_gov=1.
 *
 * Boot-args:
 *   cpufreq_gov=1            run it (0, or none: the fixed rates of cpufreq_lit= and cpufreq_big=)
 *   cpufreq_gov_ms=<ms>      the load window (20 to 200; 40)
 *   cpufreq_gov_log=<s>      a summary line this often (0: none; 1)
 *   cpufreq_gov_lit_max=<kHz>, cpufreq_gov_big_max=<kHz>   the ceilings (at most 1950000; the M3s 1794000 below ocp_guard=4)
 *   cpufreq_gov_lit_min=<kHz>, cpufreq_gov_big_min=<kHz>   the floors (at least 455000 and 650000)
 *   gov_boost_big=<kHz>      the M3s' floor while touched (1469000; 0: no touch boost for them)
 *   gov_boost_lit=<kHz>      the A55s' floor while touched (1690000; 0: none)
 *   gov_boost_ms=<ms>        how long the touch boost holds after the last finger event (300; 0: no touch boost)
 *   gov_frame_big=<kHz>      the M3s' floor while frames come (1261000; 0: no frame boost)
 *   gov_frame_ms=<ms>        a frame this recent keeps the frame boost on (100; 20 to 1000)
 *   gov_target=<percent>     the busiest core's share wanted at the new rate (60; 30 to 95)
 *   gov_down=<windows>       windows in a row that want less before the rate comes down (5; 1 to 50)
 *   gov_wake=1               a finger coming down asks for the GPU's frame floor at once, before the first frame needs it (off)
 *   gov_policy=<n>           1 the policy above; 3 the scion governor; 2 scion without parking; 4 that with frame pacing (1 for now)
 *   gov_stat=<n>             the line of numbers to compare by (1; 0: none)
 *   ocp_guard=<n>            the over-current guard, NOT run on a phone (docs/ios27/OCP.md; 0, the default: off)
 *                            1 observe, 2 and the OCP block's registers read, 3 stage one (armed, the M3s still at
 *                            1794000 at most), 4 stage two (refuses: no M3 can be taken offline yet)
 *   ocp_guard_flags=1        with ocp_guard: also the S2MPS18's INT5 read each second (BUCK2 at its current limit)
 *
 * Sources (Samsung's kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810, lineage-24.0 at e5906667068e,
 * and Silicium's Resources/DTBs/starlte.dts, the phone's own tree): arch/arm64/boot/dts/exynos/
 * exynos9810-starlte_eur_open_26.dts (cpufreq { domain@0, domain@1 }: min-freq, max-freq, policy-max, the mif-perf
 * tables; hotplug_governor: single/dual/triple/quad_freq; thermal-zones BIG and LITTLE; BIG@10070000's hotplug
 * thresholds; acpm's fvmap_offset), drivers/cpufreq/exynos-acme.c (init_domain: max-freq over the CAL's; init_table: the
 * levels outside the limits left out; set_policy), drivers/soc/samsung/exynos-hotplug_governor.c (get_hpgov_maxfreq: the
 * limit per online count), drivers/soc/samsung/cal-if/cal-if.c (cal_dfs_set_rate), acpm_dvfs.c (exynos_acpm_set_rate:
 * domain, rate, FREQ_REQ), fvmap.c and fvmap.h (the table and its layout), exynos9810/acpm_dvfs_exynos9810.h (the
 * indexes), drivers/soc/samsung/acpm/acpm.c (plugins_init) and fw_header/ (the plugin and initdata layouts). All GPL:
 * read for these facts, not one line copied.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.scion_gov.h>
#include "hakios.exynos_cpufreq_ceiling.h"

/*
 * (Starbite) osfmk/arm64/sweet_m3off.c: M3s taken offline (XNU's processor offline, Samsung's PMU writes, PSCI CPU_OFF) and
 * started again; m3off= (0 off, 1 decisions logged only, 2 run). The policy that asks for it is m3off_decide() below
 * (docs/ios27/BIG-CORES.md): with the interface moving and the M3s' load fitting on two, cores 6 and 7 go offline and the two
 * left may run at Samsung's dual_freq, 2314000, under the rule of hakios.exynos_cpufreq_ceiling.h (the PMIC's warning read on,
 * a fresh temperature under 75 C, at most two M3s on, PMU-checked). Back when the interface is still for a second, when the
 * two cannot carry the load, at 75 C (a stop for the rest of the boot), or on any failure (the same).
 */
extern uint32_t sweet_m3off_mode;
extern int sweet_m3off_take(unsigned int cpu);
extern int sweet_m3off_give(unsigned int cpu);
extern uint32_t sweet_m3off_count(void);
extern bool sweet_m3off_is_off(unsigned int cpu);
extern size_t sweet_m3off_format(char *buf, size_t len);
static bool m3off_returning;            /* cores are coming back: the M3s at 1794000 at most until they have */
static uint32_t m3off_returning_gen, m3off_seen_gen;    /* the governor's acknowledgement of it (a window decided since) */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>

extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
/* bsd/dev/starlte_touch.c, through hakios.exynos_acpm_dvfs.c: the ACPM client is there */
extern bool starlte_acpm_dvfs_ready(void);
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
/* (kern/sched_prim.h, not included: its thread_block and thread_bind are declared above as this file has used them) */
extern wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t interruptible, uint64_t deadline);
extern kern_return_t thread_wakeup_prim(event_t event, boolean_t one_thread, wait_result_t result);

#define LEVELS_MOST     32
#define TARGET_LOAD     60      /* percent: the busiest core's share wanted at the new rate (gov_target=) */
#define DOWN_WINDOWS    5       /* windows in a row that want less before the rate comes down (gov_down=) */
#define SATURATED       90      /* percent: a core this busy goes at least to its cluster's touch-boost rate at once */
#define NOT_READ_MC     (-273000)
#define HOT_CUT_MC      96000   /* BIG's hotplug_out_threshold: the cluster to its lowest level */
#define HYSTERESIS_MC   5000
#define NO_TEMP_BIG_MAX 1794000 /* without temperatures, the M3s no faster than this */
#define ANSWERS_LOGGED  8       /* changes whose APM answer is logged, per cluster */
#define SPIN_LOOPS      8192    /* measure_mhz(): 32 additions each, 262144 cycles (0.1 to 0.6 ms) */
#define BOUNCE_MS       300     /* a rate raised within this of falling has bounced */
#define RES_BUCKETS     5
#define BURST_MS        120     /* the M3s at their ceiling after a finger comes down */
#define HOLD_MS         5000    /* a finger held still keeps the touch floor this long after its last event */
#define BOOST_WINDOW_MS 20      /* the load window while a boost holds */
#define LATE_MS         25      /* one and a half refreshes at 60 Hz */
#define MOVING_MS       50      /* frames closer than this are moving something */
#define STARTUP_MS      3000    /* (Starbite) the thread's wait before its first request: after the touch thread's ACPM start */

/*
 * (Starbite) the TMU driver, bsd/dev/hakios.exynos_tmu.c (Scion's, steps 1-2): its last reading of a zone, how old it is,
 * and a zone read now; NOT_READ_MC (and an age of UINT64_MAX) without tmu= or before its first reading.
 */
extern int32_t hakios_exynos_tmu_zone_mc(unsigned zone);
extern uint64_t hakios_exynos_tmu_age_ms(void);
extern int32_t hakios_exynos_tmu_zone_now_mc(unsigned zone);

/* (Starbite) a core's cluster: phys id bits 23:16 (A55s 0x000-0x300, M3s 0x10100-0x10103; SweetCPU.cpp) */
static inline uint32_t
cpu_affinity(const ml_topology_cpu_t *cpu)
{
	return (cpu->phys_id >> 16) & 0xff;
}

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	return printf("%s", line);
}

static struct cluster {
	const char *name;               /* "lit", "big" */
	const char *zone;               /* the TMU's sysctl (logged once, beside the direct reading) */
	unsigned tmu_zone;              /* the TMU driver's zone: 0 BIG, 1 LITTLE */
	uint32_t index;                 /* the ACPM DVFS domain */
	uint32_t affinity;              /* MPIDR [15:8] of its cores */
	int32_t control_mc;             /* where Samsung starts to cool it */
	uint32_t dt_min, dt_max;        /* the device tree's limits (see the head) */
	const uint32_t *dt_levels;
	uint32_t n_dt_levels;
	uint32_t levels[LEVELS_MOST];   /* the fastest first */
	uint32_t n;
	uint32_t floor, ceiling;        /* kHz: boot-args within the device tree's limits */
	uint32_t thermal;               /* the thermal ceiling, a level index (0: none) */
	uint32_t cur;                   /* kHz, as read back, or as asked when the APM says 0 */
	bool asked;                     /* cur is the rate asked for: the APM's FREQ_GET said 0 */
	uint32_t measured;              /* MHz, measure_mhz()'s last (0: not measured) */
	uint32_t load, online, low_windows;
	uint32_t load_sum, windows;     /* this second's */
	uint32_t lowest;                /* the slowest level at or above the device tree's min-freq: as low as the heat takes it */
	uint32_t changes, errors;
	int32_t temp_mc;
	uint32_t touch_khz, frame_khz;  /* the boosts' floors (0: none), before the ceiling clamps them */
	uint32_t boost_floor;           /* the last boost_floor() (kHz; 0: no boost on) */
	/* scion */
	uint32_t scion_touch, scion_safe;       /* the touch floor, the rate with no temperature to trust */
	bool burst;                     /* to the ceiling when a finger comes down */
	struct scion_rate rate;
	/* the numbers to compare by, under any policy (the stat line) */
	uint32_t res_khz[RES_BUCKETS - 1]; /* the buckets' upper ends */
	uint64_t res[RES_BUCKETS], rate_since; /* time at each, since the last change or line */
	uint64_t last_down;
	uint32_t sat, stat_windows, reversals, sum_now, cooled;
	uint32_t core_load[SCION_BIG_CORES];    /* the window's, busiest first */
	uint64_t sum_sum;
} clusters[2];

static const uint32_t lit_dt[] = {
	1950000, 1794000, 1690000, 1456000, 1248000, 1053000, 949000, 832000, 715000, 598000, 455000,
};
static const uint32_t big_dt[] = {
	2886000, 2860000, 2704000, 2652000, 2496000, 2314000, 2106000, 2002000, 1924000, 1794000, 1690000, 1586000, 1469000,
	1261000, 1170000, 1066000, 962000, 858000, 741000, 650000, 598000,
};

static uint32_t window_ms = 40, log_seconds = 1, target_load = TARGET_LOAD, down_windows = DOWN_WINDOWS;

/*
 * The boosts (see the head). The stamps and counts are written by the touch screen's thread and the display's presenter,
 * read by this file's thread: plain 64- and 32-bit atomics, no lock. boost_event is what the thread waits on.
 */
static uint32_t boost_ms = 300, frame_ms = 100;
static uint64_t boost_span, frame_span;         /* the same in mach_absolute_time units */
static uint32_t frames_late, frames_on_time;    /* scion_pace()'s, since the last window */
static uint64_t late_span;
static uint64_t frame_gap, moving_span;         /* the last two frames apart; scion's frame floor is for frames closer than moving_span */
static uint64_t touch_last, frame_last;         /* the last finger event's and frame's time (0: none yet) */
static uint32_t touch_count, frame_count;       /* this second's, for the summary */
static uint32_t boost_raises;                   /* this second's: times a boost raised a cluster's rate */
static bool gov_running;
static int boost_event;

/* the policy (see the head): written by the boot-arg and kern.hakios.gov_policy, read by this file's thread */
static uint32_t gov_policy = 1;
static uint32_t gov_stat = 1;
static uint64_t burst_span, hold_span;
static uint32_t big_cores = SCION_BIG_CORES;    /* the M3s the scheduler may use */
static struct scion_park park;
static struct scion_pace pace;
static uint32_t gov_wake;
static enum scion_state dev_state = SCION_STATIC;
static uint64_t dev_since, dev_ms[SCION_STATES];
static uint64_t few_since, few_time;
static const struct scion_soc scion_soc = EXYNOS_SCION_GOV;
static uint32_t fingers;                        /* how many are down (hakios_exynos_cpufreq_fingers()) */
static uint64_t finger_edge;                    /* when one last came down */
static uint64_t ramp_began;                     /* a gesture's first event, until the M3s reach their ceiling or a second passes */
static bool ramp_raised;
static uint64_t finger_since;                   /* since when a finger is down (0: none), for the stat line */
static uint64_t finger_time;
static struct {
	uint32_t raise_n, top_n, top_missed, acpm_n;
	uint64_t raise_sum, raise_max, top_sum, top_max, acpm_sum, acpm_max;
} nums;

static bool
boost_on(uint64_t last, uint64_t span, uint64_t now)
{
	return last != 0 && span != 0 && now - last <= span;
}

/* a finger event (hakios.s6sy761.c): the stamp, and the thread woken when the touch boost was not already on */
void
hakios_exynos_cpufreq_touch(void)
{
	uint64_t now, before;

	if (!__atomic_load_n(&gov_running, __ATOMIC_ACQUIRE) || boost_span == 0) {
		return;
	}
	now = mach_absolute_time();
	before = __atomic_exchange_n(&touch_last, now, __ATOMIC_RELEASE);
	__atomic_fetch_add(&touch_count, 1, __ATOMIC_RELAXED);
	if (!boost_on(before, boost_span, now)) {
		__atomic_store_n(&ramp_began, now, __ATOMIC_RELEASE);
		(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
	}
}

/*
 * How many fingers are down now, after the touch screen's thread has changed it (hakios.s6sy761.c; all up when it gives up on
 * the bus). One coming down wakes this file's thread even with the touch boost on: the burst is new. No wait, no lock.
 */
/*
 * (Starbite) gov_wake=1 asks Scion's mali_gov for its frame floor when a finger comes down. This tree has no GPU governor
 * (starlte_gpu.c keeps the G72 at one fixed rate, with vdd_g3d set by hand after it): nothing to tell.
 */
static void
starlte_gpu_gov_wake(void)
{
}

void
hakios_exynos_cpufreq_fingers(uint32_t down)
{
	uint32_t before;
	uint64_t now;

	if (!__atomic_load_n(&gov_running, __ATOMIC_ACQUIRE) || boost_span == 0) {
		return;
	}
	now = mach_absolute_time();
	before = __atomic_exchange_n(&fingers, down, __ATOMIC_RELEASE);
	if (before == 0 && down != 0) {
		if (gov_wake) {
			starlte_gpu_gov_wake();
		}
		__atomic_store_n(&finger_since, now, __ATOMIC_RELEASE);
	} else if (before != 0 && down == 0) {
		uint64_t since = __atomic_exchange_n(&finger_since, 0, __ATOMIC_ACQ_REL);

		if (since != 0) {
			__atomic_fetch_add(&finger_time, now - since, __ATOMIC_RELAXED);
		}
	}
	if (down > before) {
		__atomic_store_n(&finger_edge, now, __ATOMIC_RELEASE);
		(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
	}
}

/* a frame given to the display (MiniIOMobileFramebuffer.cpp present()): the same, for the frame boost */
void
hakios_exynos_cpufreq_frame(void)
{
	uint64_t now, before;

	if (!__atomic_load_n(&gov_running, __ATOMIC_ACQUIRE) || frame_span == 0) {
		return;
	}
	now = mach_absolute_time();
	before = __atomic_exchange_n(&frame_last, now, __ATOMIC_RELEASE);
	__atomic_store_n(&frame_gap, now - before, __ATOMIC_RELAXED);
	if (now - before <= moving_span) {
		__atomic_fetch_add(now - before > late_span ? &frames_late : &frames_on_time, 1, __ATOMIC_RELAXED);
	}
	__atomic_fetch_add(&frame_count, 1, __ATOMIC_RELAXED);
	if (!boost_on(before, frame_span, now)) {
		(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
	}
}

/*
 * kern.hakios.gov_frame_big: the M3s' floor while frames come (gov_frame_big=), changed while the system runs (kHz; 0:
 * no frame boost), for trying one rate against another in one run: the USB console's "sysctl kern.hakios.gov_frame_big=N".
 */
SYSCTL_DECL(_kern_hakios);
static int
frame_big_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int khz = (int)clusters[1].frame_khz, error;

	error = sysctl_io_number(req, khz, sizeof(khz), &khz, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (khz < 0 || khz > 3000000) {
		return EINVAL;
	}
	if (khz != 0 && frame_span == 0) {
		frame_ms = 100;
		nanoseconds_to_absolutetime((uint64_t)frame_ms * NSEC_PER_MSEC, &frame_span);
	}
	clusters[1].frame_khz = (uint32_t)khz;
	say("cpufreq_gov: the M3s' frame boost now %d kHz (kern.hakios.gov_frame_big)\n", khz);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, gov_frame_big, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    frame_big_sysctl, "I", "the M3s' floor while frames come, kHz (gov_frame_big=; 0: none)");

/* kern.hakios.gov_policy: gov_policy=, changed while the system runs (the USB console's "sysctl kern.hakios.gov_policy=N") */
static int
policy_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = (int)gov_policy, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value < 1 || value > 4) {
		return EINVAL;
	}
	gov_policy = (uint32_t)value;
	say("cpufreq_gov: policy %d now (kern.hakios.gov_policy)\n", value);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, gov_policy, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    policy_sysctl, "I", "the governor's policy: 1 as first, 3 scion, 2 scion without parking, 4 that with frame pacing (gov_policy=)");

static uint64_t idle_before[MAX_CPUS];
static bool ever_idle[MAX_CPUS];
static bool idle_rebase[MAX_CPUS];      /* (m3off) a core offline: its idle time is taken afresh when it is back */

static bool
ever_idle_cpu(unsigned int i)
{
	return i < MAX_CPUS && ever_idle[i];
}

/* the M3s' limit for this many online (exynos-hotplug_governor.c get_hpgov_maxfreq with the device tree's values) */
static uint32_t
big_online_limit(uint32_t online)
{
	return online <= 2 ? 2314000 : 1794000;
}

/* the slowest level at or above khz (the fastest when none is) */
static uint32_t
level_at_least(const struct cluster *c, uint64_t khz)
{
	for (uint32_t l = c->n; l-- > 0;) {
		if (c->levels[l] >= khz) {
			return l;
		}
	}
	return 0;
}

/* the fastest level at or below khz (the slowest when none is) */
static uint32_t
level_at_most(const struct cluster *c, uint32_t khz)
{
	for (uint32_t l = 0; l < c->n; l++) {
		if (c->levels[l] <= khz) {
			return l;
		}
	}
	return c->n - 1;
}

/* the zone's temperature, to be trusted (a scion policy's fall-back to the safe rate: the TMU driver has stopped, or reads nonsense) */
static bool
temp_fresh(const struct cluster *c)
{
	return scion_temp_ok(hakios_exynos_tmu_zone_mc(c->tmu_zone), hakios_exynos_tmu_age_ms());
}


/* (Starbite) the over-current guard's ties to this tree: the PMIC through starlte_touch.c's ACPM client, SPIs through pe_fiq.c */
extern bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
extern kern_return_t starlte_gic_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
#define OCPG_PMIC_READ(reg, v)  (starlte_pmic_read(1, (reg), (v)) ? 0 : EIO)
#define OCPG_SPI_ATTACH         starlte_gic_spi_attach
#define OCPG_CPU_CLUSTER(cpu)   cpu_affinity(cpu)

/*
 * The over-current guard (ocp_guard=N; off by default, NOT run on a phone). docs/ios27/OCP.md (scion-kernel) has the facts,
 * the design and the phone tests; in short:
 *
 * Samsung's kernel runs the M3s above 1794000 (max-freq-wo-ocp, the exynos-ocp node of exynos9810-starlte_eur_open_26.dts)
 * only while three things hold: its OCP driver runs (drivers/soc/samsung/exynos-ocp.c), the S2MPS18's CPU over-current
 * warning is on (drivers/regulator/s2mps18.c writes OCP_WARN1, 0xD3, = 0x80 at every boot from the PMIC node's
 * cpu_ocp_warn_en/reset/lv 1/0/0), and the hotplug governor has taken the other M3s offline (exynos-hotplug_governor.c:
 * a core counts toward the limit until CPU_DEAD, after PSCI CPU_OFF; 2314000 for one or two, 1794000 for three or four).
 * The OCP block is the M3 cluster's own, reached only through implementation-defined system registers on an M3 (the
 * driver uses the first, cpu 4): OCPTHROTTCTL S3_0_C15_C12_7 (bit 27 OCPTHROTTERRA: a throttle event; written 0 to
 * clear), GCUCTL S3_1_C15_C11_0 (bit 24 OCPTHROTTERRAEN: its interrupt), OCPINTCTL S3_0_C15_C12_5 ([9:4] IRP, set before
 * every rate change to 2 x rate / min-freq - 2, at most 63), and two the driver names and never touches,
 * OCPTHROTTCNTA S3_0_C15_C14_4 and OCPTIDEPBOTTHRESH S3_0_C15_C15_1 (their values are the firmware's). Its interrupt is SPI
 * 181 (INTID 213), rising edge. On it the driver turns the interrupt off, and when the flag is set clears it and lowers
 * the M3s' ceiling one level under the last ceiling (down-step 1), turns the interrupt back on, and 15 ms later
 * (release-duration) releases the ceiling at once if the M3s' summed load is under 50% of the four cores' capacity
 * (release-mode 1, release-threshold 50), else looks again 15 ms later.
 *
 * Here, by level (each includes the ones under it):
 *   1  observe: the PMIC's OCP_WARN1/OCP_WARN2 read (never written), the M3s' power status read from the PMU each
 *      second, SPI 213 attached and counted, and the interrupt path tested once by pending SPI 213 at the GIC by
 *      software. No M3 register is touched. The M3s' ceiling is held at 1794000 whatever else is asked (gov_policy=3,
 *      cpufreq_gov_big_max=), as it is without the guard: hakios.exynos_cpufreq_ceiling.h is the rule for every option.
 *   2  probe: the OCP block's five registers read once on cpu 4, and its flag read with each interrupt and once a
 *      second; nothing written. (On an S9+ under iOS 16.5 the governor's thread never came back from this read: kernel
 *      commit dab99986. Here it runs on a thread of its own, so the governor keeps the heat in hand if it does not.)
 *   3  stage one: armed as Samsung's driver runs it, the ceiling still 1794000: IRP kept to the rate, the interrupt
 *      handled with Samsung's step down and release, the interrupt path tested at arming and every minute, the flag
 *      polled each second (set with no interrupt behind it: the path is dead), the enable bit and the PMIC's warning
 *      read back. Any failure latches "dead" for the boot.
 *   4  stage two: rates above 1794000 (2314000 at most, the dual_freq) only while stage one is armed and alive, no
 *      more M3s are on than Samsung allows for the rate (powered off by CPU_OFF, the PMU saying so), and the big
 *      zone is under 83 C. This kernel cannot take an M3 offline yet (SweetCPU::haltCPU, starting an M3 again after
 *      CPU_OFF failed on the S9+'s mainline port), so ocpg_m3s_off() is 0 and stage two never raises the ceiling: it
 *      says why, once a change.
 * ocp_guard_flags=1: also the S2MPS18's INT5 read once a second (bit 1 OCPB2: BUCK2, vdd_cpucl1, hit its current limit).
 * Reading INT5 clears it, as Samsung's PMIC interrupt thread does; nothing else of this tree reads it.
 */
#define OCPG_INTID              (181u + 32u)    /* exynos-ocp interrupts = <0 181 0> */
#define OCPG_GICD_PA            0x10101000ul    /* the GIC-400's distributor (interrupt-controller@10100000) */
#define OCPG_PMU_CL1_PA         0x14062000ul    /* PMU + 0x2000: CLUSTER1_CPUn_STATUS at +0x4 + 8n */
#define OCPG_WARN1              0xD3u           /* S2MPS18_PMIC_REG_OCP_WARN1 (CPU) */
#define OCPG_WARN2              0xD4u           /* (GPU) */
#define OCPG_INT5               0x04u           /* S2MPS18_PMIC_REG_INT5: OCPB1..8 */
#define OCPG_WARN_EN            0x80u           /* S2MPS18_OCP_WARN_EN_SHIFT 7 */
#define OCPG_WO_KHZ             1794000u        /* max-freq-wo-ocp */
#define OCPG_MIN_KHZ            650000u         /* domain@1 min-freq: the driver's min_freq */
#define OCPG_RELEASE_MS         15u             /* release-duration */
#define OCPG_RELEASE_SUM        200u            /* release-threshold 50% of four cores: summed busy percent */
#define OCPG_ERRA               (1ull << 27)    /* OCPTHROTTCTL OCPTHROTTERRA */
#define OCPG_ERRAEN             (1ull << 24)    /* GCUCTL OCPTHROTTERRAEN */
#define OCPG_IRP_SHIFT          4
#define OCPG_IRP_MASK           0x3full
#define OCPG_TEST_MS            100u            /* a pended interrupt must reach the handler within this */
#define OCPG_LATE_MS            50u             /* the flag polled set with no interrupt this recent: the path is dead */

static uint32_t ocpg_mode, ocpg_flags;
static vm_offset_t ocpg_gicd, ocpg_pmu;
static int ocpg_event;                          /* the guard's thread waits on it */
static uint32_t ocpg_spi_count;                 /* the handler's, interrupt context */
static uint64_t ocpg_spi_last;
static uint32_t ocpg_irp_want = ~0u, ocpg_irp_done = ~0u;
static uint32_t ocpg_clip;                      /* kHz: the ceiling after a throttle (0: none) */
static uint32_t ocpg_seen_ceiling = OCPG_WO_KHZ; /* the M3s' ceiling before the clip, as the governor last worked it out */
static bool ocpg_kick;                          /* the governor to decide again now */
static uint64_t ocpg_beat;                      /* the guard's thread last ran (mach time) */
static int ocpg_cpu = -1;                       /* the M3 it runs on */
static struct {
	bool attached, regs_read, armed, dead, pmic_on, test_ok;
	uint8_t warn1, warn2;
	uint64_t thrott, gcu, intctl, cnta, bott;
	uint32_t interrupts, tests, trips, flag_polled, releases, buck2_ocp;
	uint32_t pmu_status[4];
	uint32_t two_said;                      /* stage two's last reason said (0: none yet) */
	const char *dead_why;
} ocpg;

static inline uint64_t ocpg_ms_to_abs(uint64_t ms)
{
	uint64_t abs;

	nanoseconds_to_absolutetime(ms * NSEC_PER_MSEC, &abs);
	return abs;
}

/* the M3's OCP registers: on an M3 only (an A55 has none), from the guard's thread bound to cpu 4 */
static inline uint64_t ocpg_rd_thrott(void) { uint64_t v; __asm__ volatile ("mrs %0, S3_0_C15_C12_7" : "=r" (v)); return v; }
static inline uint64_t ocpg_rd_gcu(void) { uint64_t v; __asm__ volatile ("mrs %0, S3_1_C15_C11_0" : "=r" (v)); return v; }
static inline uint64_t ocpg_rd_intctl(void) { uint64_t v; __asm__ volatile ("mrs %0, S3_0_C15_C12_5" : "=r" (v)); return v; }
static inline uint64_t ocpg_rd_cnta(void) { uint64_t v; __asm__ volatile ("mrs %0, S3_0_C15_C14_4" : "=r" (v)); return v; }
static inline uint64_t ocpg_rd_bott(void) { uint64_t v; __asm__ volatile ("mrs %0, S3_0_C15_C15_1" : "=r" (v)); return v; }
static inline void ocpg_wr_thrott(uint64_t v) { __asm__ volatile ("msr S3_0_C15_C12_7, %0\n isb" : : "r" (v) : "memory"); }
static inline void ocpg_wr_gcu(uint64_t v) { __asm__ volatile ("msr S3_1_C15_C11_0, %0\n isb" : : "r" (v) : "memory"); }
static inline void ocpg_wr_intctl(uint64_t v) { __asm__ volatile ("msr S3_0_C15_C12_5, %0\n isb" : : "r" (v) : "memory"); }

/* exynos_ocp_irq_work()'s IRP for a rate */
static uint32_t
ocpg_irp(uint32_t khz)
{
	uint32_t irp = (uint32_t)((uint64_t)khz * 2 / OCPG_MIN_KHZ);

	irp = irp >= 2 ? irp - 2 : 0;
	return MIN(irp, (uint32_t)OCPG_IRP_MASK);
}

/* SPI 213, interrupt context on the boot processor: counted, the thread woken; the M3's registers are read on the M3 */
static void
ocpg_interrupt(void *context __unused, uint32_t intid __unused)
{
	__atomic_store_n(&ocpg_spi_last, mach_absolute_time(), __ATOMIC_RELEASE);
	__atomic_fetch_add(&ocpg_spi_count, 1, __ATOMIC_ACQ_REL);
	(void)thread_wakeup_prim((event_t)&ocpg_event, FALSE, THREAD_AWAKENED);
}

/* the governor told to look at the M3s' ceiling now (its wait cut short: gov_thread()) */
static void
ocpg_kick_now(void)
{
	__atomic_store_n(&ocpg_kick, true, __ATOMIC_RELEASE);
	(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
}

/* latched for the boot: stage two never again */
static void
ocpg_die(const char *why)
{
	if (!ocpg.dead) {
		ocpg.dead = true;
		ocpg.dead_why = why;
		say("cpufreq_gov: ocp_guard: DEAD for this boot: %s; the M3s stay at or under %u kHz\n", why, OCPG_WO_KHZ);
	}
	ocpg_kick_now();
}

/* the thread's wait: woken by the interrupt, a rate change's IRP, or the deadline */
static void
ocpg_sleep_ms(uint64_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline((uint32_t)ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline((event_t)&ocpg_event, THREAD_UNINT, deadline);
	(void)thread_block(NULL);
}

/* the GIC raises SPI 213 by software (GICD_ISPENDR): true when the handler counted it within OCPG_TEST_MS */
static bool
ocpg_test_interrupt(void)
{
	uint32_t before = __atomic_load_n(&ocpg_spi_count, __ATOMIC_ACQUIRE);
	uint64_t until = mach_absolute_time() + ocpg_ms_to_abs(OCPG_TEST_MS);

	if (ocpg_gicd == 0 || !ocpg.attached) {
		return false;
	}
	*(volatile uint32_t *)(ocpg_gicd + 0x200 + 4 * (OCPG_INTID / 32)) = 1u << (OCPG_INTID % 32);
	__builtin_arm_dsb(0xf);     /* DSB SY */
	while (__atomic_load_n(&ocpg_spi_count, __ATOMIC_ACQUIRE) == before && mach_absolute_time() < until) {
		ocpg_sleep_ms(2);
	}
	ocpg.tests++;
	return __atomic_load_n(&ocpg_spi_count, __ATOMIC_ACQUIRE) != before;
}

/* the M3s' power, as the PMU says (CLUSTER1_CPUn_STATUS [3:0], 0xF on): read, never written */
static void
ocpg_read_pmu(void)
{
	for (uint32_t n = 0; n < 4 && ocpg_pmu != 0; n++) {
		ocpg.pmu_status[n] = *(volatile uint32_t *)(ocpg_pmu + 0x4 + 8 * n) & 0xf;
	}
}

/*
 * How many M3s are off: taken offline by this kernel (PSCI CPU_OFF) and read off by the PMU. This kernel has no way to take
 * an M3 offline (SweetCPU::haltCPU panics; a parked M3 is only kept idle in WFI, a c2 one powered down until its next
 * interrupt, and Samsung counts both as online), so none is. When the fork can, this is where it says so.
 */
static uint32_t
ocpg_m3s_off(void)
{
	/* (m3off: the cores this kernel took offline and the PMU reads off; none while they are being brought back) */
	return __atomic_load_n(&m3off_returning, __ATOMIC_ACQUIRE) ? 0 : sweet_m3off_count();
}

/* the PMIC's CPU over-current warning, read (never written): on when bit 7 of OCP_WARN1 is set */
static void
ocpg_read_pmic(bool say_it)
{
	uint8_t w1 = 0, w2 = 0;
	int e1 = OCPG_PMIC_READ(OCPG_WARN1, &w1);
	int e2 = OCPG_PMIC_READ(OCPG_WARN2, &w2);
	bool on = e1 == 0 && (w1 & OCPG_WARN_EN) != 0;

	if (say_it || on != ocpg.pmic_on || (e1 == 0 && w1 != ocpg.warn1)) {
		say("cpufreq_gov: ocp_guard: the S2MPS18's OCP_WARN1 (CPU) %#x (%d), OCP_WARN2 (GPU) %#x (%d): the CPU warning %s "
		    "(Samsung's kernel writes 0x80 at every boot; this one only reads)\n", w1, e1, w2, e2, on ? "on" : "NOT on");
	}
	ocpg.warn1 = w1;
	ocpg.warn2 = w2;
	if (ocpg.pmic_on && !on && ocpg_mode >= 3) {
		ocpg_die("the PMIC's CPU over-current warning went off");
	}
	ocpg.pmic_on = on;
	if ((ocpg_flags & 1) != 0) {
		uint8_t int5 = 0;

		if (OCPG_PMIC_READ(OCPG_INT5, &int5) == 0 && (int5 & 0x2) != 0) {
			ocpg.buck2_ocp++;
			say("cpufreq_gov: ocp_guard: the S2MPS18's INT5 %#x: BUCK2 (vdd_cpucl1) reached its current limit (%u so far)\n",
			    int5, ocpg.buck2_ocp);
		}
	}
}

/* Samsung's exynos_ocp_work(), on the M3 (level 3); level 2 only reads the flag */
static void
ocpg_handle(const char *why)
{
	uint64_t gcu, thrott;

	if (ocpg_mode < 3) {
		thrott = ocpg_rd_thrott();
		if ((thrott & OCPG_ERRA) != 0) {
			ocpg.trips++;
			say("cpufreq_gov: ocp_guard: %s: OCPTHROTTCTL %#llx, the throttle flag SET (%u; level 2: left as it is)\n", why,
			    thrott, ocpg.trips);
		}
		return;
	}
	gcu = ocpg_rd_gcu();
	ocpg_wr_gcu(gcu & ~OCPG_ERRAEN);
	thrott = ocpg_rd_thrott();
	if ((thrott & OCPG_ERRA) != 0) {
		uint32_t from = __atomic_load_n(&ocpg_clip, __ATOMIC_ACQUIRE), l;
		struct cluster *b = &clusters[1];

		ocpg_wr_thrott(thrott & ~OCPG_ERRA);
		from = from != 0 ? from : __atomic_load_n(&ocpg_seen_ceiling, __ATOMIC_ACQUIRE);
		l = level_at_most(b, from);
		l = l + 1 < b->n && l < b->lowest ? l + 1 : l;
		__atomic_store_n(&ocpg_clip, b->levels[l], __ATOMIC_RELEASE);
		ocpg_kick_now();
		ocpg.trips++;
		say("cpufreq_gov: ocp_guard: THROTTLE (%s, %u so far): OCPTHROTTCTL %#llx; the M3s' ceiling %u -> %u kHz (rate %u)\n",
		    why, ocpg.trips, thrott, from, b->levels[l], b->cur);
	}
	ocpg_wr_gcu(gcu | OCPG_ERRAEN);
	ocpg.gcu = ocpg_rd_gcu();
	if ((ocpg.gcu & OCPG_ERRAEN) == 0) {
		ocpg_die("GCUCTL's interrupt enable does not read back set");
	}
}

/* the M3's IRP to what the governor is about to ask (level 3), read back */
static void
ocpg_set_irp(void)
{
	uint32_t want = __atomic_load_n(&ocpg_irp_want, __ATOMIC_ACQUIRE);
	uint64_t v;

	if (ocpg_mode < 3 || want == ~0u || want == __atomic_load_n(&ocpg_irp_done, __ATOMIC_ACQUIRE)) {
		return;
	}
	v = ocpg_rd_intctl();
	ocpg_wr_intctl((v & ~(OCPG_IRP_MASK << OCPG_IRP_SHIFT)) | ((uint64_t)want << OCPG_IRP_SHIFT));
	ocpg.intctl = ocpg_rd_intctl();
	if (((ocpg.intctl >> OCPG_IRP_SHIFT) & OCPG_IRP_MASK) == want) {
		__atomic_store_n(&ocpg_irp_done, want, __ATOMIC_RELEASE);
	} else {
		ocpg_die("OCPINTCTL's IRP does not read back");
	}
}

/* the guard's thread: bound to the first M3 for its life, as Samsung's driver does its work on data->cpu */
__attribute__((noreturn)) static void
ocpg_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t midr, last_poll = 0, last_pmic = 0, last_test = 0, trip_at = 0;
	uint32_t handled = 0;

	/* the first M3 the scheduler has (one that has been idle: the governor's measure() says so), ten seconds at most */
	for (int tries = 0; tries < 100 && ocpg_cpu < 0; tries++) {
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			if (OCPG_CPU_CLUSTER(&topology->cpus[i]) == clusters[1].affinity && ever_idle_cpu(i)) {
				ocpg_cpu = (int)topology->cpus[i].cpu_id;
				break;
			}
		}
		if (ocpg_cpu < 0) {
			ocpg_sleep_ms(100);
		}
	}
	if (ocpg_mode >= 2 && (ocpg_cpu < 0 || cpu_to_processor(ocpg_cpu) == NULL)) {
		ocpg_die("no M3 to run on");
		ocpg_mode = 1;
	}
	if (ocpg_mode >= 2) {
		(void)thread_bind(cpu_to_processor(ocpg_cpu));
		(void)thread_block(NULL);
		midr = __builtin_arm_rsr64("MIDR_EL1");
		if (((midr >> 24) & 0xff) != 0x53) {
			say("cpufreq_gov: ocp_guard: cpu %d is no M3 (MIDR %#llx): its registers left alone\n", ocpg_cpu, midr);
			ocpg_die("not on an M3");
			ocpg_mode = 1;
		}
	}
	if (ocpg_mode >= 2) {
		say("cpufreq_gov: ocp_guard: reading the OCP registers on cpu %d now (no \"read\" line after this one: the read "
		    "did not come back)\n", ocpg_cpu);
		ocpg.thrott = ocpg_rd_thrott();
		ocpg.gcu = ocpg_rd_gcu();
		ocpg.intctl = ocpg_rd_intctl();
		ocpg.cnta = ocpg_rd_cnta();
		ocpg.bott = ocpg_rd_bott();
		ocpg.regs_read = true;
		say("cpufreq_gov: ocp_guard: read: OCPTHROTTCTL %#llx (flag %u), GCUCTL %#llx (interrupt %s), OCPINTCTL %#llx (IRP %llu), "
		    "OCPTHROTTCNTA %#llx, OCPTIDEPBOTTHRESH %#llx\n", ocpg.thrott, (ocpg.thrott & OCPG_ERRA) != 0, ocpg.gcu,
		    (ocpg.gcu & OCPG_ERRAEN) ? "on" : "OFF", ocpg.intctl, (ocpg.intctl >> OCPG_IRP_SHIFT) & OCPG_IRP_MASK, ocpg.cnta,
		    ocpg.bott);
	}
	if (ocpg_mode >= 3) {
		if ((ocpg.gcu & OCPG_ERRAEN) == 0) {
			/* Samsung's driver never turns it on at its start: the firmware has; when it has not, nothing here does */
			ocpg_die("GCUCTL's interrupt enable is off as the firmware left it");
		} else if ((ocpg.thrott & OCPG_ERRA) != 0) {
			ocpg_wr_thrott(ocpg.thrott & ~OCPG_ERRA);
			say("cpufreq_gov: ocp_guard: the throttle flag was set before arming (from before this kernel): cleared\n");
		}
		__atomic_store_n(&ocpg_irp_want, ocpg_irp(clusters[1].cur), __ATOMIC_RELEASE);
		ocpg_set_irp();
	}
	ocpg.test_ok = ocpg_test_interrupt();
	say("cpufreq_gov: ocp_guard: SPI %u raised by the GIC by software: %s\n", OCPG_INTID,
	    ocpg.test_ok ? "the handler ran" : "the handler did NOT run");
	if (!ocpg.test_ok) {
		ocpg_die("the interrupt path's test failed");
	}
	ocpg.armed = ocpg_mode >= 3 && !ocpg.dead && ocpg.regs_read && ocpg.pmic_on;
	say("cpufreq_gov: ocp_guard: level %u: %s\n", ocpg_mode, ocpg.armed ? "ARMED as Samsung's driver runs it (the ceiling stays "
	    "1794000 below level 4)" : ocpg_mode >= 3 ? "NOT armed" : "observing only");
	last_test = mach_absolute_time();
	for (;;) {
		uint64_t now = mach_absolute_time();
		uint32_t count = __atomic_load_n(&ocpg_spi_count, __ATOMIC_ACQUIRE);

		__atomic_store_n(&ocpg_beat, now, __ATOMIC_RELEASE);
		if (ocpg_mode >= 2) {
			ocpg_set_irp();
			if (count != handled) {
				handled = count;
				ocpg.interrupts = count;
				ocpg_handle("interrupt");
				if (__atomic_load_n(&ocpg_clip, __ATOMIC_ACQUIRE) != 0) {
					trip_at = now;
				}
			}
			if (now - last_poll >= ocpg_ms_to_abs(1000)) {
				uint64_t thrott = ocpg_rd_thrott();

				last_poll = now;
				ocpg.cnta = ocpg_rd_cnta();
				if ((thrott & OCPG_ERRA) != 0 &&
				    now - __atomic_load_n(&ocpg_spi_last, __ATOMIC_ACQUIRE) > ocpg_ms_to_abs(OCPG_LATE_MS)) {
					ocpg.flag_polled++;
					if (ocpg_mode >= 3) {
						ocpg_die("the throttle flag set with no interrupt behind it");
					}
					ocpg_handle("polled");
					trip_at = now;
				}
				if (ocpg_mode >= 3 && (ocpg_rd_gcu() & OCPG_ERRAEN) == 0) {
					ocpg_die("GCUCTL's interrupt enable found off");
				}
			}
			if (ocpg_mode >= 3 && __atomic_load_n(&ocpg_clip, __ATOMIC_ACQUIRE) != 0 &&
			    now - trip_at >= ocpg_ms_to_abs(OCPG_RELEASE_MS)) {
				/* exynos_ocp_work_release(), CPU_UTIL_MODE: the M3s' summed load under half of the four */
				if (clusters[1].sum_now < OCPG_RELEASE_SUM) {
					__atomic_store_n(&ocpg_clip, 0, __ATOMIC_RELEASE);
					ocpg_kick_now();
					ocpg.releases++;
				} else {
					trip_at = now;
				}
			}
		}
		if (now - last_pmic >= ocpg_ms_to_abs(10000)) {
			last_pmic = now;
			ocpg_read_pmic(false);
			ocpg_read_pmu();
		}
		if (now - last_test >= ocpg_ms_to_abs(60000)) {
			last_test = now;
			if (!ocpg_test_interrupt()) {
				ocpg_die("the interrupt path's test failed");
			}
		}
		ocpg_sleep_ms(__atomic_load_n(&ocpg_clip, __ATOMIC_ACQUIRE) != 0 ? OCPG_RELEASE_MS : 250);
	}
}

/* the guard alive and armed: what stage two stands on */
static bool
ocpg_alive(void)
{
	uint64_t beat = __atomic_load_n(&ocpg_beat, __ATOMIC_ACQUIRE);

	return ocpg.armed && !ocpg.dead && ocpg.pmic_on && beat != 0 && mach_absolute_time() - beat < ocpg_ms_to_abs(1000);
}

/* level 4's rule (hakios.exynos_cpufreq_ceiling.h) as it stands now; stage two's decision said once a change */
static enum ceil_why
ocpg_top(struct cluster *c)
{
	struct ceil_in in = { .guard = ocpg_mode, .alive = ocpg_alive(), .temp_fresh = temp_fresh(c),
		              .big_mc = hakios_exynos_tmu_zone_mc(c->tmu_zone), .m3s_off = ocpg_m3s_off(),
		              .m3off = __atomic_load_n(&sweet_m3off_mode, __ATOMIC_ACQUIRE),
		              .pmic_on = ocpg_mode >= 1 && ocpg.pmic_on };
	enum ceil_why why = ceil_why(&in);

	if (why != ocpg.two_said && (ocpg_mode >= 4 || in.m3off >= 2)) {
		ocpg.two_said = why;
		say("cpufreq_gov: ocp_guard: stage two: %s (%u M3s on, big %d mC)\n", ceil_reason(why), ceil_m3s_on(&in), in.big_mc);
	}
	return why;
}

/* every option's ceiling for the M3s through the one rule: 1794000 unless level 4's holds; said once when it holds them */
static uint32_t
big_held(struct cluster *c, uint32_t max)
{
	static bool said;
	uint32_t held = ceil_big(max, ocpg_top(c));

	if (held == CEIL_HELD_KHZ && max > held && !said) {
		said = true;
		say("cpufreq_gov: big: %s asks up to %u kHz; held at 1794 MHz: needs ocp_guard=4 (docs/ios27/OCP.md)\n",
		    gov_policy == 3 ? "gov_policy=3" : "cpufreq_gov_big_max=", max);
	}
	return held;
}

/* ceiling_now()'s last step for the M3s: at most 1794000 unless level 4's rule holds (big_held()); the clip after a throttle */
static uint32_t
ocpg_ceiling(struct cluster *c, uint32_t max)
{
	uint32_t clip = __atomic_load_n(&ocpg_clip, __ATOMIC_ACQUIRE);

	max = big_held(c, max);
	__atomic_store_n(&ocpg_seen_ceiling, max, __ATOMIC_RELEASE);
	return clip != 0 ? MIN(max, clip) : max;
}

/*
 * Before the M3s go to khz (set_rate): the IRP for it asked of the guard's thread (exynos-ocp's PRECHANGE notifier, which
 * queues it to cpu 4 the same way). Above 1794000 the change waits for the IRP to read back, at most 5 ms; without it the
 * M3s go no higher than 1794000.
 */
static uint32_t
ocpg_prechange(struct cluster *c, uint32_t khz)
{
	uint32_t want;

	if (ocpg_mode < 3) {
		return khz;
	}
	want = ocpg_irp(khz);
	__atomic_store_n(&ocpg_irp_want, want, __ATOMIC_RELEASE);
	(void)thread_wakeup_prim((event_t)&ocpg_event, FALSE, THREAD_AWAKENED);
	if (khz <= OCPG_WO_KHZ) {
		return khz;
	}
	for (int i = 0; i < 50 && __atomic_load_n(&ocpg_irp_done, __ATOMIC_ACQUIRE) != want; i++) {
		delay(100);
	}
	if (__atomic_load_n(&ocpg_irp_done, __ATOMIC_ACQUIRE) != want || !ocpg_alive()) {
		return c->levels[level_at_most(c, OCPG_WO_KHZ)];
	}
	return khz;
}

/* a throttle, a release or a death changes the ceiling now: the governor brings the M3s under it at once (gov_thread()) */
static bool
ocpg_take_kick(void)
{
	return ocpg_mode != 0 && __atomic_exchange_n(&ocpg_kick, false, __ATOMIC_ACQ_REL);
}

/* a line every ten seconds with the guard on */
static void
ocpg_line(uint64_t seconds)
{
	static uint64_t last;

	if (ocpg_mode == 0 || (seconds - last < 10 && last != 0)) {
		return;
	}
	last = seconds;
	say("cpufreq_gov: ocp_guard: %llu s: level %u %s | interrupts %u (tests %u) | throttles %u, polled %u, released %u | clip %u | "
	    "IRP %u | PMIC warning %s | M3 power %x %x %x %x | buck2 limit %u%s%s\n", seconds, ocpg_mode,
	    ocpg.dead ? "DEAD" : ocpg.armed ? "armed" : "not armed", __atomic_load_n(&ocpg_spi_count, __ATOMIC_ACQUIRE), ocpg.tests,
	    ocpg.trips, ocpg.flag_polled, ocpg.releases, __atomic_load_n(&ocpg_clip, __ATOMIC_ACQUIRE),
	    __atomic_load_n(&ocpg_irp_done, __ATOMIC_ACQUIRE), ocpg.pmic_on ? "on" : "OFF", ocpg.pmu_status[0], ocpg.pmu_status[1],
	    ocpg.pmu_status[2], ocpg.pmu_status[3], ocpg.buck2_ocp, ocpg.dead ? ": " : "", ocpg.dead ? ocpg.dead_why : "");
}

/* from gov_setup(), once the clusters are known: the mappings, the PMIC read, the interrupt, the thread */
static void
ocpg_start(void)
{
	thread_t thread;
	kern_return_t kr;

	if (ocpg_mode == 0) {
		return;
	}
	ocpg_gicd = ml_io_map(OCPG_GICD_PA, 0x1000);
	ocpg_pmu = ml_io_map(OCPG_PMU_CL1_PA, 0x1000);
	ocpg_read_pmic(true);
	ocpg_read_pmu();
	kr = OCPG_SPI_ATTACH(OCPG_INTID, true, ocpg_interrupt, NULL);
	ocpg.attached = kr == KERN_SUCCESS;
	say("cpufreq_gov: ocp_guard=%u: SPI %u (exynos-ocp's 181), rising edge, on the boot processor: %s (%d); the M3s' "
	    "power %x %x %x %x (0xf on); the M3s held at or under %u kHz%s\n", ocpg_mode, OCPG_INTID,
	    ocpg.attached ? "attached" : "NOT attached", kr, ocpg.pmu_status[0], ocpg.pmu_status[1], ocpg.pmu_status[2],
	    ocpg.pmu_status[3], OCPG_WO_KHZ, ocpg_mode >= 4 ? " until stage two's rule holds" : "");
	if (!ocpg.attached) {
		ocpg_die("SPI 213 not attached");
	}
	if (kernel_thread_start(ocpg_thread, NULL, &thread) != KERN_SUCCESS) {
		ocpg_die("no thread");
		return;
	}
	thread_deallocate(thread);
}

static uint32_t
ceiling_now(struct cluster *c)
{
	uint32_t max = c->ceiling;

	if (gov_policy != 1) {
		if (!temp_fresh(c)) {
			max = MIN(max, c->scion_safe);
		} else if (c == &clusters[1] && gov_policy == 3) {
			max = scion_limit(&scion_soc, big_cores);
		}
	} else if (c == &clusters[1]) {
		max = MIN(max, big_online_limit(c->online));
		if (c->temp_mc == NOT_READ_MC) {
			max = MIN(max, NO_TEMP_BIG_MAX);
		}
	}
	if (c->thermal > 0) {
		max = MIN(max, c->levels[MIN(c->thermal, c->n - 1)]);
	}
	if (c == &clusters[1]) {
		max = ocpg_ceiling(c, max);  /* (every option: 1794000 unless ocp_guard=4's rule holds; a throttle's clip) */
	}
	return c->levels[level_at_most(c, max)];
}

/* scion: a finger holds the touch floor: an event just now, or one down and not silent for too long */
static bool
touch_held(uint64_t now)
{
	uint64_t last = __atomic_load_n(&touch_last, __ATOMIC_ACQUIRE);

	if (boost_on(last, boost_span, now)) {
		return true;
	}
	return last != 0 && boost_span != 0 && __atomic_load_n(&fingers, __ATOMIC_ACQUIRE) != 0 && now - last <= hold_span;
}

/* scion: frames that are moving something. An idle screen presents ten a second all day (fb_tcoal's comment in the framebuffer). */
static bool
frames_moving(uint64_t now)
{
	return boost_on(__atomic_load_n(&frame_last, __ATOMIC_ACQUIRE), frame_span, now) &&
	       __atomic_load_n(&frame_gap, __ATOMIC_RELAXED) <= moving_span;
}

/* scion: a boost holds: the shorter window */
static bool
interactive(uint64_t now)
{
	return touch_held(now) || frames_moving(now);
}

/* this file's thread only: time spent in each state, for the stat line */
static void
dev_state_update(uint64_t now)
{
	uint64_t last = __atomic_load_n(&frame_last, __ATOMIC_ACQUIRE), touch = __atomic_load_n(&touch_last, __ATOMIC_ACQUIRE), ns;
	enum scion_state to;

	absolutetime_to_nanoseconds(now - MAX(last, touch), &ns);
	to = scion_state(__atomic_load_n(&fingers, __ATOMIC_ACQUIRE), frames_moving(now), ns / NSEC_PER_MSEC, dev_state);
	if (dev_since != 0) {
		absolutetime_to_nanoseconds(now - dev_since, &ns);
		dev_ms[dev_state] += ns / NSEC_PER_MSEC;
	}
	dev_since = now;
	dev_state = to;
}

/* the window now, in mach_absolute_time units */
static uint64_t
window_span(uint64_t now)
{
	uint64_t span;

	nanoseconds_to_absolutetime((uint64_t)(gov_policy != 1 && interactive(now) ? BOOST_WINDOW_MS : window_ms) * NSEC_PER_MSEC,
	    &span);
	return span;
}

/* the floor the boosts on now give the cluster, a level, clamped by the ceiling now (0: no boost on) */
static uint32_t
boost_floor(struct cluster *c, uint64_t now)
{
	uint32_t khz = 0;

	if (gov_policy != 1) {
		if (c->burst && boost_on(__atomic_load_n(&finger_edge, __ATOMIC_ACQUIRE), burst_span, now)) {
			return ceiling_now(c);
		}
		if (touch_held(now)) {
			khz = c == &clusters[1] && gov_policy == 4 && pace.floor != 0 ? pace.floor : c->scion_touch;
		}
		if (frames_moving(now)) {
			khz = MAX(khz, c->frame_khz);
		}
		return khz == 0 ? 0 : MIN(c->levels[level_at_least(c, khz)], ceiling_now(c));
	}

	if (boost_on(__atomic_load_n(&touch_last, __ATOMIC_ACQUIRE), boost_span, now)) {
		khz = MAX(khz, c->touch_khz);
	}
	if (boost_on(__atomic_load_n(&frame_last, __ATOMIC_ACQUIRE), frame_span, now)) {
		khz = MAX(khz, c->frame_khz);
	}
	if (khz == 0) {
		return 0;
	}
	return MIN(c->levels[level_at_least(c, khz)], ceiling_now(c));
}

static void set_rate(struct cluster *c, uint32_t khz, uint32_t max);

/* a boost has raised the M3s: how long after the gesture's first event, the first time for that gesture */
static void
ramp_raise(uint64_t now)
{
	uint64_t began = __atomic_load_n(&ramp_began, __ATOMIC_ACQUIRE);

	if (began != 0 && now >= began && !ramp_raised) {
		ramp_raised = true;
		nums.raise_n++;
		nums.raise_sum += now - began;
		nums.raise_max = MAX(nums.raise_max, now - began);
	}
}

/* the clusters under their boost floor raised to it now (between windows; nothing measured) */
static void
boost(uint64_t now)
{
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];
		uint32_t floor = boost_floor(c, now);

		c->boost_floor = floor;
		if (c->cur != 0 && c->online != 0 && floor > c->cur) {
			set_rate(c, floor, ceiling_now(c));
			c->low_windows = 0;
			c->rate = (struct scion_rate){ 0 };
			if (i == 1) {
				ramp_raise(mach_absolute_time());
			}
			__atomic_fetch_add(&boost_raises, 1, __ATOMIC_RELAXED);
		}
	}
}

/* a boost wanted that is not applied yet: the thread does not wait out its window then */
static bool
boost_due(uint64_t now)
{
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];

		if (c->cur != 0 && c->online != 0 && boost_floor(c, now) > c->cur) {
			return true;
		}
	}
	return false;
}

/*
 * One DVFS request, as hakios_acpm_dvfs_set() sends it (domain, kHz, FREQ_REQ 0, 0), with the APM's answer kept: its four
 * words come back in place of the request's.
 */
static int
dvfs_request(uint32_t index, uint32_t khz, uint32_t answer[4])
{
	answer[0] = index;
	answer[1] = khz;
	answer[2] = 0;
	answer[3] = 0;
	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, answer);
}

/* the rate the APM says (FREQ_GET); 0 is "not said": the rate asked for stands */
static void
read_back(struct cluster *c, uint32_t asked)
{
	uint32_t got = 0;
	int e;

	/* the APM has said it does not say (every run so far): no second request for each change */
	if (gov_policy != 1 && c->asked) {
		c->cur = asked;
		return;
	}
	e = hakios_acpm_dvfs_get(c->index, &got);
	if (e == 0 && got != 0) {
		c->cur = got;
		c->asked = false;
		return;
	}
	if (!c->asked) {
		say("cpufreq_gov: %s: the APM answers FREQ_GET with %u kHz (%d): the rate asked for, %u kHz, is taken as the "
		    "rate (and measured once a log line)\n", c->name, got, e, asked);
	}
	c->cur = asked;
	c->asked = true;
}

/* the time since the last call counted to the bucket of the rate it was spent at */
static void
residency(struct cluster *c, uint64_t now)
{
	uint32_t b = 0;

	while (b < RES_BUCKETS - 1 && c->cur >= c->res_khz[b]) {
		b++;
	}
	if (c->rate_since != 0 && now > c->rate_since) {
		c->res[b] += now - c->rate_since;
	}
	c->rate_since = now;
}

static void
set_rate(struct cluster *c, uint32_t khz, uint32_t max)
{
	uint32_t answer[4];
	uint64_t began, took, bounce;
	int e;

	if (c == &clusters[1]) {
		khz = big_held(c, khz); /* (as ceiling_now(): the boot's first rate and every other request too) */
	}
	if (c == &clusters[1] && ocpg_mode != 0) {
		khz = ocpg_prechange(c, khz);   /* (ocp_guard=3 and up: the IRP first, as exynos-ocp's PRECHANGE notifier) */
	}
	began = mach_absolute_time();
	e = dvfs_request(c->index, khz, answer);

	residency(c, began);
	nanoseconds_to_absolutetime((uint64_t)BOUNCE_MS * NSEC_PER_MSEC, &bounce);
	if (e == 0 && khz > c->cur && c->last_down != 0 && began - c->last_down <= bounce) {
		c->reversals++;
	} else if (e == 0 && khz < c->cur) {
		c->last_down = began;
	}
	if (e != 0) {
		if (c->errors++ < 5) {
			say("cpufreq_gov: %s: asked %u kHz: the APM did not answer (%d); the rate stays %u kHz\n", c->name, khz, e,
			    c->cur);
		}
		return;
	}
	if (c->changes < ANSWERS_LOGGED) {
		say("cpufreq_gov: %s: %u -> %u kHz asked; the APM answered %08x %08x %08x %08x\n", c->name, c->cur, khz,
		    answer[0], answer[1], answer[2], answer[3]);
	}
	c->changes++;
	read_back(c, khz);
	if (!c->asked && c->cur > max) {
		uint32_t lower = level_at_most(c, khz);

		lower = lower + 1 < c->n ? lower + 1 : lower;
		say("cpufreq_gov: %s: asked %u kHz, the APM says %u, above %u: asking %u\n", c->name, khz, c->cur, max,
		    c->levels[lower]);
		if (dvfs_request(c->index, c->levels[lower], answer) == 0) {
			read_back(c, c->levels[lower]);
		}
	}
	took = mach_absolute_time() - began;
	nums.acpm_n++;
	nums.acpm_sum += took;
	nums.acpm_max = MAX(nums.acpm_max, took);
}

/*
 * The rate a core of the cluster really runs at, in MHz: this thread moved to the first of its cores the scheduler has
 * (one that has been idle: a core not yet the kernel's has not), and a chain of 32 dependent additions run SPIN_LOOPS
 * times with interrupts off -- one addition a cycle on both the A55 and the M3, the loop's own subtraction and branch
 * beside them -- timed with mach_absolute_time. 0 when there is no such core. Back on any core afterwards.
 */
static bool ever_idle_cpu(unsigned int i);

static uint32_t
measure_mhz(const struct cluster *c)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t loops = SPIN_LOOPS, x = 0, began, took, ns;
	boolean_t enabled;
	int cpu = -1;

	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		if (cpu_affinity(&topology->cpus[i]) == c->affinity && ever_idle_cpu(i) && !sweet_m3off_is_off(i)) {
			cpu = (int)topology->cpus[i].cpu_id;
			break;
		}
	}
	if (cpu < 0 || cpu_to_processor(cpu) == NULL) {
		return 0;
	}
	(void)thread_bind(cpu_to_processor(cpu));
	(void)thread_block(NULL);             /* over to it now */
	enabled = ml_set_interrupts_enabled(FALSE);
	began = mach_absolute_time();
	__asm__ volatile (
                "1:\n"
                ".rept 32\n"
                "add %0, %0, #1\n"
                ".endr\n"
                "subs %1, %1, #1\n"
                "b.ne 1b\n"
                : "+r" (x), "+r" (loops) : : "cc");
	took = mach_absolute_time() - began;
	(void)ml_set_interrupts_enabled(enabled);
	(void)thread_bind(NULL);
	absolutetime_to_nanoseconds(took, &ns);
	return ns != 0 && x == 32ull * SPIN_LOOPS ? (uint32_t)(32ull * SPIN_LOOPS * 1000 / ns) : 0;
}

/* the cluster's busiest core's load over the window, and how many of its cores run */
static void
measure(uint64_t span)
{
	const ml_topology_info_t *topology = ml_get_topology_info();

	clusters[0].load = clusters[1].load = 0;
	clusters[0].online = clusters[1].online = 0;
	clusters[0].sum_now = clusters[1].sum_now = 0;
	memset(clusters[1].core_load, 0, sizeof(clusters[1].core_load));
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		uint32_t aff = cpu_affinity(&topology->cpus[i]);
		uint64_t idle = 0, busy;
		struct cluster *c;

		if (sweet_m3off_is_off(i)) {
			idle_rebase[i] = true;  /* (offline, or on its way: neither measured nor counted) */
			continue;
		}
		if (!ml_cpu_idle_time((int)i, &idle)) {
			continue;
		}
		if (idle_rebase[i]) {
			idle_rebase[i] = false;
			idle_before[i] = idle;
			continue;
		}
		busy = span > idle - idle_before[i] ? span - (idle - idle_before[i]) : 0;
		idle_before[i] = idle;
		if (idle != 0) {
			ever_idle[i] = true;
		}
		c = aff == clusters[1].affinity ? &clusters[1] : (aff == clusters[0].affinity ? &clusters[0] : NULL);
		if (c == NULL || !ever_idle[i]) {
			continue;
		}
		c->online++;
		c->load = MAX(c->load, (uint32_t)(100 * busy / span));
		c->sum_now += (uint32_t)(100 * busy / span);
		if (c == &clusters[1]) {
			uint32_t load = (uint32_t)(100 * busy / span), at = MIN(c->online - 1, SCION_BIG_CORES - 1);

			for (; at > 0 && c->core_load[at - 1] < load; at--) {
				c->core_load[at] = c->core_load[at - 1];
			}
			c->core_load[at] = load;
		}
	}
}

/*
 * This many M3s for the scheduler. To fewer: the rate is raised only afterwards. To more: the rate first, down to what that many
 * may run at, and no core comes back while the APM has not taken it. The last M3s of the topology are the ones parked.
 */
static void
cores_set(uint32_t cores, uint64_t now)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	struct cluster *b = &clusters[1];
	uint32_t mask = ~0u, m3 = 0;

	if (cores == big_cores) {
		return;
	}
	if (cores > big_cores && b->cur > scion_limit(&scion_soc, cores)) {
		set_rate(b, scion_limit(&scion_soc, cores), scion_limit(&scion_soc, cores));
		if (b->cur > scion_limit(&scion_soc, cores)) {
			return;
		}
	}
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		if (cpu_affinity(&topology->cpus[i]) == b->affinity && ++m3 > cores) {
			mask &= ~(1u << topology->cpus[i].cpu_id);
		}
	}
	sched_perfcontrol_update_recommended_cores(mask);
	if (cores < big_cores) {
		few_since = now;
	} else {
		few_time += now - few_since;
	}
	big_cores = cores;
}

/* scion's window for a cluster: the M3s' count, then the rate, under the ceiling that count allows */
static void
decide_scion(struct cluster *c, uint64_t now, uint32_t window)
{
	uint32_t max, target;

	if (c == &clusters[1]) {
		int32_t mc = hakios_exynos_tmu_zone_now_mc(c->tmu_zone);

		dev_state_update(now);
		cores_set(gov_policy != 3 || !temp_fresh(c) || !scion_temp_ok(mc, 0) || c->thermal != 0 ? SCION_BIG_CORES :
		    scion_cores(&park, &scion_soc, big_cores, c->core_load, clusters[0].load, c->cur, mc, window), now);
		if (gov_policy == 4) {
			uint32_t late = __atomic_exchange_n(&frames_late, 0, __ATOMIC_RELAXED);
			uint32_t on_time = __atomic_exchange_n(&frames_on_time, 0, __ATOMIC_RELAXED);

			if (touch_held(now)) {
				(void)scion_pace(&pace, c->levels, c->n, late, on_time, scion_soc.big_frame, scion_soc.big_touch, window);
			} else {
				pace = (struct scion_pace){ 0 };
			}
		}
	}
	max = ceiling_now(c);
	target = scion_rate(&c->rate, c->levels, c->n, c->cur, c->load, MAX(c->floor, c->boost_floor), max, window);
	if (target != c->cur) {
		set_rate(c, target, max);
	}
}

/*
 * (Starbite) m3off: the policy (docs/ios27/BIG-CORES.md). Decided on this file's thread every window, done on a thread of
 * its own (taking a core offline blocks for milliseconds and takes the scheduler's locks): m3off_thread().
 *
 *   to two M3s (cores 6 and 7 offline): m3off=2 (or 1: logged only), policy 1, no stop met, up 120 s or more, a fresh big-zone
 *       temperature under 70 C, no thermal step, the PMIC's CPU warning read on (ocp_guard=1), the interface moving (a finger
 *       or frames under 50 ms apart), and for three windows in a row the M3s' load fitting on two: the third-busiest core at
 *       30% or less and the four's busy shares adding up to 150% or less; at least a second after the last change.
 *   back to four: the interface still for a second, or the two busy shares adding up to 180% or more for five windows, or no
 *       longer eligible; at least half a second after the last change. The rate first: m3off_returning holds the M3s at
 *       1794000 and the cores are started only once the governor has decided a window with it and the rate is under it
 *       (Samsung's exynos_hpgov_validate_hpin: no core online while the rate is above what one more allows).
 *   stop, for the rest of the boot, all four back: the big zone at 75 C (CEIL_M3OFF_STOP_MC; Samsung cools from 83), a core
 *       XNU will not take offline or start (sweet_m3off.c panics itself when the hardware does not follow), the rate not
 *       coming under 1794000 for the return within 2 s.
 * Above 1794000 only by the rule (ocpg_top()): two M3s on, PMU-checked; the load law of policy 1 picks the rate up to it.
 */
static uint32_t m3off_want;             /* M3s the policy wants offline: 0, or 2 (cores 6 and 7) */
static uint32_t m3off_dry_off;          /* m3off=1: what would be offline now */
static bool m3off_stopped;
static char m3off_stop_why[120];
static uint32_t m3off_floor;            /* a test's floor for the M3s, kHz (0: none) */
static uint32_t m3off_test;             /* kern.hakios.m3off_test: the test running (0: none) */
static uint64_t m3off_changed, m3off_ui_last;
static uint32_t m3off_fit, m3off_over;
static int m3off_event;
static bool m3off_started;
static uint32_t m3off_auto = 1;         /* m3off_auto=0: the policy off, only kern.hakios.m3off_test moves cores */
static uint32_t m3off_n_off, m3off_n_on, m3off_secs_two;
static uint64_t m3off_two_since;

static uint64_t
m3off_ms_abs(uint32_t ms)
{
	uint64_t abs;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &abs);
	return abs;
}

static void
m3off_stop(const char *why)
{
	if (!__atomic_exchange_n(&m3off_stopped, true, __ATOMIC_ACQ_REL)) {
		strlcpy(m3off_stop_why, why, sizeof(m3off_stop_why));
		say("cpufreq_gov: m3off: STOP: %s: all four M3s back and at 1794 MHz at most for the rest of this boot\n", why);
	}
	__atomic_store_n(&m3off_want, 0, __ATOMIC_RELEASE);
	(void)thread_wakeup_prim((event_t)&m3off_event, FALSE, THREAD_AWAKENED);
}

/* the governor's thread, every window of the M3s (policy 1) */
static void
m3off_decide(struct cluster *c, uint64_t now)
{
	uint32_t mode = __atomic_load_n(&sweet_m3off_mode, __ATOMIC_ACQUIRE), want = __atomic_load_n(&m3off_want, __ATOMIC_ACQUIRE);
	uint32_t off = mode >= 2 ? sweet_m3off_count() : m3off_dry_off;
	int32_t mc = hakios_exynos_tmu_zone_mc(c->tmu_zone);
	bool fresh = temp_fresh(c), eligible, quiet;
	uint64_t ns;

	if (mode == 0 || !m3off_started || __atomic_load_n(&m3off_test, __ATOMIC_ACQUIRE) != 0) {
		return;
	}
	if (m3off_auto == 0) {
		if (fresh && mc >= CEIL_M3OFF_STOP_MC && !m3off_stopped) {
			m3off_stop("the big zone at 75 C");
		}
		return;
	}
	if (fresh && mc >= CEIL_M3OFF_STOP_MC && !m3off_stopped) {
		char why[80];

		snprintf(why, sizeof(why), "the big zone at %d mC (this path stops at %d)", mc, CEIL_M3OFF_STOP_MC);
		m3off_stop(why);
	}
	if (m3off_stopped) {
		return;
	}
	if (touch_held(now) || frames_moving(now)) {
		m3off_ui_last = now;
	}
	quiet = m3off_ui_last == 0 || now - m3off_ui_last > m3off_ms_abs(1000);
	absolutetime_to_nanoseconds(now, &ns);
	eligible = ns >= 120ull * NSEC_PER_SEC && fresh && mc < CEIL_M3OFF_STOP_MC - 5000 && c->thermal == 0 && ocpg_mode >= 1 &&
	    ocpg.pmic_on;
	if (off == 0 && want == 0) {
		m3off_fit = c->online >= 3 && c->core_load[2] <= 30 && c->sum_now <= 150 ? m3off_fit + 1 : 0;
		if (eligible && !quiet && m3off_fit >= 3 && now - m3off_changed >= m3off_ms_abs(1000)) {
			__atomic_store_n(&m3off_want, 2, __ATOMIC_RELEASE);
			m3off_changed = now;
			m3off_over = 0;
			(void)thread_wakeup_prim((event_t)&m3off_event, FALSE, THREAD_AWAKENED);
		}
	} else if (want != 0) {
		m3off_over = c->sum_now >= 180 ? m3off_over + 1 : 0;
		if ((!eligible || quiet || m3off_over >= 5) && now - m3off_changed >= m3off_ms_abs(500)) {
			__atomic_store_n(&m3off_want, 0, __ATOMIC_RELEASE);
			m3off_changed = now;
			m3off_fit = 0;
			(void)thread_wakeup_prim((event_t)&m3off_event, FALSE, THREAD_AWAKENED);
		}
	}
}

/* the M3s under 1794000 before any core comes back: the governor told, a window decided with it, the rate read */
static bool
m3off_rate_down(void)
{
	uint32_t gen;

	__atomic_store_n(&m3off_returning, true, __ATOMIC_RELEASE);
	gen = __atomic_add_fetch(&m3off_returning_gen, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&ocpg_kick, true, __ATOMIC_RELEASE);
	(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
	for (int i = 0; i < 200; i++) {
		if (__atomic_load_n(&m3off_seen_gen, __ATOMIC_ACQUIRE) == gen && clusters[1].cur <= CEIL_HELD_KHZ) {
			return true;
		}
		delay(10 * 1000);
	}
	return false;
}

static int
m3off_take_two(void)
{
	int kr;

	if (__atomic_load_n(&sweet_m3off_mode, __ATOMIC_ACQUIRE) < 2) {
		m3off_dry_off = 2;
		return 0;
	}
	if ((kr = sweet_m3off_take(7)) != 0 || (kr = sweet_m3off_take(6)) != 0) {
		return kr;
	}
	m3off_n_off++;
	return 0;
}

static int
m3off_give_all(void)
{
	int kr = 0;

	if (__atomic_load_n(&sweet_m3off_mode, __ATOMIC_ACQUIRE) < 2) {
		m3off_dry_off = 0;
		return 0;
	}
	if (sweet_m3off_count() == 0 && !sweet_m3off_is_off(6) && !sweet_m3off_is_off(7)) {
		__atomic_store_n(&m3off_returning, false, __ATOMIC_RELEASE);
		return 0;
	}
	if (!m3off_rate_down()) {
		say("cpufreq_gov: m3off: the M3s not under 1794 MHz 2 s after the return was asked (%u kHz): no core started\n",
		    clusters[1].cur);
		return -1;
	}
	if ((kr = sweet_m3off_give(6)) == 0) {
		kr = sweet_m3off_give(7);
	}
	if (kr == 0) {
		m3off_n_on++;
		__atomic_store_n(&m3off_returning, false, __ATOMIC_RELEASE);
	}
	return kr;
}

static void
m3off_sleep_ms(uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline((event_t)&m3off_sleep_ms, THREAD_UNINT, deadline);
	(void)thread_block(NULL);
}

/*
 * kern.hakios.m3off_test (m3off=2 only; the policy waits while one runs; each step said):
 *   1 cpu 7 offline 5 s, back | 2 cpus 7 and 6 offline 5 s, back | 3 as 2 with the M3s at 1924 MHz for the 5 s | 4 the same at
 *   2314 MHz | 10+N test 1 N times | 20+N test 2 N times (N 1 to 9), a second apart
 */
static void
m3off_run_test(uint32_t t)
{
	uint32_t times = t > 20 ? t - 20 : t > 10 ? t - 10 : 1, kind = t > 20 ? 2 : t > 10 ? 1 : t;
	char line[256];

	say("cpufreq_gov: m3off: test %u: %s, %u time%s\n", t, kind == 1 ? "cpu 7 offline 5 s" : kind == 2 ? "cpus 7 and 6 offline 5 s" :
	    kind == 3 ? "cpus 7 and 6 offline, the M3s at 1924 MHz 5 s" : "cpus 7 and 6 offline, the M3s at 2314 MHz 5 s", times,
	    times == 1 ? "" : "s");
	for (uint32_t n = 0; n < times && !m3off_stopped; n++) {
		int kr = sweet_m3off_take(7);

		if (kr == 0 && kind >= 2) {
			kr = sweet_m3off_take(6);
		}
		if (kr != 0) {
			m3off_stop("a test: a core not taken offline");
			break;
		}
		if (kind >= 3) {
			__atomic_store_n(&m3off_floor, kind == 3 ? 1924000u : 2314000u, __ATOMIC_RELEASE);
		}
		for (int i = 0; i < 5; i++) {
			m3off_sleep_ms(1000);
			say("cpufreq_gov: m3off: test %u: %d s: big %u kHz (max %u), %u off, temp big %d mC\n", t, i + 1, clusters[1].cur,
			    ceiling_now(&clusters[1]), sweet_m3off_count(), hakios_exynos_tmu_zone_mc(0));
			if (hakios_exynos_tmu_zone_mc(0) >= CEIL_M3OFF_STOP_MC) {
				m3off_stop("a test: the big zone at 75 C");
				break;
			}
		}
		__atomic_store_n(&m3off_floor, 0, __ATOMIC_RELEASE);
		if (m3off_give_all() != 0) {
			m3off_stop("a test: a core not started again");
			break;
		}
		(void)sweet_m3off_format(line, sizeof(line));
		say("cpufreq_gov: m3off: test %u: round %u done: big %u kHz; %s\n", t, n + 1, clusters[1].cur, line);
		if (n + 1 < times) {
			m3off_sleep_ms(1000);
		}
	}
	say("cpufreq_gov: m3off: test %u %s\n", t, m3off_stopped ? "STOPPED" : "passed");
}

static void
m3off_thread(void *arg __unused, wait_result_t wr __unused)
{
	for (;;) {
		uint64_t deadline;
		uint32_t t, want;

		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline((event_t)&m3off_event, THREAD_UNINT, deadline);
		(void)thread_block(NULL);
		t = __atomic_load_n(&m3off_test, __ATOMIC_ACQUIRE);
		if (t != 0) {
			if (sweet_m3off_mode >= 2 && sweet_m3off_count() == 0) {
				m3off_run_test(t);
			} else {
				say("cpufreq_gov: m3off: test %u refused (m3off=%u, %u M3s offline)\n", t, sweet_m3off_mode, sweet_m3off_count());
			}
			__atomic_store_n(&m3off_test, 0, __ATOMIC_RELEASE);
			continue;
		}
		want = m3off_stopped ? 0 : __atomic_load_n(&m3off_want, __ATOMIC_ACQUIRE);
		if (want == 2 && (sweet_m3off_mode >= 2 ? sweet_m3off_count() : m3off_dry_off) < 2) {
			if (m3off_take_two() != 0) {
				m3off_stop("a core not taken offline");
			} else {
				m3off_two_since = mach_absolute_time();
				if (sweet_m3off_mode < 2) {
					say("cpufreq_gov: m3off: (decisions only) would take cpus 7 and 6 offline now\n");
				}
			}
		}
		if (want == 0 && (sweet_m3off_mode >= 2 ? sweet_m3off_count() != 0 || sweet_m3off_is_off(6) || sweet_m3off_is_off(7) :
		    m3off_dry_off != 0)) {
			if (m3off_two_since != 0) {
				uint64_t ns;

				absolutetime_to_nanoseconds(mach_absolute_time() - m3off_two_since, &ns);
				m3off_secs_two += (uint32_t)(ns / NSEC_PER_MSEC);
				m3off_two_since = 0;
			}
			if (m3off_give_all() != 0) {
				m3off_stop("the cores not brought back");
				(void)m3off_give_all();
			} else if (sweet_m3off_mode < 2) {
				say("cpufreq_gov: m3off: (decisions only) would bring cpus 6 and 7 back now\n");
			}
		}
	}
}

/* once a second with m3off on: what it holds, on one line */
static void
m3off_line(uint64_t seconds)
{
	static uint32_t last_off, last_n;
	uint32_t off = sweet_m3off_mode >= 2 ? sweet_m3off_count() : m3off_dry_off;

	if (sweet_m3off_mode == 0 || !m3off_started) {
		return;
	}
	if (off == 0 && last_off == 0 && last_n == m3off_n_off + m3off_n_on && seconds % 10 != 0) {
		return;
	}
	last_off = off;
	last_n = m3off_n_off + m3off_n_on;
	say("cpufreq_gov: m3off: %llu s: %s%u off (want %u), M3s online %u, big %u kHz max %u, sum %u%%, temp big %d mC, PMIC warning %s | "
	    "to two %u, back %u, %u ms at two%s%s\n", seconds, sweet_m3off_mode < 2 ? "(decisions only) " : "", off, m3off_want,
	    clusters[1].online, clusters[1].cur, ceiling_now(&clusters[1]), clusters[1].sum_now, hakios_exynos_tmu_zone_mc(0),
	    ocpg_mode >= 1 ? (ocpg.pmic_on ? "on" : "OFF") : "not read", m3off_n_off, m3off_n_on, m3off_secs_two,
	    m3off_stopped ? " | STOPPED: " : "", m3off_stopped ? m3off_stop_why : "");
}

static int
m3off_test_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = (int)m3off_test, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (!((value >= 1 && value <= 4) || (value >= 11 && value <= 19) || (value >= 21 && value <= 29)) ||
	    sweet_m3off_mode < 2 || !m3off_started || m3off_stopped) {
		say("cpufreq_gov: m3off: test %d refused (m3off=%u%s)\n", value, sweet_m3off_mode, m3off_stopped ? ", stopped" : "");
		return EINVAL;
	}
	__atomic_store_n(&m3off_want, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&m3off_test, (uint32_t)value, __ATOMIC_RELEASE);
	(void)thread_wakeup_prim((event_t)&m3off_event, FALSE, THREAD_AWAKENED);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, m3off_test, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    m3off_test_sysctl, "I", "m3off=2: 1 cpu 7 offline 5 s; 2 cpus 7 and 6; 3 those at 1924 MHz; 4 at 2314 MHz; 1N, 2N: N times");

static int
m3off_stats_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char buf[400];
	size_t n = sweet_m3off_format(buf, sizeof(buf));

	return SYSCTL_OUT(req, buf, n + 1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, m3off_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    m3off_stats_sysctl, "A", "the M3s taken offline and started again (m3off=)");

/* gov_setup(): the thread, with m3off= and policy 1 */
static void
m3off_start(void)
{
	thread_t thread;

	if (sweet_m3off_mode == 0) {
		return;
	}
	if (gov_policy != 1) {
		say("cpufreq_gov: m3off=%u: only under gov_policy=1; not started\n", sweet_m3off_mode);
		return;
	}
	if (ocpg_mode < 1) {
		say("cpufreq_gov: m3off=%u: the PMIC's warning is not read without ocp_guard=1: the M3s stay at 1794 MHz at most\n",
		    sweet_m3off_mode);
	}
	(void)PE_parse_boot_argn("m3off_auto", &m3off_auto, sizeof(m3off_auto));
	if (kernel_thread_start(m3off_thread, NULL, &thread) != KERN_SUCCESS) {
		say("cpufreq_gov: m3off: no thread\n");
		return;
	}
	thread_deallocate(thread);
	m3off_started = true;
	say("cpufreq_gov: m3off=%u%s: %s; to two M3s with the interface moving and the load fitting, back after a second still or when "
	    "two cannot carry it; above 1794 MHz only by the rule (PMIC warning on, temp under 75 C, two M3s on), 2314 MHz at most; "
	    "the M3s' ceiling now %u kHz\n", sweet_m3off_mode, m3off_auto ? "" : " m3off_auto=0 (the policy off: tests only)",
	    sweet_m3off_mode >= 2 ? "cores 6 and 7 taken offline and started again" :
	    "decisions logged, no core touched", clusters[1].ceiling);
}

static void
decide(struct cluster *c, uint64_t now, uint32_t window)
{
	uint32_t max = ceiling_now(c), want, target;

	c->load_sum += c->load;
	c->windows++;
	c->stat_windows++;
	c->sum_sum += c->sum_now;
	if (c->load >= SATURATED && c->cur < max) {
		c->sat++;
	}
	c->boost_floor = boost_floor(c, now);
	if (c->online == 0 || c->cur == 0) {
		return;
	}
	if (gov_policy != 1) {
		decide_scion(c, now, window);
		return;
	}
	if (c == &clusters[1]) {
		cores_set(SCION_BIG_CORES, now);
		m3off_decide(c, now);
	}
	want = c->levels[level_at_least(c, (uint64_t)c->cur * c->load / target_load)];
	if (c->load >= SATURATED && c->touch_khz != 0) {
		/* no time left over to say how much more it needs: the touch-boost rate at once, then the load's again */
		want = MAX(want, c->levels[level_at_least(c, c->touch_khz)]);
	}
	target = MAX(want, c->floor);
	target = MAX(target, c->boost_floor);
	if (c == &clusters[1]) {
		target = MAX(target, __atomic_load_n(&m3off_floor, __ATOMIC_ACQUIRE));    /* (an m3off test's) */
	}
	target = MIN(target, max);
	if (target < c->cur && c->cur <= max) {
		if (++c->low_windows < down_windows) {
			return;
		}
	}
	c->low_windows = 0;
	if (target != c->cur) {
		set_rate(c, target, max);
	}
}

/* the zone's temperature, from the TMU driver itself; the first time also what its sysctl says, for the log */
static int32_t
read_temp(struct cluster *c)
{
	static bool compared[2];
	int32_t mc = hakios_exynos_tmu_zone_mc(c->tmu_zone);

	if (!compared[c->tmu_zone] && mc != NOT_READ_MC) {
		int value = NOT_READ_MC;
		size_t size = sizeof(value);
		int e = kernel_sysctlbyname(c->zone, &value, &size, NULL, 0);

		compared[c->tmu_zone] = true;
		say("cpufreq_gov: %s: the TMU driver reads %d mC; its sysctl %s says %d (error %d)\n", c->name, mc, c->zone, value,
		    e);
	}
	return mc;
}

/* once a second: each zone's temperature moves its cluster's thermal ceiling by a level at most */
static void
cool(struct cluster *c)
{
	uint32_t before = c->thermal;

	c->temp_mc = read_temp(c);
	if (c->temp_mc == NOT_READ_MC) {
		c->thermal = 0;
	} else if (c->temp_mc >= HOT_CUT_MC) {
		c->thermal = c->lowest;
	} else if (c->temp_mc >= c->control_mc) {
		/* one level under where it runs now (or under the last thermal ceiling) */
		uint32_t from = c->thermal ? c->thermal : level_at_most(c, MIN(c->cur, ceiling_now(c)));

		c->thermal = MIN(from + 1, c->lowest);
	} else if (c->thermal > 0 && c->temp_mc < c->control_mc - HYSTERESIS_MC) {
		c->thermal = c->thermal > 1 && c->levels[c->thermal - 1] < c->ceiling ? c->thermal - 1 : 0;
	}
	if (c->thermal != before) {
		c->cooled++;
		say("cpufreq_gov: %s at %d.%d C: its ceiling %s %u kHz\n", c->name, c->temp_mc / 1000,
		    (c->temp_mc < 0 ? -c->temp_mc : c->temp_mc) % 1000 / 100, c->thermal ? "now" : "back to",
		    c->thermal ? c->levels[c->thermal] : c->ceiling);
	}
}

static void
temp_text(char *out, size_t size, int32_t mc)
{
	if (mc == NOT_READ_MC) {
		snprintf(out, size, "-");
	} else {
		snprintf(out, size, "%d.%d", mc / 1000, (mc < 0 ? -mc : mc) % 1000 / 100);
	}
}

/* a gesture's progress: its floor reached, its ceiling reached, or a second gone (this file's thread, after any change) */
static void
ramp_check(uint64_t now)
{
	struct cluster *b = &clusters[1];
	uint64_t began = __atomic_load_n(&ramp_began, __ATOMIC_ACQUIRE), second;

	if (began == 0 || now < began) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	if (b->cur >= ceiling_now(b)) {
		nums.top_n++;
		nums.top_sum += now - began;
		nums.top_max = MAX(nums.top_max, now - began);
	} else if (now - began > second) {
		nums.top_missed++;
	} else {
		return;
	}
	(void)__atomic_compare_exchange_n(&ramp_began, &began, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	ramp_raised = false;
}

static uint64_t
to_us(uint64_t span)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(span, &ns);
	return ns / NSEC_PER_USEC;
}

/* the numbers to compare the policies by, since the line before (see the head); a line only after a second with any */
static void
stat_line(uint64_t seconds, uint64_t now, bool active)
{
	struct cluster *l = &clusters[0], *b = &clusters[1];
	uint64_t fingered, since;
	bool any = active || nums.acpm_n != 0;

	if (!gov_stat || !any) {
		return;
	}
	residency(l, now);
	residency(b, now);
	fingered = __atomic_exchange_n(&finger_time, 0, __ATOMIC_RELAXED);
	since = __atomic_load_n(&finger_since, __ATOMIC_ACQUIRE);
	if (since != 0 && __atomic_compare_exchange_n(&finger_since, &since, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		fingered += now - since;
	}
	say("cpufreq_gov: state ms static %llu touched %llu animating %llu\n", dev_ms[SCION_STATIC], dev_ms[SCION_TOUCHED],
	    dev_ms[SCION_ANIMATING]);
	dev_ms[SCION_STATIC] = dev_ms[SCION_TOUCHED] = dev_ms[SCION_ANIMATING] = 0;
	say("cpufreq_gov: stat %llu s: policy %u | big ms %llu %llu %llu %llu %llu, lit ms %llu %llu | sat big %u lit %u of %u | rev big "
	    "%u lit %u | sum big %llu%% lit %llu%% | raise %u avg %llu us max %llu | top %u avg %llu ms max %llu, %u missed | acpm %u "
	    "avg %llu us max %llu | fingers %llu ms | few %llu ms | cooled %u\n", seconds, gov_policy, to_us(b->res[0]) / 1000,
	    to_us(b->res[1]) / 1000, to_us(b->res[2]) / 1000, to_us(b->res[3]) / 1000, to_us(b->res[4]) / 1000, to_us(l->res[0]) / 1000,
	    to_us(l->res[1] + l->res[2] + l->res[3] + l->res[4]) / 1000,
	    b->sat, l->sat, b->stat_windows, b->reversals, l->reversals, b->stat_windows ? b->sum_sum / b->stat_windows : 0,
	    l->stat_windows ? l->sum_sum / l->stat_windows : 0, nums.raise_n, nums.raise_n ? to_us(nums.raise_sum) / nums.raise_n : 0,
	    to_us(nums.raise_max), nums.top_n, nums.top_n ? to_us(nums.top_sum) / nums.top_n / 1000 : 0, to_us(nums.top_max) / 1000,
	    nums.top_missed, nums.acpm_n, nums.acpm_n ? to_us(nums.acpm_sum) / nums.acpm_n : 0, to_us(nums.acpm_max),
	    to_us(fingered) / 1000, to_us(few_time + (big_cores < SCION_BIG_CORES ? now - few_since : 0)) / 1000, b->cooled + l->cooled);
	few_time = 0;
	few_since = now;
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];

		for (int r = 0; r < RES_BUCKETS; r++) {
			c->res[r] = 0;
		}
		c->sat = c->stat_windows = c->reversals = c->cooled = 0;
		c->sum_sum = 0;
	}
	nums = (typeof(nums)){ 0 };
}

static void
summary(uint64_t seconds)
{
	struct cluster *l = &clusters[0], *b = &clusters[1];
	char tb[16], tl[16], ml[16], mb[16];
	uint32_t touches, frames, raises;
	uint64_t now;
	bool touched, framed;

	l->measured = measure_mhz(l);
	b->measured = measure_mhz(b);
	temp_text(tb, sizeof(tb), b->temp_mc);
	temp_text(tl, sizeof(tl), l->temp_mc);
	snprintf(ml, sizeof(ml), l->measured ? "%u" : "?", l->measured);
	snprintf(mb, sizeof(mb), b->measured ? "%u" : "?", b->measured);
	now = mach_absolute_time();
	touched = boost_on(__atomic_load_n(&touch_last, __ATOMIC_ACQUIRE), boost_span, now);
	framed = boost_on(__atomic_load_n(&frame_last, __ATOMIC_ACQUIRE), frame_span, now);
	touches = __atomic_exchange_n(&touch_count, 0, __ATOMIC_RELAXED);
	frames = __atomic_exchange_n(&frame_count, 0, __ATOMIC_RELAXED);
	raises = __atomic_exchange_n(&boost_raises, 0, __ATOMIC_RELAXED);
	say("cpufreq_gov: %llu s: lit %u MHz%s load %u%% (avg %u%%, %u online, %u max, measured %s) | big %u MHz%s load %u%% "
	    "(avg %u%%, %u online, %u max, measured %s) | temp big %s lit %s C | %u changes | boost: %u touch, %u frames, %u "
	    "raised, %s\n", seconds, l->cur / 1000,
	    l->asked ? " asked" : "", l->load, l->windows ? l->load_sum / l->windows : 0, l->online, ceiling_now(l) / 1000, ml,
	    b->cur / 1000, b->asked ? " asked" : "", b->load, b->windows ? b->load_sum / b->windows : 0, b->online,
	    ceiling_now(b) / 1000, mb, tb, tl, l->changes + b->changes, touches, frames, raises,
	    touched && framed ? "touch and frames on" : touched ? "touch on" : framed ? "frames on" : "off");
	l->load_sum = l->windows = b->load_sum = b->windows = 0;
	stat_line(seconds, now, touches != 0 || frames != 0 || raises != 0);
	ocpg_line(seconds);
}

static bool gov_setup(void);

static void
gov_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t then, start, last_second, last_log, second, log_every, wake_at;

	/* (Starbite) after the touch thread's start of the ACPM client (2 s, then its supplies): then the first request */
	clock_interval_to_deadline(STARTUP_MS, NSEC_PER_MSEC, &wake_at);
	(void)assert_wait_deadline((event_t)&gov_setup, THREAD_UNINT, wake_at);
	(void)thread_block(NULL);
	if (!gov_setup()) {
		return;
	}
	__atomic_store_n(&gov_running, true, __ATOMIC_RELEASE);
	then = mach_absolute_time();
	start = last_second = last_log = then;

	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	log_every = second * log_seconds;
	measure(1);     /* the idle times so far, and which cores have been idle: the first window starts here */
	for (;;) {
		uint64_t now, ns, deadline, span_ms;
		bool tried = false;

		/*
		 * The window waited out, but woken by a boost (hakios_exynos_cpufreq_touch/frame) to raise a cluster at once. The
		 * wait is asserted before the check, so a wakeup between the two is not lost: it ends the wait at once.
		 */
		for (;;) {
			now = mach_absolute_time();
			deadline = then + window_span(now);     /* (a boost starting shortens it, under scion) */
			if (now >= deadline) {
				break;
			}
			(void)assert_wait_deadline((event_t)&boost_event, THREAD_UNINT, deadline);
			if (!tried && boost_due(now)) {
				(void)thread_wakeup_prim((event_t)&boost_event, FALSE, THREAD_AWAKENED);
			}
			(void)thread_block(NULL);
			now = mach_absolute_time();
			if (ocpg_take_kick() && clusters[1].cur > ceiling_now(&clusters[1])) {
				/* (ocp_guard=: a throttle's clip or the end of stage two: the M3s under it now, as cpufreq_update_policy) */
				set_rate(&clusters[1], ceiling_now(&clusters[1]), ceiling_now(&clusters[1]));
			}
			if (boost_due(now)) {
				/* (not woken by itself again this window: a request the APM refused waits for the next) */
				tried = true;
				boost(now);
			}
			ramp_check(mach_absolute_time());
		}
		measure(now - then > 0 ? now - then : 1);
		absolutetime_to_nanoseconds(now - then, &ns);
		span_ms = ns / NSEC_PER_MSEC;
		then = now;
		if (now - last_second >= second) {
			last_second = now;
			cool(&clusters[0]);
			cool(&clusters[1]);
		}
		decide(&clusters[0], now, (uint32_t)span_ms);
		{
			/* (m3off: a window decided with the return asked: m3off_rate_down() may start the cores once the rate is under) */
			uint32_t gen = __atomic_load_n(&m3off_returning_gen, __ATOMIC_ACQUIRE);

			decide(&clusters[1], now, (uint32_t)span_ms);
			__atomic_store_n(&m3off_seen_gen, gen, __ATOMIC_RELEASE);
		}
		ramp_check(now);
		if (log_seconds != 0 && now - last_log >= log_every) {
			last_log = now;
			absolutetime_to_nanoseconds(now - start, &ns);
			summary(ns / NSEC_PER_SEC);
			m3off_line(ns / NSEC_PER_SEC);
		}
	}
}

static uint32_t lit_max, lit_min, big_max, big_min, touch_big = 1469000, touch_lit = 1690000, frame_big = 1261000;

static void
cluster_setup(struct cluster *c, uint32_t asked_max, uint32_t asked_min, uint32_t max_default, uint32_t floor_default)
{
	uint32_t uv[LEVELS_MOST];
	char line[256];
	int at;

	c->n = hakios_acpm_dvfs_table(c->index, c->levels, uv, LEVELS_MOST, say);
	if (c->n > 0) {
		at = snprintf(line, sizeof(line), "cpufreq_gov: %s: the APM's table (DVFS %u), kHz/uV:", c->name, c->index);
		for (uint32_t l = 0; l < c->n && at < (int)sizeof(line) - 20; l++) {
			at += snprintf(line + at, sizeof(line) - at, " %u/%u", c->levels[l], uv[l]);
		}
		say("%s\n", line);
	} else {
		for (uint32_t l = 0; l < c->n_dt_levels; l++) {
			c->levels[l] = c->dt_levels[l];
		}
		c->n = c->n_dt_levels;
		say("cpufreq_gov: %s: the APM's table not read: the device tree's %u levels, %u to %u kHz\n", c->name, c->n,
		    c->levels[c->n - 1], c->levels[0]);
	}
	c->ceiling = MIN(asked_max ? asked_max : max_default, c->dt_max);
	c->floor = MAX(asked_min ? asked_min : floor_default, c->dt_min);
	c->ceiling = c->levels[level_at_most(c, c->ceiling)];
	c->lowest = level_at_least(c, c->dt_min);
	c->floor = MIN(c->levels[level_at_least(c, c->floor)], c->ceiling);
	c->temp_mc = NOT_READ_MC;
}

bool
hakios_exynos_cpufreq_gov_start(void)
{
	uint32_t on = 0, value = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("cpufreq_gov", &on, sizeof(on)) || on == 0) {
		return false;
	}
	if (PE_parse_boot_argn("cpufreq_gov_ms", &value, sizeof(value)) && value >= 20 && value <= 200) {
		window_ms = value;
	}
	value = 1;
	if (PE_parse_boot_argn("cpufreq_gov_log", &value, sizeof(value)) && value <= 3600) {
		log_seconds = value;
	}
	(void)PE_parse_boot_argn("cpufreq_gov_lit_max", &lit_max, sizeof(lit_max));
	(void)PE_parse_boot_argn("cpufreq_gov_lit_min", &lit_min, sizeof(lit_min));
	{
		/* sweetspeed&131072: the A55s no slower than 1456 MHz (the symmetric scheduler puts UI work there) */
		extern uint32_t sweet_speed_flags(void);
		if ((sweet_speed_flags() & 131072) != 0 && lit_min < 1456000) {
			lit_min = 1456000;
		}
	}
	(void)PE_parse_boot_argn("cpufreq_gov_big_max", &big_max, sizeof(big_max));
	(void)PE_parse_boot_argn("cpufreq_gov_big_min", &big_min, sizeof(big_min));
	(void)PE_parse_boot_argn("gov_boost_big", &touch_big, sizeof(touch_big));
	(void)PE_parse_boot_argn("gov_boost_lit", &touch_lit, sizeof(touch_lit));
	(void)PE_parse_boot_argn("gov_frame_big", &frame_big, sizeof(frame_big));
	if (PE_parse_boot_argn("gov_boost_ms", &value, sizeof(value)) && value <= 5000) {
		boost_ms = value;
	}
	if (PE_parse_boot_argn("gov_frame_ms", &value, sizeof(value)) && value >= 20 && value <= 1000) {
		frame_ms = value;
	}
	if (PE_parse_boot_argn("gov_target", &value, sizeof(value)) && value >= 30 && value <= 95) {
		target_load = value;
	}
	if (PE_parse_boot_argn("gov_down", &value, sizeof(value)) && value >= 1 && value <= 50) {
		down_windows = value;
	}
	if (PE_parse_boot_argn("gov_policy", &value, sizeof(value)) && value >= 1 && value <= 4) {
		gov_policy = value;
	}
	(void)PE_parse_boot_argn("gov_stat", &gov_stat, sizeof(gov_stat));
	(void)PE_parse_boot_argn("gov_wake", &gov_wake, sizeof(gov_wake));
	(void)PE_parse_boot_argn("ocp_guard", &ocpg_mode, sizeof(ocpg_mode));
	(void)PE_parse_boot_argn("ocp_guard_flags", &ocpg_flags, sizeof(ocpg_flags));
	ocpg_mode = MIN(ocpg_mode, 4u);
	if (ocpg_mode >= 2) {
		/*
		 * Levels 2-4 read the M3's OCP registers (S3_0_C15_C12_7 and four more). Samsung's own Exynos 9810 kernel never does:
		 * exynos-ocp.c is built only with CONFIG_EXYNOS_OCP (drivers/soc/samsung/Makefile 49, Kconfig 162-167, "default n"),
		 * and every 9810 config has it off (exynos9810-starlte_defconfig "# CONFIG_EXYNOS_OCP is not set", the stock
		 * 4.9.118 config the same). On this SoC the driver is dead code; the 9820 (M4) driver that is built reaches the block
		 * by MMIO (GCU_BASE_ADDR 0x1E4C0000), not these encodings. The first such read stopped the core for good on two S9s
		 * (16.5: dab99986; 27: T3, 18 Nov 2026): it does not return, so nothing here can recover from it. Not done again.
		 */
		say("cpufreq_gov: ocp_guard=%u asked: levels 2-4 are retired (the M3's OCP registers: Samsung's 9810 kernel never "
		    "reads them, and here the first read never returns); level 1, observing only\n", ocpg_mode);
		ocpg_mode = 1;
	}
	if (boost_ms == 0) {
		touch_big = touch_lit = 0;
	}
	if (touch_big == 0 && touch_lit == 0) {
		boost_ms = 0;
	}
	if (frame_big == 0) {
		frame_ms = 0;
	}
	nanoseconds_to_absolutetime((uint64_t)boost_ms * NSEC_PER_MSEC, &boost_span);
	nanoseconds_to_absolutetime((uint64_t)frame_ms * NSEC_PER_MSEC, &frame_span);
	nanoseconds_to_absolutetime((uint64_t)BURST_MS * NSEC_PER_MSEC, &burst_span);
	nanoseconds_to_absolutetime((uint64_t)LATE_MS * NSEC_PER_MSEC, &late_span);
	nanoseconds_to_absolutetime((uint64_t)MOVING_MS * NSEC_PER_MSEC, &moving_span);
	nanoseconds_to_absolutetime((uint64_t)HOLD_MS * NSEC_PER_MSEC, &hold_span);
	if (kernel_thread_start(gov_thread, NULL, &thread) != KERN_SUCCESS) {
		say("cpufreq_gov: no thread: the rates stay as they are\n");
		return true;
	}
	thread_deallocate(thread);
	say("cpufreq_gov: cpufreq_gov=1: the governor's thread started; it owns both clusters' rates %u ms from now\n", STARTUP_MS);
	return true;
}

/* (Starbite) the setup Scion did in hakios_exynos_cpufreq_gov_start(), on the governor's thread */

static bool
gov_setup(void)
{
	if (!starlte_acpm_dvfs_ready()) {
		say("cpufreq_gov: no ACPM: not started (the rates stay as S-Boot set them)\n");
		return false;
	}
	clusters[0] = (struct cluster){ .name = "lit", .zone = "kern.hakios.temp.little_mc", .tmu_zone = 1, .index = 2, .affinity = 0,
		                        .control_mc = 76000, .dt_min = 455000, .dt_max = 1950000, .dt_levels = lit_dt,
		                        .n_dt_levels = sizeof(lit_dt) / sizeof(lit_dt[0]) };
	clusters[1] = (struct cluster){ .name = "big", .zone = "kern.hakios.temp.big_mc", .tmu_zone = 0, .index = 3, .affinity = 1,
		                        .control_mc = 83000, .dt_min = 650000, .dt_max = 2314000, .dt_levels = big_dt,
		                        .n_dt_levels = sizeof(big_dt) / sizeof(big_dt[0]) };
	cluster_setup(&clusters[0], lit_max, lit_min, 1794000, 455000);       /* (Starbite: no loader core; Scion 1053000) */
	cluster_setup(&clusters[1], big_max, big_min, 1794000, 650000);
	clusters[0].touch_khz = touch_lit;
	clusters[1].touch_khz = touch_big;
	clusters[1].frame_khz = frame_big;
	clusters[0].scion_touch = scion_soc.lit_touch;
	clusters[1].scion_touch = scion_soc.big_touch;
	clusters[0].scion_safe = scion_soc.lit_safe;
	clusters[1].scion_safe = scion_soc.big_safe;
	clusters[1].burst = true;
	clusters[0].res_khz[0] = clusters[0].res_khz[1] = clusters[0].res_khz[2] = clusters[0].res_khz[3] = 1053001;
	clusters[1].res_khz[0] = 1261000;
	clusters[1].res_khz[1] = 1469000;
	clusters[1].res_khz[2] = 1794000;
	clusters[1].res_khz[3] = 1794001;
	for (int i = 0; i < 2; i++) {
		struct cluster *c = &clusters[i];
		uint32_t found = 0;
		int e = hakios_acpm_dvfs_get(c->index, &found);

		c->cur = e == 0 ? found : 0;
		say("cpufreq_gov: %s (DVFS %u): %u to %u kHz, now %u kHz (%d)%s\n", c->name, c->index, c->floor, c->ceiling,
		    c->cur, e, i == 1 ? "; and by M3s online: 1-2 2314000, 3-4 1794000; 1794000 without temperatures" : "");
		if (c->cur == 0) {
			/* (not said: the ceiling is asked for, and from then on the rate is the one asked for) */
			set_rate(c, c->levels[level_at_most(c, c->ceiling)], c->ceiling);
		}
	}
	say("cpufreq_gov: every %u ms, the busiest core %u%% busy wanted, down after %u windows; cooling from %d C (big), %d C "
	    "(lit), the lowest level from %d C; a line every %u s\n", window_ms, target_load, down_windows,
	    clusters[1].control_mc / 1000, clusters[0].control_mc / 1000, HOT_CUT_MC / 1000, log_seconds);
	say("cpufreq_gov: boosts: touch big %u lit %u kHz for %u ms after the last finger event; frames big %u kHz while one "
	    "came in the last %u ms; %u%% busy or more: the touch-boost rate at once; all within the ceilings above\n",
	    boost_ms ? touch_big : 0, boost_ms ? touch_lit : 0, boost_ms, frame_ms ? frame_big : 0, frame_ms, SATURATED);
	say("cpufreq_gov: policy %u (kern.hakios.gov_policy; 3 scion, 2 scion without parking)\n", gov_policy);
	ocpg_start();
	m3off_start();
	return true;
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
