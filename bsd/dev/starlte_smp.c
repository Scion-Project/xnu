/*
 * STARLTE (Galaxy S9, Exynos 9810) secondary-CPU survey, boot-arg
 * starsmpprobe=1 only. The starlte DTB lists the Cortex-A55 cores as reg
 * 0x0..0x3 and the Mongoose M3 cores as 0x100..0x103, but the boot core
 * reads MPIDR_EL1 0x81000000 (MT set), the DynamIQ-style layout in which
 * the core number is in Aff1. Before describing more CPUs to XNU, ask the
 * firmware: PSCI_VERSION, AFFINITY_INFO for candidate MPIDRs, and, if
 * AFFINITY_INFO is not supported (PSCI 0.1 DTB), CPU_ON with a stub that
 * stores the started core's own MPIDR and parks it in WFE (until reboot).
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <sys/proc_internal.h>
#include <sys/bsdtask_info.h>
#include <machine/machine_routines.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>     /* 11417: kmem_alloc_contig, KMA_* */
#include <vm/pmap.h>
#include <pexpert/pexpert.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
void starlte_smp_probe(void);
void starlte_brand_banner(bool full);

/* Project Starbite: the console's banner (ASCII only). */
void
starlte_brand_banner(bool full)
{
	if (!full) {
		printf("[ PROJECT STARBITE | multi-boot for the Galaxy S9 ]\n");
		return;
	}
	printf("==========================================================================\n");
	printf("  PROJECT STARBITE - iOS 16 on a Galaxy S9 (starlte, with a bite out of it)\n");
	printf("  Multi-boot for the Galaxy S9: iOS, macOS, Windows, Android, Linux.\n");
	printf("  Bootloader, drivers and OS ports by the Starbite project.\n");
	printf("  github.com/ (Project Starbite)\n");
	printf("==========================================================================\n");
}

