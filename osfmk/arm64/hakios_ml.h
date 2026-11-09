/*
 * What osfmk/arm64/machine_routines.c includes (once, before ml_parse_cpu_topology()): the slower cores paired each with a faster one
 * of its own, the processor's idle time for hakios.cpufreq.c, and the numbering of cores whose MPIDR the device tree has one
 * affinity field lower (the Galaxy S9's Cortex-A55s). It reads machine_routines.c's own static state (topology_info).
 * The calls in the file: ml_parse_cpu_topology() reads small_cores= and marks a core the loader calls small,
 *
 *   A core the boot loader calls small (on the phone the Cortex-A55s; the Cortex-A78 the
 *   kernel boots on is about 2.5 times as fast) is to be used only when the faster ones are all
 *   busy: what XNU does with a core that has an L2 access penalty (ml_pair_slow_processors()).
 *   The loader gives it a name of its own because a kernel without that function would make
 *   every such core the sibling of the boot processor, and stop at the assertion that a
 *   processor has only one. Boot-arg small_cores=0: all cores alike, as before; 1: paired as
 *   XNU pairs the two threads of a hyperthreaded core (a secondary kept idle while its primary
 *   runs a real-time thread, and so on); 2 (the default): as cores of their own (see
 *   sched_slow_secondaries in osfmk/kern/sched.h).
 *
 * and ml_processor_register() pairs each core as it registers:
 *
 *   Cores that have a non-zero L2 access penalty compared
 *   to the boot processor should be de-prioritized by the
 *   scheduler, so that threads use the cores with better L2
 *   preferentially. (Each with a faster core of its own, not all with the boot
 *   processor: see ml_choose_slow_core_partners().)
 *
 */

/*
 * The slower cores (an L2 access penalty, or the loader's "small-core"): each is made the SMT
 * sibling ("secondary") of a faster core of its own, and the scheduler then treats it as XNU
 * treats the second thread of a hyperthreaded core: a thread is put on it only when no faster
 * core is idle or running something of lower priority; it looks for work only when none of
 * them is idle; and what runs on it goes back to the run queue at every rescheduling, to be
 * put on a faster core that has come free meanwhile. (XNU itself pairs such a core with the
 * boot processor, which allows one of them; the phone has three: with cpus=8, cores 3, 2 and
 * 1 go with 4, 5 and 6, so that 7, the boot core and the fastest, where real-time threads go
 * first, stays alone: the scheduler idles a secondary while its primary runs a real-time
 * thread.)
 */
static uint32_t small_cores_apart = 2;          /* boot-arg small_cores, read in ml_parse_cpu_topology() (before tunables are) */
static int ml_slow_core_partner[MAX_CPUS];      /* by cpu_id: the faster core it is paired with, -1 if none */

static void
ml_choose_slow_core_partners(void)
{
	bool taken[MAX_CPUS] = { false };

	for (int i = 0; i < MAX_CPUS; i++) {
		ml_slow_core_partner[i] = -1;
	}
	for (int i = 0; i < (int)topology_info.num_cpus; i++) {
		int partner = -1;

		if (topology_info.cpus[i].l2_access_penalty == 0) {
			continue;
		}
		/* the last fast core not yet taken; the boot core only if there is no other */
		for (int j = (int)topology_info.num_cpus - 1; j >= 0; j--) {
			if (topology_info.cpus[j].l2_access_penalty != 0 || taken[j]) {
				continue;
			}
			if (&topology_info.cpus[j] == topology_info.boot_cpu) {
				if (partner < 0) {
					partner = j;
				}
				continue;
			}
			partner = j;
			break;
		}
		if (partner >= 0) {
			taken[partner] = true;
			ml_slow_core_partner[i] = partner;
		}
	}
}

extern void loader_log_line(const char *line);

/* At the registration of each processor: pairs it with its partner, if that one is registered too. */
static void
ml_pair_slow_processors(int cpu_id)
{
	for (int i = 0; i < (int)topology_info.num_cpus; i++) {
		int partner = ml_slow_core_partner[i];
		processor_t slow, fast;
		char line[128];

		if (partner < 0 || (i != cpu_id && partner != cpu_id)) {
			continue;
		}
		slow = processor_array[i];
		fast = processor_array[partner];
		if (slow == PROCESSOR_NULL || fast == PROCESSOR_NULL || slow->processor_primary == fast) {
			continue;
		}
		sched_slow_secondaries = small_cores_apart >= 2;
		processor_set_primary(slow, fast);
		snprintf(line, sizeof(line), "cpu %d (phys id 0x%x) is a slower core: the scheduler uses it after cpu %d (phys id 0x%x)\n",
		    i, topology_info.cpus[i].phys_id, partner, topology_info.cpus[partner].phys_id);
		printf("%s", line);
		loader_log_line(line);          /* (the kernel's own log of a run keeps only its end) */
	}
}

/* How long processor cpu_id has been idle since it started, in mach_absolute_time() units (hakios.cpufreq.c's cpuload). */
bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);

bool
ml_cpu_idle_time(int cpu_id, uint64_t *idle)
{
	struct recount_usage usage = { 0 };

	if (cpu_id < 0 || cpu_id >= MAX_SCHED_CPUS || processor_array[cpu_id] == PROCESSOR_NULL) {
		return false;
	}
	recount_processor_usage(&processor_array[cpu_id]->pr_recount, &usage, idle);
	return true;
}

/*
 * HakiOS (patch 1504): a device tree that numbers the cores as the firmware does, where the cores' own MPIDR has them
 * one affinity field higher. The Galaxy S9's Cortex-A55s sit in a DynamIQ cluster: MT set, the core in Affinity1
 * (core 2 reads 0x81000200). Samsung's device tree, its EL3 and Carbon's PSCI all call that core 2 (Affinity0). Set
 * when the boot processor's MPIDR is in the device tree only that way; read by the reset vector as well (start.s),
 * with the MMU off, so it is cleaned to memory.
 */
uint32_t ml_mpidr_mt_aff1;

uint32_t
ml_phys_id_from_mpidr(uint64_t mpidr)
{
	if (ml_mpidr_mt_aff1 && (mpidr & (1ULL << 24)) != 0) {
		return (uint32_t)((mpidr >> MPIDR_AFF1_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK));
	}
	return (uint32_t)(mpidr & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK));
}

static void
ml_mpidr_numbering(void)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");

	if (ml_get_cpu_number((uint32_t)mpidr) >= 0 || (mpidr & (1ULL << 24)) == 0) {
		return;
	}
	if (ml_get_cpu_number((uint32_t)(mpidr >> MPIDR_AFF1_SHIFT)) >= 0) {
		ml_mpidr_mt_aff1 = 1;
		CleanPoC_DcacheRegion((vm_offset_t)&ml_mpidr_mt_aff1, sizeof(ml_mpidr_mt_aff1));
	}
}
