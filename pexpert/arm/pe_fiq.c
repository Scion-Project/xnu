/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
 */

#include <stdint.h>
#if defined(ARM64_BOARD_CONFIG_A5XELTE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include <libkern/section_keywords.h>
#endif
#include <arm64/proc_reg.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>


#if defined(ARM64_BOARD_CONFIG_A5XELTE) || defined(ARM64_BOARD_CONFIG_SWEET)
#if HAS_GIC_V3
#define GICR_WAKE_TIMEOUT_NS (1000000000ULL) // timeout for redistributor wakeup (default 1s)
MACHINE_TIMEOUT(gicr_wake_timeout_ns, "gicr-wake-timeout", GICR_WAKE_TIMEOUT_NS, MACHINE_TIMEOUT_UNIT_NSEC, NULL);

#if defined(SWEET)
/* Phone default; QEMU virt wires the architected virtual timer to INTID 27. */
SECURITY_READ_ONLY_LATE(uint32_t) sweet_vtimer_intid = SWEET_VTIMER_INTID;
#endif

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

static uint32_t
_gic_read32(vm_offset_t addr)
{
	return *((volatile uint32_t *) addr);
}

static uint64_t
_gic_read64(vm_offset_t addr)
{
	return *((volatile uint64_t *) addr);
}

static void
_gic_write32(vm_offset_t addr, uint32_t value)
{
	*((volatile uint32_t *) addr) = value;
}

#define gicd_read32(offset) (_gic_read32(gicd_base + (offset)))
#define gicd_write32(offset, data) (_gic_write32(gicd_base + (offset), (data)))
#define gicr_read32(offset) (_gic_read32(gicr_pe_base + (offset)))
#define gicr_write32(offset, data) (_gic_write32(gicr_pe_base + (offset), (data)))
#define gicr_read64(offset) (_gic_read64(gicr_pe_base + (offset)))

static vm_offset_t
find_gicr_pe_base()
{
	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

	for (vm_offset_t offset = 0; offset < gicr_size; offset += GICR_PE_SIZE) {
		vm_offset_t gicr_pe_base = gicr_base + offset;
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

		if (phys_id == aff_value) {
			return gicr_pe_base;
		}

		if (gicr_typer & GICR_TYPER_LAST) {
			break;
		}
	}

	panic("%s: cannot find GICR base for core %u", __func__, ml_get_cpu_number(phys_id));
}

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	// gicd_base is 0x0 only before it's initialized by the boot processor.
	// Avoid calling SecureDT* routines on secondary processors to avoid
	// race conditions because they are not thread-safe.
	if (!gicd_base) {
		// Find GIC DT node
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}

		// Find "reg" property
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess) {
			panic("%s: cannot find GIC MMIO regions in DT", __func__);
		}

		// Need at least GICD base, GICD size, GICR base and GICR size
		if (prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: incorrect reg property size in GIC DT node; expecting 32 bytes but got %u bytes", __func__, prop_size);
		}

#if defined(SWEET)
		/* Read only on the boot CPU, before the GIC and RO-late lockdown. */
		void const *timer_prop;
		unsigned int timer_size;
		if (SecureDTGetProperty(entry, "virtual-timer-intid", &timer_prop, &timer_size) == kSuccess) {
			if (timer_size != sizeof(uint32_t)) {
				panic("%s: invalid virtual-timer-intid size %u", __func__, timer_size);
			}
			uint32_t intid = *(const uint32_t *)timer_prop;
			if (intid < 16 || intid >= 32) {
				panic("%s: virtual timer INTID %u is not a PPI", __func__, intid);
			}
			sweet_vtimer_intid = intid;
		}
		kprintf("SWEET GIC: virtual timer INTID %u\n", sweet_vtimer_intid);
