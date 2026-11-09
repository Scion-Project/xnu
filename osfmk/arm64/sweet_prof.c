/*
 * SWEET PROF, a statistical CPU profiler for the phone (no debugger, no ktrace tools).
 *
 * A running timer (only armed while a non-idle thread runs on the CPU, like kperf's) fires every
 * sweetprof_us (default 1000) us on each CPU and records the interrupted pc: user pc (shared cache
 * slid, symbolicate with the slide printed in the report) or unslid kernel pc, with the pid. Two
 * open tables (double buffered); the STARLTE TOP thread calls sweet_prof_report() every 10 s, which
 * flips the table and prints per-pid user/kernel samples and the hottest pcs:
 *   "SWEET PROF (10 s): N samples (lost L), period P us, a55 A m3 M; slide S kslide K; pid u/k: ..."
 *   "SWEET PROF pcs: u56:1a2753e60:123 k56:fffffff007abc120:44 ..."
 * Plus the time spent inside the arm64e/LSE emulator (sweet_a64e.c), by class:
 *   "SWEET PROF emu: lse N x A ns, a64e N x A ns, pins P"
 * Enabled by boot-arg sweetspeed&16384 (sweet_prof_init from st_top's thread); off, none of it runs (the hooks in
 * processor.c and sched_prim.c test one flag). docs/ios27/OPTIMISE.md.
 */
#include <pexpert/arm64/board_config.h>
#include <kern/timer_call.h>
#include <kern/processor.h>
#include <stdint.h>

#if !defined(ARM64_BOARD_CONFIG_SWEET) && !defined(ARM64_BOARD_CONFIG_A5XELTE)
/* other machine configurations: the hooks exist (processor.c, sched_prim.c) and do nothing */
void sweet_prof_expire(timer_call_param_t p0, timer_call_param_t p1);
void
sweet_prof_expire(__unused timer_call_param_t p0, __unused timer_call_param_t p1)
{
}

void sweet_prof_running_setup(processor_t processor, uint64_t now);
void
sweet_prof_running_setup(__unused processor_t processor, __unused uint64_t now)
{
}
#else
#include <mach/mach_types.h>
#include <arm64/proc_reg.h>
#include <arm/cpu_data_internal.h>
#include <kern/processor.h>
#include <kern/timer_call.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/misc_protos.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>

extern int task_pid(task_t task);
extern void qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *));
extern vm_offset_t vm_kernel_slide;

uint32_t sweet_prof_on;
static uint64_t sweet_prof_period_abs;
static uint32_t sweet_prof_period_us = 1000;

#define SP_SLOTS 8192
struct sp_slot { uint64_t key; uint32_t count; uint32_t pad; };
static struct sp_slot sp_tab[2][SP_SLOTS];
static volatile uint32_t sp_cur;
static uint64_t sp_lost[2], sp_cpu[2][2];

/* emulator timing (sweet_a64e.c wrapper) */
uint64_t sweet_emu_n[2], sweet_emu_abs[2];

#define SP_KERN (1ULL << 63)
#define SP_M3   (1ULL << 62)
#define SP_MASK44 0xfffffffffffULL

static inline uint64_t
sp_key(bool kern, int pid, uint64_t pc)
{
	return (kern ? SP_KERN : 0) | ((uint64_t)(pid & 0x7fff) << 44) | (pc & SP_MASK44);
}

static void
sp_add(uint64_t key)
{
	uint32_t t = os_atomic_load(&sp_cur, relaxed) & 1;
	struct sp_slot *tab = sp_tab[t];
	unsigned h = (unsigned)(((key >> 2) ^ (key >> 44)) * 0x9E3779B1u) % SP_SLOTS;
	for (unsigned i = 0; i < 32; i++) {
		struct sp_slot *e = &tab[(h + i) % SP_SLOTS];
		uint64_t k = os_atomic_load(&e->key, relaxed);
		if (k == 0) {
			if (os_atomic_cmpxchg(&e->key, 0ULL, key, relaxed)) {
				os_atomic_inc(&e->count, relaxed);
				return;
			}
			k = os_atomic_load(&e->key, relaxed);
		}
		if (k == key) {
			os_atomic_inc(&e->count, relaxed);
			return;
		}
	}
	os_atomic_inc(&sp_lost[t], relaxed);
}

