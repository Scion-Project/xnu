#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_HEROLTE)
/*
 * SWEET process sampler: where is a daemon stuck?
 *
 * The phone has no debugger and daemons log nowhere readable. Every
 * interval a kernel thread prints, for each process named in boot-arg
 * sweetsample (comma separated, default "SpringBoard,backboardd"; "-" = off)
 * and for each of its threads, the user pc/lr/sp and a frame-pointer
 * backtrace as "SMP name[pid] tN pc ... lr ... bt ..." (header line with the
 * shared region slide and the main binary's mach header, to symbolicate
 * with tools/sweet-crash.py). A thread blocked in a syscall shows the
 * syscall stub in pc and its caller in lr/bt.
 *
 * First sample 90 s after start, then every 120 s, 40 samples at most.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>

extern task_t proc_task(proc_t);
extern int hz;
extern void sweet_sample_start(void);

static char sweet_sample_names[128];

static bool
sweet_sample_wanted(const char *name)
{
	size_t n = strlen(name);
	for (const char *t = sweet_sample_names; *t;) {
		const char *end = strchr(t, ',');
		size_t tl = end ? (size_t)(end - t) : strlen(t);
		if (tl == n && n > 0 && strncmp(t, name, n) == 0) {
			return true;
		}
		if (!end) {
			break;
		}
		t = end + 1;
	}
	return false;
}

extern void sweet_sample_task(const char *name, int pid, task_t task);  /* osfmk/kern/exception.c */

static int
sweet_sample_proc(proc_t p, __unused void *arg)
{
	char name[32];
	proc_name(proc_pid(p), name, sizeof(name));
	if (sweet_sample_wanted(name)) {
		task_t task = proc_task(p);
		if (task != TASK_NULL) {
			sweet_sample_task(name, proc_pid(p), task);
		}
	}
	return PROC_RETURNED;
}