#endif

		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();

		uint64_t const gicd_base_prop = ((uint64_t const *) prop)[0];
		uint64_t const gicd_size_prop = ((uint64_t const *) prop)[1];
		uint64_t const gicr_base_prop = ((uint64_t const *) prop)[2];
		uint64_t const gicr_size_prop = ((uint64_t const *) prop)[3];

		// Find GICD base address
		gicd_base = ml_io_map(soc_base_phys + gicd_base_prop, gicd_size_prop);

		if (!gicd_base) {
			panic("%s: cannot map GICD region", __func__);
		}

		// Find GICR base address
		gicr_base = ml_io_map(soc_base_phys + gicr_base_prop, gicr_size_prop);

		if (!gicr_base) {
			panic("%s: cannot map GICR region", __func__);
		}

		gicr_size = gicr_size_prop;
	}

	// Find the redistributor for this processor
	vm_offset_t gicr_pe_base = find_gicr_pe_base();

	// Mark this PE to be awake
	uint32_t gicr_waker = gicr_read32(GICR_WAKER);
	if (gicr_waker & GICR_WAKER_CHILDRENASLEEP) {
		gicr_waker &= ~GICR_WAKER_PROCESSORSLEEP;

		gicr_write32(GICR_WAKER, gicr_waker);

		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		while (gicr_read32(GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) {
			// Spin
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				panic("%s: core %u timed out waiting for redistributor to wake up",
				    __func__, ml_get_cpu_number_local());
			}
		}
	}

#if defined(SWEET)
	/*
	 * Real hardware with two security states: Group 0 is TrustZone's and
	 * non-secure writes to it are ignored. Everything we own is Group 1 NS
	 * and arrives as an IRQ (see sleh_irq). GICR_IGROUPR0 reads as zero
	 * from the non-secure side even after this write; delivery still works.
	 */
	gicr_write32(GICR_IGROUPR0, 0xFFFFFFFF);
	for (unsigned int i = 0; i < 32; i += 4) {
		gicr_write32(GICR_IPRIORITYR0 + i, 0xA0A0A0A0);
	}
	gicr_write32(GICR_ISENABLER0, (1U << sweet_vtimer_intid) | (1U << SWEET_IPI_SGI));
#else
	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));
#endif /* SWEET */

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

#if defined(SWEET)
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
#else
	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
#endif /* SWEET */
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

#if defined(SWEET)
	// ABL leaves only ARE_NS set; Linux writes ARE_NS | EnableGrp1A | EnableGrp1
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ARE_NS | GICD_CTLR_ENABLEGRP1A | GICD_CTLR_ENABLEGRP1;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
		;
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#else
	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#endif /* SWEET */
}
#else
#if defined(A5XELTE)
/* Exynos7580 GIC-400: distributor and CPU interface, both non-secure Group 1. */
static vm_offset_t a5_gicd_base;
static vm_offset_t a5_gicc_base;