static void
sweet_prof_sample(void)
{
	cpu_data_t *cdp = getCpuDatap();
	arm_saved_state_t *st = cdp->cpu_int_state;
	thread_t th = current_thread();
	task_t task = get_threadtask(th);
	int pid = task_pid(task);
	uint64_t pc = 0;
	bool kern = true;
	if (st != NULL) {
		pc = get_saved_state_pc(st);
		if (PSR64_IS_USER(get_saved_state_cpsr(st))) {
			kern = false;
		} else {
			pc = VM_KERNEL_UNSLIDE(pc);
		}
	}
	pc &= ~(uint64_t)63;    /* 64-byte buckets: one function's samples stay few keys (prof.py names the bucket) */
	uint32_t t = os_atomic_load(&sp_cur, relaxed) & 1;
	bool m3 = (__builtin_arm_rsr64("MIDR_EL1") >> 24) == 0x53;
	os_atomic_inc(&sp_cpu[t][m3 ? 1 : 0], relaxed);
	sp_add(sp_key(kern, pid, pc) | (m3 ? SP_M3 : 0) | ((uint64_t)(cdp->cpu_number & 7) << 59));
}

void sweet_prof_expire(timer_call_param_t p0, timer_call_param_t p1);
void
sweet_prof_expire(__unused timer_call_param_t p0, __unused timer_call_param_t p1)
{
	if (!sweet_prof_on) {
		return;
	}
	sweet_prof_sample();
	uint64_t now = mach_absolute_time();
	running_timer_enter(current_processor(), RUNNING_TIMER_SWEETPROF, NULL, now + sweet_prof_period_abs, now);
}

/* sched_prim.c thread_dispatch: a CPU leaves idle */
void sweet_prof_running_setup(processor_t processor, uint64_t now);
void
sweet_prof_running_setup(processor_t processor, uint64_t now)
{
	if (sweet_prof_on) {
		/* a little jitter so the CPUs do not sample in lock step */
		uint64_t j = (now >> 3) % sweet_prof_period_abs;
		running_timer_setup(processor, RUNNING_TIMER_SWEETPROF, NULL, now + sweet_prof_period_abs / 2 + j, now);
	}
}

void sweet_prof_init(void);
void
sweet_prof_init(void)
{
	extern uint32_t sweet_speed_flags(void);
	if ((sweet_speed_flags() & 16384) == 0) {
		return;
	}
	(void)PE_parse_boot_argn("sweetprof_us", &sweet_prof_period_us, sizeof(sweet_prof_period_us));
	if (sweet_prof_period_us < 100) {
		sweet_prof_period_us = 100;
	}
	nanoseconds_to_absolutetime((uint64_t)sweet_prof_period_us * NSEC_PER_USEC, &sweet_prof_period_abs);
	os_atomic_store(&sweet_prof_on, 1, release);
	printf("SWEET PROF: on, period %u us per CPU while not idle\n", sweet_prof_period_us);
}

