/*
 * iOS 27 on STARLTE: the warm restart (scion-kernel docs/ios27/WARM-RESTART.md). NOT RUN ON A PHONE.
 *
 * Every restart of the S9 goes through Samsung's S-Boot, which vibrates the motor and takes about ten seconds; S-Boot
 * cannot be changed. A warm restart does not reset the SoC, so S-Boot is simply not in the path: this kernel reads the
 * BOOT partition (read only, the UFS driver's hakios_ufs_read_partition), puts the boot image's kernel slot -- the
 * loader Image with its appended SWEETXNU payload -- where S-Boot puts it, stops its devices, its other processors
 * and its interrupts, turns its MMU off and enters the loader at its second entry (base + 0x40) with a re-entry record
 * (scion-kernel xnuloader/sboot/scion_reentry.h; the same contract as scion-boot-exynos9810's include/boot.h). The
 * loader checks the record and the payload's SHA-256 and starts XNU as on a cold boot. Nothing is written to any
 * partition: only RAM.
 *
 * Boot-args (all off by default; without sweetwarm= nothing of this runs and nothing is mapped):
 *   sweetwarm=1      the USB console's "warm" and kern.sweetwarm_go / kern.sweetwarm_dry may start one
 *   sweetwarm=2      and every restart iOS asks for (with sweethalt=1: the power menu's restart, reboot(2)) is a warm one
 *                    when it can be had; when it cannot, the restart is the PMU reset it was
 *   sweetwarm_smp=1  with more than one processor running, the others are switched off with PSCI CPU_OFF first (step 2
 *                    of the plan: Samsung's own kernel takes cores off this way on this firmware, but this code has not
 *                    done it). Without it a warm restart needs a boot with cpus=1 (step 1)
 * The console:  warm                 BOOT again, the payload's own line + sweetwarmed=N
 *               warm <boot-args>     the same with these appended (a later one of the same name wins in XNU)
 *               warm line <line>     BOOT with exactly this line (+ sweetwarmed=N)
 *               warm recovery        TWRP from RECOVERY (step 3: needs a cold boot with sweetwarm= on its line,
 *                                    which has the loader keep S-Boot's FDT)
 *               warm dry             everything up to the point of no return, then back: read, checked, nothing stopped
 * sysctl kern.sweetwarm_go=1 (BOOT), =2 (RECOVERY); kern.sweetwarm_dry=1 (the dry run).
 *
 * Before the point of no return (the line "STARLTE WARM: point of no return") any failure is reported and the system
 * runs on. After it a failure resets through the PMU, and the cluster-1 watchdog (armed for 60 s, stopped by the loader
 * right before the next kernel) resets a hang: either way an ordinary restart through S-Boot, never a dead phone.
 * Lines "STARLTE WARM: ...".
 */
#include <stdint.h>
#include <stdbool.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/kalloc.h>
#include <machine/machine_routines.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm64/proc_reg.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <libkern/libkern.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>

void hakios_warm_console(const char *arg);      /* bsd/dev/hakios.dwc3.c: the USB console's "warm" */

#if !defined(ARM64_BOARD_CONFIG_STARLTE)
void
hakios_warm_console(__unused const char *arg)
{
	printf("usb: warm: the warm restart is the Galaxy S9's (STARLTE) only\n");
}
#else

/* ---- the contract (xnuloader/sboot/scion_reentry.h; keep byte for byte) ---------------------------------------- */
#define SCNR_MAGIC        0x524e4353U  /* "SCNR" */
#define SCNR_VERSION      1U
#define SCNR_SIZE         728U
#define SCNR_ENTRY_OFFSET 0x40U
#define SCNR_MARK_OFFSET  0x44U
#define SCNR_LINE         608U
#define SCNR_F_DIRECT     (1U << 0)
#define SCNR_F_LINE       (1U << 1)
#define SCNR_F_RESTAGED   (1U << 3)
#define SCNR_F_WDT_ARMED  (1U << 4)
#define SCNR_F_APPEND     (1U << 6)     /* with SCNR_F_LINE: the line appended to the payload's */
#define SCNR_M_XNU        0U
#define SCNR_M_ANDROID    1U
#define SCNR_R_USER       0U
#define SCNR_R_TEST       3U
#define SCNR_FDT_SAVE     0x91100000ULL
#define SCNR_FDT_MAGIC    0x31544446524e4353ULL   /* "SCNRFDT1" */

struct scion_reentry {
	uint32_t magic;
	uint16_t version, size;
	uint32_t reason, flags, entry, crc32;
	char cmdline[SCNR_LINE];
	uint32_t method, wdt_ms;
	uint64_t image_pa, image_size, kernel_pa, kernel_size, ramdisk_pa, ramdisk_size, fdt_pa;
	uint8_t image_sha256[32];
};
_Static_assert(__builtin_offsetof(struct scion_reentry, method) == 0x278, "scion_reentry layout");
_Static_assert(__builtin_offsetof(struct scion_reentry, fdt_pa) == 0x2b0, "scion_reentry layout");
_Static_assert(sizeof(struct scion_reentry) == SCNR_SIZE, "scion_reentry size");

/* ---- where things go ----------------------------------------------------------------------------------------- */
/*
 * The loader's kernel slot goes where S-Boot puts it (the BOOT image's kernel_addr is not used by S-Boot for arm64:
 * the Image lands at 0x80080000, text_offset 0x80000 from the bank's base). A TWRP kernel and ramdisk (step 3) go after
 * it, below the loader's STAGE (0x8c000000); the loader's ram_start copies them to their places from there.
 * The handoff area 0x91060000-0x910fffff (the record, the stub, the identity tables, the copy list) is in no OS's RAM
 * (tools/ios27/adt-extra-ram.py leaves 0x8F000000-0x91FFFFFF out of XNU's) and unused by the loader on this path
 * (fastboot_dev.c: 0x91000000-0x91200000 is used by nothing but its USB DMA, below 0x91050000).
 */
