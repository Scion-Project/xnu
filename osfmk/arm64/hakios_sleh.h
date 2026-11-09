/* included by sleh.c before handle_msr_trap(): EL0's cache maintenance by address, done for the process when SCTLR_EL1.UCI is clear (the Exynos M3s) */
/*
 * HakiOS (Galaxy S9, patch 1511): EL0's cache maintenance by VA, trapped because osfmk/arm/arm_init.c cleared
 * SCTLR_EL1.UCI (hakios_ucache_apply(): the Exynos M3s need new code cleaned from their L1 data cache before they can
 * fetch it, and iOS's sys_icache_invalidate() never cleans). Done here for the process, on the kernel's alias of the
 * page (the caches are physically tagged for data and VIPT without aliases for instructions at 16 KB pages, so the
 * alias reaches the same lines, and a page unmapped meanwhile cannot fault the kernel): DC CVAC (DC CIVAC for a DC
 * CIVAC) of the line, then IC IVAU of it for an IC IVAU. A line that is not mapped, or not RAM, is passed over, as a
 * no-op. Returns false for anything else, which handle_msr_trap() takes as before.
 */
extern uint32_t hakios_ucache_trap;     /* osfmk/arm/arm_init.c, patch 1511 */
extern char *proc_name_address(struct proc *p);
extern int task_pid(task_t task);
uint64_t hakios_ucache_ops;              /* machdep.hakios_ucache_ops (bsd/dev/arm64/sysctl.c) */

/* the ISS of a trapped SYS instruction: Op0 [21:20], Op2 [19:17], Op1 [16:14], CRn [13:10], Rt [9:5], CRm [4:1], dir [0] */
#define HAKIOS_SYS_ISS_MASK     0x003ffc1fu                     /* everything but Rt */
#define HAKIOS_SYS_CACHE(crm)   ((1u << 20) | (1u << 17) | (3u << 14) | (7u << 10) | ((uint32_t)(crm) << 1))

static bool
hakios_user_cache_op(arm_saved_state_t *state, uint32_t esr)
{
	uint32_t iss = ESR_ISS(esr), op = iss & HAKIOS_SYS_ISS_MASK, rt = (iss >> 5) & 0x1f;
	bool icache = false;

	if (!hakios_ucache_trap || !is_saved_state64(state) || PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {
		return false;
	}
	switch (op) {
	case HAKIOS_SYS_CACHE(5):       /* IC IVAU */
		icache = true;
		break;
	case HAKIOS_SYS_CACHE(10):      /* DC CVAC */
	case HAKIOS_SYS_CACHE(11):      /* DC CVAU */
	case HAKIOS_SYS_CACHE(12):      /* DC CVAP */
	case HAKIOS_SYS_CACHE(13):      /* DC CVADP */
	case HAKIOS_SYS_CACHE(14):      /* DC CIVAC */
		break;
	default:
		return false;
	}

	uint64_t va = (rt == 31 ? 0 : get_saved_state_reg(state, rt)) & ~63ULL;
	pmap_paddr_t pa = pmap_find_pa_nofault(vm_map_pmap(current_map()), va);

	if (pa != 0 && pmap_valid_address(pa)) {
		vm_offset_t kva = phystokv(pa) & ~63UL;

		if (op == HAKIOS_SYS_CACHE(14)) {
			__asm__ volatile ("dc civac, %0" : : "r"(kva) : "memory");
		} else {
			__asm__ volatile ("dc cvac, %0" : : "r"(kva) : "memory");
		}
		__builtin_arm_dsb(DSB_ISH);
		if (icache) {
			__asm__ volatile ("ic ivau, %0" : : "r"(kva) : "memory");
			__builtin_arm_dsb(DSB_ISH);
		}
	}
	if (hakios_ucache_ops++ == 0) {     /* machdep.hakios_ucache_ops; the first one said */
		struct proc *proc = get_bsdtask_info(current_task());

		printf("ucache_trap: the first cache maintenance by VA from user space (%s, %s[%d]) done for it, the data cleaned "
		    "to the point of coherency first, for the Exynos M3s (patch 1511)\n", icache ? "IC IVAU" : "DC",
		    proc ? proc_name_address(proc) : "?", task_pid(current_task()));
	}
	add_saved_state_pc(state, 4);
	return true;
}
