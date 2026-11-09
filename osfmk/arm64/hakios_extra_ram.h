/*
 * What osfmk/arm64/arm_vm_init.c includes after the ptov table: the boot loader's extra RAM (arm/pmap/pmap.h has its type), read from the
 * device tree in arm_vm_init(), and given to the VM in arm_vm_prot_finalize().
 */

/*
 * HakiOS (patch 1512): the boot loader's extra RAM beside the window (arm/pmap/pmap.h). Read once from the device tree in
 * arm_vm_init(), before anything is sized from the memory there is; boot-arg extra_ram=0 leaves it out.
 */
SECURITY_READ_ONLY_LATE(hakios_extra_ram_t) hakios_extra_ram[HAKIOS_EXTRA_RAM_MAX];
SECURITY_READ_ONLY_LATE(unsigned int) hakios_extra_ram_count = 0;
SECURITY_READ_ONLY_LATE(unsigned int) hakios_extra_ram_pages = 0;
SECURITY_READ_ONLY_LATE(static unsigned int) hakios_extra_ram_refused = 0;

static void
hakios_extra_ram_init(void)
{
	DTEntry chosen;
	const uint64_t *ranges;
	unsigned int size = 0, first_pai = (unsigned int)atop(mem_size);
	uint32_t on = 1;

	if (PE_parse_boot_argn("extra_ram", &on, sizeof(on)) && on == 0) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hakios-extra-ram", (void const **)&ranges, &size) != kSuccess) {
		return;
	}
	for (unsigned int i = 0; i + 1 < size / sizeof(uint64_t) && hakios_extra_ram_count < HAKIOS_EXTRA_RAM_MAX; i += 2) {
		pmap_paddr_t pa = round_page(ranges[i]), end = trunc_page(ranges[i] + ranges[i + 1]);
		bool ok = end > pa && ranges[i] + ranges[i + 1] > ranges[i];

		/* never the window, nor another range */
		if (ok && pa < gPhysBase + real_phys_size && end > gPhysBase) {
			ok = false;
		}
		for (unsigned int j = 0; ok && j < hakios_extra_ram_count; j++) {
			ok = !(pa < hakios_extra_ram[j].end && end > hakios_extra_ram[j].pa);
		}
		if (!ok) {
			hakios_extra_ram_refused++;     /* (said in arm_vm_prot_finalize(): too early for printf here) */
			continue;
		}
		hakios_extra_ram[hakios_extra_ram_count].pa = pa;
		hakios_extra_ram[hakios_extra_ram_count].end = end;
		hakios_extra_ram[hakios_extra_ram_count].pai = first_pai + hakios_extra_ram_pages;
		hakios_extra_ram_pages += (unsigned int)atop(end - pa);
		hakios_extra_ram_count++;
	}
}

/* [pa, pa + len) is all in one of the extra ranges */
static bool
hakios_extra_ram_covers(pmap_paddr_t pa, pmap_paddr_t len)
{
	for (unsigned int i = 0; i < hakios_extra_ram_count; i++) {
		if (pa >= hakios_extra_ram[i].pa && pa + len <= hakios_extra_ram[i].end) {
			return true;
		}
	}
	return false;
}

/* [pa, pa + len) meets one of the extra ranges */
static bool
hakios_extra_ram_meets(pmap_paddr_t pa, pmap_paddr_t len)
{
	for (unsigned int i = 0; i < hakios_extra_ram_count; i++) {
		if (pa < hakios_extra_ram[i].end && pa + len > hakios_extra_ram[i].pa) {
			return true;
		}
	}
	return false;
}


/*
 * The extra RAM to the VM: arm_vm_prot_finalize() calls this.
 */
static void
hakios_extra_ram_to_vm(void)
{
	/*
	 * The boot loader's extra RAM to the VM, its pages made as vm_page_create() makes those of
	 * ml_static_mfree(), now that the zone they come from is up. (Before bsd_init() and vm_pageout(), so that jetsam's
	 * and the pageout daemon's levels count it.)
	 */
	for (unsigned int i = 0; i < hakios_extra_ram_count; i++) {
		vm_page_create((ppnum_t)atop(hakios_extra_ram[i].pa), (ppnum_t)atop(hakios_extra_ram[i].end));
		/*
		 * vm_page_bootstrap() counted every page of max_mem that was not free then as wired, these with them: they are
		 * not (seen on the phone, 3 November: "wired" 1.1 GB up with them). As ml_static_mfree() says of its pages.
		 */
		vm_page_lockspin_queues();
		vm_page_wire_count -= (unsigned int)atop(hakios_extra_ram[i].end - hakios_extra_ram[i].pa);
		vm_page_wire_count_initial -= (unsigned int)atop(hakios_extra_ram[i].end - hakios_extra_ram[i].pa);
		vm_page_unlock_queues();
		printf("hakios: extra RAM 0x%llx..0x%llx (%llu MB) given to the VM\n", (unsigned long long)hakios_extra_ram[i].pa,
		    (unsigned long long)hakios_extra_ram[i].end - 1,
		    (unsigned long long)(hakios_extra_ram[i].end - hakios_extra_ram[i].pa) >> 20);
	}
	if (hakios_extra_ram_refused != 0) {
		printf("hakios: extra RAM: %u of the boot loader's ranges refused (the window's, or each other's)\n",
		    hakios_extra_ram_refused);
	}
	if (hakios_extra_ram_count != 0) {
		printf("hakios: extra RAM: %u pages (%llu MB) in %u ranges beside the window 0x%llx + 0x%llx; max_mem 0x%llx\n",
		    hakios_extra_ram_pages, (unsigned long long)ptoa((uint64_t)hakios_extra_ram_pages) >> 20, hakios_extra_ram_count,
		    (unsigned long long)gPhysBase, (unsigned long long)mem_size, (unsigned long long)max_mem);
	}
}