#define WARM_SLOT_PA      0x80080000ULL
#define WARM_TWRP_K_PA    0x84000000ULL
#define WARM_TWRP_R_PA    0x88000000ULL
#define WARM_LOW_END      0x8c000000ULL
#define WARM_AREA_PA      0x91060000ULL
#define WARM_AREA_SIZE    0x000a0000ULL
#define WARM_REC_PA       (WARM_AREA_PA + 0x0000)
#define WARM_STUB_PA      (WARM_AREA_PA + 0x1000)
#define WARM_L1_PA        (WARM_AREA_PA + 0x2000)
#define WARM_L2_PA        (WARM_AREA_PA + 0x3000)
#define WARM_LIST_PA      (WARM_AREA_PA + 0x4000)
#define WARM_LIST_MAX     ((WARM_AREA_SIZE - 0x4000) / 16)
#define WARM_PG           4096ULL
#define BOOT_PART_MAX     (14080ULL * 4096)        /* the PIT's BOOT and RECOVERY: 14080 blocks of 4 KiB */

#define PMU_PA            0x14060000ULL
#define WDT1_PA           0x10060000ULL
#define WDT_PMU_BIT       (1U << 23)
#define WDT_SECONDS       60U
#define WDT_COUNT         ((uint32_t)(WDT_SECONDS * 793457ULL / 1000))   /* 26 MHz / 256 / 128 */

extern int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes);
extern int starlte_ufs_hold_for_handoff(void);           /* herolte_ufs.c: its lock taken for good, the cache synced */
extern void herolte_log_flush_now(void);
extern void starlte_pmsg_note(const char *hdr);   /* herolte_ufs.c: a header + the console's tail into the pmsg ring */
extern void a5xelte_gic_quiesce(void);                   /* pexpert/arm/pe_fiq.c */
extern void hakios_max98512_quiesce(void);
extern void hakios_cs47l93_quiesce(void);
extern void hakios_exynos_abox_quiesce(void);
extern void hakios_exynos_camera_quiesce(void);
extern void hakios_shannon_quiesce(void);
extern void hakios_exynos_pcie_quiesce(void);
extern void hakios_usb_gadget_quiesce(void);
extern void starlte_gpu_quiesce(char *out, size_t outlen);   /* bsd/dev/starlte_gpu.c: silent, says what it did */
extern void starlte_pmsg_append(const char *s);   /* herolte_ufs.c: one line appended to the pmsg ring */
extern void console_suspend(void);                /* osfmk/console/serial_console.c */
extern unsigned int processor_avail_count;
extern int get_preemption_level(void);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern processor_t thread_bind(processor_t processor);
extern processor_t cpu_to_processor(int cpu);
extern int cpu_number(void);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);

static uint32_t warm_mode, warm_smp, warm_count;
static volatile uint32_t warm_busy;
static volatile uint32_t *warm_pmu, *warm_wdt;
static volatile uint8_t *warm_area;
static volatile uint64_t *warm_fdt;

/* ---- the stub: copied to WARM_STUB_PA and entered there through an identity map ---------------------------------- */
/*
 * x0 the record, x1 the copy list ({ source PA, destination PA } per 4 KiB page), x2 its length, x3 the loader's warm
 * entry, x4/x5 and x6/x7 two ranges (start, length) to clean and invalidate by address after the copy (the
 * destination and the next kernel's span: no stale line of them may stay in any cache, a powered-down cluster's
 * included). The MMU and both caches off first; then the whole data cache cleaned and invalidated by set/way on this
 * core (with the cache off nothing new comes in: the lines this kernel wrote last, its stack, go out now); the copy;
 * the two ranges; the instruction cache; then the loader with x0 = the record and x1-x3 = 0. Uses x0-x17 only.
 */
extern const char starlte_warm_stub[], starlte_warm_stub_end[];
__asm__(
	"	.text\n"
	"	.p2align 4\n"
	"	.globl _starlte_warm_stub\n"
	"_starlte_warm_stub:\n"
	"	mrs x9, SCTLR_EL1\n"
	"	bic x9, x9, #1\n"
	"	bic x9, x9, #4\n"
	"	bic x9, x9, #0x1000\n"
	"	msr SCTLR_EL1, x9\n"
	"	isb\n"
	"	mrs x9, CLIDR_EL1\n"
	"	and x10, x9, #0x7000000\n"
	"	lsr x10, x10, #23\n"
	"	cbz x10, 5f\n"
	"	mov x11, #0\n"
	"1:	add x12, x11, x11, lsr #1\n"
	"	lsr x13, x9, x12\n"
	"	and x13, x13, #7\n"
	"	cmp x13, #2\n"
	"	b.lt 4f\n"
	"	msr CSSELR_EL1, x11\n"
	"	isb\n"
	"	mrs x13, CCSIDR_EL1\n"
	"	and x14, x13, #7\n"
	"	add x14, x14, #4\n"
	"	ubfx x15, x13, #3, #10\n"
	"	clz w16, w15\n"
	"	ubfx x17, x13, #13, #15\n"
	"2:	mov x8, x15\n"
	"3:	lsl x12, x8, x16\n"
	"	orr x12, x12, x11\n"
	"	lsl x13, x17, x14\n"
	"	orr x12, x12, x13\n"
	"	dc cisw, x12\n"
	"	subs x8, x8, #1\n"
	"	b.ge 3b\n"
	"	subs x17, x17, #1\n"
	"	b.ge 2b\n"
	"4:	add x11, x11, #2\n"
	"	cmp x10, x11\n"
	"	b.gt 1b\n"
	"5:	msr CSSELR_EL1, xzr\n"
	"	dsb sy\n"
	"	isb\n"
	"	mov x9, x1\n"
	"	mov x10, x2\n"
	"6:	cbz x10, 8f\n"
	"	ldp x11, x12, [x9], #16\n"
	"	mov x13, #4096\n"
	"7:	ldp x14, x15, [x11], #16\n"
	"	stp x14, x15, [x12], #16\n"
	"	subs x13, x13, #16\n"
	"	b.ne 7b\n"
	"	sub x10, x10, #1\n"
	"	b 6b\n"
	"8:	dsb sy\n"
	"	mrs x9, CTR_EL0\n"
	"	ubfx x9, x9, #16, #4\n"
	"	mov x10, #4\n"
	"	lsl x9, x10, x9\n"
	"	sub x10, x9, #1\n"
	"	add x5, x4, x5\n"
	"	bic x4, x4, x10\n"
	"9:	cmp x4, x5\n"
	"	b.hs 10f\n"
	"	dc civac, x4\n"
	"	add x4, x4, x9\n"
	"	b 9b\n"
	"10:	add x7, x6, x7\n"
	"	bic x6, x6, x10\n"
	"11:	cmp x6, x7\n"
	"	b.hs 12f\n"
	"	dc civac, x6\n"
	"	add x6, x6, x9\n"
	"	b 11b\n"
	"12:	dsb sy\n"
	"	ic iallu\n"
	"	dsb sy\n"
	"	isb\n"
	"	mov x16, x3\n"
	"	mov x1, xzr\n"
	"	mov x2, xzr\n"
	"	mov x3, xzr\n"
	"	br x16\n"
	"	.globl _starlte_warm_stub_end\n"
	"_starlte_warm_stub_end:\n"
	);