static uint64_t
st_smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	        "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
	        "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * PSCI 0.1 CPU_ON passes no context id (x0 is undefined at entry), so the
 * stub finds its mailbox PC-relative (build/stub/park.S, linked at 0):
 *   mrs x1, mpidr_el1; ubfx x2, x1, #8, #4; ubfx x3, x1, #0, #4;
 *   add x2, x3, x2, lsl #4; adr x0, _start + 0x800;
 *   str x1, [x0, x2, lsl #3]; dsb sy; 1: wfe; b 1b
 */
static const uint32_t st_park[] = {
	0xd53800a1, 0xd3482c22, 0xd3400c23, 0x8b021062,
	0x10003f80, 0xf8227801, 0xd5033f9f, 0xd503205f, 0x17ffffff,
};

/* Clock of the calling core: PMU cycle counter against the 26 MHz timer. */
void starlte_cpu_clock_report(void);

void
starlte_cpu_clock_report(void)
{
	uint64_t pmcr, t0, t1, c0, c1, mpidr;
	__asm__ volatile ("mrs %0, pmcr_el0" : "=r"(pmcr));
	__asm__ volatile ("msr pmcr_el0, %0" : : "r"(pmcr | 1u));   /* E */
	__asm__ volatile ("msr pmcntenset_el0, %0" : : "r"(1ull << 31));
	__asm__ volatile ("isb; mrs %0, cntvct_el0; mrs %1, pmccntr_el0" : "=r"(t0), "=r"(c0));
	delay(20000);
	__asm__ volatile ("isb; mrs %0, cntvct_el0; mrs %1, pmccntr_el0" : "=r"(t1), "=r"(c1));
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
	uint64_t ticks = t1 - t0;
	printf("STARLTE CPU: mpidr %#llx runs at ~%llu MHz (%llu cycles in %llu timer ticks)\n",
	    mpidr, ticks ? (c1 - c0) * 26 / ticks : 0, c1 - c0, ticks);
}

/*
 * What each core really runs at (nothing here sets CPU clocks: they are as S-Boot left them), 30 s
 * after boot when all eight are up: bound to each processor in turn, a chain of dependent adds (one
 * a cycle on the A55 and the M3 alike) timed with mach_absolute_time. The PMU line above reads a
 * cycle counter nobody enabled and says nothing.
 */
extern processor_t thread_bind(processor_t processor);   /* (sched_prim.h: MACH_KERNEL_PRIVATE only) */

static void st_top_a64e(uint64_t window_ns);
extern unsigned int vm_page_free_count, vm_page_wire_count, vm_page_active_count, vm_page_inactive_count;
extern uint32_t c_segment_pages_compressed, c_segment_pages_compressed_nearing_limit;
extern uint64_t get_task_phys_footprint(task_t);
static void st_top_mem(void);

/*
 * Which processes use the CPU (the S7 and S9 ports found a Bluetooth audio plug-in polling a
 * missing daemon at 300-460% of the CPUs): every 10 s, each process's CPU time (task, user +
 * system) against the last snapshot, the eight busiest as a percentage of one core:
 * "STARLTE TOP (10 s, all 812%): backboardd[60] 95% ...". Boot-arg startop=0: off.
 */
#define ST_TOP_PROCS 640
static struct st_top_entry {
	int pid;
	uint64_t ticks;
	uint64_t footprint;
	char name[24];
} st_top_now[ST_TOP_PROCS], st_top_last[ST_TOP_PROCS];
static unsigned st_top_n, st_top_last_n;

static int
st_top_one(proc_t p, __unused void *arg)
{
	task_t task = proc_task(p);
	if (task == TASK_NULL || st_top_n >= ST_TOP_PROCS) {
		return PROC_RETURNED;
	}
	struct proc_taskinfo_internal info;
	bzero(&info, sizeof(info));
	fill_taskprocinfo(task, &info);
	struct st_top_entry *e = &st_top_now[st_top_n++];
	e->pid = proc_pid(p);
	e->ticks = info.pti_total_user + info.pti_total_system;
	e->footprint = get_task_phys_footprint(task);
	strlcpy(e->name, proc_best_name(p), sizeof(e->name));
	return PROC_RETURNED;
}

/* SWEET PROF helper (sweetspeed&16384): name the file mapping a user pc outside the shared region (once per mapping) */
#include <sys/bsdtask_info.h>
#include <sys/vnode.h>
extern int fill_procregioninfo(task_t, uint64_t, struct proc_regioninfo_internal *, uintptr_t *, uint32_t *);
static struct { int pid; uint64_t start, end; } st_prof_maps[256];
static unsigned st_prof_nmaps;
static void
st_prof_region(int pid, uint64_t pc)
{
	for (unsigned i = 0; i < st_prof_nmaps; i++) {
		if (st_prof_maps[i].pid == pid && st_prof_maps[i].start <= pc && pc < st_prof_maps[i].end) {
			return;
		}
	}
	proc_t p = proc_find(pid);
	if (p == PROC_NULL) {
		return;
	}
	task_t task = proc_task(p);
	struct proc_regioninfo_internal ri;
	uintptr_t vp = 0;
	uint32_t vid = 0;
	bzero(&ri, sizeof(ri));
	if (task != TASK_NULL && fill_procregioninfo(task, pc, &ri, &vp, &vid) && ri.pri_address <= pc) {
		char path[160] = "-";
		if (vp != 0 && vnode_getwithvid((vnode_t)vp, vid) == 0) {
			int len = sizeof(path);
			(void)vn_getpath((vnode_t)vp, path, &len);
			vnode_put((vnode_t)vp);
		}
		printf("SWEET PROF map: %d %llx-%llx off %llx %s\n", pid, ri.pri_address, ri.pri_address + ri.pri_size,
		    ri.pri_offset, path);
		if (st_prof_nmaps < 256) {
			st_prof_maps[st_prof_nmaps].pid = pid;
			st_prof_maps[st_prof_nmaps].start = ri.pri_address;
			st_prof_maps[st_prof_nmaps].end = ri.pri_address + ri.pri_size;
			st_prof_nmaps++;
		}
	}
	proc_rele(p);
}

static void
st_top(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("startop", &on, sizeof(on));
	if (!on) {
		return;
	}
	uint64_t last_time = 0;
	extern void sweet_prof_init(void);
	sweet_prof_init();
	for (;;) {
		st_top_n = 0;
		uint64_t now = mach_absolute_time();
		proc_iterate(PROC_ALLPROCLIST, st_top_one, NULL, NULL, NULL);
		if (last_time != 0) {
			uint64_t window_ns = 0;
			absolutetime_to_nanoseconds(now - last_time, &window_ns);
			uint64_t used[ST_TOP_PROCS], all = 0;
			for (unsigned i = 0; i < st_top_n; i++) {
				used[i] = 0;
				for (unsigned j = 0; j < st_top_last_n; j++) {
					if (st_top_last[j].pid == st_top_now[i].pid && st_top_now[i].ticks >= st_top_last[j].ticks) {
						absolutetime_to_nanoseconds(st_top_now[i].ticks - st_top_last[j].ticks, &used[i]);
						break;
					}
				}
				all += used[i];
			}
			char line[512];
			int n = snprintf(line, sizeof(line), "STARLTE TOP (%llu s, all %llu%%):", window_ns / NSEC_PER_SEC,
			    window_ns ? all * 100 / window_ns : 0);
			for (unsigned k = 0; k < 8; k++) {
				unsigned best = ST_TOP_PROCS;
				for (unsigned i = 0; i < st_top_n; i++) {
					if (used[i] != 0 && (best == ST_TOP_PROCS || used[i] > used[best])) {
						best = i;
					}
				}
				if (best == ST_TOP_PROCS || n >= (int)sizeof(line) - 40) {
					break;
				}
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %s[%d] %llu%%", st_top_now[best].name,
				    st_top_now[best].pid, used[best] * 100 / window_ns);
				used[best] = 0;
			}
			printf("%s\n", line);
			st_top_a64e(window_ns);
			extern void sweet_prof_report(uint64_t, void (*)(int, char *, int), void (*)(int, uint64_t));
			sweet_prof_report(window_ns, proc_name, st_prof_region);
			st_top_mem();
		}
		bcopy(st_top_now, st_top_last, sizeof(st_top_now[0]) * st_top_n);
		st_top_last_n = st_top_n;
		last_time = now;
		delay(10 * 1000 * 1000);
	}
}

