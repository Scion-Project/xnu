/*
 * STARLTE (Galaxy S9 SM-G960F, Exynos 9810) Mali-G72 MP18 (Bifrost), stage 1:
 * read-only probe, the S9 counterpart of herolte_gpu.c (Mali-T880).
 *
 * From the S9+ mainline port (SheroAbi/galaxy-s9plus-mainline-linux, which
 * drives this GPU with Panfrost on the same SoC): gpu@17500000 (0x5000; SPIs
 * job 282, mmu 281, gpu 283), G3D domain PMU EMBEDDED_G3D_CONFIGURATION/STATUS
 * 0x14064040/0x14064044, CMU_G3D 0x17400000 (PLL_G3D CON0 at +0x140),
 * vdd_g3d = S2MPS18 BUCK6 (reg 0x2b, 300 mV + n * 6.25 mV; S-Boot leaves
 * 644 mV, jobs need 950 mV). S-Boot hands the domain over switched on; after
 * a power cycle the domain's TZPC must be restored through EL3
 * (SMC 0x82000410(1, 0x17410204, 0)) or the first GPU access is an SError.
 *
 * The probe writes nothing. CMU_G3D and the GPU are read only while the
 * domain status low nibble is 0xf. Boot-arg stargpu=0 skips everything,
 * 1 probes only, 2 (default) also runs the stage 2 jobs (below).
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <sys/errno.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
extern bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
extern bool starlte_acpm_dvfs(uint32_t domain, uint32_t khz);
static void sg_clock_up(void);
/* SweetFramebuffer.cpp: no CPU copies to the panel before this mach time. */
extern uint64_t sweet_fb_hold_until;
void starlte_gpu_probe(void);

#define SG_PMU_G3D_PA   0x14064000u     /* page with CONFIGURATION +0x40, STATUS +0x44 */
#define SG_PMU_G3D_CONF 0x40u
#define SG_PMU_G3D_STAT 0x44u
#define SG_CMU_PA       0x17400000u
#define SG_GPU_PA       0x17500000u

/* Mali Bifrost register offsets (Linux Panfrost panfrost_regs.h). */
#define GPU_ID                  0x000
#define L2_FEATURES             0x004
#define CORE_FEATURES           0x008
#define TILER_FEATURES          0x00c
#define MEM_FEATURES            0x010
#define MMU_FEATURES            0x014
#define AS_PRESENT              0x018
#define JS_PRESENT              0x01c
#define GPU_IRQ_RAWSTAT         0x020
#define GPU_STATUS              0x034
#define THREAD_MAX_THREADS      0x0a0
#define SHADER_PRESENT_LO       0x100
#define TILER_PRESENT_LO        0x110
#define L2_PRESENT_LO           0x120
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160
#define COHERENCY_FEATURES      0x300

static vm_offset_t sg_gpu;

static uint32_t
sg_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__builtin_arm_dmb(0xb);
	return v;
}

static void
sg_msleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	clock_delay_until(deadline);
}

static void
sg_probe(vm_offset_t pmu, const char *when)
{
	uint32_t conf = sg_rd(pmu, SG_PMU_G3D_CONF), stat = sg_rd(pmu, SG_PMU_G3D_STAT);
	printf("STARLTE GPU: %s: PMU G3D conf=%#x status=%#x\n", when, conf, stat);
	if ((stat & 0xf) != 0xf) {
		printf("STARLTE GPU: %s: G3D domain off; CMU and Mali not read\n", when);
		return;
	}
	static vm_offset_t cmu;
	if (!cmu) {
		cmu = ml_io_map(SG_CMU_PA, 0x2000);
	}
	if (!sg_gpu) {
		sg_gpu = ml_io_map(SG_GPU_PA, 0x5000);
	}
	vm_offset_t gpu = sg_gpu;
	if (!cmu || !gpu) {
		printf("STARLTE GPU: MMIO mapping failed\n");
		return;
	}
	uint32_t con0 = sg_rd(cmu, 0x140);
	uint64_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	uint64_t khz = (con0 & 0x10) && p ? 26000ull * m / (p << s) : 26000;
	printf("STARLTE GPU: %s: PLL_G3D CON0=%#x CON2=%#x (%llu kHz%s), USER mux=%#x BUSD mux=%#x div=%#x busp=%#x\n",
	    when, con0, sg_rd(cmu, 0x148), khz, (con0 & (1u << 29)) ? ", locked" : ", not stable",
	    sg_rd(cmu, 0x100), sg_rd(cmu, 0x1000), sg_rd(cmu, 0x1800), sg_rd(cmu, 0x1804));
	if ((sg_rd(pmu, SG_PMU_G3D_STAT) & 0xf) != 0xf) {
		printf("STARLTE GPU: %s: domain went off; Mali not read\n", when);
		return;
	}
	uint32_t id = sg_rd(gpu, GPU_ID);
	printf("STARLTE GPU: %s: GPU_ID=%#x product %#x (0x6221 = Mali-G72) r%up%u\n",
	    when, id, id >> 16, (id >> 12) & 0xf, (id >> 4) & 0xff);
	printf("STARLTE GPU: L2_FEATURES=%#x CORE=%#x TILER=%#x MEM=%#x MMU=%#x AS=%#x JS=%#x THREADS=%u\n",
	    sg_rd(gpu, L2_FEATURES), sg_rd(gpu, CORE_FEATURES), sg_rd(gpu, TILER_FEATURES),
	    sg_rd(gpu, MEM_FEATURES), sg_rd(gpu, MMU_FEATURES), sg_rd(gpu, AS_PRESENT),
	    sg_rd(gpu, JS_PRESENT), sg_rd(gpu, THREAD_MAX_THREADS));
	printf("STARLTE GPU: present shader=%#x tiler=%#x l2=%#x; ready shader=%#x tiler=%#x l2=%#x\n",
	    sg_rd(gpu, SHADER_PRESENT_LO), sg_rd(gpu, TILER_PRESENT_LO), sg_rd(gpu, L2_PRESENT_LO),
	    sg_rd(gpu, SHADER_READY_LO), sg_rd(gpu, TILER_READY_LO), sg_rd(gpu, L2_READY_LO));
	printf("STARLTE GPU: GPU_STATUS=%#x IRQ_RAWSTAT=%#x COHERENCY_FEATURES=%#x\n",
	    sg_rd(gpu, GPU_STATUS), sg_rd(gpu, GPU_IRQ_RAWSTAT), sg_rd(gpu, COHERENCY_FEATURES));
}

/*
 * Stage 2: the first GPU jobs, polled (no interrupts yet), as herolte_gpu.c
 * did on the T880 (herogpu=3). Steps:
 *   1. checks: domain on, GPU_ID 0x6221, PLL_G3D locked (S-Boot's 260 MHz,
 *      the lowest Panfrost OPP on this SoC; the clock is not touched)
 *   2. vdd_g3d: S2MPS18 BUCK6 (reg 0x2b) raised to 950 mV (0x68) through
 *      ACPM, only from a value in 0.5..1.0 V, read back
 *   3. soft reset (GPU_COMMAND 1, wait RESET_COMPLETED), as Panfrost does
 *   4. L2, then all shader cores, then the tiler powered on (bounded polls)
 *   5. AS0 with Mali-LPAE tables (Panfrost runs this G72 in LPAE mode,
 *      TRANSCFG = 0 = legacy), MEMATTR 0x888d88, TRANSTAB | 7
 *   6. WRITE_VALUE "zero" on JS1 (the T880's first job), then WRITE_VALUE
 *      "immediate 64" writing "STARBITE", both on every shader core
 *   7. stage 3: fragment clears (sg_clear below), first of a private
 *      64x64 buffer, then of a 256x256 square in the middle of the panel
 *   8. stage 4: the first draw, a tiler job + fragment job built offline by
 *      tools/gpu/starlte-g72-tri.c with Mesa (starlte_gpu_tri.h below)
 *   9. stage 5: if every stage passed, the GPU stays powered at 950 mV and
 *      takes over SweetFramebuffer's full-frame presents (sweetfb_gpu=1,
 *      starlte_gpu_present_* below); otherwise cores off, BUCK6 back to the
 *      value S-Boot left
 * The GPU memory is 352 KiB at 0x8f0000000, in bank 2 above XNU's 1 GiB
 * (0x880000000, tools/mkadt_starlte.py --bank2), mapped uncached on the
 * CPU (COHERENCY_FEATURES = 0: the GPU is not IO-coherent): page tables at
 * +0x0000, the panel's level-3 tables at +0x4000, data pages from
 * +0x10000 at GPU VA 0x02000000. The panel scanout (mkadt_starlte.py
 * FB_BASE 0xcc000000, 1440x2960, stride 5760) is mapped at GPU VA
 * 0x04000000 (both 2 MiB aligned). Stage 4's tiler heap is 16 MiB more at
 * 0x8f1000000, GPU VA 0x10000000, its 8 level-3 tables at +0x40000 (no
 * growable heap: nothing here handles GPU page faults). Stage 5's surface
 * slots: 4 x 12 MiB at GPU VA 0x20000000.., their level-3 tables at +0x48000
 * (12 MiB: iOS's 1242x2208 canvas at 5120 bytes a row is 11.3 MB).
 */
#define SG_MEM_PA       0x8f0000000ull
#define SG_MEM_BYTES    0x60000u
#define SG_VA           0x02000000ull   /* the data pages, from +SG_DATA_OFF */
#define SG_DATA_OFF     0x10000u
#define SG_DATA_PAGES   48u
#define SG_PANEL_L3_OFF 0x4000u
#define SG_FB_PA        0xcc000000ull
#define SG_FB_W         1440u
#define SG_FB_H         2960u
#define SG_FB_STRIDE    (SG_FB_W * 4u)
#define SG_PANEL_VA     0x04000000ull
#define SG_HEAP_PA      0x8f1000000ull
#define SG_HEAP_L3_OFF  0x40000u

#include "starlte_gpu_tri.h"
#define SG_BUCK6        0x2bu
#define SG_BUCK6_950MV  0x68u           /* 300 mV + 0x68 * 6.25 mV */