/* ---- staging ----------------------------------------------------------------------------------------------------- */
struct warm_part {
	const char *name;       /* "BOOT" or "RECOVERY" */
	uint64_t offset, size;  /* in the partition */
	uint64_t dst;           /* where it goes */
	vm_offset_t va;         /* staged here */
	vm_size_t alloc;
};

struct warm_plan {
	struct warm_part part[3];
	unsigned nparts;
	uint32_t method;
	uint64_t slot_size;             /* BOOT's kernel slot: the loader Image + payload, its kernel_size (what S-Boot loads) */
	uint64_t declared;              /* the Image header's image_size (S-Boot's reservation; the packer pads it) */
	uint64_t payload_off;           /* where the SWEETXNU payload starts in the slot */
	uint64_t span_pa, span_len;     /* the next kernel's span (the loader writes it) */
	char line[SCNR_LINE];
	uint32_t flags;
	uint8_t sha[32];
	uint64_t list_n;
	vm_offset_t spare[64];          /* replacement pages taken (freed on an abort) */
	unsigned nspare;
};

static struct warm_plan warm_plan;

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static uint32_t
warm_crc32(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t crc = 0xFFFFFFFFu;

	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
		}
	}
	return ~crc;
}

/* The Android boot image header of a partition: kernel and ramdisk sizes and offsets (page 2048, v0 as S-Boot takes). */
static const char *
warm_header(const char *name, uint64_t *koff, uint64_t *ksize, uint64_t *roff, uint64_t *rsize)
{
	static uint8_t h[2048];
	uint64_t bytes = 0;
	int e = hakios_ufs_read_partition(name, 0, h, sizeof(h), &bytes);

	if (e != 0) {
		return "the partition could not be read";
	}
	if (memcmp(h, "ANDROID!", 8) != 0 || rd32(h + 36) != 2048) {
		return "no Android boot image header (page 2048)";
	}
	*ksize = rd32(h + 8);
	*rsize = rd32(h + 16);
	*koff = 2048;
	*roff = 2048 + ((*ksize + 2047) & ~2047ULL);
	if (*ksize < 0x1000 || *koff + *ksize > bytes || *roff + *rsize > bytes || bytes > BOOT_PART_MAX) {
		return "the header's sizes do not fit the partition";
	}
	return NULL;
}

static const char *
warm_read_part(struct warm_part *p)
{
	p->alloc = round_page(p->size);
	if (kmem_alloc(kernel_map, &p->va, p->alloc, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		p->va = 0;
		return "no memory to stage it";
	}
	if (hakios_ufs_read_partition(p->name, p->offset, (uint8_t *)p->va, p->size, NULL) != 0) {
		return "the read failed";
	}
	return NULL;
}

static uint64_t
warm_kvtophys(vm_offset_t va)
{
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)va);
	return pn == 0 ? 0 : (ptoa_64(pn) | (va & PAGE_MASK));
}

static bool
warm_in_dst(const struct warm_plan *w, uint64_t pa)
{
	for (unsigned i = 0; i < w->nparts; i++) {
		const struct warm_part *p = &w->part[i];
		if (pa + WARM_PG > p->dst && pa < p->dst + round_page(p->size)) {
			return true;
		}
	}
	return false;
}

/*
 * The sizes of BOOT's kernel slot (scion-kernel tools/warm_slot.c is this function for the host test; keep the two the
 * same). S-Boot loads the BOOT header's kernel_size bytes of the slot to 0x80080000; the arm64 Image header's
 * image_size is only how much room it keeps there, and tools/ios27/starlte-pack-boot.py pads it on purpose to the
 * template's (the TWRP kernel's 0x2842000, about 42 MB) or to the slot + 64 KiB, whichever is larger: it is always
 * larger than the slot. So kernel_size is what is read again, hashed and copied (rounded up to whole pages: the rest of
 * the last page is the zeroes of the staging buffer); image_size is only checked to be what the cold boot used too
 * (the loader's boot_payload looks for the payload in [_image_end, base + image_size), which must end below its
 * RAM_BASE 0x92000000) and never decides how much is read or copied. NULL, or why not (in `why`, with the numbers).
 */