static int
sp_cmp(const void *a, const void *b)
{
	const struct sp_slot *x = a, *y = b;
	return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

#define SP_PIDS 64
static struct sp_slot sp_sorted[SP_SLOTS];

void sweet_prof_report(uint64_t window_ns, void (*namefn)(int pid, char *buf, int size), void (*regionfn)(int pid, uint64_t pc));
void
sweet_prof_report(uint64_t window_ns, void (*namefn)(int pid, char *buf, int size), void (*regionfn)(int pid, uint64_t pc))
{
	static uint64_t emu_last_n[2], emu_last_abs[2];
	uint64_t en[2], ens[2];
	for (int i = 0; i < 2; i++) {
		en[i] = sweet_emu_n[i] - emu_last_n[i];
		uint64_t a = sweet_emu_abs[i] - emu_last_abs[i];
		emu_last_n[i] = sweet_emu_n[i];
		emu_last_abs[i] = sweet_emu_abs[i];
		absolutetime_to_nanoseconds(a, &ens[i]);
	}
	if (!sweet_prof_on) {
		return;
	}
	extern uint64_t sweet_a55_retries;
	printf("SWEET PROF emu: lse %llu x %llu ns = %llu ms, a64e %llu x %llu ns = %llu ms (in %llu ms); a55 retries %llu\n",
	    en[0], en[0] ? ens[0] / en[0] : 0, ens[0] / NSEC_PER_MSEC, en[1], en[1] ? ens[1] / en[1] : 0,
	    ens[1] / NSEC_PER_MSEC, window_ns / NSEC_PER_MSEC, sweet_a55_retries);

	uint32_t old = os_atomic_load(&sp_cur, relaxed) & 1;
	os_atomic_store(&sp_cur, old ^ 1, release);
	delay(2000);            /* samplers in flight on the old table finish */
	struct sp_slot *tab = sp_tab[old];
	unsigned n = 0;
	uint64_t total = 0;
	for (unsigned i = 0; i < SP_SLOTS; i++) {
		if (tab[i].key != 0 && tab[i].count != 0) {
			sp_sorted[n++] = tab[i];
			total += tab[i].count;
		}
	}
	bzero(tab, sizeof(sp_tab[0]));
	uint64_t lost = sp_lost[old], a55 = sp_cpu[old][0], m3 = sp_cpu[old][1];
	sp_lost[old] = 0;
	sp_cpu[old][0] = sp_cpu[old][1] = 0;
	qsort(sp_sorted, n, sizeof(sp_sorted[0]), sp_cmp);

	/* per pid user / kernel */
	struct { int pid; uint32_t u, k, m3; } pids[SP_PIDS];
	unsigned np = 0;
	for (unsigned i = 0; i < n; i++) {
		int pid = (int)((sp_sorted[i].key >> 44) & 0x7fff);
		bool kern = (sp_sorted[i].key & SP_KERN) != 0;
		unsigned j;
		for (j = 0; j < np && pids[j].pid != pid; j++) {
		}
		if (j == np) {
			if (np == SP_PIDS) {
				continue;
			}
			pids[np].pid = pid;
			pids[np].u = pids[np].k = pids[np].m3 = 0;
			np++;
		}
		if (sp_sorted[i].key & SP_M3) {
			pids[j].m3 += sp_sorted[i].count;
		}
		if (kern) {
			pids[j].k += sp_sorted[i].count;
		} else {
			pids[j].u += sp_sorted[i].count;
		}
	}
	char line[640];
	int ln = snprintf(line, sizeof(line), "SWEET PROF (%llu s): %llu samples (%u pcs, lost %llu), period %u us, a55 %llu m3 %llu; kslide 0x%lx; pid u/k/m3:",
	    window_ns / NSEC_PER_SEC, total, n, lost, sweet_prof_period_us, a55, m3, (unsigned long)vm_kernel_slide);
	for (unsigned k = 0; k < 9; k++) {
		unsigned best = SP_PIDS;
		for (unsigned j = 0; j < np; j++) {
			if (pids[j].u + pids[j].k != 0 && (best == SP_PIDS || pids[j].u + pids[j].k > pids[best].u + pids[best].k)) {
				best = j;
			}
		}
		if (best == SP_PIDS || ln >= (int)sizeof(line) - 48) {
			break;
		}
		char name[24];
		namefn(pids[best].pid, name, sizeof(name));
		ln += snprintf(line + ln, sizeof(line) - (size_t)ln, " %s[%d] %u/%u/m%u", name, pids[best].pid, pids[best].u, pids[best].k, pids[best].m3);
		pids[best].u = pids[best].k = 0;
	}
	printf("%s\n", line);
	/* the hottest pcs: every pc with >= 3 samples, at most 400 */
	ln = snprintf(line, sizeof(line), "SWEET PROF pcs:");
	unsigned printed = 0;
	for (unsigned i = 0; i < n && printed < 600 && sp_sorted[i].count >= 2; i++, printed++) {
		uint64_t key = sp_sorted[i].key;
		bool kern = (key & SP_KERN) != 0;
		uint64_t pc = key & SP_MASK44;
		if (kern) {
			pc |= 0xfffff00000000000ULL;
		}
		if (!kern && pc < 0x180000000ULL && pc != 0) {
			regionfn((int)((key >> 44) & 0x7fff), pc);     /* prints a "SWEET PROF map:" line once per mapping */
		}
		bool m3 = (key & SP_M3) != 0;
		ln += snprintf(line + ln, sizeof(line) - (size_t)ln, " %c%d:%llx:%u@%u", kern ? (m3 ? 'K' : 'k') : (m3 ? 'U' : 'u'),
		    (int)((key >> 44) & 0x7fff), pc, sp_sorted[i].count, (unsigned)((key >> 59) & 7));
		if (ln > 180) {
			printf("%s\n", line);
			ln = snprintf(line, sizeof(line), "SWEET PROF pcs:");
		}
	}
	if (ln > 16) {
		printf("%s\n", line);
	}
}
#endif
