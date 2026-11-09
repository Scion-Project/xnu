/*
 * Bring-up aid: with boot-arg systrace set, describe every fatal-looking
 * exception a user thread takes, before it is delivered. There is no crash
 * reporter in this system, so this is the only place a crash is recorded.
 *
 * Besides the registers it prints the shared cache slide (most crashes are
 * inside system libraries: subtract the slide and look the address up in
 * the cache with "ipsw dyld a2s"), any text a register points at (the
 * library crash helpers leave their message in one) and the return
 * addresses found by walking the frame pointers.
 */

#include <arm/thread.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/exception_types.h>
#include <mach/machine/thread_status.h>
#include <string.h>
#include <stdarg.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>

extern char *proc_name_address(struct proc *p);

void user_exception_trace(exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t count, thread_t thread);

/* usertrap:, userabort:, top: and ustacks: lines go to the console and to the loader's log (the USB log, esslog) */
extern void loader_log_line(const char *line);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
static void
ut_say(const char *fmt, ...) __printflike(1, 2);
static void
ut_say(const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}

static TUNABLE(uint32_t, user_exception_trace_enabled, "systrace", 0);

static bool
plausible_user_address(uint64_t address)
{
	return address >= 0x100000000ULL && address < MACH_VM_MAX_ADDRESS;
}