#define WARM_LOADER_RAM_BASE 0x92000000ULL
static const char *
warm_slot(const uint8_t *s, uint64_t ksize, uint64_t base, uint64_t limit, uint64_t *declared, uint64_t *payload_off,
    char *why, size_t whylen)
{
	uint64_t copy = (ksize + WARM_PG - 1) & ~(WARM_PG - 1);

	*declared = 0;
	*payload_off = 0;
	if (ksize < 0x1000 || ksize > limit - base || copy > limit - base) {
		snprintf(why, whylen, "BOOT's kernel slot (%llu bytes) does not fit 0x%llx..0x%llx", ksize, base, limit);
		return why;
	}
	if (rd32(s + 56) != 0x644d5241U) {
		return "BOOT's kernel slot is not an arm64 Image";
	}
	if (memcmp(s + SCNR_MARK_OFFSET, "SCNR", 4) != 0 || rd32(s + SCNR_MARK_OFFSET + 4) != SCNR_VERSION) {
		return "BOOT's loader has no warm entry (an older loader: make the boot image with this one)";
	}
	*declared = rd64(s + 16);
	if (*declared < ksize || *declared > WARM_LOADER_RAM_BASE - base) {
		snprintf(why, whylen, "the loader's Image header image_size 0x%llx is below the slot's %llu bytes or past "
		    "0x%llx: the loader would not see the payload as it does on a cold boot", *declared, ksize,
		    WARM_LOADER_RAM_BASE);
		return why;
	}
	for (uint64_t o = 0x10000; o + 0x2a8 <= ksize; o += WARM_PG) {
		const uint8_t *p = s + o;
		if (memcmp(p, "SWEETXNU", 8) == 0 && rd32(p + 8) == 1 && rd32(p + 12) >= 1 && rd32(p + 12) <= 32) {
			*payload_off = o;
			return NULL;
		}
	}
	return "no SWEETXNU payload after the loader";
}

/*
 * The loader slot (the Image + its appended SWEETXNU payload): the loader has the warm entry, the payload is one the
 * loader will take, the next kernel's span is known. Mirrors xnu_boot.c payload_bad().
 */
static const char *
warm_check_slot(struct warm_plan *w)
{
	static char why[200];
	const uint8_t *s = (const uint8_t *)w->part[0].va;
	const char *bad;

	w->slot_size = w->part[0].size;
	if ((bad = warm_slot(s, w->slot_size, WARM_SLOT_PA, WARM_TWRP_K_PA, &w->declared, &w->payload_off, why,
	    sizeof(why))) != NULL) {
		return bad;
	}
	const uint8_t *p = s + w->payload_off;
	uint64_t length = w->slot_size - w->payload_off;
	uint32_t nsegs = rd32(p + 12);
	uint64_t virt_base = rd64(p + 0x10), entry_va = rd64(p + 0x18), adt_off = rd64(p + 0x20), adt_size = rd64(p + 0x28);
	uint64_t kernel_end = rd64(p + 0x30), phys_base = rd64(p + 0x38), mem_size = rd64(p + 0x40);
	bool low = phys_base >= 0x92000000ULL && phys_base < 0xaf800000ULL;
	bool bank2 = phys_base >= 0x880000000ULL && phys_base < 0x900000000ULL;
	uint64_t window_end = bank2 ? 0x900000000ULL : 0xaf800000ULL;

	if (0x2a8 + (uint64_t)nsegs * 32 > length || adt_off > length || adt_size > length - adt_off) {
		return "the payload's header does not fit it";
	}
	if (!(low || bank2) || mem_size == 0 || mem_size > window_end - phys_base || entry_va < virt_base ||
	    kernel_end < virt_base || kernel_end - virt_base >= mem_size) {
		return "the payload's RAM is outside the loader's windows";
	}
	w->span_pa = phys_base;
	w->span_len = (kernel_end - virt_base) + round_page(adt_size) + 2 * WARM_PG;
	return NULL;
}