static uint32_t
a5_gic_read32(vm_offset_t base, unsigned int offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
a5_gic_write32(vm_offset_t base, unsigned int offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

uint32_t
a5xelte_irq_ack(void)
{
	return a5_gic_read32(a5_gicc_base, 0x00c);
}

void
a5xelte_irq_eoi(uint32_t iar)
{
	a5_gic_write32(a5_gicc_base, 0x010, iar);
}

void
a5xelte_send_self_ipi(void)
{
	__builtin_arm_dsb(DSB_ISHST);
	a5_gic_write32(a5_gicd_base, 0xf00, (2U << 24) | 1U);
}

#if defined(HEROLTE)
/* GIC CPU-interface bit of each logical CPU (banked ITARGETSR0). */
static uint8_t a5_gic_cpu_masks[SWEET_SMP_MAX_CPUS];

void
a5xelte_send_ipi(unsigned int logical)
{
	if (logical >= SWEET_SMP_MAX_CPUS || a5_gic_cpu_masks[logical] == 0) {
		panic("herolte: GICv2 target CPU%u unavailable", logical);
	}
	__builtin_arm_dsb(DSB_ISHST);
	a5_gic_write32(a5_gicd_base, 0xf00,
	    ((uint32_t)a5_gic_cpu_masks[logical] << 16) | 1U);
}
#endif

#if defined(STARLTE)
/*
 * A device interrupt (an SPI, INTID 32 to 1019) for a driver of the Galaxy S9, on the GIC-400. Until 17 November 2026 no
 * driver of this tree took one: the timer and the two signals between processors were the only interrupts enabled, and
 * every S9 driver polled. This is the GICv2 half of what Scion's iOS 16.5 kernel does in osfmk/arm64/gic_v3_irq.c
 * (gic_v3_spi_attach): the interrupt is configured (priority as the timer's, level or edge), sent to the boot processor's
 * CPU interface (GICD_ITARGETSR; the boot processor is never switched off), enabled, and its handler called from
 * sleh_irq() in interrupt context, before the interrupt is ended. A handler must quiet its device (a level-triggered
 * line stays up until it does) and wake a thread, no more. The interrupt's group is the secure world's to set (the
 * normal world reads GICD_IGROUPR as zero): S-Boot's monitor is expected to have left the SPIs in Group 1, as Samsung's
 * Linux needs them; starlte_gic_spi_attach() reads the enable bit back and says so if it did not stick.
 * First user: the GPU's job interrupt (bsd/dev/starlte_gpu.c, sgu_irq=1). NOT run on a phone yet.
 */
#define STARLTE_SPI_MOST        8
static struct {
	uint32_t intid;                         /* 0: free */
	uint32_t busy;
	void (*handler)(void *context, uint32_t intid);
	void *context;
	uint64_t count;
} starlte_spi[STARLTE_SPI_MOST];

kern_return_t starlte_gic_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void starlte_gic_spi_detach(uint32_t intid);
uint64_t starlte_gic_spi_count(uint32_t intid);
bool starlte_gic_spi_handle(uint32_t intid);

/* Thread context. KERN_SUCCESS; KERN_FAILURE when the enable bit does not read back (a Group 0 line, the secure world's). */
kern_return_t
starlte_gic_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	if (!a5_gicd_base || intid < 32 || intid >= 1020 || handler == NULL || a5_gic_cpu_masks[0] == 0) {
		return KERN_INVALID_ARGUMENT;
	}
	unsigned slot = STARLTE_SPI_MOST;
	for (unsigned i = 0; i < STARLTE_SPI_MOST; i++) {
		if (__atomic_load_n(&starlte_spi[i].intid, __ATOMIC_ACQUIRE) == intid) {
			return KERN_NAME_EXISTS;
		}
	}
	for (unsigned i = 0; i < STARLTE_SPI_MOST && slot == STARLTE_SPI_MOST; i++) {
		uint32_t free_id = 0;
		if (__atomic_load_n(&starlte_spi[i].busy, __ATOMIC_ACQUIRE) == 0 &&
		    __atomic_compare_exchange_n(&starlte_spi[i].intid, &free_id, 0xffffffffu, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			slot = i;
		}
	}
	if (slot == STARLTE_SPI_MOST) {
		return KERN_RESOURCE_SHORTAGE;
	}
	starlte_spi[slot].handler = handler;
	starlte_spi[slot].context = context;
	starlte_spi[slot].count = 0;

	uint32_t bit = 1U << (intid % 32);
	a5_gic_write32(a5_gicd_base, 0x180 + 4 * (intid / 32), bit);           /* GICD_ICENABLER */
	a5_gic_write32(a5_gicd_base, 0x280 + 4 * (intid / 32), bit);           /* GICD_ICPENDR */
	a5_gic_write32(a5_gicd_base, 0x380 + 4 * (intid / 32), bit);           /* GICD_ICACTIVER */
	*(volatile uint8_t *)(a5_gicd_base + 0x400 + intid) = 0xa0;             /* GICD_IPRIORITYR: as the timer's */
	uint32_t shift = (intid % 16) * 2;
	uint32_t config = a5_gic_read32(a5_gicd_base, 0xc00 + 4 * (intid / 16)) & ~(3U << shift);
	a5_gic_write32(a5_gicd_base, 0xc00 + 4 * (intid / 16), config | ((edge ? 2U : 0U) << shift));  /* GICD_ICFGR */
	*(volatile uint8_t *)(a5_gicd_base + 0x800 + intid) = a5_gic_cpu_masks[0];   /* GICD_ITARGETSR: the boot processor */
	__builtin_arm_dsb(DSB_SY);
	__atomic_store_n(&starlte_spi[slot].intid, intid, __ATOMIC_RELEASE);
	a5_gic_write32(a5_gicd_base, 0x100 + 4 * (intid / 32), bit);           /* GICD_ISENABLER */
	__builtin_arm_dsb(DSB_SY);
	if (!(a5_gic_read32(a5_gicd_base, 0x100 + 4 * (intid / 32)) & bit)) {
		starlte_gic_spi_detach(intid);
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

/* Disabled, nothing pending, and no handler of it running when this returns. */
void
starlte_gic_spi_detach(uint32_t intid)
{
	if (!a5_gicd_base || intid < 32 || intid >= 1020) {
		return;
	}
	uint32_t bit = 1U << (intid % 32);
	a5_gic_write32(a5_gicd_base, 0x180 + 4 * (intid / 32), bit);
	a5_gic_write32(a5_gicd_base, 0x280 + 4 * (intid / 32), bit);
	__builtin_arm_dsb(DSB_SY);
	for (unsigned i = 0; i < STARLTE_SPI_MOST; i++) {
		if (__atomic_load_n(&starlte_spi[i].intid, __ATOMIC_ACQUIRE) == intid) {
			__atomic_store_n(&starlte_spi[i].intid, 0, __ATOMIC_RELEASE);
			while (__atomic_load_n(&starlte_spi[i].busy, __ATOMIC_ACQUIRE) != 0) {
				/* a handler of it on another processor is finishing */
			}
		}
	}
}

/* How many have arrived since it was attached. */
uint64_t
starlte_gic_spi_count(uint32_t intid)
{
	for (unsigned i = 0; i < STARLTE_SPI_MOST; i++) {
		if (__atomic_load_n(&starlte_spi[i].intid, __ATOMIC_ACQUIRE) == intid) {
			return starlte_spi[i].count;
		}
	}
	return 0;
}

/* From sleh_irq(), interrupts off, between the acknowledge and the end: true when a driver has it. */
bool
starlte_gic_spi_handle(uint32_t intid)
{
	for (unsigned i = 0; i < STARLTE_SPI_MOST; i++) {
		if (__atomic_load_n(&starlte_spi[i].intid, __ATOMIC_ACQUIRE) == intid) {
			__atomic_fetch_add(&starlte_spi[i].busy, 1, __ATOMIC_ACQUIRE);
			if (__atomic_load_n(&starlte_spi[i].intid, __ATOMIC_RELAXED) == intid) {
				starlte_spi[i].count++;
				starlte_spi[i].handler(starlte_spi[i].context, intid);
			}
			__atomic_fetch_sub(&starlte_spi[i].busy, 1, __ATOMIC_RELEASE);
			return true;
		}
	}
	return false;
}
#endif /* STARLTE */

void
pe_init_fiq(void)
{
	/* Runs on every CPU. The boot CPU maps the controller before any
	 * secondary starts; SecureDT iterators are not safe on secondaries. */
	if (!a5_gicd_base) {
		DTEntry entry;
		const void *reg;
		unsigned int size;
		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &reg, &size) != kSuccess ||
		    size < 4 * sizeof(uint64_t)) {
			panic("a5xelte: missing GICv2 reg in ADT");
		}
		const uint64_t *r = reg;
		vm_offset_t soc = pe_arm_get_soc_base_phys();
		a5_gicd_base = ml_io_map(soc + r[0], r[1]);
		a5_gicc_base = ml_io_map(soc + r[2], r[3]);
	}
	if (!a5_gicd_base || !a5_gicc_base) {
		panic("a5xelte: cannot map GICv2");
	}
#if defined(HEROLTE)
	unsigned int logical = ml_get_cpu_number_local();
	uint8_t mask = a5_gic_read32(a5_gicd_base, 0x800) & 0xff;
	if (logical >= SWEET_SMP_MAX_CPUS || mask == 0 || (mask & (mask - 1)) != 0) {
		panic("herolte: invalid GICv2 CPU%u target mask 0x%x", logical, mask);
	}
	a5_gic_cpu_masks[logical] = mask;
#endif
	/* INTID 27 is the virtual timer PPI; SGI 1 is the local self signal. */
	a5_gic_write32(a5_gicd_base, 0x080,
	    a5_gic_read32(a5_gicd_base, 0x080) | (1U << 27) | (1U << 1));
	for (unsigned int i = 0; i < 32; i += 4) {
		a5_gic_write32(a5_gicd_base, 0x400 + i, 0xa0a0a0a0U);
	}
	a5_gic_write32(a5_gicd_base, 0x100, (1U << 27) | (1U << 1));
	a5_gic_write32(a5_gicc_base, 0x004, 0xff); /* GICC_PMR */
	a5_gic_write32(a5_gicc_base, 0x008, 0);    /* GICC_BPR */
	a5_gic_write32(a5_gicc_base, 0x000,
	    a5_gic_read32(a5_gicc_base, 0x000) | 1U);
	a5_gic_write32(a5_gicd_base, 0x000,
	    a5_gic_read32(a5_gicd_base, 0x000) | 1U);
	__builtin_arm_dsb(DSB_SY);
	__builtin_arm_isb(ISB_SY);
}

#if defined(STARLTE)
/*
 * osfmk/arm64/sweet_c2.c, on a core back from a power-down (sweetc2, NOT run on a phone): which of the settings
 * pe_init_fiq() made for this core are gone. bit 0: GICC_CTLR's enable; bit 1: GICC_PMR; bit 2: the banked enables
 * of the virtual timer and SGI 1. The GIC-400 is outside the cores, so the answer is expected to be 0.
 */
uint32_t a5xelte_gic_cpu_lost(void);
uint32_t
a5xelte_gic_cpu_lost(void)
{
	uint32_t lost = 0;
	if (!(a5_gic_read32(a5_gicc_base, 0x000) & 1U)) {
		lost |= 1;
	}
	if ((a5_gic_read32(a5_gicc_base, 0x004) & 0xf0U) != 0xf0U) {
		lost |= 2;
	}
	uint32_t want = (1U << 27) | (1U << 1);
	if ((a5_gic_read32(a5_gicd_base, 0x100) & want) != want) {
		lost |= 4;
	}
	return lost;
}

/*
 * bsd/dev/starlte_warm.c (sweetwarm=, NOT run on a phone): before a warm restart hands the SoC to the loader and the
 * next kernel without a reset, every interrupt is disabled, none is left pending or active, and this core's CPU
 * interface is off; the next kernel's pe_init_fiq() enables the distributor, the interface and what it uses again.
 * GICD_TYPER's ITLinesNumber says how many lines there are. SGIs' pending bits clear through GICD_CPENDSGIR.
 */
void a5xelte_gic_quiesce(void);
void
a5xelte_gic_quiesce(void)
{
	if (!a5_gicd_base || !a5_gicc_base) {
		return;
	}
	unsigned int words = (a5_gic_read32(a5_gicd_base, 0x004) & 0x1fU) + 1;
	for (unsigned int n = 0; n < words; n++) {
		a5_gic_write32(a5_gicd_base, 0x180 + 4 * n, 0xffffffffU);      /* GICD_ICENABLER */
		a5_gic_write32(a5_gicd_base, 0x280 + 4 * n, 0xffffffffU);      /* GICD_ICPENDR */
		a5_gic_write32(a5_gicd_base, 0x380 + 4 * n, 0xffffffffU);      /* GICD_ICACTIVER */
	}
	for (unsigned int n = 0; n < 4; n++) {
		a5_gic_write32(a5_gicd_base, 0xf10 + 4 * n, 0xffffffffU);      /* GICD_CPENDSGIR */
	}
	a5_gic_write32(a5_gicc_base, 0x000, a5_gic_read32(a5_gicc_base, 0x000) & ~1U);   /* GICC_CTLR: off */
	__builtin_arm_dsb(DSB_SY);
}
#endif
#else
void
pe_init_fiq()
{
}
#endif /* A5XELTE */
#endif
#else
#if HAS_GIC_V3
#define GICR_WAKE_TIMEOUT_NS (1000000000ULL) // timeout for redistributor wakeup (default 1s)
MACHINE_TIMEOUT(gicr_wake_timeout_ns, "gicr-wake-timeout", GICR_WAKE_TIMEOUT_NS, MACHINE_TIMEOUT_UNIT_NSEC, NULL);

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

static uint32_t
_gic_read32(vm_offset_t addr)
{
	return *((volatile uint32_t *) addr);
}

static uint64_t
_gic_read64(vm_offset_t addr)
{
	return *((volatile uint64_t *) addr);
}

static void
_gic_write32(vm_offset_t addr, uint32_t value)
{
	*((volatile uint32_t *) addr) = value;
}

#define gicd_read32(offset) (_gic_read32(gicd_base + (offset)))
#define gicd_write32(offset, data) (_gic_write32(gicd_base + (offset), (data)))
#define gicr_read32(offset) (_gic_read32(gicr_pe_base + (offset)))
#define gicr_write32(offset, data) (_gic_write32(gicr_pe_base + (offset), (data)))
#define gicr_read64(offset) (_gic_read64(gicr_pe_base + (offset)))

static vm_offset_t
find_gicr_pe_base()
{
	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

	for (vm_offset_t offset = 0; offset < gicr_size; offset += GICR_PE_SIZE) {
		vm_offset_t gicr_pe_base = gicr_base + offset;
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

		if (phys_id == aff_value) {
			return gicr_pe_base;
		}

		if (gicr_typer & GICR_TYPER_LAST) {
			break;
		}
	}

	panic("%s: cannot find GICR base for core %u", __func__, ml_get_cpu_number(phys_id));
}

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	// gicd_base is 0x0 only before it's initialized by the boot processor.
	// Avoid calling SecureDT* routines on secondary processors to avoid
	// race conditions because they are not thread-safe.
	if (!gicd_base) {
		// Find GIC DT node
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}

		// Find "reg" property
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess) {
			panic("%s: cannot find GIC MMIO regions in DT", __func__);
		}

		// Need at least GICD base, GICD size, GICR base and GICR size
		if (prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: incorrect reg property size in GIC DT node; expecting 32 bytes but got %u bytes", __func__, prop_size);
		}

		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();

		uint64_t const gicd_base_prop = ((uint64_t const *) prop)[0];
		uint64_t const gicd_size_prop = ((uint64_t const *) prop)[1];
		uint64_t const gicr_base_prop = ((uint64_t const *) prop)[2];
		uint64_t const gicr_size_prop = ((uint64_t const *) prop)[3];

		// Find GICD base address
		gicd_base = ml_io_map(soc_base_phys + gicd_base_prop, gicd_size_prop);

		if (!gicd_base) {
			panic("%s: cannot map GICD region", __func__);
		}

		// Find GICR base address
		gicr_base = ml_io_map(soc_base_phys + gicr_base_prop, gicr_size_prop);

		if (!gicr_base) {
			panic("%s: cannot map GICR region", __func__);
		}

		gicr_size = gicr_size_prop;
	}

	// Find the redistributor for this processor
	vm_offset_t gicr_pe_base = find_gicr_pe_base();

	// Mark this PE to be awake
	uint32_t gicr_waker = gicr_read32(GICR_WAKER);
	if (gicr_waker & GICR_WAKER_CHILDRENASLEEP) {
		gicr_waker &= ~GICR_WAKER_PROCESSORSLEEP;

		gicr_write32(GICR_WAKER, gicr_waker);

		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		while (gicr_read32(GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) {
			// Spin
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				panic("%s: core %u timed out waiting for redistributor to wake up",
				    __func__, ml_get_cpu_number_local());
			}
		}
	}

	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
	__builtin_arm_isb(ISB_SY);
}
#else
void
pe_init_fiq()
{
}
#endif
#endif
