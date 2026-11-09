/*
 * Minimal GICv3 support for generic (non-Apple) ARMv8 boards.
 *
 * Open-source XNU has no interrupt controller driver: on Apple hardware that
 * is a kext (AppleInterruptController), and the timer arrives as FIQ. Here
 * everything is a Group 1 IRQ, which is what an EL1 guest gets from a stock
 * hypervisor or from QEMU's virt machine:
 *
 *   - the CPU interface is driven through the ICC_*_EL1 system registers
 *   - the EL1 virtual timer PPI is routed to the kernel's rtclock
 *   - with more than one processor (psci_smp.c), two software-generated
 *     interrupts carry the kernel's signals between them: cpu_signal() and
 *     the order to stop when the system goes back to the boot loader
 *   - a shared peripheral interrupt (SPI, numbers 32 to 1019) is a driver's once it
 *     is attached (gic_v3_spi_attach()): configured, routed to the processor that
 *     attaches it, enabled, and its handler called in interrupt context for each
 *     one that arrives, before it is ended
 *   - any other interrupt that fires is masked and counted, until a real
 *     driver registers a handler for it
 *
 * Every processor has its own CPU interface, redistributor frame and timer,
 * and sets them up for itself (gic_v3_irq_cpu_init(), from PE_init_cpu()).
 *
 * Registers come from the device tree node /arm-io/gic:
 *   reg = <distributor offset, size, redistributor offset, size>
 *
 * A GICv2 (GIC-400: the Exynos 9810 of the Galaxy S9) is the same controller as far as the kernel
 * is concerned, reached another way: its node's compatible is "arm,gic-400" and its reg the
 * distributor and the CPU interface, which is memory (GICC_*) instead of ICC_* registers -- a core
 * whose GIC is a GICv2 may not have those registers at all. There are no redistributors: the
 * distributor's registers for interrupts 0-31 are each processor's own, an SPI is sent to a
 * processor by a bit for its CPU interface (GICD_ITARGETSR) and not by its MPIDR, and so is a
 * software-generated interrupt (GICD_SGIR). Seen from the normal world, the interrupts are those of
 * Group 1 and bit 0 of the control registers turns them on.
 */
#include <mach/mach_types.h>
#include <kern/debug.h>
#include <kern/simple_lock.h>
#include <os/atomic_private.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_cpu.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <string.h>

#if defined(GENERIC_GIC_V3_IRQ)

#define GICD_CTLR               0x0000
#define GICD_CTLR_ENABLE_G1NS   (1u << 1)
#define GICD_CTLR_ARE_NS        (1u << 4)
#define GICD_CTLR_RWP           (1u << 31)
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x0380 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + (n))
#define GICD_ICFGR(n)           (0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n)        (0x0d00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))

#define GICR_FRAME_SIZE         0x20000     /* RD_base + SGI_base per CPU */
#define GICR_FRAME_SIZE_V4      0x40000     /* ... and VLPI_base and a reserved 64 KB, where GICR_TYPER.VLPIS says so */
#define GICR_TYPER              0x0008
#define GICR_TYPER_VLPIS        (1u << 1)
#define GICR_TYPER_LAST         (1u << 4)
#define GICR_WAKER              0x0014
#define GICR_WAKER_PROCSLEEP    (1u << 1)
#define GICR_WAKER_CHILDASLEEP  (1u << 2)
#define GICR_SGI_BASE           0x10000
#define GICR_IGROUPR0           (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0         (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0         (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0           (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0         (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR(n)      (GICR_SGI_BASE + 0x0400 + (n))
#define GICR_IGRPMODR0          (GICR_SGI_BASE + 0x0d00)

/* GICv2 only */
#define GICD_ITARGETSR(n)       (0x0800 + (n))
#define GICD_SGIR               0x0f00
#define GICD_CPENDSGIR(n)       (0x0f10 + 4 * (n))  /* a byte per SGI, a bit per sender: this processor's */
#define GICD_CTLR_ENABLE        (1u << 0)   /* as the normal world sees it: Group 1 */
#define GICC_CTLR               0x0000
#define GICC_CTLR_ENABLE        (1u << 0)
#define GICC_PMR                0x0004
#define GICC_BPR                0x0008
#define GICC_IAR                0x000c
#define GICC_EOIR               0x0010
#define GICC_APR0               0x00d0