#define GPU_IRQ_CLEAR           0x024
#define GPU_COMMAND             0x030
#define GPU_FAULTSTATUS         0x03c
#define GPU_FAULTADDR_LO        0x040
#define SHADER_PWRON_LO         0x180
#define TILER_PWRON_LO          0x190
#define L2_PWRON_LO             0x1a0
#define SHADER_PWROFF_LO        0x1c0
#define TILER_PWROFF_LO         0x1d0
#define L2_PWROFF_LO            0x1e0
#define JOB_IRQ_RAWSTAT         0x1000
#define JOB_IRQ_CLEAR           0x1004
#define JS_BASE(n)              (0x1800u + (n) * 0x80u)
#define MMU_IRQ_RAWSTAT         0x2000
#define MMU_IRQ_CLEAR           0x2004
#define AS0                     0x2400
#define ASN(n)                  (0x2400u + (n) * 0x40u)

static vm_offset_t sg_mem;

static void
sg_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static bool
sg_power(uint32_t on, uint32_t ready, uint32_t mask, const char *name)
{
	sg_wr(sg_gpu, on, mask);
	uint32_t r = 0;
	unsigned us;
	for (us = 0; us < 20000; us += 10) {
		r = sg_rd(sg_gpu, ready);
		if ((r & mask) == mask) {
			break;
		}
		delay(10);
	}
	printf("STARLTE GPU: power %s mask=%#x ready=%#x after %u us\n", name, mask, r, us);
	return (r & mask) == mask;
}

static void
sg_cores_off(void)
{
	sg_wr(sg_gpu, TILER_PWROFF_LO, sg_rd(sg_gpu, TILER_READY_LO));
	sg_wr(sg_gpu, SHADER_PWROFF_LO, sg_rd(sg_gpu, SHADER_READY_LO));
	delay(100);
	sg_wr(sg_gpu, L2_PWROFF_LO, sg_rd(sg_gpu, L2_READY_LO));
	delay(100);
	printf("STARLTE GPU: cores off: ready shader=%#x tiler=%#x l2=%#x\n",
	    sg_rd(sg_gpu, SHADER_READY_LO), sg_rd(sg_gpu, TILER_READY_LO), sg_rd(sg_gpu, L2_READY_LO));
}

static bool
sg_soft_reset(void)
{
	sg_wr(sg_gpu, GPU_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, GPU_COMMAND, 1);                  /* SOFT_RESET */
	uint32_t raw = 0;
	unsigned us;
	for (us = 0; us < 10000; us += 10) {
		raw = sg_rd(sg_gpu, GPU_IRQ_RAWSTAT);
		if (raw & 0x100) {                      /* RESET_COMPLETED */
			break;
		}
		delay(10);
	}
	sg_wr(sg_gpu, GPU_IRQ_CLEAR, 0xffffffffu);
	printf("STARLTE GPU: soft reset %s after %u us (rawstat %#x), GPU_STATUS=%#x\n",
	    (raw & 0x100) ? "done" : "TIMEOUT", us, raw, sg_rd(sg_gpu, GPU_STATUS));
	return raw & 0x100;
}

/* Page tables: SG_DATA_PAGES pages from +SG_DATA_OFF at GPU VA SG_VA, the
 * panel at SG_PANEL_VA (9 level-3 tables of 2 MiB). */