/* Each staged page's physical address; a page that lies where a part is copied to is replaced by one that does not. */
static const char *
warm_list(struct warm_plan *w)
{
	volatile uint64_t *list = (volatile uint64_t *)(void *)(warm_area + (WARM_LIST_PA - WARM_AREA_PA));
	uint64_t n = 0;

	for (unsigned i = 0; i < w->nparts; i++) {
		struct warm_part *p = &w->part[i];
		uint64_t pages = round_page(p->size) / WARM_PG;
		for (uint64_t k = 0; k < pages; k++) {
			vm_offset_t va = p->va + k * WARM_PG;
			uint64_t pa = warm_kvtophys(va);
			while (pa == 0 || warm_in_dst(w, pa)) {
				vm_offset_t r = 0;
				if (w->nspare == sizeof(w->spare) / sizeof(w->spare[0]) ||
				    kmem_alloc(kernel_map, &r, PAGE_SIZE, KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
					return "too many staged pages lie where the image goes";
				}
				w->spare[w->nspare++] = r;
				memcpy((void *)r, (const void *)va, WARM_PG);
				pa = warm_kvtophys(r);
			}
			if (n == WARM_LIST_MAX) {
				return "the copy list is full";
			}
			list[2 * n] = pa;
			list[2 * n + 1] = p->dst + k * WARM_PG;
			n++;
		}
	}
	w->list_n = n;
	return NULL;
}

static void
warm_free(struct warm_plan *w)
{
	for (unsigned i = 0; i < w->nparts; i++) {
		if (w->part[i].va != 0) {
			kmem_free(kernel_map, w->part[i].va, w->part[i].alloc);
			w->part[i].va = 0;
		}
	}
	for (unsigned i = 0; i < w->nspare; i++) {
		kmem_free(kernel_map, w->spare[i], PAGE_SIZE);
	}
	w->nspare = 0;
}

static void
warm_line_add(char *line, const char *more)
{
	size_t n = strlen(line), m = strlen(more);
	if (m == 0) {
		return;
	}
	if (n + (n ? 1 : 0) + m >= SCNR_LINE) {
		return;
	}
	if (n) {
		line[n++] = ' ';
	}
	memcpy(line + n, more, m + 1);
}

/* Everything that can be undone: read, checked, listed, the record written. NULL, or why not. */
static const char *
warm_prepare(struct warm_plan *w, uint32_t method, const char *args, bool whole_line)
{
	uint64_t koff, ksize, roff, rsize;
	const char *why;
	char tail[48];

	bzero(w, sizeof(*w));
	w->method = method;
	if (warm_area == NULL || warm_pmu == NULL || warm_wdt == NULL) {
		return "the handoff area, the PMU or the watchdog is not mapped";
	}
	if (processor_avail_count > 1 && !warm_smp) {
		return "more than one processor runs: boot with cpus=1 (step 1), or sweetwarm_smp=1 (step 2)";
	}
	{
		uint32_t on = 0;
		if ((PE_parse_boot_argn("fb_flip", &on, sizeof(on)) && on) || (PE_parse_boot_argn("fb_dpp", &on, sizeof(on)) && on) ||
		    (PE_parse_boot_argn("sweetfb_dpp", &on, sizeof(on)) && on)) {
			return "fb_flip/fb_dpp/sweetfb_dpp move the DECON off the boot framebuffer, which the loader and the next "
			       "kernel expect";
		}
	}
	{
		uint64_t tcr = __builtin_arm_rsr64("TCR_EL1"), t0sz = tcr & 0x3f;
		if (((tcr >> 14) & 3) != 0 || t0sz < 25 || t0sz > 33 || (WARM_STUB_PA >> (64 - t0sz)) != 0) {
			return "TTBR0 cannot map the stub (not a 4 KiB granule starting at level 1)";
		}
	}
	if ((size_t)(starlte_warm_stub_end - starlte_warm_stub) > WARM_PG) {
		return "the stub is larger than its page";
	}
	if ((why = warm_header("BOOT", &koff, &ksize, &roff, &rsize)) != NULL) {
		return why;
	}
	w->part[0] = (struct warm_part){ .name = "BOOT", .offset = koff, .size = ksize, .dst = WARM_SLOT_PA };
	w->nparts = 1;
	if ((why = warm_read_part(&w->part[0])) != NULL || (why = warm_check_slot(w)) != NULL) {
		return why;
	}
	if (method == SCNR_M_ANDROID) {
		if (warm_fdt == NULL || warm_fdt[0] != SCNR_FDT_MAGIC) {
			return "no S-Boot FDT kept by the loader: cold-boot once with sweetwarm= on the line";
		}
		if ((why = warm_header("RECOVERY", &koff, &ksize, &roff, &rsize)) != NULL) {
			return why;
		}
		if (ksize > WARM_TWRP_R_PA - WARM_TWRP_K_PA || rsize == 0 || rsize > WARM_LOW_END - WARM_TWRP_R_PA) {
			return "RECOVERY's kernel or ramdisk is too large for the room below the loader's STAGE";
		}
		w->part[1] = (struct warm_part){ .name = "RECOVERY", .offset = koff, .size = ksize, .dst = WARM_TWRP_K_PA };
		w->part[2] = (struct warm_part){ .name = "RECOVERY", .offset = roff, .size = rsize, .dst = WARM_TWRP_R_PA };
		w->nparts = 3;
		if ((why = warm_read_part(&w->part[1])) != NULL || (why = warm_read_part(&w->part[2])) != NULL) {
			return why;
		}
		if (rd32((const uint8_t *)w->part[1].va + 56) != 0x644d5241U) {
			return "RECOVERY's kernel is not an uncompressed arm64 Image";
		}
	}
	{
		SHA256_CTX c;
		SHA256_Init(&c);
		SHA256_Update(&c, (const uint8_t *)w->part[0].va + w->payload_off, (size_t)(w->slot_size - w->payload_off));
		SHA256_Final(w->sha, &c);
	}
	if ((why = warm_list(w)) != NULL) {
		return why;
	}
	/* the line: the payload's own + what was asked + sweetwarmed=N (BOOT); RECOVERY's header line is TWRP's own */
	w->flags = SCNR_F_DIRECT | SCNR_F_RESTAGED | SCNR_F_WDT_ARMED;
	if (method == SCNR_M_XNU) {
		w->flags |= whole_line ? SCNR_F_LINE : (SCNR_F_LINE | SCNR_F_APPEND);
		if (args != NULL) {
			strlcpy(w->line, args, sizeof(w->line));
		}
		snprintf(tail, sizeof(tail), "sweetwarmed=%u", warm_count + 1);
		warm_line_add(w->line, tail);
		if (strlen(w->line) + 1 >= SCNR_LINE) {
			return "the boot line is too long";
		}
	} else if (args != NULL) {
		strlcpy(w->line, args, sizeof(w->line));
	}
	return NULL;
}

static void
warm_write_record(struct warm_plan *w)
{
	struct scion_reentry r;
	volatile uint8_t *d = warm_area + (WARM_REC_PA - WARM_AREA_PA);

	bzero(&r, sizeof(r));
	r.magic = SCNR_MAGIC;
	r.version = SCNR_VERSION;
	r.size = SCNR_SIZE;
	r.reason = SCNR_R_USER;
	r.flags = w->flags;
	r.entry = ~0U;
	memcpy(r.cmdline, w->line, sizeof(r.cmdline));
	r.cmdline[SCNR_LINE - 1] = 0;
	r.method = w->method;
	r.wdt_ms = WDT_SECONDS * 1000;
	r.image_pa = WARM_SLOT_PA;
	r.image_size = w->slot_size;
	if (w->method == SCNR_M_ANDROID) {
		r.kernel_pa = WARM_TWRP_K_PA;
		r.kernel_size = w->part[1].size;
		r.ramdisk_pa = WARM_TWRP_R_PA;
		r.ramdisk_size = w->part[2].size;
		r.fdt_pa = SCNR_FDT_SAVE + 0x1000;     /* the FDT itself, after the save's 4 KiB header */
	}
	memcpy(r.image_sha256, w->sha, 32);
	r.crc32 = 0;
	r.crc32 = warm_crc32(&r, SCNR_SIZE);
	for (unsigned i = 0; i < SCNR_SIZE; i++) {
		d[i] = ((const uint8_t *)&r)[i];
	}
	/* the stub and the identity map: L1[PA >> 30] -> L2, L2[(PA >> 21) & 511] = the 2 MiB block, read-only, executable */
	volatile uint8_t *stub = warm_area + (WARM_STUB_PA - WARM_AREA_PA);
	for (size_t i = 0; i < (size_t)(starlte_warm_stub_end - starlte_warm_stub); i++) {
		stub[i] = (uint8_t)starlte_warm_stub[i];
	}
	volatile uint64_t *l1 = (volatile uint64_t *)(void *)(warm_area + (WARM_L1_PA - WARM_AREA_PA));
	volatile uint64_t *l2 = (volatile uint64_t *)(void *)(warm_area + (WARM_L2_PA - WARM_AREA_PA));
	for (unsigned i = 0; i < 512; i++) {
		l1[i] = 0;
		l2[i] = 0;
	}
	l1[WARM_STUB_PA >> 30] = WARM_L2_PA | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
	l2[(WARM_STUB_PA >> 21) & 0x1ff] = (WARM_STUB_PA & ~0x1fffffULL) | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
	    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_AP(AP_RONA) | ARM_TTE_BLOCK_NX |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);
	__builtin_arm_dsb(DSB_SY);
}