#define GIC_SGI_SIGNAL          0           /* cpu_signal(): another processor wants this one's attention */
#define GIC_SGI_STOP            1           /* psci_smp_stop_here(): the system is going back to the boot loader */
#define GIC_INTID_VTIMER        27          /* EL1 virtual timer PPI */
#define GIC_INTID_SPI_MIN       32
#define GIC_INTID_SPURIOUS_MIN  1020
#define GIC_SPI_MOST            48          /* attached at a time */
#define GIC_PRIORITY_DEFAULT    0xa0

static vm_offset_t gicd_base, gicr_base;
static vm_size_t   gicr_size;
static vm_offset_t gicr_cpu_base[MAX_CPUS]; /* each processor's redistributor frame */

/* a GICv2: its CPU interface, and each processor's bit in the distributor's target lists and its MPIDR */
static bool        gic_v2;
static vm_offset_t gicc_base;
static uint8_t     gic_v2_target[MAX_CPUS];
static uint32_t    gic_v2_phys[MAX_CPUS];
/* a GICv3: each processor's GICD_IROUTER value, as gic_v3_spi_attach() writes it (patch 1517: gic_v3_spi_move()) */
static uint64_t    gic_v3_route[MAX_CPUS];

/*
 * Acknowledged, not yet EOI'd; 0: none. The signal between processors is interrupt 0 and reads
 * as 0 from ICC_IAR1_EL1, hence the mark: without it a core switched off in the middle of that
 * signal (which is where every core but one is during a panic) kept it active, and with the
 * active priority still set in its CPU interface the next kernel on that core saw no interrupt
 * at all.
 */
#define GIC_IAR_TAKEN           (1ULL << 63)
static uint64_t gic_active_iar[MAX_CPUS];
uint64_t        gic_unhandled_count;
uint32_t        gic_last_unhandled;

/*
 * The attached SPIs. A slot is filled before its interrupt is enabled and emptied after it has
 * been disabled and no handler of it runs any more (busy), so the handler path reads the table
 * without a lock.
 */
struct gic_spi {
	uint32_t intid;                     /* 0: a free slot */
	void   (*handler)(void *context, uint32_t intid);
	void    *context;
	uint32_t busy;                      /* handlers of it running now */
	uint64_t count;
};
static struct gic_spi gic_spi[GIC_SPI_MOST];
static decl_simple_lock_data(, gic_spi_lock);

void gic_v3_irq_cpu_init(void);
void gic_v3_irq_quiesce(void);
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);
void gic_v3_irq_cpu_shutdown(void);
uint32_t gic_v3_spi_move(int from_cpu, int to_cpu);
void gic_v3_irq_signal(uint32_t phys_id);
void gic_v3_irq_signal_stop(uint32_t phys_id);