static void
sg_tables(void)
{
	volatile uint64_t *q = (volatile uint64_t *)sg_mem;
	for (unsigned i = 0; i < SG_MEM_BYTES / 8; i++) {
		q[i] = 0;
	}
	volatile uint64_t *l0 = (volatile uint64_t *)sg_mem;
	volatile uint64_t *l1 = (volatile uint64_t *)(sg_mem + 0x1000);
	volatile uint64_t *l2 = (volatile uint64_t *)(sg_mem + 0x2000);
	volatile uint64_t *l3 = (volatile uint64_t *)(sg_mem + 0x3000);
	l0[0] = (SG_MEM_PA + 0x1000) | 3;
	l1[0] = (SG_MEM_PA + 0x2000) | 3;
	l2[(SG_VA >> 21) & 0x1ff] = (SG_MEM_PA + 0x3000) | 3;
	for (unsigned i = 0; i < SG_DATA_PAGES; i++) {
		/* Mali-LPAE leaf: read/write, outer shareable, MEMATTR index 0. */
		l3[i] = (SG_MEM_PA + SG_DATA_OFF + i * 0x1000) | 0x2c1;
	}
	unsigned pages = (SG_FB_STRIDE * SG_FB_H + 0xfff) / 0x1000;
	for (unsigned t = 0; t * 512 < pages; t++) {
		uint32_t table = SG_PANEL_L3_OFF + t * 0x1000;
		volatile uint64_t *pl3 = (volatile uint64_t *)(sg_mem + table);
		l2[((SG_PANEL_VA >> 21) & 0x1ff) + t] = (SG_MEM_PA + table) | 3;
		for (unsigned i = 0; i < 512 && t * 512 + i < pages; i++) {
			pl3[i] = (SG_FB_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x2c1;
		}
	}
	for (unsigned t = 0; t * 512 < SG_TRI_HEAP_BYTES / 0x1000; t++) {
		uint32_t table = SG_HEAP_L3_OFF + t * 0x1000;
		volatile uint64_t *hl3 = (volatile uint64_t *)(sg_mem + table);
		l2[((SG_TRI_HEAP_VA >> 21) & 0x1ff) + t] = (SG_MEM_PA + table) | 3;
		for (unsigned i = 0; i < 512; i++) {
			hl3[i] = (SG_HEAP_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x2c1;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
sg_mmu_setup(void)
{
	uint64_t transtab = SG_MEM_PA | 0x7;            /* ADRMODE_TABLE | READ_INNER */
	printf("STARLTE GPU: AS0 before: TRANSCFG=%#x STATUS=%#x\n",
	    sg_rd(sg_gpu, AS0 + 0x30), sg_rd(sg_gpu, AS0 + 0x28));
	sg_wr(sg_gpu, AS0 + 0x00, (uint32_t)transtab);
	sg_wr(sg_gpu, AS0 + 0x04, (uint32_t)(transtab >> 32));
	sg_wr(sg_gpu, AS0 + 0x08, 0x888d88);            /* MEMATTR */
	sg_wr(sg_gpu, AS0 + 0x0c, 0);
	sg_wr(sg_gpu, AS0 + 0x30, 0);                   /* TRANSCFG: legacy (LPAE) */
	sg_wr(sg_gpu, AS0 + 0x34, 0);
	sg_wr(sg_gpu, AS0 + 0x18, 1);                   /* AS_COMMAND_UPDATE */
	for (unsigned i = 0; i < 10000 && (sg_rd(sg_gpu, AS0 + 0x28) & 1); i++) {
		delay(10);
	}
	if (sg_rd(sg_gpu, AS0 + 0x28) & 1) {
		printf("STARLTE GPU: AS0 update timeout\n");
		return false;
	}
	return true;
}

/* Log only failed jobs (the per-frame GPU present). */
static bool sg_run_quiet;
/* Address space and poll bound (x 10 us) of the jobs sg_run() starts: AS0
 * and 200 ms for the kernel's own jobs, AS1 and longer for apps' (stage 6). */
static unsigned sg_run_as;
static unsigned sg_run_polls = 20000;

/* an app chain may still be on the GPU (async submits, stage 6): finish it first. */
static bool sgu_busy;
static int sgu_retire_locked(bool block);

/* Start the chain at GPU VA job_va on slot js, wait (bounded), log. */
static bool
sg_run(unsigned js, uint64_t job_va, uint32_t affinity, const char *what)
{
	if (sgu_busy) {
		(void)sgu_retire_locked(true);
	}
	uint32_t slot = JS_BASE(js);
	sg_wr(sg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, slot + 0x40, (uint32_t)job_va);           /* HEAD_NEXT */
	sg_wr(sg_gpu, slot + 0x44, (uint32_t)(job_va >> 32));
	sg_wr(sg_gpu, slot + 0x50, affinity);                   /* AFFINITY_NEXT */
	sg_wr(sg_gpu, slot + 0x54, 0);
	/* AS sg_run_as, flush clean+invalidate at start and end, priority 8 */
	sg_wr(sg_gpu, slot + 0x58, (8u << 16) | (3u << 8) | (3u << 12) | sg_run_as);
	uint64_t t0 = mach_absolute_time();
	sg_wr(sg_gpu, slot + 0x60, 1);                          /* COMMAND_NEXT START */

	uint32_t jirq = 0, mirq = 0, girq = 0;
	for (unsigned i = 0; i < sg_run_polls; i++) {
		jirq = sg_rd(sg_gpu, JOB_IRQ_RAWSTAT);
		mirq = sg_rd(sg_gpu, MMU_IRQ_RAWSTAT);
		girq = sg_rd(sg_gpu, GPU_IRQ_RAWSTAT);
		if ((jirq & ((1u << js) | (1u << (js + 16)))) || mirq || (girq & 0x81)) {
			break;
		}
		delay(10);
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	/* The job header, for the log: only the kernel's own jobs (AS0) are
	 * mapped here; an app's job header is read back by the app. */
	static const uint32_t no_hdr[4];
	volatile const uint32_t *hdr = sg_run_as == 0 ?
	    (volatile const uint32_t *)(sg_mem + SG_DATA_OFF + (job_va - SG_VA)) : no_hdr;
	bool ok = (jirq & (1u << js)) && !(jirq & (1u << (js + 16))) && !mirq && !(girq & 0x81);
	if (!ok || !sg_run_quiet) {
		printf("STARLTE GPU: %s: %s in %llu us: irq=%#x mmu=%#x gpu=%#x js_status=%#x "
		    "as_fault=%#x@%#x exception=%#x fault=%#x:%#x gpu_fault=%#x@%#x\n", what,
		    ok ? "done" : "FAILED", ns / 1000, jirq, mirq, girq,
		    sg_rd(sg_gpu, slot + 0x24), sg_rd(sg_gpu, ASN(sg_run_as) + 0x1c),
		    sg_rd(sg_gpu, ASN(sg_run_as) + 0x20),
		    hdr[0], hdr[3], hdr[2], sg_rd(sg_gpu, GPU_FAULTSTATUS), sg_rd(sg_gpu, GPU_FAULTADDR_LO));
	}
	sg_wr(sg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	return ok;
}

/*
 * One WRITE_VALUE job at data offset job_off (Job Header, 32 bytes, then
 * the payload: address, type, immediate; Mesa genxml v6), writing to
 * out_off. Types: 3 = zero, 7 = immediate 64.
 */
static bool
sg_write_value(uint32_t job_off, uint32_t out_off, uint32_t type, uint64_t value,
    uint64_t expect, uint32_t affinity, const char *what)
{
	vm_offset_t d = sg_mem + SG_DATA_OFF;
	volatile uint8_t *job = (volatile uint8_t *)(d + job_off);
	for (unsigned i = 0; i < 64; i++) {
		job[i] = 0;
	}
	job[16] = 1 | (2 << 1);                         /* 64-bit, WRITE_VALUE */
	*(volatile uint16_t *)(job + 18) = 1;           /* index */
	*(volatile uint64_t *)(job + 32) = SG_VA + out_off;
	*(volatile uint32_t *)(job + 40) = type;
	*(volatile uint64_t *)(job + 48) = value;
	volatile uint64_t *out = (volatile uint64_t *)(d + out_off);
	*out = 0xa5a5a5a55a5a5a5aull;
	__asm__ volatile ("dsb sy" ::: "memory");
	bool ok = sg_run(1, SG_VA + job_off, affinity, what) && *out == expect;
	printf("STARLTE GPU: %s: output=%#llx (want %#llx) -> %s\n", what, *out, expect,
	    ok ? "THE MALI-G72 RAN A JOB" : "failed");
	return ok;
}

/*
 * Stage 3: a fragment job that only clears (Mesa genxml v6.xml and
 * pan_desc.c, GENX(pan_emit_fbd) for PAN_ARCH 6). No draws, so no tiler job:
 * Mesa then leaves the framebuffer's tiler pointer 0; with_tiler instead
 * points it at a Tiler Context + Tiler Heap as Mesa's jm_emit_tiler_desc()
 * builds for batches with draws (the fallback if a null tiler faults).
 * Data offsets: fragment job +0x400, Framebuffer +0x1000 (Parameters 64 B,
 * Padding 64 B) with one Render Target at +0x1080, sample positions
 * +0x1200, Tiler Heap +0x1400, Tiler Context +0x1440, heap +0x10000.
 * One 16x16 tile (256 px, the v6 maximum) of RGBA8 = 1 KiB of tile buffer.
 */
#define SG_FRAG_OFF     0x0400u
#define SG_FBD_OFF      0x1000u
#define SG_SAMPLES_OFF  0x1200u
#define SG_HEAPD_OFF    0x1400u
#define SG_TILER_OFF    0x1440u
#define SG_PIX_OFF      0x2000u         /* private 64x64 RGBA8, 16 KiB */
#define SG_HEAP_OFF     0x10000u
#define SG_HEAP_BYTES   0x20000u

static void
sg_put32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(sg_mem + SG_DATA_OFF + off) = v;
}

static void
sg_put64(uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(sg_mem + SG_DATA_OFF + off) = v;
}

static bool
sg_clear(uint64_t pixels_va, uint32_t w, uint32_t h, uint32_t stride, uint32_t color,
    bool with_tiler, uint32_t affinity, const char *what)
{
	for (uint32_t off = SG_FRAG_OFF; off < SG_FRAG_OFF + 64; off += 4) {
		sg_put32(off, 0);
	}
	for (uint32_t off = SG_FBD_OFF; off < SG_PIX_OFF; off += 4) {
		sg_put32(off, 0);
	}
	/* Sample positions, single-sampled (pan_samples.c): sample 0 and the
	 * origin at the pixel centre, (0 + 8) * 16 = 128 in 1/256 pixel. */
	sg_put32(SG_SAMPLES_OFF + 0 * 4, 128u | 128u << 16);
	sg_put32(SG_SAMPLES_OFF + 32 * 4, 128u | 128u << 16);

	uint64_t tiler = 0;
	if (with_tiler) {
		/* Tiler Heap: size, base, bottom, top. */
		sg_put32(SG_HEAPD_OFF + 4, SG_HEAP_BYTES);
		sg_put64(SG_HEAPD_OFF + 8, SG_VA + SG_HEAP_OFF);
		sg_put64(SG_HEAPD_OFF + 16, SG_VA + SG_HEAP_OFF);
		sg_put64(SG_HEAPD_OFF + 24, SG_VA + SG_HEAP_OFF + SG_HEAP_BYTES);
		/* Tiler Context: hierarchy mask 0xff (8 levels, TILER_FEATURES
		 * 0x809; pan_select_tiler_hierarchy_mask), single-sampled, FB size,
		 * heap. */
		sg_put32(SG_TILER_OFF + 8, 0xff);
		sg_put32(SG_TILER_OFF + 12, (w - 1) | (h - 1) << 16);
		sg_put64(SG_TILER_OFF + 24, SG_VA + SG_HEAPD_OFF);
		tiler = SG_VA + SG_TILER_OFF;
	}

	/* Framebuffer Parameters. Pre/post frame shaders never (word 0). */
	sg_put64(SG_FBD_OFF + 16, SG_VA + SG_SAMPLES_OFF);
	sg_put32(SG_FBD_OFF + 32, (w - 1) | (h - 1) << 16);    /* width, height */
	sg_put32(SG_FBD_OFF + 36, 0);                           /* bound min */
	sg_put32(SG_FBD_OFF + 40, (w - 1) | (h - 1) << 16);    /* bound max */
	/* 1 sample, single pattern, tie-break MINUS_180_IN_0_OUT (2), tile
	 * 256 px (log2 8), 1 render target, colour buffer 1 KiB (>> 10). */
	sg_put32(SG_FBD_OFF + 44, 2u << 6 | 8u << 9 | 1u << 24);
	sg_put32(SG_FBD_OFF + 48, 1u << 16);                    /* Z internal D24 */
	sg_put64(SG_FBD_OFF + 56, tiler);

	/* Render Target: internal R8G8B8A8; write enable, writeback R8G8B8A8
	 * (19), linear (2), dithering, swizzle RGBA (0x688), clean pixel
	 * write; base, row stride; clear colour 0. */
	uint32_t rt = SG_FBD_OFF + 128;
	sg_put32(rt + 0, 1u << 26);
	sg_put32(rt + 4, 1u | 19u << 3 | 2u << 10 | 1u << 15 | 0x688u << 16 | 1u << 31);
	sg_put64(rt + 32, pixels_va);
	sg_put32(rt + 40, stride);
	sg_put32(rt + 48, color);

	/* Fragment job: header (64-bit, type 9, index 1), bounds in 16x16
	 * tiles, framebuffer pointer tagged MFBD (bit 0), 1 render target. */
	sg_put32(SG_FRAG_OFF + 16, 1u | 9u << 1 | 1u << 16);
	sg_put32(SG_FRAG_OFF + 32, 0);
	sg_put32(SG_FRAG_OFF + 36, ((w - 1) >> 4) | ((h - 1) >> 4) << 16);
	sg_put64(SG_FRAG_OFF + 40, (SG_VA + SG_FBD_OFF) | 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	return sg_run(0, SG_VA + SG_FRAG_OFF, affinity, what);
}

/* Reset, power everything on, fresh page tables, AS0. */
static bool
sg_gpu_up(uint32_t shaders)
{
	if (!sg_soft_reset()) {
		return false;
	}
	bool on = sg_power(L2_PWRON_LO, L2_READY_LO, sg_rd(sg_gpu, L2_PRESENT_LO), "L2") &&
	    sg_power(SHADER_PWRON_LO, SHADER_READY_LO, shaders, "shader cores") &&
	    sg_power(TILER_PWRON_LO, TILER_READY_LO, sg_rd(sg_gpu, TILER_PRESENT_LO), "tiler");
	printf("STARLTE GPU: GPU_STATUS=%#x rawstat=%#x after power-up\n",
	    sg_rd(sg_gpu, GPU_STATUS), sg_rd(sg_gpu, GPU_IRQ_RAWSTAT));
	if (!on) {
		return false;
	}
	sg_tables();
	return sg_mmu_setup();
}

#define SG_GREEN        0xff00ff00u     /* RGBA8 and BGRA8 alike */
#define SG_SQ           256u
#define SG_SQ_X         592u            /* (1440 - 256) / 2, 16-pixel aligned */
#define SG_SQ_Y         1344u           /* about (2960 - 256) / 2, 16-aligned */

static void
sg_stage3(uint32_t shaders)
{
	/* Private 64x64 buffer, filled with a marker first. */
	volatile uint32_t *pix = (volatile uint32_t *)(sg_mem + SG_DATA_OFF + SG_PIX_OFF);
	bool with_tiler = false, ok = false;
	for (unsigned attempt = 0; attempt < 2 && !ok; attempt++) {
		with_tiler = attempt == 1;
		if (attempt && !sg_gpu_up(shaders)) {
			return;
		}
		for (unsigned i = 0; i < 64 * 64; i++) {
			pix[i] = 0x12345678u;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		bool ran = sg_clear(SG_VA + SG_PIX_OFF, 64, 64, 64 * 4, SG_GREEN, with_tiler, shaders,
		    with_tiler ? "FRAGMENT clear 64x64 (tiler context)" : "FRAGMENT clear 64x64 (no tiler)");
		unsigned good = 0;
		for (unsigned i = 0; i < 64 * 64; i++) {
			good += pix[i] == SG_GREEN;
		}
		ok = ran && good == 64 * 64;
		printf("STARLTE GPU: private clear: %u of 4096 pixels green, first %#x last %#x -> %s\n",
		    good, pix[0], pix[64 * 64 - 1], ok ? "THE MALI-G72 CLEARED A BUFFER" : "failed");
	}
	if (!ok) {
		printf("STARLTE GPU: stage 3 private clear did not pass; panel left alone\n");
		return;
	}
	/* The panel: a 256x256 green square in the middle, judged by the job
	 * alone. The CPU no longer reads it back: iOS draws there too by now (8
	 * cores), and every mapping of it cost the small I/O mapping space for
	 * good (ml_io_map entries are permanent; ml_io_unmap panics). */
	uint64_t va = SG_PANEL_VA + (uint64_t)SG_SQ_Y * SG_FB_STRIDE + SG_SQ_X * 4u;
	bool ok3 = sg_clear(va, SG_SQ, SG_SQ, SG_FB_STRIDE, SG_GREEN, with_tiler, shaders,
	    "FRAGMENT clear panel 256x256");
	printf("STARLTE GPU: stage 3 %s\n", ok3 ? "PASSED" : "did not pass");
}

/*
 * Stage 4: one magenta triangle on a dark grey 512x512 square in the middle
 * of the panel. The blob (tiler job, fragment job, framebuffer, tiler
 * context/heap descriptors, renderer state + blend, viewport, positions,
 * and the fragment shader from Mesa's Bifrost compiler) is position
 * dependent: it goes to data offset 0x8000 = GPU VA SG_TRI_BLOB_VA. The
 * tiler job bins the triangle (JS1), then the fragment job clears and
 * shades the tiles (JS0), as Panfrost submits them.
 */
static bool
sg_stage4(uint32_t shaders)
{
	uint32_t base = (uint32_t)(SG_TRI_BLOB_VA - SG_VA);
	for (unsigned i = 0; i < SG_TRI_BLOB_WORDS; i++) {
		sg_put32(base + i * 4, sg_tri_blob[i]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	bool tiled = sg_run(1, SG_TRI_BLOB_VA + SG_TRI_TILER_JOB, shaders, "TILER triangle");
	bool shaded = tiled && sg_run(0, SG_TRI_BLOB_VA + SG_TRI_FRAG_JOB, shaders,
	    "FRAGMENT triangle 512x512");

	/* Judged by the jobs alone, like stage 3's panel square (no CPU
	 * mapping of the panel, see there). */
	bool ok = shaded;
	printf("STARLTE GPU: stage 4 %s\n", ok ? "PASSED" : "did not pass");
	return ok;
}

/*
 * Stage 5: the GPU present, as the S7's herolte_gpu_present_* (patch 0303
 * points SweetFramebuffer's gpuPresent() here on STARLTE). iOS's canvas
 * (SG_PRES_CANVAS_W x _H, the ADT canvas / sweetfb_lw,lh) is a texture on
 * a quad at full panel height between black bars, bilinear, on all 18
 * cores; the blob (tools/gpu/starlte-g72-tri.c) is copied fresh for every
 * frame (the GPU writes back job headers and tiler state) and only the
 * texture's surface pointer and strides are patched. iOS surfaces get GPU
 * VAs in 12 MiB slots (6 level-3 tables each); a slot is keyed by surface
 * id and first page, a remapped slot takes a fresh VA and the MMU's page
 * table caches are flushed. Called only from SweetFramebuffer's present(),
 * which its vsync timer serializes, and only after sgp_ready is set.
 */
#define SGP_SLOTS       4u
#define SGP_SLOT_TABLES 6u
#define SGP_SLOT_PAGES  (SGP_SLOT_TABLES * 512u)
#define SGP_L3_OFF      0x48000u
#define SGP_VA_BASE     0x20000000ull
#define SGP_VA_END      0x40000000ull   /* the one level-2 table covers 1 GiB */

struct sgp_slot {
	uint32_t        id;
	uint64_t        first_pa;
	uint64_t        va;
};
static struct sgp_slot sgp_slots[SGP_SLOTS];
static unsigned sgp_victim;
static uint64_t sgp_next_va = SGP_VA_BASE;
static uint32_t sgp_shaders;
static volatile bool sgp_ready;
static uint64_t sgp_frames, sgp_ns_sum, sgp_ns_max;
/* One user of the job slots at a time: the present (vsync work loop) and
 * apps' submits (stage 6, StarbiteMali.cpp). */
static LCK_GRP_DECLARE(sg_lck_grp, "starlte_gpu");
static LCK_MTX_DECLARE(sg_lock, &sg_lck_grp);

int starlte_gpu_present_ready(void);
int starlte_gpu_present_slot(uint32_t id, uint64_t first_pa);
int starlte_gpu_present_map(uint32_t id, const uint64_t *pages, unsigned n);
bool starlte_gpu_present_draw(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h);

static bool
sgp_as_idle(void)
{
	for (unsigned i = 0; i < 10000 && (sg_rd(sg_gpu, AS0 + 0x28) & 1); i++) {
		delay(1);
	}
	return !(sg_rd(sg_gpu, AS0 + 0x28) & 1);
}

/* Lock a VA range and flush the MMU's page table caches for it (kbase's
 * mmu_hw_do_operation(): LOCKADDR, LOCK, FLUSH_PT, which also unlocks). */
static bool
sgp_mmu_flush(uint64_t va, unsigned log2_size)
{
	uint64_t lock = (va & ~((1ull << log2_size) - 1)) | (log2_size - 1);
	if (!sgp_as_idle()) {
		return false;
	}
	sg_wr(sg_gpu, AS0 + 0x10, (uint32_t)lock);              /* AS_LOCKADDR */
	sg_wr(sg_gpu, AS0 + 0x14, (uint32_t)(lock >> 32));
	sg_wr(sg_gpu, AS0 + 0x18, 2);                           /* LOCK */
	if (!sgp_as_idle()) {
		return false;
	}
	sg_wr(sg_gpu, AS0 + 0x18, 4);                           /* FLUSH_PT */
	return sgp_as_idle();
}

int
starlte_gpu_present_ready(void)
{
	return sgp_ready;
}

/* Is the surface (id, first page) mapped? Returns its slot or -1. */
int
starlte_gpu_present_slot(uint32_t id, uint64_t first_pa)
{
	if (!sgp_ready) {
		return -1;
	}
	for (unsigned i = 0; i < SGP_SLOTS; i++) {
		if (sgp_slots[i].id == id && sgp_slots[i].first_pa == first_pa && id != 0) {
			return (int)i;
		}
	}
	return -1;
}

/* Map n 4 KiB pages of a surface into a slot (round robin); slot or -1. */
int
starlte_gpu_present_map(uint32_t id, const uint64_t *pages, unsigned n)
{
	if (!sgp_ready || n == 0 || n > SGP_SLOT_PAGES) {
		return -1;
	}
	unsigned s = sgp_victim;
	sgp_victim = (sgp_victim + 1) % SGP_SLOTS;
	if (sgp_next_va + SGP_SLOT_PAGES * 0x1000ull > SGP_VA_END) {
		sgp_next_va = SGP_VA_BASE;
	}
	uint64_t old_va = sgp_slots[s].va, va = sgp_next_va;
	sgp_next_va += SGP_SLOT_PAGES * 0x1000ull;
	volatile uint64_t *l2 = (volatile uint64_t *)(sg_mem + 0x2000);
	if (old_va) {
		for (unsigned t = 0; t < SGP_SLOT_TABLES; t++) {
			l2[((old_va >> 21) & 0x1ff) + t] = 0;
		}
	}
	for (unsigned t = 0; t < SGP_SLOT_TABLES; t++) {
		uint32_t table = SGP_L3_OFF + (s * SGP_SLOT_TABLES + t) * 0x1000;
		volatile uint64_t *l3 = (volatile uint64_t *)(sg_mem + table);
		for (unsigned i = 0; i < 512; i++) {
			unsigned p = t * 512 + i;
			l3[i] = p < n ? (pages[p] | 0x2c1) : 0;
		}
		l2[((va >> 21) & 0x1ff) + t] = (SG_MEM_PA + table) | 3;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!sgp_mmu_flush(SGP_VA_BASE, 30)) {
		printf("STARLTE GPU PRESENT: MMU flush timeout\n");
	}
	sgp_slots[s].id = id;
	sgp_slots[s].first_pa = pages[0];
	sgp_slots[s].va = va;
	printf("STARLTE GPU PRESENT: surface %u (%u pages, pa %#llx) in slot %u at VA %#llx\n",
	    id, n, pages[0], s, va);
	return (int)s;
}

/*
 * Draw the w x h canvas at byte offset off (row stride bytes) of the
 * surface in slot s to the panel. The caller has cleaned it to the point of
 * coherency. False: the caller falls back to the CPU copy; after a GPU
 * fault the present path switches itself off.
 */
bool
starlte_gpu_present_draw(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h)
{
	if (!sgp_ready || s < 0 || s >= (int)SGP_SLOTS || w != SG_PRES_CANVAS_W ||
	    h != SG_PRES_CANVAS_H || stride < w * 4) {
		return false;
	}
	lck_mtx_lock(&sg_lock);
	uint32_t base = (uint32_t)(SG_PRES_BLOB_VA - SG_VA);
	for (unsigned i = 0; i < SG_PRES_BLOB_WORDS; i++) {
		sg_put32(base + i * 4, sg_pres_blob[i]);
	}
	sg_put64(base + SG_PRES_SURFACE + 0, sgp_slots[s].va + off);
	sg_put32(base + SG_PRES_SURFACE + 8, stride);
	sg_put32(base + SG_PRES_SURFACE + 12, stride * h);
	__asm__ volatile ("dsb sy" ::: "memory");

	uint64_t t0 = mach_absolute_time();
	sg_run_quiet = true;
	bool ok = sg_run(1, SG_PRES_BLOB_VA + SG_PRES_TILER_JOB, sgp_shaders, "PRESENT tiler") &&
	    sg_run(0, SG_PRES_BLOB_VA + SG_PRES_FRAG_JOB, sgp_shaders, "PRESENT fragment");
	sg_run_quiet = false;
	lck_mtx_unlock(&sg_lock);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	sgp_frames++;
	sgp_ns_sum += ns;
	if (ns > sgp_ns_max) {
		sgp_ns_max = ns;
	}
	if (sgp_frames <= 3 || !ok) {
		printf("STARLTE GPU PRESENT: frame %llu %ux%u stride %u slot %d: %llu us -> %s\n",
		    sgp_frames, w, h, stride, s, ns / 1000, ok ? "ok" : "FAILED, back to the CPU copy");
	} else if (sgp_frames % 600 == 0) {
		printf("STARLTE GPU PRESENT: %llu frames, average %llu us, max %llu us\n",
		    sgp_frames, sgp_ns_sum / 600 / 1000, sgp_ns_max / 1000);
		sgp_ns_sum = sgp_ns_max = 0;
	}
	if (!ok) {
		sgp_ready = false;
	}
	return ok;
}

/*
 * Stage 6: the GPU for apps (iokit/Kernel/arm/StarbiteMali.cpp, the
 * Panfrost-style user client Mesa will talk to). Apps share address space 1
 * (the kernel's own jobs and the present keep AS0); its Mali-LPAE tables
 * live in 4 MiB at 0x8f2000000 (bank 2, above the stage 4 heap), mapped
 * uncached: L0 at +0, L1 at +0x1000, the four L2 tables for VA 0..4 GiB at
 * +0x2000, level-3 tables handed out from +0x6000 (1018 of them, 2 GiB
 * of mappings at a time) and given back when empty. Mapped as four 1 MiB pieces: a 4 MiB
 * ml_io_map() panicked in kmem_alloc (the kernel's I/O mapping space is small), and 1 MiB
 * (250 tables, 500 MiB) ran out once CoreAnimation drew with Metal (metal2: "out of level-3
 * tables" x12260, SpringBoard's device and backboardd's command buffers OutOfMemory). Buffers are 4 KiB pages of XNU memory that the
 * user client maps uncached on the CPU side (the GPU is not IO-coherent).
 * Jobs run synchronously under sg_lock, polled (no interrupts yet); a
 * faulting or hung job gets the GPU reset and both address spaces back.
 */
#define SGU_PT_PA       0x8f2000000ull
#define SGU_PT_BYTES    0x400000u
#define SGU_PT_PIECE    0x100000u
#define SGU_L3_FIRST    0x6000u

static vm_offset_t sgu_pt_piece[SGU_PT_BYTES / SGU_PT_PIECE];
/* The kernel address of byte `off` of the tables. */
#define SGU_PT(off)     (sgu_pt_piece[(uint32_t)(off) / SGU_PT_PIECE] + (uint32_t)(off) % SGU_PT_PIECE)
static uint32_t sgu_next_table = SGU_L3_FIRST;
/*
 * Level-3 tables are counted and given back: a table whose last page is unmapped leaves its level-2
 * entry and goes to sgu_l3_free, and the next map takes it again. They used to be handed out from
 * sgu_next_table and never freed: 500 MiB of mappings in all, gone within minutes once
 * CoreAnimation drew with the GPU ("out of level-3 tables" x1282 in one session).
 */
#define SGU_L3_TABLES   ((SGU_PT_BYTES - SGU_L3_FIRST) / 0x1000)
static uint16_t sgu_l3_count[SGU_L3_TABLES];    /* pages mapped through each table */
static uint16_t sgu_l3_free[SGU_L3_TABLES];
static unsigned sgu_l3_nfree, sgu_l3_live, sgu_l3_said;
static bool sgu_ready;
static uint64_t sgu_jobs, sgu_faults;

int starlte_gpu_user_ready(void);
bool starlte_gpu_user_param(uint32_t param, uint64_t *value);
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n);
void starlte_gpu_user_unmap(uint64_t va, unsigned n);
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements);
extern void starbite_mali_publish(void);        /* iokit/Kernel/arm/StarbiteMali.cpp */

static bool
sgu_as_wait(unsigned as)
{
	for (unsigned i = 0; i < 10000 && (sg_rd(sg_gpu, ASN(as) + 0x28) & 1); i++) {
		delay(1);
	}
	return !(sg_rd(sg_gpu, ASN(as) + 0x28) & 1);
}

/* Program address space as (TRANSTAB, MEMATTR, TRANSCFG legacy) and UPDATE. */
static bool
sgu_as_enable(unsigned as, uint64_t table_pa)
{
	uint64_t transtab = table_pa | 0x7;
	if (!sgu_as_wait(as)) {
		return false;
	}
	sg_wr(sg_gpu, ASN(as) + 0x00, (uint32_t)transtab);
	sg_wr(sg_gpu, ASN(as) + 0x04, (uint32_t)(transtab >> 32));
	sg_wr(sg_gpu, ASN(as) + 0x08, 0x888d88);
	sg_wr(sg_gpu, ASN(as) + 0x0c, 0);
	sg_wr(sg_gpu, ASN(as) + 0x30, 0);
	sg_wr(sg_gpu, ASN(as) + 0x34, 0);
	sg_wr(sg_gpu, ASN(as) + 0x18, 1);               /* UPDATE */
	return sgu_as_wait(as);
}

/* LOCK + FLUSH_PT over all 4 GiB of address space as. */
static bool
sgu_flush(unsigned as)
{
	if (!sgu_as_wait(as)) {
		return false;
	}
	sg_wr(sg_gpu, ASN(as) + 0x10, 31);              /* LOCKADDR: base 0, 2^32 */
	sg_wr(sg_gpu, ASN(as) + 0x14, 0);
	sg_wr(sg_gpu, ASN(as) + 0x18, 2);               /* LOCK */
	if (!sgu_as_wait(as)) {
		return false;
	}
	sg_wr(sg_gpu, ASN(as) + 0x18, 4);               /* FLUSH_PT */
	return sgu_as_wait(as);
}

/* Called with the GPU powered and AS0 set up (end of stage 5). */
static bool
sgu_setup(void)
{
	for (unsigned i = 0; i < SGU_PT_BYTES / SGU_PT_PIECE; i++) {
		sgu_pt_piece[i] = ml_io_map((vm_offset_t)(SGU_PT_PA + i * SGU_PT_PIECE), SGU_PT_PIECE);
		if (!sgu_pt_piece[i]) {
			return false;
		}
	}
	volatile uint64_t *q = (volatile uint64_t *)SGU_PT(0);
	for (unsigned i = 0; i < SGU_L3_FIRST / 8; i++) {
		q[i] = 0;
	}
	volatile uint64_t *l0 = q, *l1 = (volatile uint64_t *)SGU_PT(0x1000);
	l0[0] = (SGU_PT_PA + 0x1000) | 3;
	for (unsigned i = 0; i < 4; i++) {
		l1[i] = (SGU_PT_PA + 0x2000 + i * 0x1000) | 3;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return sgu_as_enable(1, SGU_PT_PA);
}

int
starlte_gpu_user_ready(void)
{
	return sgu_ready;
}

/* DRM_PANFROST_PARAM_* values (enum drm_panfrost_param, tools/gpu/panfrost_drm.h,
 * MIT), from the GPU registers as Linux panfrost_gpu.c reads them. */
bool
starlte_gpu_user_param(uint32_t param, uint64_t *value)
{
	static const uint16_t reg[] = {
		[2] = SHADER_PRESENT_LO, [3] = TILER_PRESENT_LO, [4] = L2_PRESENT_LO,
		[6] = AS_PRESENT, [7] = JS_PRESENT, [8] = L2_FEATURES, [9] = CORE_FEATURES,
		[10] = TILER_FEATURES, [11] = MEM_FEATURES, [12] = MMU_FEATURES,
		[13] = 0x0ac /* THREAD_FEATURES */, [14] = THREAD_MAX_THREADS,
		[15] = 0x0a4 /* THREAD_MAX_WORKGROUP_SIZE */, [16] = 0x0a8 /* THREAD_MAX_BARRIER_SIZE */,
		[17] = COHERENCY_FEATURES, [39] = 0x310 /* THREAD_TLS_ALLOC */,
	};
	if (!sgu_ready) {
		return false;
	}
	uint32_t id = sg_rd(sg_gpu, GPU_ID);
	if (param == 0) {                               /* GPU_PROD_ID */
		*value = id >> 16;
	} else if (param == 1) {                        /* GPU_REVISION */
		*value = id & 0xffff;
	} else if (param == 5 || param == 40) {         /* STACK_PRESENT, AFBC_FEATURES */
		*value = 0;
	} else if (param == 44) {                       /* SELECTED_COHERENCY */
		*value = 31;                            /* DRM_PANFROST_GPU_COHERENCY_NONE */
	} else if (param >= 18 && param <= 21) {        /* TEXTURE_FEATURES0..3 */
		*value = sg_rd(sg_gpu, 0x0b0 + (param - 18) * 4);
	} else if (param >= 22 && param <= 37) {        /* JS_FEATURES0..15 */
		*value = sg_rd(sg_gpu, 0x0c0 + (param - 22) * 4);
	} else if (param == 38) {                       /* NR_CORE_GROUPS */
		*value = 1;
	} else if (param < sizeof(reg) / sizeof(reg[0]) && reg[param]) {
		*value = sg_rd(sg_gpu, reg[param]);
		if (param >= 2 && param <= 4) {
			*value |= (uint64_t)sg_rd(sg_gpu, reg[param] + 4) << 32;
		}
	} else {
		return false;
	}
	return true;
}

static unsigned
sgu_l3_index(uint64_t l2e)
{
	return (unsigned)(((l2e & ~0xfffull) - SGU_PT_PA - SGU_L3_FIRST) / 0x1000);
}

/* Unmap n pages at va; tables left empty go back to the pool. sg_lock held. */
static void
sgu_unmap_locked(uint64_t va, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		uint64_t v = va + (uint64_t)i * 0x1000;
		volatile uint64_t *l2 = (volatile uint64_t *)SGU_PT(0x2000 + (v >> 30) * 0x1000);
		unsigned l2i = (v >> 21) & 0x1ff;
		uint64_t l2e = l2[l2i];
		if (!l2e) {
			continue;
		}
		volatile uint64_t *l3 = (volatile uint64_t *)SGU_PT((l2e & ~0xfffull) - SGU_PT_PA);
		unsigned t = sgu_l3_index(l2e);
		if (l3[(v >> 12) & 0x1ff] != 0) {
			l3[(v >> 12) & 0x1ff] = 0;
			if (t < SGU_L3_TABLES && sgu_l3_count[t] > 0 && --sgu_l3_count[t] == 0) {
				l2[l2i] = 0;
				sgu_l3_free[sgu_l3_nfree++] = (uint16_t)t;
				sgu_l3_live--;
			}
		}
	}
}

/* Map n 4 KiB pages at GPU VA va (4 KiB aligned, below 4 GiB) in AS1; all or nothing. */
bool
starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n)
{
	if (!sgu_ready || (va & 0xfff) || va + (uint64_t)n * 0x1000 > (1ull << 32)) {
		return false;
	}
	lck_mtx_lock(&sg_lock);
	unsigned i;
	for (i = 0; i < n; i++) {
		uint64_t v = va + (uint64_t)i * 0x1000;
		volatile uint64_t *l2 = (volatile uint64_t *)SGU_PT(0x2000 + (v >> 30) * 0x1000);
		unsigned l2i = (v >> 21) & 0x1ff;
		if (!l2[l2i]) {
			uint32_t table;
			if (sgu_l3_nfree > 0) {
				table = SGU_L3_FIRST + (uint32_t)sgu_l3_free[--sgu_l3_nfree] * 0x1000;
			} else if (sgu_next_table + 0x1000 <= SGU_PT_BYTES) {
				table = sgu_next_table;
				sgu_next_table += 0x1000;
			} else {
				printf("STARLTE GPU USER: out of level-3 tables (%u in use)\n", sgu_l3_live);
				break;
			}
			volatile uint64_t *nt = (volatile uint64_t *)SGU_PT(table);
			for (unsigned j = 0; j < 512; j++) {
				nt[j] = 0;
			}
			l2[l2i] = (SGU_PT_PA + table) | 3;
			sgu_l3_count[(table - SGU_L3_FIRST) / 0x1000] = 0;
			sgu_l3_live++;
		}
		volatile uint64_t *l3 = (volatile uint64_t *)SGU_PT((l2[l2i] & ~0xfffull) - SGU_PT_PA);
		if (l3[(v >> 12) & 0x1ff] == 0) {
			sgu_l3_count[sgu_l3_index(l2[l2i])]++;
		}
		l3[(v >> 12) & 0x1ff] = (pages[i] & ~0xfffull) | 0x2c1;
	}
	bool ok = i == n;
	if (!ok) {
		sgu_unmap_locked(va, i);
	}
	/* how full the pool gets: a line at each quarter, on the way up */
	unsigned quarter = sgu_l3_live * 4 / SGU_L3_TABLES;
	if (quarter > sgu_l3_said) {
		sgu_l3_said = quarter;
		printf("STARLTE GPU USER: %u of %u level-3 tables in use\n", sgu_l3_live, (unsigned)SGU_L3_TABLES);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!sgu_flush(1)) {
		printf("STARLTE GPU USER: AS1 flush timeout\n");
	}
	lck_mtx_unlock(&sg_lock);
	return ok;
}

void
starlte_gpu_user_unmap(uint64_t va, unsigned n)
{
	if (!sgu_ready) {
		return;
	}
	lck_mtx_lock(&sg_lock);
	sgu_unmap_locked(va, n);
	__asm__ volatile ("dsb sy" ::: "memory");
	(void)sgu_flush(1);
	lck_mtx_unlock(&sg_lock);
}

/* After a fault or a hung job: reset, power, AS0 and AS1 as they were. */
static void
sgu_recover(void)
{
	(void)sg_soft_reset();
	(void)sg_power(L2_PWRON_LO, L2_READY_LO, sg_rd(sg_gpu, L2_PRESENT_LO), "L2");
	(void)sg_power(SHADER_PWRON_LO, SHADER_READY_LO, sgp_shaders, "shader cores");
	(void)sg_power(TILER_PWRON_LO, TILER_READY_LO, sg_rd(sg_gpu, TILER_PRESENT_LO), "tiler");
	bool as0 = sg_mmu_setup(), as1 = sgu_as_enable(1, SGU_PT_PA);
	printf("STARLTE GPU USER: GPU reset after a failed job, AS0 %s AS1 %s\n",
	    as0 ? "ok" : "FAILED", as1 ? "ok" : "FAILED");
}

/*
 * Run the job chain at jc (GPU VA in AS1) to completion: fragment chains
 * (PANFROST_JD_REQ_FS) on JS0, the rest on JS1, all shader cores, up to
 * 2 s. 0, or an errno (EIO: the job faulted, ETIMEDOUT).
 */
int
starlte_gpu_user_submit(uint64_t jc, uint32_t requirements)
{
	if (!sgu_ready) {
		return ENODEV;
	}
	lck_mtx_lock(&sg_lock);
	sg_run_as = 1;
	sg_run_polls = 200000;
	sg_run_quiet = true;
	uint64_t t0 = mach_absolute_time();
	bool ok = sg_run((requirements & 1) ? 0 : 1, jc, sgp_shaders,
	    (requirements & 1) ? "APP fragment chain" : "APP vertex/tiler chain");
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	sg_run_quiet = false;
	sg_run_polls = 20000;
	sg_run_as = 0;
	sgu_jobs++;
	if (!ok) {
		sgu_faults++;
		sgu_recover();
	}
	lck_mtx_unlock(&sg_lock);
	if (sgu_jobs <= 8 || !ok) {
		printf("STARLTE GPU USER: job %llu chain %#llx req %#x: %s in %llu us\n", sgu_jobs, jc,
		    requirements, ok ? "done" : "FAILED", ns / 1000);
	}
	return ok ? 0 : (ns >= 1900ull * NSEC_PER_MSEC ? ETIMEDOUT : EIO);
}

/*
 * Asynchronous submits. A client that asks for it (StarbiteMali SBM_ASYNC, boot-arg
 * sbmasync=1) gets its chain started and the call back at once; the chain is finished ("retired")
 * by whoever comes next: the next submit (one chain on the GPU at a time, in order), a wait (WAIT_BO,
 * a fence, GEM_CLOSE) or the kernel's own jobs (sg_run, the present). Polled, as before: no IRQ.
 */
static unsigned sgu_busy_js;
static uint64_t sgu_busy_t0, sgu_busy_jc;
static uint32_t sgu_busy_req;
static uint64_t sgu_seq_sub, sgu_seq_done;
static uint64_t sgu_as_n, sgu_as_prev_waits, sgu_as_prev_ns, sgu_as_gpu_ns, sgu_as_gpu_max, sgu_as_t0;

/*
 * The apps' chains in a queue, run by a thread of their own (boot-arg sweetspeed&262144, for clients with async
 * submits, sweetspeed&64). Without it an "asynchronous" submit returns at once only while the GPU is free: the next
 * submit spins (delay() under sg_lock) until the chain before it is done, and so does every wait. A frame is a dozen
 * or more chains (a pass is a vertex/tiler chain and a fragment chain; a blur is many passes), so the render thread
 * spent the GPU's time spinning on a core and the next frame's CPU work never overlapped this frame's GPU work.
 * With the queue a submit only appends (FIFO, one chain on the GPU at a time, in the order given, which Panfrost
 * needs), the worker starts each chain and, after sgu_spin_us of looking (a chain takes 0.3 to 0.5 ms), sleeps in
 * short naps until it is done, and waiters sleep until their
 * chain's number is done. sg_lock is not held while a chain runs (as before), so buffers can be mapped meanwhile.
 * This is what Scion's iOS 16.5 Mali driver does (drivers/gpu/mali/hakios.mali.c: mali_queue, mali_worker).
 */
#define SGU_QUEUE       256
static struct {
	uint64_t jc, seq, queued;
	uint32_t req;
} sgu_q[SGU_QUEUE];
static unsigned sgu_q_head, sgu_q_count, sgu_q_deepest;
static uint64_t sgu_q_seq, sgu_q_done;                  /* last number given, last number done */
static bool sgu_q_on, sgu_q_started;
static int sgu_q_checked;
static int sgu_q_nap_event;
static uint32_t sgu_q_nap_us = 50;                      /* boot-arg sgu_nap_us (10 to 1000) */
static uint32_t sgu_q_spin_us = 200;                    /* boot-arg sgu_spin_us (0 to 2000): looks 10 us apart before the first nap */
/*
 * boot-arg sgu_depth (16 to 256; default 256, the whole queue): how many chains may wait. A submit beyond it sleeps until
 * the queue is half that long. A frame is 20 to 40 chains; with the whole queue free the window server ran three frames
 * ahead of the GPU on average and up to ten (queued 55 ms on average in a 57 fps scroll: frames on time, each one late).
 */
static uint32_t sgu_q_depth = SGU_QUEUE;
static LCK_MTX_DECLARE(sgu_q_lock, &sg_lck_grp);
static LCK_MTX_DECLARE(sgu_q_nap_lock, &sg_lck_grp);
extern uint32_t sweet_speed_flags(void);                /* boot-arg sweetspeed (osfmk/arm64/sweet_a64e.c) */

/* sg_lock held. 0: nothing on the GPU any more; EBUSY: still running (block false); EIO: failed. */
static int
sgu_retire_locked(bool block)
{
	if (!sgu_busy) {
		return 0;
	}
	unsigned js = sgu_busy_js;
	uint64_t deadline = 0;
	nanoseconds_to_absolutetime(2000ull * NSEC_PER_MSEC, &deadline);
	deadline += sgu_busy_t0;
	uint32_t jirq = 0, mirq = 0, girq = 0;
	for (unsigned spins = 0;; spins++) {
		jirq = sg_rd(sg_gpu, JOB_IRQ_RAWSTAT);
		mirq = sg_rd(sg_gpu, MMU_IRQ_RAWSTAT);
		girq = sg_rd(sg_gpu, GPU_IRQ_RAWSTAT);
		if ((jirq & ((1u << js) | (1u << (js + 16)))) || mirq || (girq & 0x81)) {
			break;
		}
		/* (the queue's worker only ever looks: its looks must end a hung chain too) */
		if (mach_absolute_time() >= deadline && (block || sgu_q_on)) {
			break;
		}
		if (!block) {
			return EBUSY;
		}
		delay(spins < 1500 ? 1 : 20);
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - sgu_busy_t0, &ns);
	bool ok = (jirq & (1u << js)) && !(jirq & (1u << (js + 16))) && !mirq && !(girq & 0x81);
	if (!ok) {
		printf("STARLTE GPU USER: async chain %#llx req %#x FAILED after %llu us: irq=%#x mmu=%#x gpu=%#x "
		    "js_status=%#x as_fault=%#x@%#x\n", sgu_busy_jc, sgu_busy_req, ns / 1000, jirq, mirq, girq,
		    sg_rd(sg_gpu, JS_BASE(js) + 0x24), sg_rd(sg_gpu, ASN(1) + 0x1c), sg_rd(sg_gpu, ASN(1) + 0x20));
	}
	sg_wr(sg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	sgu_busy = false;
	sgu_seq_done = sgu_seq_sub;
	sgu_jobs++;
	sgu_as_gpu_ns += ns;
	if (ns > sgu_as_gpu_max) {
		sgu_as_gpu_max = ns;
	}
	if (!ok) {
		sgu_faults++;
		sgu_recover();
	}
	return ok ? 0 : EIO;
}

int starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq);
int starlte_gpu_user_wait_seq(uint64_t seq, bool block);

/* sg_lock held, nothing on the GPU: start the chain. */
static void
sgu_start_locked(uint64_t jc, uint32_t requirements)
{
	unsigned js = (requirements & 1) ? 0 : 1;
	uint32_t slot = JS_BASE(js);
	sg_wr(sg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, slot + 0x40, (uint32_t)jc);                       /* HEAD_NEXT */
	sg_wr(sg_gpu, slot + 0x44, (uint32_t)(jc >> 32));
	sg_wr(sg_gpu, slot + 0x50, sgp_shaders);                        /* AFFINITY_NEXT */
	sg_wr(sg_gpu, slot + 0x54, 0);
	sg_wr(sg_gpu, slot + 0x58, (8u << 16) | (3u << 8) | (3u << 12) | 1u);   /* AS1, flushes, priority 8 */
	sgu_busy_js = js;
	sgu_busy_jc = jc;
	sgu_busy_req = requirements;
	sgu_busy_t0 = mach_absolute_time();
	sg_wr(sg_gpu, slot + 0x60, 1);                                  /* COMMAND_NEXT START */
	sgu_busy = true;
}

/* The queue's thread: one chain at a time, in order; naps (not a spin) while the GPU works. */
static void
sgu_q_worker(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t win_t0 = 0, win_gpu = 0, win_gpu_max = 0, win_wait = 0, win_n = 0, win_naps = 0;
	for (;;) {
		lck_mtx_lock(&sgu_q_lock);
		while (sgu_q_count == 0) {
			lck_mtx_sleep(&sgu_q_lock, LCK_SLEEP_DEFAULT, (event_t)&sgu_q_count, THREAD_UNINT);
		}
		uint64_t jc = sgu_q[sgu_q_head].jc, seq = sgu_q[sgu_q_head].seq, queued = sgu_q[sgu_q_head].queued;
		uint32_t req = sgu_q[sgu_q_head].req;
		lck_mtx_unlock(&sgu_q_lock);

		lck_mtx_lock(&sg_lock);
		if (sgu_busy) {
			(void)sgu_retire_locked(true);          /* (a chain of a client without the queue) */
		}
		uint64_t began = mach_absolute_time();
		sgu_start_locked(jc, req);
		sgu_seq_sub++;
		lck_mtx_unlock(&sg_lock);
		for (unsigned looks = 0;; looks++) {
			lck_mtx_lock(&sg_lock);
			/* (the present or a client without the queue may have retired it for us: then it is not busy) */
			int state = sgu_busy ? sgu_retire_locked(false) : 0;
			lck_mtx_unlock(&sg_lock);
			if (state != EBUSY) {
				break;
			}
			if (looks < sgu_q_spin_us / 10) {
				delay(10);
				continue;
			}
			uint64_t nap;
			clock_interval_to_deadline(sgu_q_nap_us, NSEC_PER_USEC, &nap);
			lck_mtx_lock(&sgu_q_nap_lock);
			(void)lck_mtx_sleep_deadline(&sgu_q_nap_lock, LCK_SLEEP_DEFAULT, (event_t)&sgu_q_nap_event, THREAD_UNINT, nap);
			lck_mtx_unlock(&sgu_q_nap_lock);
			win_naps++;
		}
		uint64_t ended = mach_absolute_time();

		lck_mtx_lock(&sgu_q_lock);
		sgu_q_head = (sgu_q_head + 1) % SGU_QUEUE;
		sgu_q_count--;
		sgu_q_done = seq;
		unsigned deepest = sgu_q_deepest;
		lck_mtx_unlock(&sgu_q_lock);
		wakeup((caddr_t)&sgu_q_done);
		wakeup((caddr_t)&sgu_q_head);                   /* (room in the queue) */

		/* every 10 s: how many chains, how long the GPU ran them, how long they waited for their turn */
		win_n++;
		win_gpu += ended - began;
		win_wait += began - queued;
		if (ended - began > win_gpu_max) {
			win_gpu_max = ended - began;
		}
		if (win_t0 == 0) {
			win_t0 = queued;
		}
		uint64_t window = 0;
		absolutetime_to_nanoseconds(ended - win_t0, &window);
		if (window >= 10ull * NSEC_PER_SEC && win_n >= 50) {
			uint64_t gpu = 0, gmax = 0, wait = 0;
			absolutetime_to_nanoseconds(win_gpu, &gpu);
			absolutetime_to_nanoseconds(win_gpu_max, &gmax);
			absolutetime_to_nanoseconds(win_wait, &wait);
			printf("STARLTE GPU QUEUE (%llu s): %llu chains, gpu busy %llu%% avg %llu us (max %llu), queued avg %llu us, "
			    "deepest %u, %llu naps\n", window / NSEC_PER_SEC, win_n, gpu * 100 / window, gpu / win_n / 1000,
			    gmax / 1000, wait / win_n / 1000, deepest, win_naps);
			win_t0 = ended;
			win_gpu = win_gpu_max = win_wait = win_n = win_naps = 0;
			lck_mtx_lock(&sgu_q_lock);
			sgu_q_deepest = 0;
			lck_mtx_unlock(&sgu_q_lock);
		}
	}
}

/* Is the queue on (sweetspeed&262144)? Read once; the worker starts with the first chain. */
static bool
sgu_q_enabled(void)
{
	if (!sgu_q_checked) {
		lck_mtx_lock(&sgu_q_lock);
		if (!sgu_q_checked) {
			uint32_t nap = 0;
			if (PE_parse_boot_argn("sgu_nap_us", &nap, sizeof(nap)) && nap >= 10 && nap <= 1000) {
				sgu_q_nap_us = nap;
			}
			uint32_t spin = 0;
			if (PE_parse_boot_argn("sgu_spin_us", &spin, sizeof(spin)) && spin <= 2000) {
				sgu_q_spin_us = spin;
			}
			uint32_t depth = 0;
			if (PE_parse_boot_argn("sgu_depth", &depth, sizeof(depth)) && depth >= 16 && depth <= SGU_QUEUE) {
				sgu_q_depth = depth;
			}
			sgu_q_on = (sweet_speed_flags() & 262144) != 0;
			sgu_q_checked = 1;
			printf("STARLTE GPU QUEUE: %s (sweetspeed&262144), spin %u us, nap %u us, depth %u\n", sgu_q_on ? "on" : "off",
			    sgu_q_spin_us, sgu_q_nap_us, sgu_q_depth);
		}
		lck_mtx_unlock(&sgu_q_lock);
	}
	return sgu_q_on;
}

/* A chain into the queue: its number. */
static uint64_t
sgu_q_enqueue(uint64_t jc, uint32_t requirements)
{
	lck_mtx_lock(&sgu_q_lock);
	if (!sgu_q_started) {
		thread_t thread;
		if (kernel_thread_start(sgu_q_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's app chains");
			thread_deallocate(thread);
			sgu_q_started = true;
		}
	}
	if (sgu_q_count >= sgu_q_depth) {
		/* (down to half: a submitter that woke for every chain done would ride the limit, one wake per chain) */
		while (sgu_q_count > sgu_q_depth / 2) {
			lck_mtx_sleep(&sgu_q_lock, LCK_SLEEP_DEFAULT, (event_t)&sgu_q_head, THREAD_UNINT);
		}
	}
	uint64_t seq = ++sgu_q_seq;
	unsigned at = (sgu_q_head + sgu_q_count) % SGU_QUEUE;
	sgu_q[at].jc = jc;
	sgu_q[at].req = requirements;
	sgu_q[at].seq = seq;
	sgu_q[at].queued = mach_absolute_time();
	if (++sgu_q_count > sgu_q_deepest) {
		sgu_q_deepest = sgu_q_count;
	}
	lck_mtx_unlock(&sgu_q_lock);
	wakeup((caddr_t)&sgu_q_count);
	return seq;
}

/* The number of the last chain an app has submitted (0: none, or no queue): what a frame shown now must wait for. */
uint64_t starlte_gpu_user_seq(void);
uint64_t
starlte_gpu_user_seq(void)
{
	if (!sgu_ready || !sgu_q_enabled()) {
		return 0;
	}
	lck_mtx_lock(&sgu_q_lock);
	uint64_t seq = sgu_q_seq;
	lck_mtx_unlock(&sgu_q_lock);
	return seq;
}

/* Start the chain (after the one before it); async false: and wait for it, as starlte_gpu_user_submit. */
int
starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq)
{
	if (!sgu_ready) {
		return ENODEV;
	}
	if (async && sgu_q_enabled()) {
		*seq = sgu_q_enqueue(jc, requirements);
		return 0;
	}
	lck_mtx_lock(&sg_lock);
	uint64_t t0 = mach_absolute_time(), ns = 0;
	if (sgu_busy) {
		(void)sgu_retire_locked(true);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		sgu_as_prev_waits++;
		sgu_as_prev_ns += ns;
	}
	sgu_start_locked(jc, requirements);
	*seq = ++sgu_seq_sub;
	int ret = 0;
	if (!async) {
		ret = sgu_retire_locked(true);
	}
	sgu_as_n++;
	if (sgu_as_t0 == 0) {
		sgu_as_t0 = t0;
	}
	uint64_t window = 0;
	absolutetime_to_nanoseconds(t0 - sgu_as_t0, &window);
	if (window >= 10ull * NSEC_PER_SEC && sgu_as_n >= 50) {
		printf("STARLTE GPU ASYNC (%llu s): %llu chains, gpu avg %llu us (max %llu), next submit waited %llu x avg %llu us\n",
		    window / NSEC_PER_SEC, sgu_as_n, sgu_as_gpu_ns / sgu_as_n / 1000, sgu_as_gpu_max / 1000, sgu_as_prev_waits,
		    sgu_as_prev_waits ? sgu_as_prev_ns / sgu_as_prev_waits / 1000 : 0);
		sgu_as_n = sgu_as_prev_waits = sgu_as_prev_ns = sgu_as_gpu_ns = sgu_as_gpu_max = 0;
		sgu_as_t0 = t0;
	}
	lck_mtx_unlock(&sg_lock);
	return ret;
}

/* Wait until chain `seq` (0: every chain submitted so far) is done. 0, or ETIMEDOUT (block false and still running). */
int
starlte_gpu_user_wait_seq(uint64_t seq, bool block)
{
	if (!sgu_ready) {
		return 0;
	}
	if (sgu_q_enabled()) {
		/* asleep until the worker says so; 3 s at most (a hung chain is ended by the worker after 2 s) */
		uint64_t deadline;
		clock_interval_to_deadline(3000, NSEC_PER_MSEC, &deadline);
		lck_mtx_lock(&sgu_q_lock);
		if (seq == 0) {
			seq = sgu_q_seq;
		}
		while (block && sgu_q_done < seq && mach_absolute_time() < deadline) {
			(void)lck_mtx_sleep_deadline(&sgu_q_lock, LCK_SLEEP_DEFAULT, (event_t)&sgu_q_done, THREAD_UNINT, deadline);
		}
		int done = sgu_q_done >= seq ? 0 : ETIMEDOUT;
		lck_mtx_unlock(&sgu_q_lock);
		return done;
	}
	lck_mtx_lock(&sg_lock);
	if (seq == 0) {
		seq = sgu_seq_sub;
	}
	if (seq > sgu_seq_done) {
		(void)sgu_retire_locked(block);
	}
	int ret = seq <= sgu_seq_done ? 0 : ETIMEDOUT;
	lck_mtx_unlock(&sg_lock);
	return ret;
}

static void
sg_jobs(vm_offset_t pmu)
{
	if ((sg_rd(pmu, SG_PMU_G3D_STAT) & 0xf) != 0xf || !sg_gpu ||
	    (sg_rd(sg_gpu, GPU_ID) >> 16) != 0x6221) {
		printf("STARLTE GPU: stage 2: domain off or not a G72; no jobs\n");
		return;
	}
	vm_offset_t cmu = ml_io_map(SG_CMU_PA, 0x1000);
	if (!cmu || !(sg_rd(cmu, 0x140) & (1u << 29))) {
		printf("STARLTE GPU: stage 2: PLL_G3D not locked; no jobs\n");
		return;
	}
	uint8_t was = 0, now = 0;
	if (!starlte_pmic_read(1, SG_BUCK6, &was)) {
		printf("STARLTE GPU: stage 2: BUCK6 unreadable; no jobs\n");
		return;
	}
	/* 0x20 = 500 mV .. 0x70 = 1000 mV; anything else is not vdd_g3d as known. */
	if (was < 0x20 || was > 0x70) {
		printf("STARLTE GPU: stage 2: BUCK6 vsel %#x unexpected; no jobs\n", was);
		return;
	}
	bool volt = starlte_pmic_write(1, SG_BUCK6, SG_BUCK6_950MV);
	delay(1000);                                    /* ramp: well under 1 ms */
	(void)starlte_pmic_read(1, SG_BUCK6, &now);
	printf("STARLTE GPU: vdd_g3d BUCK6 %#x (%u uV) -> %#x (%u uV) %s\n", was,
	    300000u + was * 6250u, now, 300000u + now * 6250u,
	    volt && now == SG_BUCK6_950MV ? "ok" : "FAILED");
	if (!volt || now != SG_BUCK6_950MV) {
		goto restore;
	}
	sg_mem = ml_io_map((vm_offset_t)SG_MEM_PA, SG_MEM_BYTES);
	if (!sg_mem) {
		printf("STARLTE GPU: GPU memory mapping failed\n");
		goto restore;
	}
	uint32_t shaders = sg_rd(sg_gpu, SHADER_PRESENT_LO);
	if (sg_gpu_up(shaders)) {
		bool a = sg_write_value(0x000, 0x100, 3, 0, 0, shaders, "WRITE_VALUE zero");
		bool b = sg_write_value(0x200, 0x300, 7, 0x4554494252415453ull,
		    0x4554494252415453ull, shaders, "WRITE_VALUE STARBITE");
		printf("STARLTE GPU: stage 2 %s\n", a && b ? "PASSED" : "did not pass");
		if (a && b) {
			/* Stages 3 and 4 draw on the panel and read it back: hold
			 * SweetFramebuffer's CPU copies meanwhile (with 8 cores iOS
			 * is already presenting by now; one landed in the triangle
			 * check), and let a copy in flight finish first. */
			uint64_t hold;
			clock_interval_to_deadline(3, NSEC_PER_SEC, &hold);
			sweet_fb_hold_until = hold;
			sg_msleep(100);
			sg_stage3(shaders);
			/* Fresh reset, tables and AS0 for the draw, then again for
			 * the present (stage 5), which keeps the GPU powered. */
			bool drawn = sg_gpu_up(shaders) && sg_stage4(shaders);
			sweet_fb_hold_until = 0;
			if (drawn && sg_gpu_up(shaders)) {
				sg_clock_up();
				sgp_shaders = shaders;
				__atomic_thread_fence(__ATOMIC_RELEASE);
				sgp_ready = true;
				sgu_ready = sgu_setup();
				printf("STARLTE GPU: stage 6: GPU for apps %s (AS1, tables at %#llx)\n",
				    sgu_ready ? "ready" : "NOT ready", SGU_PT_PA);
				if (sgu_ready) {
					starbite_mali_publish();
				}
				printf("STARLTE GPU: stage 5: GPU present ready (%u x %u canvas), cores on, "
				    "vdd_g3d stays at 950 mV\n", SG_PRES_CANVAS_W, SG_PRES_CANVAS_H);
				return;
			}
		}
	}
	sg_cores_off();
restore:
	if (starlte_pmic_write(1, SG_BUCK6, was) && starlte_pmic_read(1, SG_BUCK6, &now)) {
		printf("STARLTE GPU: vdd_g3d BUCK6 back to %#x (%u uV)\n", now, 300000u + now * 6250u);
	}
}

/*
 * The GPU's clock: S-Boot leaves PLL_G3D at 260 MHz, the lowest level of the DT's gpu_dvfs_table
 * (572000 546000 455000 338000 299000 260000 kHz). Apps' frames wait for their jobs synchronously
 * (~340 us each, ~50 a frame while scrolling: half of every second, boot67), so the clock goes up
 * through ACPM DVFS (domain g3d = 4) before the present and apps start, and vdd_g3d is put back to
 * 950 mV in case ACPM's own table set it lower (the top of what this GPU is run at). Not locked
 * afterwards: back to 260 MHz. Boot-arg stargpu=0: S-Boot's clock; stargpu=KHZ: a level above.
 */
static uint32_t
sg_clock_khz(void)
{
	static vm_offset_t cmu;
	if (!cmu) {
		cmu = ml_io_map(SG_CMU_PA, 0x2000);
	}
	uint32_t con0 = cmu ? sg_rd(cmu, 0x140) : 0;
	uint64_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	if (!(con0 & (1u << 29)) || !p) {
		return 0;                       /* not locked */
	}
	return (con0 & 0x10) ? (uint32_t)(26000ull * m / (p << s)) : 26000;
}

static void
sg_clock_up(void)
{
	static const uint32_t levels[] = { 572000, 546000, 455000, 338000, 299000, 260000 };
	uint32_t want = 572000;
	PE_parse_boot_argn("stargpu", &want, sizeof(want));
	bool listed = false;
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		listed |= levels[i] == want;
	}
	if (want == 0 || !listed) {
		printf("STARLTE GPU: clock left at %u kHz (stargpu=%u)\n", sg_clock_khz(), want);
		return;
	}
	uint32_t before = sg_clock_khz();
	bool sent = starlte_acpm_dvfs(4, want);
	delay(1000);
	uint8_t volt = 0;
	bool v = starlte_pmic_write(1, SG_BUCK6, SG_BUCK6_950MV) && starlte_pmic_read(1, SG_BUCK6, &volt);
	uint32_t after = sg_clock_khz();
	printf("STARLTE GPU: clock %u -> %u kHz (asked %u, %s), vdd_g3d %s %u uV\n", before, after, want,
	    sent ? "sent" : "no reply", v ? "at" : "NOT SET,", 300000u + volt * 6250u);
	if (sent && after == 0) {
		(void)starlte_acpm_dvfs(4, 260000);
		printf("STARLTE GPU: PLL not locked at %u kHz: back to 260000 (%u kHz)\n", want, sg_clock_khz());
	}
}

static void
sg_thread(void *arg, __unused wait_result_t wr)
{
	int mode = (int)(uintptr_t)arg;
	vm_offset_t pmu = ml_io_map(SG_PMU_G3D_PA, 0x1000);
	if (!pmu) {
		printf("STARLTE GPU: PMU mapping failed\n");
		return;
	}
	/* Once early, once after the UI is up (in case something changed it). */
	sg_msleep(5000);
	sg_probe(pmu, "5s");
	if (mode >= 2) {
		/* After the touch thread's ACPM traffic (2 s) and before the UI. */
		sg_msleep(3000);
		sg_jobs(pmu);
	}
	sg_msleep(52000);
	sg_probe(pmu, "60s");
}

void
starlte_gpu_probe(void)
{
	int enabled = 2;
	thread_t thread;

	PE_parse_boot_argn("stargpu", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("STARLTE GPU: probe disabled\n");
		return;
	}
	if (kernel_thread_start(sg_thread, (void *)(uintptr_t)enabled, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