/*
 * Where the RAM goes (the iOS 26 port runs out of it with two Metal clients): every 30 s, the
 * page queues and the compressor, then the six largest physical footprints:
 * "STARLTE MEM: free 40 wired 600 active 300 inactive 200 compressed 450 (near 700) MiB; top: ...".
 */

static void
st_top_mem(void)
{
	static unsigned tick;
	if (tick++ % 3 != 0) {
		return;
	}
	char line[512];
	int n = snprintf(line, sizeof(line), "STARLTE MEM: free %llu wired %llu active %llu inactive %llu "
	    "compressed %llu (near %llu) MiB; top:", (uint64_t)vm_page_free_count * PAGE_SIZE >> 20,
	    (uint64_t)vm_page_wire_count * PAGE_SIZE >> 20, (uint64_t)vm_page_active_count * PAGE_SIZE >> 20,
	    (uint64_t)vm_page_inactive_count * PAGE_SIZE >> 20, (uint64_t)c_segment_pages_compressed * PAGE_SIZE >> 20,
	    (uint64_t)c_segment_pages_compressed_nearing_limit * PAGE_SIZE >> 20);
	bool taken[ST_TOP_PROCS] = { false };
	for (unsigned k = 0; k < 6; k++) {
		unsigned best = ST_TOP_PROCS;
		for (unsigned i = 0; i < st_top_n; i++) {
			if (!taken[i] && (best == ST_TOP_PROCS || st_top_now[i].footprint > st_top_now[best].footprint)) {
				best = i;
			}
		}
		if (best == ST_TOP_PROCS || n >= (int)sizeof(line) - 40) {
			break;
		}
		taken[best] = true;
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s[%d] %llu", st_top_now[best].name,
		    st_top_now[best].pid, st_top_now[best].footprint >> 20);
	}
	printf("%s\n", line);
}

/* arm64e instructions emulated in the window (osfmk/arm64/sweet_a64e.c), and by whom */
struct sweet_a64e_pid { int pid; uint64_t count; };
extern uint64_t sweet_a64e_counts[8];
extern struct sweet_a64e_pid sweet_a64e_pids[32];