static inline uint32_t
gic_read32(vm_offset_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
gic_write32(vm_offset_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

/*
 * A GICv2 keeps a software-generated interrupt pending for each processor that sent it, and only
 * GICD_CPENDSGIR takes that back (GICD_ICPENDR0 does nothing to one); on a GIC-400 they cannot even
 * be disabled. One left pending when the kernel goes back to the loader is UEFI's to take, and
 * UEFI's GIC driver does not survive it.
 */
static void
gic_v2_sgis_clear(void)
{
	for (int n = 0; n < 4; n++) {
		gic_write32(gicd_base + GICD_CPENDSGIR(n), 0xffffffffu);
	}
}

static void
gic_mask(uint32_t intid)
{
	if (intid < 32 && gic_v2) {
		gic_write32(gicd_base + GICD_ICENABLER(0), 1u << intid);    /* this processor's own */
	} else if (intid < 32) {
		gic_write32(gicr_cpu_base[cpu_number()] + GICR_ICENABLER0, 1u << intid);
	} else {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A software-generated interrupt for the processor with that MPIDR (its two
 * low affinity fields, as in the device tree's "reg").
 * ICC_SGI1R_EL1: Aff1 in [23:16], the interrupt in [27:24], and in [15:0] a
 * bit for every Aff0 addressed -- which is why Aff0 has to be below 16.
 */
static void
gic_send_sgi(uint32_t phys_id, uint32_t sgi)
{
	uint64_t aff0 = phys_id & 0xff, aff1 = (phys_id >> 8) & 0xff;

	if (gic_v2) {
		/* GICD_SGIR: the CPU interfaces addressed in [23:16], the interrupt in [3:0] */
		for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
			uint8_t target = os_atomic_load(&gic_v2_target[cpu], acquire);

			if (target != 0 && gic_v2_phys[cpu] == (phys_id & 0xffff)) {
				__builtin_arm_dsb(DSB_ISHST);   /* what the signal is about, seen before it */
				gic_write32(gicd_base + GICD_SGIR, (uint32_t)target << 16 | sgi);
				return;
			}
		}
		return;         /* a processor that has not set up its interface yet: nothing to interrupt */
	}

	__builtin_arm_wsr64("ICC_SGI1R_EL1", ((uint64_t)sgi << 24) | (aff1 << 16) | (1ULL << (aff0 & 15)));
	__builtin_arm_isb(ISB_SY);
}

/* The platform's half of cpu_signal(): interrupt that processor (GenericARMCPU::signalCPU()). */
void
gic_v3_irq_signal(uint32_t phys_id)
{
	gic_send_sgi(phys_id, GIC_SGI_SIGNAL);
}

void
gic_v3_irq_signal_stop(uint32_t phys_id)
{
	gic_send_sgi(phys_id, GIC_SGI_STOP);
}

/* An SPI that a driver has attached: its handler called. false: nobody has it. */
static bool
gic_spi_handle(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		struct gic_spi *spi = &gic_spi[i];

		if (os_atomic_load(&spi->intid, acquire) == intid) {
			os_atomic_inc(&spi->busy, acquire);
			if (os_atomic_load(&spi->intid, relaxed) == intid) {
				spi->count++;
				spi->handler(spi->context, intid);
			}
			os_atomic_dec(&spi->busy, release);
			return true;
		}
	}
	return false;
}

static void
gic_distributor_settle(void)
{
	while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
		/* wait for the change to take effect */
	}
}

void
gic_v3_spi_mask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		gic_distributor_settle();
	}
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A driver takes an SPI: edge- or level-triggered, Group 1, the default priority, routed to the
 * processor this runs on, and enabled. The handler runs in interrupt context, with interrupts off,
 * before the interrupt is ended; for a level-triggered one it has to quiet the source or mask the
 * interrupt (gic_v3_spi_mask()). Thread context.
 */
kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	struct gic_spi *slot = NULL;
	uint32_t config;

	if (gicd_base == 0 || intid < GIC_INTID_SPI_MIN || intid >= GIC_INTID_SPURIOUS_MIN || handler == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	simple_lock(&gic_spi_lock, LCK_GRP_NULL);
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			simple_unlock(&gic_spi_lock);
			return KERN_NAME_EXISTS;
		}
		if (slot == NULL && gic_spi[i].intid == 0 && gic_spi[i].busy == 0) {
			slot = &gic_spi[i];
		}
	}
	if (slot == NULL) {
		simple_unlock(&gic_spi_lock);
		return KERN_RESOURCE_SHORTAGE;
	}
	slot->handler = handler;
	slot->context = context;
	slot->count = 0;
	os_atomic_store(&slot->intid, intid, release);
	simple_unlock(&gic_spi_lock);

	gic_v3_spi_mask(intid);
	gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	gic_write32(gicd_base + GICD_ICACTIVER(intid / 32), 1u << (intid % 32));
	if (!gic_v2) {
		/* (a GICv2's groups are the secure world's to set: the normal world's view of them is read-as-zero) */
		gic_write32(gicd_base + GICD_IGROUPR(intid / 32), gic_read32(gicd_base + GICD_IGROUPR(intid / 32)) | 1u << (intid % 32));
		gic_write32(gicd_base + GICD_IGRPMODR(intid / 32), gic_read32(gicd_base + GICD_IGRPMODR(intid / 32)) & ~(1u << (intid % 32)));
	}
	*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(intid)) = GIC_PRIORITY_DEFAULT;
	config = gic_read32(gicd_base + GICD_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
	gic_write32(gicd_base + GICD_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
	if (gic_v2) {
		/* GICD_ITARGETSR: a byte per interrupt, a bit per CPU interface */
		*(volatile uint8_t *)(gicd_base + GICD_ITARGETSR(intid)) = gic_v2_target[cpu_number()];
	} else {
		/* GICD_IROUTER: Aff3 in [39:32], Aff2.Aff1.Aff0 in [23:0]; bit 31 (any processor) clear */
		*(volatile uint64_t *)(gicd_base + GICD_IROUTER(intid)) = (mpidr & 0xff00000000ull) | (mpidr & 0xffffff);
	}
	gic_distributor_settle();
	gic_v3_spi_unmask(intid);
	return KERN_SUCCESS;
}

/* ... and gives it back: disabled, nothing pending, and no handler of it running when this returns. */
void
gic_v3_spi_detach(uint32_t intid)
{
	gic_v3_spi_mask(intid);
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	}
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			os_atomic_store(&gic_spi[i].intid, 0, release);
			while (os_atomic_load(&gic_spi[i].busy, acquire) != 0) {
				/* a handler of it on another processor is finishing */
			}
		}
	}
}