void
user_exception_trace(exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t count, thread_t thread)
{
	static const unsigned message_registers[] = { 0, 1, 2, 3, 8, 16 };
	const arm_saved_state_t *state;
	vm_shared_region_t region;
	uint64_t frame[2], fp;
	uint32_t slide = 0;
	task_t task;
	struct proc *proc;

	if (!user_exception_trace_enabled || thread != current_thread()) {
		return;
	}
	if (exception != EXC_BAD_ACCESS && exception != EXC_BAD_INSTRUCTION && exception != EXC_ARITHMETIC &&
	    exception != EXC_BREAKPOINT) {
		return;
	}
	task = get_threadtask(thread);
	state = get_user_regs(thread);
	if (task == kernel_task || state == NULL || !is_saved_state64(state)) {
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		vm_shared_region_deallocate(region);
	}
	proc = get_bsdtask_info(task);

	ut_say("usertrap: %s[%d] exception %d code 0x%llx 0x%llx: pc 0x%llx lr 0x%llx sp 0x%llx, shared cache slide 0x%x\n",
	    proc ? proc_name_address(proc) : "?", proc ? proc_pid(proc) : -1, exception,
	    count > 0 ? (unsigned long long)code[0] : 0, count > 1 ? (unsigned long long)code[1] : 0,
	    (unsigned long long)get_saved_state_pc(state), (unsigned long long)get_saved_state_lr(state),
	    (unsigned long long)get_saved_state_sp(state), slide);

	for (unsigned i = 0; i < sizeof(message_registers) / sizeof(message_registers[0]); i++) {
		uint64_t value = (uint64_t)get_saved_state_reg(state, message_registers[i]);
		char text[128];
		vm_size_t length = 0;
		bool printable = true;

		if (!plausible_user_address(value)) {
			continue;
		}
		bzero(text, sizeof(text));
		(void)copyinstr((user_addr_t)value, text, sizeof(text) - 1, &length);
		for (unsigned c = 0; c < 6; c++) {
			printable = printable && text[c] >= 0x20 && text[c] < 0x7f;
		}
		if (printable) {
			ut_say("usertrap:   x%u -> \"%s\"\n", message_registers[i], text);
		}
	}

	fp = (uint64_t)get_saved_state_fp(state);
	for (int depth = 0; depth < 10 && plausible_user_address(fp) && (fp & 7) == 0; depth++) {
		if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
			break;
		}
		ut_say("usertrap:   called from 0x%llx\n", (unsigned long long)frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
}

/*
 * A program calls abort(): say which, and from where. The reason is usually an exception nobody
 * caught, or an assertion; the text of it goes to the program's standard error (boot-arg applog=1
 * puts that on the console), the stack is here. Then where every other thread of it is (the
 * "ustacks:" lines below, all threads, to the loader's log too): a program that aborts because a
 * watchdog of its own found it stuck (bluetoothd's "Stack Mutex try lock failed") was stuck in one
 * of those, and x16, the system call number, says in which call.
 */
void user_abort_trace(void);
static void ustacks_task(task_t task, struct proc *proc, bool on_abort);

void
user_abort_trace(void)
{
	thread_t thread = current_thread();
	task_t task = get_threadtask(thread);
	const arm_saved_state_t *state = get_user_regs(thread);
	struct proc *proc = get_bsdtask_info(task);
	vm_shared_region_t region;
	uint64_t frame[2], fp;
	uint32_t slide = 0;

	if (task == kernel_task || state == NULL || !is_saved_state64(state)) {
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		vm_shared_region_deallocate(region);
	}
	ut_say("userabort: %s[%d] aborts, shared cache slide 0x%x; called from:", proc ? proc_name_address(proc) : "?",
	    proc ? proc_pid(proc) : -1, slide);
	fp = (uint64_t)get_saved_state_fp(state);
	for (int depth = 0; depth < 24 && plausible_user_address(fp) && (fp & 7) == 0; depth++) {
		if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
			break;
		}
		ut_say(" 0x%llx", (unsigned long long)frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	ut_say("\n");
	if (proc != NULL) {
		ustacks_task(task, proc, true);
	}
}

/*
 * Boot-args ustacks=NAME ustacks_at=SECONDS [ustacks_ms=MILLISECONDS]: that long after boot
 * (and then periodically) print where every thread of every process called NAME is -- pc, lr
 * and the return addresses up the stack. Shows what a daemon that "does nothing" waits for.
 * ustacks_now(NAME) does it at once: under QEMU, "qemu/touch.py stacks NAME" on the host.
 * The addresses are symbolized on the host (qemu/symstack.py).
 * The name "top" asks for something else: who has had the processor (see ustacks_top()).
 */
#include <kern/recount.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/queue.h>
#include <pexpert/pexpert.h>

extern queue_head_t tasks;
extern lck_mtx_t tasks_threads_lock;

#define USTACKS_MAX_THREADS 96
#define USTACKS_MAX_FRAMES  14

static char ustacks_name[32];
static uint32_t ustacks_every;
static uint32_t ustacks_threads = USTACKS_MAX_THREADS;  /* boot-arg ustacks_threads=N: only the first N (1: the main thread) */
/*
 * Boot-arg ustacks_tname=TEXT: only the threads whose name has TEXT in it (ustacks_tname=coreanimation:
 * the window server's render-server and its other threads), each line with the thread's name and, in
 * the first, the time in microseconds of mach_absolute_time (as the "tl:" lines of tools/latency.py);
 * the lines are few enough then to go to the loader's log as well, where all of a long run is kept.
 * ustacks_tname=* takes every thread, named or not, and sends them to the loader's log too.
 */
static char ustacks_tname[32];
static uint32_t ustacks_count;          /* boot-arg ustacks_count=N: so many times, then no more (0: for ever) */
static thread_call_t ustacks_call;

extern void bsd_getthreadname(void *uth, char *buffer);
extern void loader_log_line(const char *line);

static void
ustacks_say(const char *line, bool on_abort)
{
	printf("%s", line);
	if (ustacks_tname[0] != 0 || on_abort) {
		loader_log_line(line);
	}
}

/* on_abort: from user_abort_trace(), every thread whatever the boot-args, with x16 (the system call it is in, if any) */
static void
ustacks_task(task_t task, struct proc *proc, bool on_abort)
{
	static struct {
		uint64_t tid, pc, lr, fp, x16;
		uint64_t arg[6];        /* x0-x5: a psynch call's arguments (on_abort) */
		int state;
		char name[64];          /* (bsd_getthreadname() copies MAXTHREADNAMESIZE, 64) */
	} snap[USTACKS_MAX_THREADS];
	char line[512];
	int at;
	vm_shared_region_t region;
	vm_map_t map, old_map;
	thread_t thread;
	uint32_t slide = 0;
	unsigned count = 0;

	task_lock(task);
	queue_iterate(&task->threads, thread, thread_t, task_threads) {
		const arm_saved_state_t *state = get_user_regs(thread);

		if (count == USTACKS_MAX_THREADS || (!on_abort && count >= ustacks_threads) || state == NULL ||
		    !is_saved_state64(state)) {
			continue;
		}
		snap[count].name[0] = 0;
		if (get_bsdthread_info(thread) != NULL) {
			bsd_getthreadname(get_bsdthread_info(thread), snap[count].name);
			snap[count].name[63] = 0;
		}
		if (!on_abort && ustacks_tname[0] != 0 && strcmp(ustacks_tname, "*") != 0 &&
		    strnstr(snap[count].name, ustacks_tname, sizeof(snap[count].name)) == NULL) {
			continue;
		}
		snap[count].tid = thread_tid(thread);
		snap[count].state = thread->state;
		snap[count].pc = get_saved_state_pc(state);
		snap[count].lr = get_saved_state_lr(state);
		snap[count].fp = get_saved_state_fp(state);
		snap[count].x16 = get_saved_state_reg(state, 16);
		for (int r = 0; r < 6; r++) {
			snap[count].arg[r] = get_saved_state_reg(state, r);
		}
		count++;
	}
	map = task->map;
	if (map != VM_MAP_NULL) {
		vm_map_reference(map);
	}
	task_unlock(task);
	if (map == VM_MAP_NULL) {
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		vm_shared_region_deallocate(region);
	}
	clock_sec_t secs;
	clock_usec_t usecs;

	uint64_t nanos;

	clock_get_system_microtime(&secs, &usecs);
	absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
	snprintf(line, sizeof(line), "ustacks: %s[%d] %u threads, shared cache slide 0x%x, uptime %lu.%03u, at %llu\n",
	    proc_name_address(proc), proc_pid(proc), count, slide, (unsigned long)secs, (unsigned)usecs / 1000, nanos / 1000);
	ustacks_say(line, on_abort);

	old_map = vm_map_switch(map);
	for (unsigned i = 0; i < count; i++) {
		uint64_t frame[2], fp = snap[i].fp;

		/* (a thread that runs shows where it last came into the kernel, not where it is) */
		at = snprintf(line, sizeof(line), "ustacks:  thread 0x%llx%s%s%s%s: 0x%llx 0x%llx", (unsigned long long)snap[i].tid,
		    snap[i].name[0] ? " \"" : "", snap[i].name, snap[i].name[0] ? "\"" : "",
		    (snap[i].state & TH_RUN) ? " (running)" : "", (unsigned long long)snap[i].pc, (unsigned long long)snap[i].lr);
		if (on_abort) {
			at += snprintf(line + at, sizeof(line) - at, " x16 %lld", (long long)snap[i].x16);
		}
		for (int depth = 0; depth < USTACKS_MAX_FRAMES && plausible_user_address(fp) && (fp & 7) == 0; depth++) {
			if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
				break;
			}
			if (at < (int)sizeof(line) - 24) {
				at += snprintf(line + at, sizeof(line) - at, " 0x%llx", (unsigned long long)frame[1]);
			}
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
		snprintf(line + at, sizeof(line) - at, "\n");
		ustacks_say(line, on_abort);
		/*
		 * Scion: a thread waiting in psynch_mutexwait (301) or psynch_cvwait (305) when its program aborted -- bluetoothd's
		 * stack watchdog, with every thread but one in 301 -- says what it waits on: the call's arguments as it made them
		 * (301: the mutex, mgen, ugen, the owner's thread id as user space saw it, flags; 305: the condition, its L and S
		 * words, U, the mutex, its generation, flags) and the mutex's first 64 bytes now (its lock word, sequence words and
		 * owner id: libpthread's layout). The owner id against the threads above tells a dead or waiting owner (user space)
		 * from a mutex nobody holds (a wake-up lost).
		 */
		if (on_abort && (snap[i].x16 == 301 || snap[i].x16 == 305)) {
			uint32_t words[16];
			user_addr_t mutex = (user_addr_t)(snap[i].x16 == 301 ? snap[i].arg[0] : snap[i].arg[3]);

			snprintf(line, sizeof(line), "ustacks:   %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)\n",
			    snap[i].x16 == 301 ? "psynch_mutexwait" : "psynch_cvwait", snap[i].arg[0], snap[i].arg[1], snap[i].arg[2],
			    snap[i].arg[3], snap[i].arg[4], snap[i].arg[5]);
			ustacks_say(line, on_abort);
			if (plausible_user_address(mutex) && (mutex & 7) == 0 && copyin(mutex, words, sizeof(words)) == 0) {
				at = snprintf(line, sizeof(line), "ustacks:   mutex 0x%llx:", (unsigned long long)mutex);
				for (int w = 0; w < 16; w++) {
					at += snprintf(line + at, sizeof(line) - at, " %08x", words[w]);
				}
				snprintf(line + at, sizeof(line) - at, "\n");
				ustacks_say(line, on_abort);
			}
		}
	}
	vm_map_switch(old_map);
	vm_map_deallocate(map);
}

/*
 * ustacks_now("top"): the processes that have had the processor since this was last asked (the
 * first time: since boot), each with its share of that time. A system that feels slow is either
 * busy -- this says with what -- or waiting for something, and the two look the same on the screen.
 */
#define USTACKS_TOP_TASKS 512
#define USTACKS_TOP_LINES 12

static void
ustacks_top(void)
{
	static struct {
		int pid;
		uint64_t total;                 /* user + system time so far, in Mach time units */
		uint64_t used;                  /* ... of it since the last time */
		char name[33];                  /* as long as a process name gets (2 * MAXCOMLEN + 1) */
	} now[USTACKS_TOP_TASKS];
	static struct {
		int pid;
		uint64_t total;
	} before[USTACKS_TOP_TASKS];
	static unsigned before_count;
	static uint64_t before_when;
	uint64_t when = mach_absolute_time(), elapsed = when - before_when, elapsed_ns, busy = 0;
	unsigned count = 0;
	task_t task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		struct proc *proc = get_bsdtask_info(task);
		struct recount_times_mach times;

		if (count == USTACKS_TOP_TASKS || proc == NULL) {
			continue;
		}
		times = recount_task_times(task);
		now[count].pid = proc_pid(proc);
		now[count].total = times.rtm_user + times.rtm_system;
		strlcpy(now[count].name, proc_name_address(proc), sizeof(now[count].name));
		count++;
	}
	lck_mtx_unlock(&tasks_threads_lock);

	/* a process that was not there the last time has used all it has since */
	for (unsigned i = 0; i < count; i++) {
		uint64_t earlier = 0;

		for (unsigned j = 0; j < before_count; j++) {
			if (before[j].pid == now[i].pid) {
				earlier = before[j].total;
				break;
			}
		}
		now[i].used = now[i].total - MIN(earlier, now[i].total);
		busy += now[i].used;
	}
	absolutetime_to_nanoseconds(elapsed, &elapsed_ns);
	ut_say("top: in the last %llu ms the processor was busy %llu%% of the time\n", elapsed_ns / NSEC_PER_MSEC,
	    elapsed ? busy * 100 / elapsed : 0);
	for (unsigned i = 0; i < count; i++) {
		before[i].pid = now[i].pid;
		before[i].total = now[i].total;
	}
	before_count = count;
	before_when = when;
	for (unsigned line = 0; line < USTACKS_TOP_LINES && count != 0 && elapsed != 0; line++) {
		unsigned most = 0;

		for (unsigned i = 1; i < count; i++) {
			if (now[i].used > now[most].used) {
				most = i;
			}
		}
		if (now[most].used * 1000 / elapsed == 0) {
			break;
		}
		ut_say("top:  %3llu.%llu%%  %s[%d]\n", now[most].used * 100 / elapsed, now[most].used * 1000 / elapsed % 10,
		    now[most].name, now[most].pid);
		now[most].used = 0;
	}
}

static void
ustacks_fire(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static task_t found[8];
	unsigned count = 0;
	task_t task;

	if (strcmp(ustacks_name, "top") == 0) {
		ustacks_top();
		goto again;
	}
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		struct proc *proc = get_bsdtask_info(task);

		if (count < 8 && proc != NULL && strncmp(proc_name_address(proc), ustacks_name, sizeof(ustacks_name)) == 0) {
			task_reference(task);
			found[count++] = task;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	if (count == 0) {
		ut_say("ustacks: no process called %s\n", ustacks_name);
	}
	for (unsigned i = 0; i < count; i++) {
		struct proc *proc = get_bsdtask_info(found[i]);

		if (proc != NULL) {
			ustacks_task(found[i], proc, false);
		}
		task_deallocate(found[i]);
	}
again:
	if (ustacks_count != 0 && --ustacks_count == 0) {
		return;
	}
	if (ustacks_every != 0) {
		uint64_t deadline;

		clock_interval_to_deadline(ustacks_every, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(ustacks_call, deadline);
	}
}

/* The same at once, for whoever asks (a debugging key sequence, see drivers/virtio/hakios.virtio_input.c). */
void ustacks_now(const char *name);

void
ustacks_now(const char *name)
{
	strlcpy(ustacks_name, name, sizeof(ustacks_name));
	if (ustacks_call == NULL) {
		ustacks_call = thread_call_allocate(ustacks_fire, NULL);
	}
	thread_call_enter(ustacks_call);
}

static void
ustacks_init(void)
{
	uint32_t at = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("ustacks", ustacks_name, sizeof(ustacks_name)) ||
	    !PE_parse_boot_argn("ustacks_at", &at, sizeof(at)) || at == 0) {
		return;
	}
	PE_parse_boot_argn("ustacks_ms", &ustacks_every, sizeof(ustacks_every));
	PE_parse_boot_argn("ustacks_threads", &ustacks_threads, sizeof(ustacks_threads));
	PE_parse_boot_argn("ustacks_tname", ustacks_tname, sizeof(ustacks_tname));
	PE_parse_boot_argn("ustacks_count", &ustacks_count, sizeof(ustacks_count));
	ustacks_call = thread_call_allocate(ustacks_fire, NULL);
	clock_interval_to_deadline(at, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(ustacks_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, ustacks_init);

/*
 * kern.hakios.ksample (boot-arg ksample=N): where one thread is while it runs, a sample every ksample_us (boot-arg,
 * default 1000) microseconds. ustacks above shows where a thread last came into the kernel, which for a thread that
 * computes is nowhere useful; this stops the processor it runs on (an IPI, cpu_xcall()), and there, with the thread
 * interrupted, reads its registers and walks its frame pointers through the hardware's own translation of its
 * addresses (mmu_uvtop(): no fault can be taken in an interrupt), so what is printed is where it was at that moment.
 * Only while the thread runs: a thread that waits costs nothing and prints nothing.
 *   1  SpringBoard's main thread
 *   2  backboardd's render thread (the thread whose name has "render" in it)
 *   N  (above 2) the main thread of process N
 * One line a sample, "tl: k US CPU U|K PC LR RET...": microseconds of mach_absolute_time (as the other "tl:" lines),
 * the processor, whether the thread was in user space (U) or in the kernel (K: the PC is then where it came in), then
 * the addresses: inside the shared cache as an offset from the cache's unslid base 0x180000000 (so "ipsw dyld a2s CACHE
 * 0x180000000+OFFSET" names it), others whole with an "@" before them. "tl: K NAME PID THREAD slide S" says whose.
 */
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <arm/cpu_data_internal.h>

extern pmap_paddr_t mmu_uvtop(vm_offset_t va);
extern kern_return_t cpu_xcall(int cpu_number, broadcastFunc func, void *param);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

#define KSAMPLE_FRAMES  18
#define KSAMPLE_CACHE   0x180000000ULL

static TUNABLE(uint32_t, ksample_us, "ksample_us", 1000);
static uint32_t ksample_mode;
static int ksample_wakeup;

struct ksample {
	thread_t thread;
	volatile int done;              /* 1: taken, 2: the thread was not there */
	bool user;
	uint64_t pc[KSAMPLE_FRAMES];
	unsigned count;
};

/* a user word through the translation in force on this processor (the interrupted thread's), or false */
static bool
ksample_word(uint64_t va, uint64_t *value)
{
	pmap_paddr_t pa;

	if (!plausible_user_address(va) || (va & 7) != 0) {
		return false;
	}
	pa = mmu_uvtop((vm_offset_t)va);
	if (pa == 0 || !pmap_valid_address(pa)) {
		return false;
	}
	*value = *(volatile uint64_t *)phystokv(pa);
	return true;
}

/* on the thread's processor, in the IPI */
static void
ksample_take(void *arg)
{
	struct ksample *s = arg;
	thread_t thread = current_thread();
	arm_saved_state_t *state, *interrupted = getCpuDatap()->cpu_int_state;
	uint64_t fp, frame[2];

	if (thread != s->thread || (state = get_user_regs(thread)) == NULL || !is_saved_state64(state)) {
		s->done = 2;
		return;
	}
	s->user = interrupted != NULL && PSR64_IS_USER(get_saved_state_cpsr(interrupted));
	s->count = 0;
	s->pc[s->count++] = get_saved_state_pc(state);
	s->pc[s->count++] = get_saved_state_lr(state);
	fp = get_saved_state_fp(state);
	while (s->count < KSAMPLE_FRAMES && ksample_word(fp, &frame[0]) && ksample_word(fp + 8, &frame[1])) {
		s->pc[s->count++] = frame[1];
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	s->done = 1;
}

/* the thread to watch for this mode, referenced; and its task's shared cache slide */
static thread_t
ksample_find(uint32_t mode, uint32_t *slide)
{
	thread_t found = THREAD_NULL, thread;
	task_t task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		struct proc *proc = get_bsdtask_info(task);
		const char *name = proc ? proc_name_address(proc) : "";

		if (proc == NULL || !(mode == 1 ? strcmp(name, "SpringBoard") == 0 : mode == 2 ? strcmp(name, "backboardd") == 0 :
		    proc_pid(proc) == (int)mode)) {
			continue;
		}
		task_lock(task);
		queue_iterate(&task->threads, thread, thread_t, task_threads) {
			char tname[64] = "";

			if (mode == 2) {
				if (get_bsdthread_info(thread) == NULL) {
					continue;
				}
				bsd_getthreadname(get_bsdthread_info(thread), tname);
				tname[63] = 0;
				if (strnstr(tname, "render", sizeof(tname)) == NULL) {
					continue;
				}
			}
			found = thread;
			thread_reference(found);
			break;
		}
		task_unlock(task);
		if (found != THREAD_NULL) {
			vm_shared_region_t region = vm_shared_region_get(task);

			*slide = 0;
			if (region != NULL) {
				*slide = region->sr_slide;
				vm_shared_region_deallocate(region);
			}
			hakios_tl("tl: K %s %d 0x%llx slide 0x%x\n", name, proc_pid(proc), thread_tid(found), *slide);
		}
		break;
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

static void
ksample_line(const struct ksample *s, uint64_t when, int cpu, uint32_t slide)
{
	char line[192];
	uint64_t base = KSAMPLE_CACHE + slide, us;
	int at;

	absolutetime_to_nanoseconds(when, &us);
	us /= 1000;
	at = snprintf(line, sizeof(line), "tl: k %llu %d %c", us, cpu, s->user ? 'U' : 'K');
	for (unsigned i = 0; i < s->count && at < (int)sizeof(line) - 16; i++) {
		uint64_t a = s->pc[i] & 0x0000000fffffffffULL;  /* (no pointer authentication on this processor; a mask anyway) */

		if (a >= base && a < base + 0x100000000ULL) {
			at += snprintf(line + at, sizeof(line) - at, " %llx", a - base);
		} else {
			at += snprintf(line + at, sizeof(line) - at, " @%llx", a);
		}
	}
	snprintf(line + at, sizeof(line) - at, "\n");
	hakios_tl("%s", line);
}

static void
ksample_thread(__unused void *param, __unused wait_result_t wr)
{
	static struct ksample sample;
	thread_t target = THREAD_NULL;
	uint32_t mode = 0, slide = 0;
	uint64_t looked = 0, every, refind;

	nanoseconds_to_absolutetime((uint64_t)MAX(ksample_us, 200) * NSEC_PER_USEC, &every);
	nanoseconds_to_absolutetime(2 * NSEC_PER_SEC, &refind);
	for (;;) {
		uint64_t now = mach_absolute_time(), deadline;
		processor_t processor;

		if (ksample_mode != mode || (target == THREAD_NULL && ksample_mode != 0 && now - looked > refind)) {
			if (target != THREAD_NULL) {
				thread_deallocate(target);
				target = THREAD_NULL;
			}
			mode = ksample_mode;
			looked = now;
			if (mode != 0) {
				target = ksample_find(mode, &slide);
				if (target == THREAD_NULL) {
					hakios_tl("tl: K none for %u\n", mode);
				}
			}
		}
		if (mode == 0) {
			assert_wait((event_t)&ksample_wakeup, THREAD_UNINT);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		if (target != THREAD_NULL && (target->state & TH_TERMINATE)) {
			thread_deallocate(target);
			target = THREAD_NULL;
		}
		processor = target != THREAD_NULL ? target->last_processor : PROCESSOR_NULL;
		if (processor != PROCESSOR_NULL && processor != current_processor() && processor->active_thread == target &&
		    (target->state & TH_RUN)) {
			uint64_t until = now + every / 2;

			sample.thread = target;
			sample.done = 0;
			if (cpu_xcall(processor->cpu_id, ksample_take, &sample) == KERN_SUCCESS) {
				while (sample.done == 0 && mach_absolute_time() < until) {
					;
				}
				if (sample.done == 1) {
					ksample_line(&sample, now, processor->cpu_id, slide);
				}
			}
		}
		deadline = now + every;
		assert_wait_deadline((event_t)&ksample_wakeup, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* kern.hakios.ksample (MiniIOMobileFramebuffer.cpp, which has the frames' sysctls): the mode now, and a new one */
int hakios_ksample_get(void);
bool hakios_ksample_set(int value);

int
hakios_ksample_get(void)
{
	return (int)ksample_mode;
}

bool
hakios_ksample_set(int value)
{
	static bool started;

	if (value < 0) {
		return false;
	}
	if (!started) {
		thread_t thread;

		if (kernel_thread_start_priority(ksample_thread, NULL, MAXPRI_KERNEL, &thread) != KERN_SUCCESS) {
			return false;
		}
		thread_deallocate(thread);
		started = true;
	}
	ksample_mode = (uint32_t)value;
	thread_wakeup((event_t)&ksample_wakeup);
	return true;
}

static void
ksample_init(void)
{
	uint32_t mode = 0;

	if (PE_parse_boot_argn("ksample", &mode, sizeof(mode)) && mode != 0) {
		(void)hakios_ksample_set((int)mode);
	}
}
STARTUP(LOCKDOWN, STARTUP_RANK_LAST, ksample_init);