/* ---- after the point of no return ----------------------------------------------------------------------------- */
__attribute__((noreturn))
static void
warm_reset(const char *why)
{
	printf("STARLTE WARM: %s: an ordinary restart through the PMU instead\n", why);
	(void)ml_set_interrupts_enabled(FALSE);
	warm_pmu[0x808 / 4] = 0x12345678U;
	warm_pmu[0x80c / 4] = 0x12345670U;      /* "unknown": S-Boot boots BOOT, as sweethalt's restart */
	__builtin_arm_dsb(DSB_SY);
	warm_pmu[0x400 / 4] = 1;
	for (;;) {
		__builtin_arm_wfe();
	}
}

static void
warm_wdt_arm(void)
{
	warm_wdt[0] = 0;                                /* WTCON off while it is set up */
	warm_wdt[1] = WDT_COUNT;                        /* WTDAT */
	warm_wdt[2] = WDT_COUNT;                        /* WTCNT */
	warm_pmu[0x408 / 4] &= ~WDT_PMU_BIT;            /* DISABLE_WDT: the PMU does not stop it */
	warm_pmu[0x40c / 4] &= ~WDT_PMU_BIT;            /* MASK_WDT_RESET_REQUEST: its expiry resets the SoC */
	__builtin_arm_dsb(DSB_SY);
	warm_wdt[0] = 0xFF39U;                          /* prescaler 255, divider 128, on, the reset */
	__builtin_arm_dsb(DSB_SY);
}

static uint32_t warm_off_sync, warm_off_count;

/* On every other processor (step 2, sweetwarm_smp=1; NOT run): its caches out, its GIC CPU interface off, PSCI CPU_OFF. */
static void
warm_cpu_off(void *arg)
{
	uint32_t *sync = arg;

	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	__builtin_arm_wsr64("CNTP_CTL_EL0", 0);
	FlushPoC_Dcache();
	os_atomic_inc(&warm_off_count, relaxed);
	if (os_atomic_dec(sync, relaxed) == 0) {
		thread_wakeup((event_t)sync);
	}
	{
		register uint64_t x0 __asm__("x0") = 0x84000002ULL;     /* CPU_OFF (the vendor tree's cpu_off) */
		register uint64_t x1 __asm__("x1") = 0;
		__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1) : : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
		    "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	}
	for (;;) {
		__builtin_arm_wfi();                    /* the firmware refused: out of the way all the same */
	}
}

/*
 * The steps once interrupts are off: printf may not be called then (XNU panics "Console I/O from interrupt-disabled
 * context", serial_console.c: the phone, 18 Nov, at the first such printf). Each step is appended to the pmsg ring at
 * once (a hang leaves it there for TWRP) and kept here, to be appended again after the note before the jump (which
 * starts the ring again).
 */
static char warm_steps[1024];

static void
warm_mark(const char *fmt, ...) __printflike(1, 2);
static void
warm_mark(const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	starlte_pmsg_append(line);
	strlcat(warm_steps, line, sizeof(warm_steps));
}