/* How many have arrived since it was attached. */
uint64_t
gic_v3_spi_count(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			return gic_spi[i].count;
		}
	}
	return 0;
}

static void
gic_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
	int cpu = cpu_number();

	for (;;) {
		/* (a GICv2's says in [12:10] which processor sent a software-generated one: the end must say so too) */
		uint64_t iar = gic_v2 ? gic_read32(gicc_base + GICC_IAR) : __builtin_arm_rsr64("ICC_IAR1_EL1");
		uint32_t intid = (uint32_t)(iar & (gic_v2 ? 0x3ff : 0xffffff));

		if (gic_v2 && intid < 16) {
			/* what the sender wrote before its signal, seen after we took it (Linux's smp_rmb() in arm-gic.c) */
			__builtin_arm_dmb(DMB_ISHLD);
		}

		if (intid >= GIC_INTID_SPURIOUS_MIN) {
			break;
		}
		gic_active_iar[cpu] = iar | GIC_IAR_TAKEN;

		if (intid == GIC_INTID_VTIMER) {
			/* same sequence as the FIQ path in sleh_fiq() */
			getCpuDatap()->cpu_decrementer = -1; /* Large */
			rtclock_intr(TRUE);
		} else if (intid == GIC_SGI_SIGNAL) {
			cpu_signal_handler();
		} else if (intid == GIC_SGI_STOP) {
			/* comes back only if nobody has in fact asked for that */
			psci_smp_stop_here();
		} else if (intid >= GIC_INTID_SPI_MIN && gic_spi_handle(intid)) {
			/* a driver's */
		} else {
			/* nobody owns this interrupt yet: keep it from storming */
			gic_mask(intid);
			gic_unhandled_count++;
			gic_last_unhandled = intid;
		}

		if (gic_v2) {
			gic_write32(gicc_base + GICC_EOIR, (uint32_t)iar);
		} else {
			__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
			__builtin_arm_isb(ISB_SY);
		}
		gic_active_iar[cpu] = 0;
	}
}

/*
 * Leave the controller in a state another environment can take over from:
 * finish any interrupt that this processor acknowledged but did not complete.
 * Used when control goes back to the boot loader (see loader_return.c), and
 * by a processor that is about to be switched off for that (psci_smp.c).
 */
void
gic_v3_irq_quiesce(void)
{
	int cpu;

	if (gicd_base == 0) {
		return;         /* as early as this there is no interrupt, nor a cpu number to ask for */
	}
	cpu = cpu_number();
	/* (what drivers had of the shared interrupts goes with this kernel: none of them may fire under the loader) */
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		uint32_t intid = gic_spi[i].intid;

		if (intid != 0) {
			gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		}
	}
	if (gic_v2) {
		gic_v2_sgis_clear();
	}
	if (gic_active_iar[cpu] != 0) {
		if (gic_v2) {
			gic_write32(gicc_base + GICC_EOIR, (uint32_t)gic_active_iar[cpu]);
		} else {
			__builtin_arm_wsr64("ICC_EOIR1_EL1", gic_active_iar[cpu] & ~GIC_IAR_TAKEN);
			__builtin_arm_isb(ISB_SY);
		}
		gic_active_iar[cpu] = 0;
	}
}

/*
 * Scion (patch 1517): this core is switching itself off and will be started again in this same run, because
 * sleep_cores= is taking it off (its bit is set in hakios_cpu_exit_mask, GenericARMPE.cpp). Forget that its interface
 * was set up, so that gic_v3_irq_cpu_init() sets it up again at its start rather than returning at once (a core started
 * so would take no interrupt, not even the timer's or a signal). Its target bit gone, nobody signals it while it is
 * off. Any other core's shutdown is for the rest of the run, and is left as it was.
 */