static void
st_top_a64e(uint64_t window_ns)
{
	static uint64_t last[8];
	uint64_t d[8], all = 0;
	for (unsigned i = 0; i < 8; i++) {
		uint64_t c = sweet_a64e_counts[i];
		d[i] = c - last[i];
		last[i] = c;
		all += d[i];
	}
	char line[256];
	int n = snprintf(line, sizeof(line), "STARLTE A64E: %llu emulated (%llu/s): ret %llu br %llu ldra %llu other %llu; by pid:",
	    all, window_ns ? all * NSEC_PER_SEC / window_ns : 0, d[0], d[1], d[2], all - d[0] - d[1] - d[2]);
	for (unsigned k = 0; k < 3; k++) {
		unsigned best = 32;
		for (unsigned i = 0; i < 32; i++) {
			if (sweet_a64e_pids[i].count != 0 && (best == 32 || sweet_a64e_pids[i].count > sweet_a64e_pids[best].count)) {
				best = i;
			}
		}
		if (best == 32) {
			break;
		}
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %d:%llu", sweet_a64e_pids[best].pid,
		    sweet_a64e_pids[best].count);
		sweet_a64e_pids[best].count = 0;
	}
	/* (the rest start again from zero) */
	for (unsigned i = 0; i < 32; i++) {
		sweet_a64e_pids[i].count = 0;
		sweet_a64e_pids[i].pid = 0;
	}
	printf("%s\n", line);

	/* the hottest non-LSE emulated PCs of the window, with their instruction words */
	struct sweet_a64e_site { uint64_t pc, count; uint32_t insn; };
	extern struct sweet_a64e_site sweet_a64e_sites[512];
	char ahot[400];
	int an = snprintf(ahot, sizeof(ahot), "STARLTE A64E hot:");
	for (unsigned k = 0; k < 10; k++) {
		unsigned best = 512;
		for (unsigned i = 0; i < 512; i++) {
			if (sweet_a64e_sites[i].count != 0 &&
			    (best == 512 || sweet_a64e_sites[i].count > sweet_a64e_sites[best].count)) {
				best = i;
			}
		}
		if (best == 512 || an >= (int)sizeof(ahot) - 40) {
			break;
		}
		an += snprintf(ahot + an, sizeof(ahot) - (size_t)an, " %llx:%llu/%08x", sweet_a64e_sites[best].pc,
		    sweet_a64e_sites[best].count, sweet_a64e_sites[best].insn);
		sweet_a64e_sites[best].count = 0;
	}
	if (an > 18) {
		printf("%s\n", ahot);
	}
	bzero(sweet_a64e_sites, sizeof(sweet_a64e_sites));

	/* of which LSE/LRCPC on the Mongoose M3s, and threads pinned to an A55 for another instruction */
	extern uint64_t sweet_lse_counts[4], sweet_m3_pins, sweet_m3_unpins;
	static uint64_t lse_last[4], pins_last;
	uint64_t ld[4];
	for (unsigned i = 0; i < 4; i++) {
		ld[i] = sweet_lse_counts[i] - lse_last[i];
		lse_last[i] = sweet_lse_counts[i];
	}
	if (ld[0] + ld[1] + ld[2] + ld[3] + (sweet_m3_pins - pins_last) != 0) {
		printf("STARLTE LSE (M3): cas %llu casp %llu ldop %llu ldapr %llu in %llu ms; %llu threads pinned to A55 (%llu all, %llu released)\n",
		    ld[0], ld[1], ld[2], ld[3], window_ns / NSEC_PER_MSEC, sweet_m3_pins - pins_last, sweet_m3_pins, sweet_m3_unpins);
	}
	pins_last = sweet_m3_pins;

	/* the hottest trapping PCs of the window */
	struct sweet_lse_site { uint64_t pc, count; };
	extern struct sweet_lse_site sweet_lse_sites[512];
	char hot[400];
	int hn = snprintf(hot, sizeof(hot), "STARLTE LSE hot:");
	for (unsigned k = 0; k < 10; k++) {
		unsigned best = 512;
		for (unsigned i = 0; i < 512; i++) {
			if (sweet_lse_sites[i].count != 0 &&
			    (best == 512 || sweet_lse_sites[i].count > sweet_lse_sites[best].count)) {
				best = i;
			}
		}
		if (best == 512 || hn >= (int)sizeof(hot) - 40) {
			break;
		}
		hn += snprintf(hot + hn, sizeof(hot) - (size_t)hn, " %llx:%llu", sweet_lse_sites[best].pc,
		    sweet_lse_sites[best].count);
		sweet_lse_sites[best].count = 0;
	}
	if (hn > 17) {
		printf("%s\n", hot);
	}
	bzero(sweet_lse_sites, sizeof(sweet_lse_sites));
}