__attribute__((noreturn))
static void
warm_go(struct warm_plan *w)
{
	uint32_t others = processor_avail_count - 1;
	char gpu[160];

	printf("STARLTE WARM: point of no return: %llu pages to 0x%llx (+ %u more parts), payload SHA-256 %02x%02x%02x%02x..., "
	    "the next kernel at 0x%llx (%llu MiB); the watchdog armed for %u s\n", w->list_n, WARM_SLOT_PA, w->nparts - 1,
	    w->sha[0], w->sha[1], w->sha[2], w->sha[3], w->span_pa, w->span_len >> 20, WDT_SECONDS);
	herolte_log_flush_now();
	/* the drivers that move memory or drive power on their own, as the console's "return" stops them, then the
	 * console itself (its controller's DMA) and the storage (its lock kept: no request after this) */
	hakios_max98512_quiesce();
	hakios_cs47l93_quiesce();
	hakios_exynos_abox_quiesce();
	hakios_exynos_camera_quiesce();
	hakios_shannon_quiesce();
	hakios_exynos_pcie_quiesce();
	delay(50 * 1000);
	hakios_usb_gadget_quiesce();
	/* From here the console is gone: each step is a line in the log's ring, which a panic (sweetreboot=: to TWRP) and
	 * the note before the jump put into the pmsg ring (TWRP: /sys/fs/pstore/pmsg-ramoops-0). */
	printf("STARLTE WARM: step 1: the USB console stopped\n");
	if (starlte_ufs_hold_for_handoff() != 0) {
		warm_reset("the storage did not come to rest");
	}
	printf("STARLTE WARM: step 2: the storage held, its cache synced\n");
	warm_wdt_arm();
	printf("STARLTE WARM: step 3: the watchdog armed (WTCNT %u)\n", warm_wdt[2]);
	if (others != 0) {
		warm_off_sync = others;
		(void)cpu_broadcast_xcall(&warm_off_sync, FALSE, warm_cpu_off, &warm_off_sync);
		delay(20 * 1000);
		if (os_atomic_load(&warm_off_count, relaxed) != others) {
			warm_reset("not every processor went off");
		}
	}
	/* No printf from here on (interrupts off: see warm_mark). The console suspended as for sleep, so that a line
	 * some driver still prints goes straight out (and into the log's ring) instead of panicking. */
	warm_steps[0] = '\0';
	console_suspend();
	(void)ml_set_interrupts_enabled(FALSE);
	warm_mark("STARLTE WARM: step 4: the console suspended, interrupts off\n");
	/* with nothing else running from here (one processor, interrupts off), what other threads could have started
	 * again: the GPU's jobs (a soft reset and its cores off) */
	starlte_gpu_quiesce(gpu, sizeof(gpu));
	warm_mark("STARLTE WARM: step 5: the GPU: %s\n", gpu);
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	__builtin_arm_wsr64("CNTP_CTL_EL0", 0);
	a5xelte_gic_quiesce();
	warm_mark("STARLTE WARM: step 6: the timers and the GIC off\n");
	/* the staged pages to the point of coherency by address (any cache that holds them), then this core's caches */
	for (unsigned i = 0; i < w->nparts; i++) {
		for (vm_offset_t p = w->part[i].va; p < w->part[i].va + round_page(w->part[i].size); p += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
		}
	}
	for (unsigned i = 0; i < w->nspare; i++) {
		for (vm_offset_t p = w->spare[i]; p < w->spare[i] + PAGE_SIZE; p += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
		}
	}
	__builtin_arm_dsb(DSB_SY);
	warm_mark("STARLTE WARM: step 7: the staged pages cleaned; TCR_EL1 %#llx SCTLR_EL1 %#llx; into the stub at 0x%llx "
	    "(%llu pages, then the loader at 0x%llx)\n", __builtin_arm_rsr64("TCR_EL1"), __builtin_arm_rsr64("SCTLR_EL1"),
	    WARM_STUB_PA, w->list_n, WARM_SLOT_PA + SCNR_ENTRY_OFFSET);
	starlte_pmsg_note("STARBITE: warm restart: XNU's handoff reached the stub (no panic); the console's last lines:\n");
	starlte_pmsg_append(warm_steps);
	FlushPoC_Dcache();

	register uint64_t x0 __asm__("x0") = WARM_REC_PA;
	register uint64_t x1 __asm__("x1") = WARM_LIST_PA;
	register uint64_t x2 __asm__("x2") = w->list_n;
	register uint64_t x3 __asm__("x3") = WARM_SLOT_PA + SCNR_ENTRY_OFFSET;
	register uint64_t x4 __asm__("x4") = WARM_SLOT_PA;
	register uint64_t x5 __asm__("x5") = (w->method == SCNR_M_ANDROID ? WARM_LOW_END : WARM_SLOT_PA + round_page(w->slot_size)) - WARM_SLOT_PA;
	register uint64_t x6 __asm__("x6") = w->span_pa;
	register uint64_t x7 __asm__("x7") = w->span_len;
	register uint64_t x8 __asm__("x8") = WARM_L1_PA;
	register uint64_t x9 __asm__("x9") = __builtin_arm_rsr64("TCR_EL1") & ~0x80ULL;    /* EPD0 clear: TTBR0 walks */
	register uint64_t x10 __asm__("x10") = WARM_STUB_PA;
	__asm__ volatile (
                "msr DAIFSet, #0xf\n"
                "msr TCR_EL1, x9\n"
                "msr TTBR0_EL1, x8\n"
                "isb\n"
                "tlbi vmalle1\n"
                "dsb sy\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "br x10\n"
                : : "r" (x0), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5), "r" (x6), "r" (x7), "r" (x8), "r" (x9),
                "r" (x10) : "memory");
	__builtin_unreachable();
}

/* ---- the ways in -------------------------------------------------------------------------------------------- */
/* Runs on the boot processor (bound to it here). Returns only when it did not go; says why. */
static int
warm_start(uint32_t method, const char *args, bool whole_line, bool dry)
{
	const char *why;

	if (warm_mode == 0) {
		printf("STARLTE WARM: off (boot-arg sweetwarm=1 or 2)\n");
		return ENOTSUP;
	}
	if (!os_atomic_cmpxchg(&warm_busy, 0, 1, acquire)) {
		printf("STARLTE WARM: one is already under way\n");
		return EBUSY;
	}
	thread_bind(cpu_to_processor(ml_get_boot_cpu_number()));
	thread_block(THREAD_CONTINUE_NULL);
	if (cpu_number() != ml_get_boot_cpu_number()) {
		why = "not on the boot processor";
	} else {
		why = warm_prepare(&warm_plan, method, args, whole_line);
	}
	if (why == NULL && !dry) {
		warm_write_record(&warm_plan);
		warm_go(&warm_plan);
	}
	if (why == NULL) {
		printf("STARLTE WARM: dry run: %s read and checked: kernel slot %llu bytes (kernel_size; Image header "
		    "image_size 0x%llx not used for the copy), %llu pages listed (0x%llx..0x%llx), payload at +0x%llx, "
		    "%llu bytes hashed, SHA-256 %02x%02x%02x%02x..., the next kernel at 0x%llx (%llu MiB), line \"%s\": "
		    "nothing stopped, running on\n",
		    method == SCNR_M_ANDROID ? "BOOT and RECOVERY" : "BOOT", warm_plan.slot_size, warm_plan.declared,
		    warm_plan.list_n, WARM_SLOT_PA, WARM_SLOT_PA + round_page(warm_plan.slot_size), warm_plan.payload_off,
		    warm_plan.slot_size - warm_plan.payload_off, warm_plan.sha[0], warm_plan.sha[1], warm_plan.sha[2],
		    warm_plan.sha[3], warm_plan.span_pa, warm_plan.span_len >> 20, warm_plan.line);
	} else {
		printf("STARLTE WARM: not started: %s; the system runs on\n", why);
	}
	warm_free(&warm_plan);
	thread_bind(PROCESSOR_NULL);
	os_atomic_store(&warm_busy, 0, release);
	return why == NULL ? 0 : EIO;
}