static void
gic_v3_irq_cpu_forget(void)
{
	int cpu = cpu_number();

#if defined(GENERIC_PSCI_SMP)
	extern uint32_t hakios_cpu_exit_mask;   /* osfmk/arm/cpu_common.c: the cores sleep_cores= takes off */

	if (cpu < 0 || cpu >= 32 || !(*(volatile uint32_t *)&hakios_cpu_exit_mask & (1u << cpu))) {
		return;                         /* any other core's shutdown is for the rest of the run: as before */
	}
#endif
	if (cpu >= 0 && cpu < MAX_CPUS) {
		os_atomic_store(&gic_v2_target[cpu], 0, release);
		gicr_cpu_base[cpu] = 0;
	}
}

/*
 * This processor is leaving for good: the firmware is about to be asked to switch it off
 * (psci_smp_cpu_off(), and the boot processor in the loader's stub when the kernel has a core of
 * its own). Nothing may be left that could interrupt it. The redistributor outlives the core, and
 * so does what is pending in it: the "stop" signal that reached a core whose interrupts were off
 * (every core is in that state while another one panics), or its timer. A core asked to go with
 * an interrupt pending and the CPU interface still passing it on does not go cleanly, and the
 * next kernel that is started on it hangs.
 */
void
gic_v3_irq_cpu_shutdown(void)
{
	vm_offset_t gicr;

	if (gicd_base == 0) {
		return;
	}
	gic_v3_irq_quiesce();
	if (gic_v2) {
		/* interrupts 0-31 in the distributor are this processor's own */
		gic_write32(gicd_base + GICD_ICENABLER(0), 0xffffffffu);
		gic_write32(gicd_base + GICD_ICPENDR(0), 0xffffffffu);
		gic_write32(gicd_base + GICD_ICACTIVER(0), 0xffffffffu);
		gic_write32(gicc_base + GICC_CTLR, gic_read32(gicc_base + GICC_CTLR) & ~GICC_CTLR_ENABLE);
		gic_v3_irq_cpu_forget();
		return;
	}
	gicr = gicr_cpu_base[cpu_number()];
	if (gicr != 0) {
		gic_write32(gicr + GICR_ICENABLER0, 0xffffffffu);
		gic_write32(gicr + GICR_ICPENDR0, 0xffffffffu);
		gic_write32(gicr + GICR_ICACTIVER0, 0xffffffffu);
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 0);
	__builtin_arm_isb(ISB_SY);
	gic_v3_irq_cpu_forget();
}

/*
 * Scion (patch 1517): every attached SPI routed to processor from_cpu is routed to to_cpu instead: a core about to be
 * taken off (sleep_cores=, GenericARMPE.cpp) gives its devices' interrupts (the side button, the touch, the storage ...)
 * to one that stays. They are not given back when it returns: where an SPI goes changes nothing but which core takes it.
 * How many were moved.
 */
uint32_t
gic_v3_spi_move(int from_cpu, int to_cpu)
{
	uint32_t moved = 0;

	if (gicd_base == 0 || from_cpu < 0 || to_cpu < 0 || from_cpu >= MAX_CPUS || to_cpu >= MAX_CPUS || from_cpu == to_cpu ||
	    (gic_v2 ? gic_v2_target[to_cpu] == 0 : gic_v3_route[to_cpu] == 0)) {
		return 0;
	}
	simple_lock(&gic_spi_lock, LCK_GRP_NULL);
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		uint32_t intid = os_atomic_load(&gic_spi[i].intid, acquire);

		if (intid == 0) {
			continue;
		}
		if (gic_v2) {
			volatile uint8_t *target = (volatile uint8_t *)(gicd_base + GICD_ITARGETSR(intid));

			if (gic_v2_target[from_cpu] != 0 && *target == gic_v2_target[from_cpu]) {
				*target = gic_v2_target[to_cpu];
				moved++;
			}
		} else {
			volatile uint64_t *route = (volatile uint64_t *)(gicd_base + GICD_IROUTER(intid));

			if (gic_v3_route[from_cpu] != 0 && *route == gic_v3_route[from_cpu]) {
				*route = gic_v3_route[to_cpu];
				moved++;
			}
		}
	}
	simple_unlock(&gic_spi_lock);
	gic_distributor_settle();
	return moved;
}