static void
st_cpu_survey(__unused void *arg, __unused wait_result_t wr)
{
	delay(30 * 1000 * 1000);
	/* only the CPUs this boot has (cpus=4: the A55s; cpu_to_processor asserts) */
	for (int cpu = 0; cpu <= ml_get_max_cpu_number() && cpu < 8; cpu++) {
		processor_t p = cpu_to_processor(cpu);
		if (p == PROCESSOR_NULL) {
			continue;
		}
		thread_bind(p);
		thread_block(THREAD_CONTINUE_NULL);
		uint64_t best = UINT64_MAX;
		for (int round = 0; round < 3; round++) {
			uint64_t n = 1000000, x = 0, t0 = mach_absolute_time();
			__asm__ volatile (
				"1:\n"
				"add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
				"add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
				"subs %0, %0, #1\n b.ne 1b\n" : "+r"(n), "+r"(x));
			uint64_t dt = mach_absolute_time() - t0;
			if (dt < best) {
				best = dt;
			}
		}
		uint64_t ns = 0, mpidr;
		absolutetime_to_nanoseconds(best, &ns);
		__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
		printf("STARLTE CPU: cpu %d (mpidr %#llx): %llu MHz by 8M dependent adds\n", cpu, mpidr,
		    ns ? 8000000ull * 1000 / ns : 0);
	}
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
	/*
	 * sweetspeed&65536: the scheduler here is symmetric (no __AMP__), so half of the UI's work lands on
	 * the A55s (half the M3's speed). Recommend only cpu0 (an A55, the boot CPU) and the four M3s: threads go to
	 * the M3s; threads bound to an A55 (FP16 pins, sleh.c) still run there.
	 */
	extern uint32_t sweet_speed_flags(void);
	if ((sweet_speed_flags() & 65536) != 0 && ml_get_max_cpu_number() >= 7) {
		extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
		sched_perfcontrol_update_recommended_cores(0xF1);
		printf("STARLTE CPU: recommended cores 0xf1 (cpu0 + the M3s; sweetspeed&65536)\n");
	}
	st_top();
}

void
starlte_smp_probe(void)
{
	thread_t survey;
	if (kernel_thread_start(st_cpu_survey, NULL, &survey) == KERN_SUCCESS) {
		thread_deallocate(survey);
	}
	uint32_t on = 0;
	PE_parse_boot_argn("starsmpprobe", &on, sizeof(on));
	if (!on) {
		return;
	}
	uint64_t ver = st_smc(0x84000000, 0, 0, 0);
	printf("STARLTE SMP: PSCI_VERSION %#llx\n", ver);

	static const uint64_t cand[] = {
		0x0, 0x1, 0x2, 0x3, 0x100, 0x200, 0x300,
		0x400, 0x500, 0x600, 0x700, 0x10000, 0x10100,
	};
	bool affinity = false;
	for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
		int64_t r = (int64_t)st_smc(0xc4000004, cand[i], 0, 0);
		printf("STARLTE SMP: AFFINITY_INFO(%#llx) = %lld\n", cand[i], r);
		if (r == 0 || r == 1 || r == 2) {
			affinity = true;
		}
	}
	if (affinity) {
		return;
	}

	/* No AFFINITY_INFO: start candidates on the park stub, one at a time. */
	vm_offset_t page;
	if (kmem_alloc_contig(kernel_map, &page, PAGE_SIZE, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("STARLTE SMP: no page\n");
		return;
	}
	uint64_t pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)page));
	memcpy((void *)page, st_park, sizeof(st_park));
	static const uint64_t start[] = { 0x1, 0x2, 0x3, 0x100, 0x101 };
	volatile uint64_t *box = (volatile uint64_t *)(page + 0x800);   /* 256 slots */
	for (unsigned i = 0; i < sizeof(start) / sizeof(start[0]); i++) {
		flush_dcache64((addr64_t)pa, PAGE_SIZE, TRUE);
		int64_t r = (int64_t)st_smc(0xc4000003, start[i], pa, 0);
		delay(300000);
		flush_dcache64((addr64_t)(pa + 0x800), 256 * 8, TRUE);
		printf("STARLTE SMP: CPU_ON(%#llx) = %lld; mailbox:", start[i], r);
		for (unsigned s = 0; s < 256; s++) {
			if (box[s]) {
				printf(" [aff1 %u aff0 %u] %#llx", s >> 4, s & 15u, box[s]);
			}
		}
		printf("\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