/* herolte_ufs.c starlte_halt_restart, kPERestartCPU: with sweetwarm=2 a restart iOS asked for is a warm one if it can be */
void starlte_warm_from_restart(void);
void
starlte_warm_from_restart(void)
{
	if (warm_mode != 2) {
		return;
	}
	if (!ml_get_interrupts_enabled() || get_preemption_level() != 0) {
		printf("STARLTE WARM: the restart came with interrupts or preemption off: an ordinary restart\n");
		return;
	}
	printf("STARLTE WARM: sweetwarm=2: this restart is a warm one\n");
	(void)warm_start(SCNR_M_XNU, NULL, false, false);
}

struct warm_req {
	uint32_t method;
	bool whole, dry;
	char args[SCNR_LINE];
};

static void
warm_thread(void *arg, __unused wait_result_t wr)
{
	struct warm_req *q = arg;
	(void)warm_start(q->method, q->args[0] ? q->args : NULL, q->whole, q->dry);
	kfree_type(struct warm_req, q);
}

static int
warm_spawn(uint32_t method, const char *args, bool whole, bool dry)
{
	struct warm_req *q = kalloc_type(struct warm_req, Z_WAITOK | Z_ZERO);
	thread_t t;

	if (q == NULL) {
		return ENOMEM;
	}
	q->method = method;
	q->whole = whole;
	q->dry = dry;
	if (args != NULL) {
		strlcpy(q->args, args, sizeof(q->args));
	}
	if (kernel_thread_start(warm_thread, q, &t) != KERN_SUCCESS) {
		kfree_type(struct warm_req, q);
		return ENOMEM;
	}
	thread_deallocate(t);
	return 0;
}

void
hakios_warm_console(const char *arg)
{
	while (arg != NULL && *arg == ' ') {
		arg++;
	}
	if (arg == NULL || *arg == 0) {
		printf("usb: warm: BOOT again without a reset\n");
		(void)warm_spawn(SCNR_M_XNU, NULL, false, false);
	} else if (strcmp(arg, "dry") == 0) {
		(void)warm_spawn(SCNR_M_XNU, NULL, false, true);
	} else if (strcmp(arg, "recovery") == 0) {
		printf("usb: warm: TWRP from RECOVERY without a reset (step 3)\n");
		(void)warm_spawn(SCNR_M_ANDROID, NULL, false, false);
	} else if (strncmp(arg, "line ", 5) == 0) {
		(void)warm_spawn(SCNR_M_XNU, arg + 5, true, false);
	} else {
		(void)warm_spawn(SCNR_M_XNU, arg, false, false);
	}
}

static int
warm_sysctl(__unused struct sysctl_oid *oidp, void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = 0, error = sysctl_handle_int(oidp, &value, 0, req);
	bool dry = arg1 != NULL;

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (dry ? value != 1 : (value != 1 && value != 2)) {
		return EINVAL;
	}
	return warm_spawn(value == 2 ? SCNR_M_ANDROID : SCNR_M_XNU, NULL, false, dry);
}
SYSCTL_PROC(_kern, OID_AUTO, sweetwarm_go, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, warm_sysctl,
    "I", "sweetwarm: 1 = a warm restart into BOOT, 2 = into RECOVERY (TWRP)");
SYSCTL_PROC(_kern, OID_AUTO, sweetwarm_dry, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, (void *)1, 0,
    warm_sysctl, "I", "sweetwarm: 1 = read and check BOOT as a warm restart would, then run on");

static void
warm_setup(void)
{
	uint32_t v = 0;

	(void)PE_parse_boot_argn("sweetwarmed", &warm_count, sizeof(warm_count));
	if (warm_count != 0) {
		printf("STARLTE WARM: this boot was started warm (sweetwarmed=%u): no reset, no S-Boot\n", warm_count);
	}
	if (!PE_parse_boot_argn("sweetwarm", &v, sizeof(v)) || v == 0) {
		return;
	}
	warm_mode = v >= 2 ? 2 : 1;
	(void)PE_parse_boot_argn("sweetwarm_smp", &warm_smp, sizeof(warm_smp));
	warm_area = (volatile uint8_t *)ml_io_map(WARM_AREA_PA, WARM_AREA_SIZE);
	warm_pmu = (volatile uint32_t *)ml_io_map(PMU_PA, PAGE_SIZE);
	warm_wdt = (volatile uint32_t *)ml_io_map(WDT1_PA, PAGE_SIZE);
	warm_fdt = (volatile uint64_t *)ml_io_map(SCNR_FDT_SAVE, PAGE_SIZE);
	printf("STARLTE WARM: sweetwarm=%u%s: the console's \"warm\"%s (handoff area %p, S-Boot FDT %s)\n", warm_mode,
	    warm_smp ? " sweetwarm_smp=1" : "", warm_mode == 2 ? " and every restart of iOS" : "", warm_area,
	    warm_fdt != NULL && warm_fdt[0] == SCNR_FDT_MAGIC ? "kept" : "not kept");
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, warm_setup);

#endif /* ARM64_BOARD_CONFIG_STARLTE */