static vm_offset_t
gic_find_redistributor(void)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	/* GICR_TYPER[63:32] packs Aff3.Aff2.Aff1.Aff0 */
	uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));

	/*
	 * A redistributor that can take virtual LPIs (GICv4: GICR_TYPER.VLPIS) is four 64 KB frames, not two
	 * (Arm's GIC architecture specification, IHI 0069, "Redistributor memory map"): the Google Tensor G5's
	 * GIC is laid out so, 2 MB of redistributors for 8 cores (devices/frankel/README.md).
	 */
	vm_offset_t step = GICR_FRAME_SIZE;
	for (vm_offset_t frame = gicr_base; frame + GICR_FRAME_SIZE <= gicr_base + gicr_size; frame += step) {
		uint64_t typer = *(volatile uint64_t *)(frame + GICR_TYPER);

		if ((uint32_t)(typer >> 32) == affinity) {
			return frame;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
		step = (typer & GICR_TYPER_VLPIS) ? GICR_FRAME_SIZE_V4 : GICR_FRAME_SIZE;
	}
	panic("gic: no redistributor for MPIDR 0x%llx", mpidr);
}

/*
 * The same for a GICv2, whose CPU interface is memory and whose distributor holds each processor's
 * own interrupts 0-31: which bit is this processor's in the distributor's target lists is what that
 * register says when this processor reads it (GICD_ITARGETSR0 to 7, read-only, each processor's own).
 */
static void
gic_v2_cpu_init(int cpu)
{
	uint32_t mine = 1u << GIC_INTID_VTIMER;
	uint8_t target = *(volatile uint8_t *)(gicd_base + GICD_ITARGETSR(0));

	if (target == 0) {
		/* (a GIC with a single CPU interface may say nothing: it is the only one) */
		target = 1;
	}
	gic_v2_phys[cpu] = ml_phys_id_from_mpidr(__builtin_arm_rsr64("MPIDR_EL1"));    /* as phys_id has it (patch 1504) */

	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write32(gicd_base + GICD_ICACTIVER(0), sgis);
		gic_v2_sgis_clear();
		*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
		*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
		mine |= sgis;
	}
	*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
	gic_write32(gicd_base + GICD_ISENABLER(0), mine);

	uint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);
	if (!(ctlr & GICD_CTLR_ENABLE)) {
		gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ENABLE);
	}

	/* the CPU interface: as for a GICv3 (see below), through its registers in memory */
	gic_write32(gicc_base + GICC_APR0, 0);
	gic_write32(gicc_base + GICC_PMR, 0xff);
	gic_write32(gicc_base + GICC_BPR, 0);
	/*
	 * Written whole, not added to: an EOImodeNS (bit 9) the firmware left set would make an end of interrupt drop its
	 * priority only, every interrupt then staying active (the timer's would fire once). The bypass-disable bits (5, 6)
	 * are the firmware's and kept, as Linux keeps them.
	 */
	gic_write32(gicc_base + GICC_CTLR, (gic_read32(gicc_base + GICC_CTLR) & ((1u << 5) | (1u << 6))) | GICC_CTLR_ENABLE);
	__builtin_arm_dsb(DSB_SY);

	ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
	/* last: from here on another processor may signal this one */
	os_atomic_store(&gic_v2_target[cpu], target, release);
	kprintf("gic: v2, distributor %p, CPU interface %p, cpu %d is interface %d, virtual timer on PPI %d\n",
	    (void *)gicd_base, (void *)gicc_base, cpu, __builtin_ctz(target), GIC_INTID_VTIMER);
	/*
	 * Two things only the hardware can say: whether the timer's PPI really is enabled (one the secure world left in
	 * Group 0 reads as 0 from here and never arrives), and how far this core's virtual counter is from the physical one
	 * (CNTVOFF_EL2 is EL2's to set: under a hypervisor that leaves it, cores could disagree on the time).
	 */
	kprintf("gic: v2, cpu %d: ISENABLER0 %#x (the timer's PPI %s), CTLR %#x; CNTPCT - CNTVCT = %lld\n", cpu,
	    gic_read32(gicd_base + GICD_ISENABLER(0)),
	    (gic_read32(gicd_base + GICD_ISENABLER(0)) & (1u << GIC_INTID_VTIMER)) ? "on" : "NOT ON", gic_read32(gicc_base + GICC_CTLR),
	    (long long)(__builtin_arm_rsr64("CNTPCT_EL0") - __builtin_arm_rsr64("CNTVCT_EL0")));
}