static void
sweet_sample_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	if (!PE_parse_boot_argn("sweetsample", sweet_sample_names, sizeof(sweet_sample_names))) {
		strlcpy(sweet_sample_names, "SpringBoard,backboardd", sizeof(sweet_sample_names));
	}
	if (sweet_sample_names[0] == '-') {
		return;
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	const int first = 40, every = 60;
#else
	const int first = 90, every = 120;
#endif
	(void)tsleep(&chan, PZERO, "sweetsmp", first * hz);
	for (int i = 0; i < 40; i++) {
		printf("SMP --- sample %d\n", i);
		proc_iterate(PROC_ALLPROCLIST, sweet_sample_proc, NULL, NULL, NULL);
		(void)tsleep(&chan, PZERO, "sweetsmp", every * hz);
	}
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/*
 * The burst sampler: where do a process's threads wait while the scripted input runs?
 *
 * boot-arg smpburst=NAME (off without it): smpburst_at seconds (default 62: the bench's swipes) after the scripted input
 * (sweetspeed&4096) saw its first frame, every smpburst_ms (default 20) for smpburst_s seconds (default 31), every thread
 * of the process is looked at once (osfmk/kern/exception.c, sweet_sample_burst) and counted by what it was doing: runnable,
 * or waiting with this user stack. Afterwards, on the console:
 *   "SMPB name[pid] slide 0x.. mh 0x..: N samples every M ms, L lost"
 *   "SMPB thread TID 'thread name': N samples, R running"                   (one per thread, busiest first)
 *   "SMPB TID xCOUNT pc 0x.. lr 0x.. bt 0x.. ..."                           (the 48 commonest waits; symbolicate as SMP lines)
 * One kernel thread, asleep between looks; a look takes the task's lock for the walk and reads at most ten frames a thread.
 */
struct sweet_burst_rec {
	uint64_t tid, key, pc, lr, bt[10];
	uint32_t count, running, nbt, pad;
};
extern unsigned sweet_sample_burst(task_t task, struct sweet_burst_rec *tab, unsigned slots, uint64_t *tids, char (*names)[24],
    unsigned max_threads);
extern unsigned long long starlte_touch_script_t0;      /* bsd/dev/starlte_touch.c */
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
extern uint64_t sweet_sample_slide(task_t task, uint64_t *mh);

#define SWEET_BURST_SLOTS 1024
#define SWEET_BURST_THREADS 96
static char sweet_burst_name[32];
static int sweet_burst_pid;

static int
sweet_burst_find(proc_t p, __unused void *arg)
{
	char name[32];
	proc_name(proc_pid(p), name, sizeof(name));
	if (strncmp(name, sweet_burst_name, sizeof(name)) == 0) {
		sweet_burst_pid = proc_pid(p);
	}
	return PROC_RETURNED;
}

static void
sweet_burst_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	uint32_t ms = 20, at = 62, seconds = 31;
	PE_parse_boot_argn("smpburst_ms", &ms, sizeof(ms));
	PE_parse_boot_argn("smpburst_at", &at, sizeof(at));
	PE_parse_boot_argn("smpburst_s", &seconds, sizeof(seconds));
	if (ms < 5) {
		ms = 5;
	}
	for (int i = 0; i < 6000 && starlte_touch_script_t0 == 0; i++) {
		(void)tsleep(&chan, PZERO, "smpburst", hz / 10 ? hz / 10 : 1);
	}
	if (starlte_touch_script_t0 == 0) {
		printf("SMPB: no scripted input in ten minutes; not sampling\n");
		return;
	}
	(void)tsleep(&chan, PZERO, "smpburst", (int)at * hz);
	static struct sweet_burst_rec tab[SWEET_BURST_SLOTS];
	static uint64_t tids[SWEET_BURST_THREADS];
	static char names[SWEET_BURST_THREADS][24];
	proc_iterate(PROC_ALLPROCLIST, sweet_burst_find, NULL, NULL, NULL);
	if (sweet_burst_pid == 0) {
		printf("SMPB: no process named %s\n", sweet_burst_name);
		return;
	}
	uint64_t samples = 0, lost = 0, slide = 0, mh = 0, t0 = mach_absolute_time(), ns = 0;
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ns >= (uint64_t)seconds * NSEC_PER_SEC) {
			break;
		}
		proc_t p = proc_find(sweet_burst_pid);
		if (p == PROC_NULL) {
			break;
		}
		task_t task = proc_task(p);
		if (task != TASK_NULL) {
			if (samples == 0) {
				slide = sweet_sample_slide(task, &mh);
			}
			lost += sweet_sample_burst(task, tab, SWEET_BURST_SLOTS, tids, names, SWEET_BURST_THREADS);
			samples++;
		}
		proc_rele(p);
		struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000 };
		(void)msleep(&chan, NULL, PZERO, "smpburst", &ts);
	}
	printf("SMPB %s[%d] slide 0x%llx mh 0x%llx: %llu samples every %u ms, %llu lost\n", sweet_burst_name, sweet_burst_pid, slide, mh,
	    samples, ms, lost);
	/* the threads, busiest (most often runnable) first; then the commonest waits */
	for (unsigned k = 0; k < SWEET_BURST_THREADS; k++) {
		unsigned best = SWEET_BURST_THREADS;
		uint64_t best_run = 0, best_all = 0;
		for (unsigned i = 0; i < SWEET_BURST_THREADS && tids[i] != 0; i++) {
			if (tids[i] == ~0ull) {
				continue;
			}
			uint64_t run = 0, all = 0;
			for (unsigned j = 0; j < SWEET_BURST_SLOTS; j++) {
				if (tab[j].key != 0 && tab[j].tid == tids[i]) {
					all += tab[j].count;
					run += tab[j].running ? tab[j].count : 0;
				}
			}
			if (best == SWEET_BURST_THREADS || run > best_run) {
				best = i;
				best_run = run;
				best_all = all;
			}
		}
		if (best == SWEET_BURST_THREADS) {
			break;
		}
		if (best_run != 0 || k < 12) {
			printf("SMPB thread %llu '%s': %llu samples, %llu running\n", tids[best], names[best], best_all, best_run);
		}
		tids[best] = ~0ull;
	}
	for (unsigned k = 0; k < 48; k++) {
		unsigned best = SWEET_BURST_SLOTS;
		for (unsigned j = 0; j < SWEET_BURST_SLOTS; j++) {
			if (tab[j].key != 0 && !tab[j].running && tab[j].count != 0 &&
			    (best == SWEET_BURST_SLOTS || tab[j].count > tab[best].count)) {
				best = j;
			}
		}
		if (best == SWEET_BURST_SLOTS || tab[best].count < 3) {
			break;
		}
		char line[400];
		int n = snprintf(line, sizeof(line), "SMPB %llu x%u pc 0x%llx lr 0x%llx bt", tab[best].tid, tab[best].count,
		    tab[best].pc, tab[best].lr);
		for (unsigned i = 0; i < tab[best].nbt && n < (int)sizeof(line) - 20; i++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " 0x%llx", tab[best].bt[i]);
		}
		printf("%s\n", line);
		tab[best].count = 0;
	}
	printf("SMPB done\n");
}

#endif /* ARM64_BOARD_CONFIG_STARLTE */

void
sweet_sample_start(void)
{
	thread_t thread;
	if (kernel_thread_start(sweet_sample_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (PE_parse_boot_argn("smpburst", sweet_burst_name, sizeof(sweet_burst_name)) && sweet_burst_name[0] != 0 &&
	    kernel_thread_start(sweet_burst_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
#endif
}

#endif /* ARM64_BOARD_CONFIG_SWEET || ARM64_BOARD_CONFIG_HEROLTE */