/* Per-CPU bring-up: every processor calls this for itself, the boot processor first. */
void
gic_v3_irq_cpu_init(void)
{
	int cpu = cpu_number();
	vm_offset_t gicr;
	uint32_t mine = 1u << GIC_INTID_VTIMER;

	if (gicd_base == 0) {
		DTEntry entry;
		void const *prop;
		unsigned int prop_size;

		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
		    prop_size < 4 * sizeof(uint64_t)) {
			panic("gic: no usable /arm-io/gic node in the device tree");
		}
		uint64_t const *reg = prop;
		vm_offset_t soc_base = pe_arm_get_soc_base_phys();

		simple_lock_init(&gic_spi_lock, 0);
		gic_v2 = SecureDTGetProperty(entry, "compatible", &prop, &prop_size) == kSuccess &&
		    strncmp(prop, "arm,gic-400", prop_size) == 0;
		gicd_base = ml_io_map(soc_base + reg[0], reg[1]);
		if (gic_v2) {
			gicc_base = ml_io_map(soc_base + reg[2], reg[3]);
		} else {
			gicr_base = ml_io_map(soc_base + reg[2], reg[3]);
			gicr_size = reg[3];
		}
		if (gicd_base == 0 || (gic_v2 ? gicc_base : gicr_base) == 0) {
			panic("gic: cannot map the distributor/%s", gic_v2 ? "CPU interface" : "redistributor");
		}
	} else if (gic_v2 ? gic_v2_target[cpu] != 0 : gicr_cpu_base[cpu] != 0) {
		return;
	}
	if (gic_v2) {
		gic_v2_cpu_init(cpu);
		return;
	}

	/* System-register CPU interface */
	__builtin_arm_wsr64("ICC_SRE_EL1", __builtin_arm_rsr64("ICC_SRE_EL1") | 1);
	__builtin_arm_isb(ISB_SY);

	gicr = gicr_cpu_base[cpu] = gic_find_redistributor();
	{
		uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");

		gic_v3_route[cpu] = (mpidr & 0xff00000000ull) | (mpidr & 0xffffff);
	}

	uint32_t waker = gic_read32(gicr + GICR_WAKER);
	if (waker & (GICR_WAKER_PROCSLEEP | GICR_WAKER_CHILDASLEEP)) {
		gic_write32(gicr + GICR_WAKER, waker & ~GICR_WAKER_PROCSLEEP);
		while (gic_read32(gicr + GICR_WAKER) & GICR_WAKER_CHILDASLEEP) {
			/* spin until the redistributor is awake */
		}
	}

	/*
	 * The interrupts between processors, where there is more than one. A core that an earlier
	 * kernel switched off in the middle of one (a return to the boot loader, started again
	 * since) may still have it marked active, and would then never see another.
	 */
	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write32(gicr + GICR_ICACTIVER0, sgis);
		gic_write32(gicr + GICR_ICPENDR0, sgis);        /* ... or one that it never took */
		*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
		*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
		mine |= sgis;
	}

	/* Virtual timer PPI (and those): Group 1 (IRQ), default priority, enabled */
	gic_write32(gicr + GICR_IGROUPR0, gic_read32(gicr + GICR_IGROUPR0) | mine);
	gic_write32(gicr + GICR_IGRPMODR0, gic_read32(gicr + GICR_IGRPMODR0) & ~mine);
	*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
	gic_write32(gicr + GICR_ISENABLER0, mine);

	/* Distributor: affinity routing, Group 1 enabled (normally already so) */
	uint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);
	if ((ctlr & (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) != (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) {
		gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
		while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
			/* wait for the write to take effect */
		}
	}

	/*
	 * CPU interface: accept all priorities, no preemption grouping, Group 1 on. And no priority
	 * active: a core that the firmware switches off and on again need not lose that, and a
	 * kernel that left in the middle of an interrupt without ending it would leave this one
	 * waiting for an end that never comes.
	 */
	__builtin_arm_wsr64("ICC_AP1R0_EL1", 0);
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xff);
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);

	ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
	kprintf("gic: v3, distributor %p, cpu %d's redistributor %p, virtual timer on PPI %d\n",
	    (void *)gicd_base, cpu, (void *)gicr, GIC_INTID_VTIMER);
}

#endif /* GENERIC_GIC_V3_IRQ */
