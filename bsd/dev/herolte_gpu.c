/*
 * HEROLTE (Galaxy S7 SM-G930F, Exynos 8890) Mali-T880 MP12, stage 1:
 * read-only probe, as A5XELTE's 0126 did for its Mali-T720. From the stock
 * BOOT DTB: mali@14AC0000 (0x5000, SPIs JOB 0xe1, MMU 0xe2, GPU 0xe0),
 * CMU_G3D 0x14AA0000, PMU G3D domain CONFIGURATION/STATUS 0x105C4060/4064
 * (exynos-snapshot reads CMU_G3D only when 0x105C4064 and 0x105C6104 say
 * powered), vdd_g3d = S2MPS16 BUCK6, regulator-always-on.
 *
 * The probe writes nothing. CMU_G3D and the GPU are read only when the
 * domain status low nibble is 0xf: a powered-off block can hang the bus.
 * Boot-arg herogpu=0 skips everything, 1 (default) probes, 2 also dumps the
 * PMIC, 3-5 also run GPU jobs on one core (see the stage comment below).
 */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void herolte_gpu_probe(void);
/* GPU present, for iokit/Kernel/arm/SweetFramebuffer.cpp. */
int herolte_gpu_present_slot(uint32_t id, uint64_t first_pa);
int herolte_gpu_present_map(uint32_t id, const uint64_t *pages, unsigned n);
bool herolte_gpu_present_draw(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h);

#define HG_PMU_PA       0x105c0000u
#define HG_PMU_G3D_CONF 0x4060u
#define HG_PMU_G3D_STAT 0x4064u
#define HG_PMU_G3D_AUX  0x6104u         /* second exynos-snapshot condition */
#define HG_CMU_PA       0x14aa0000u
#define HG_GPU_PA       0x14ac0000u

/* Mali Midgard register offsets (Linux mali_kbase / Panfrost). */
#define GPU_ID                  0x000
#define L2_FEATURES             0x004
#define TILER_FEATURES          0x00c
#define MEM_FEATURES            0x010
#define MMU_FEATURES            0x014
#define AS_PRESENT              0x018
#define JS_PRESENT              0x01c
#define GPU_IRQ_RAWSTAT         0x020
#define GPU_STATUS              0x034
#define SHADER_PRESENT_LO       0x100
#define TILER_PRESENT_LO        0x110
#define L2_PRESENT_LO           0x120
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160

static int hg_sleep;

static uint32_t
hg_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
hg_msleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&hg_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
hg_powered(vm_offset_t pmu)
{
	return (hg_rd(pmu, HG_PMU_G3D_STAT) & 0xf) == 0xf;
}

static void
hg_probe(vm_offset_t pmu, const char *when)
{
	uint32_t conf = hg_rd(pmu, HG_PMU_G3D_CONF);
	uint32_t stat = hg_rd(pmu, HG_PMU_G3D_STAT);
	uint32_t aux = hg_rd(pmu, HG_PMU_G3D_AUX);
	printf("HEROLTE GPU: %s: PMU G3D conf=%#x status=%#x aux=%#x\n", when, conf, stat, aux);
	if ((stat & 0xf) != 0xf) {
		printf("HEROLTE GPU: %s: G3D domain off; CMU and Mali not read\n", when);
		return;
	}
	static vm_offset_t cmu, gpu;
	if (!cmu) {
		cmu = ml_io_map(HG_CMU_PA, 0x2000);
	}
	if (!gpu) {
		gpu = ml_io_map(HG_GPU_PA, 0x5000);
	}
	if (!cmu || !gpu) {
		printf("HEROLTE GPU: MMIO mapping failed\n");
		return;
	}
	/* The exynos-snapshot CMU_G3D list: muxes 0x2xx, dividers 0x4xx,
	 * 0x6xx, gates 0x8xx/0x9xx/0xa00. */
	static const uint16_t regs[] = {
		0x200, 0x204, 0x20c, 0x210, 0x214, 0x400, 0x404, 0x408, 0x40c,
		0x600, 0x604, 0x60c, 0x610, 0x614, 0x800, 0x804, 0x900, 0xa00,
	};
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i += 6) {
		printf("HEROLTE GPU: CMU_G3D");
		for (unsigned j = i; j < i + 6 && j < sizeof(regs) / sizeof(regs[0]); j++) {
			printf(" %03x=%#x", regs[j], hg_rd(cmu, regs[j]));
		}
		printf("\n");
	}
	if (!hg_powered(pmu)) {
		printf("HEROLTE GPU: %s: domain went off; Mali not read\n", when);
		return;
	}
	uint32_t id = hg_rd(gpu, GPU_ID);
	printf("HEROLTE GPU: %s: GPU_ID=%#x product %#x (0x880 = Mali-T880) rev r%up%u\n",
	    when, id, id >> 16, (id >> 12) & 0xf, (id >> 4) & 0xff);
	printf("HEROLTE GPU: L2_FEATURES=%#x TILER=%#x MEM=%#x MMU=%#x AS=%#x JS=%#x\n",
	    hg_rd(gpu, L2_FEATURES), hg_rd(gpu, TILER_FEATURES), hg_rd(gpu, MEM_FEATURES),
	    hg_rd(gpu, MMU_FEATURES), hg_rd(gpu, AS_PRESENT), hg_rd(gpu, JS_PRESENT));
	printf("HEROLTE GPU: present shader=%#x tiler=%#x l2=%#x; ready shader=%#x tiler=%#x l2=%#x\n",
	    hg_rd(gpu, SHADER_PRESENT_LO), hg_rd(gpu, TILER_PRESENT_LO), hg_rd(gpu, L2_PRESENT_LO),
	    hg_rd(gpu, SHADER_READY_LO), hg_rd(gpu, TILER_READY_LO), hg_rd(gpu, L2_READY_LO));
	printf("HEROLTE GPU: GPU_STATUS=%#x IRQ_RAWSTAT=%#x COHERENCY_FEATURES=%#x ENABLE=%#x; no GPU jobs\n",
	    hg_rd(gpu, GPU_STATUS), hg_rd(gpu, GPU_IRQ_RAWSTAT), hg_rd(gpu, 0x300), hg_rd(gpu, 0x304));
}

/*
 * Stage 2 (herogpu>=2): S2MPS16 registers, read through the APM like the
 * touch supplies, to find vdd_g3d (BUCK6). Nothing is written.
 */
extern bool herolte_apm_pmic_read(uint8_t reg, uint8_t *value);

static void
hg_pmic_dump(void)
{
	for (unsigned base = 0; base < 0x80; base += 16) {
		uint8_t v[16];
		for (unsigned i = 0; i < 16; i++) {
			if (!herolte_apm_pmic_read((uint8_t)(base + i), &v[i])) {
				printf("HEROLTE GPU: PMIC read %#x failed (APM not started?)\n", base + i);
				return;
			}
		}
		printf("HEROLTE GPU: S2MPS16 %02x: %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x\n", base, v[0], v[1], v[2], v[3],
		    v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
	}
}

/*
 * Stages 3-5: GPU jobs in a private 64 KiB block of the first DRAM bank,
 * outside XNU's RAM, mapped uncached on the CPU side (no cache
 * maintenance): Mali-LPAE page tables at +0x0000..+0x3fff, job and data
 * pages at +0x4000.. mapped at GPU VA 0x02000000. Only L2, shader core 0
 * and the tiler are powered (voltage and clock as S-Boot left them), all
 * polls are bounded, and the cores are powered off at the end.
 *
 *   herogpu=3  WRITE_VALUE on JS1 (A5XELTEGPU.cpp; done on the phone, 0226)
 *   herogpu=4  + fragment clear of a private 16x16 RGBA8 target on JS0
 *   herogpu=5  + fragment clear of a 128x128 green square in the middle of
 *              the panel scanout buffer (0xe2a00000, stride 5760)
 *   herogpu=6  + the whole 1440x2560 panel green; the CPU copies of iOS
 *              frames are held 8 s around each panel job so it stays visible
 *   herogpu=7  + the GPU copies the last iOS frame to the panel (tile
 *              preload, 256 rows lower), then all 12 cores clear the panel
 *   herogpu=8  WRITE_VALUE, then one triangle drawn with a Midgard shader
 *   herogpu=9  WRITE_VALUE, then a 180x320 texture scaled 8x to the panel,
 *              on one shader core and again on all 12
 *
 * The fragment job is the A5's (0131-0133): Mesa Panfrost Midgard v4
 * single-target framebuffer descriptor and the empty polygon list with
 * hierarchical tiling disabled, in its own page. Mesa uses the multi-target
 * descriptor from v5 (T760..T880) on; whether the T880 still takes the
 * single-target one is what herogpu=4 finds out.
 */
/* 128 KiB ending below the SMP trampoline (0x8fffe000): page tables at
 * +0x0000..+0x3fff, 12 data pages at +0x4000, panel L3 tables at +0x10000. */
#define HG_TEST_PA      0x8ffc0000u
#define HG_TEST_BYTES   0x20000u
#define HG_TEST_VA      0x02000000ull
#define HG_DATA_PAGES   12u
#define HG_PANEL_L3_OFF 0x10000u
/* The whole panel at GPU VA 0x04000000 (2 MiB aligned, like its PA). */
#define HG_PANEL_VA     0x04000000ull
#define HG_PANEL_BYTES  (1440u * 2560u * 4u)
#define HG_PANEL_W      1440u
#define HG_PANEL_H      2560u
/* An iOS surface (GPU copy test) at GPU VA 0x08000000, L3 tables at +0x18000. */
#define HG_SRC_L3_OFF   0x18000u
#define HG_SRC_VA       0x08000000ull
#define HG_SRC_MAX_PAGES (8u * 512u)
#define HG_DATA_OFF     0x4000u         /* GPU VA HG_TEST_VA: 12 data pages */
#define HG_OUT_OFF      0x100u
#define HG_FBD_OFF      0x100u
#define HG_POLY_OFF     0x1000u
#define HG_PIX_OFF      0x2000u
#define HG_RED          0xff0000ffu
#define HG_GREEN        0xff00ff00u

/* Panel scanout (mkadt_herolte.py FB_BASE) and the square to clear. */
#define HG_FB_PA        0xe2a00000ull
#define HG_FB_STRIDE    5760u
#define HG_SQ_X         656u            /* (1440 - 128) / 2, 16-pixel aligned */
#define HG_SQ_Y         1216u           /* (2560 - 128) / 2 */
#define HG_SQ_SIZE      128u

#define SHADER_PWRON_LO 0x180
#define TILER_PWRON_LO  0x190
#define L2_PWRON_LO     0x1a0
#define SHADER_PWROFF_LO 0x1c0
#define TILER_PWROFF_LO 0x1d0
#define L2_PWROFF_LO    0x1e0
#define GPU_FAULTSTATUS 0x03c
#define GPU_FAULTADDR_LO 0x040
#define JOB_IRQ_RAWSTAT 0x1000
#define JOB_IRQ_CLEAR   0x1004
#define JS_BASE(n)      (0x1800u + (n) * 0x80u)
#define MMU_IRQ_RAWSTAT 0x2000
#define MMU_IRQ_CLEAR   0x2004
#define AS0             0x2400

static vm_offset_t hg_gpu, hg_mem;

static void
hg_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static bool
hg_power(uint32_t on, uint32_t ready, uint32_t mask, const char *name)
{
	hg_wr(hg_gpu, on, mask);
	uint32_t r = 0;
	for (unsigned us = 0; us < 20000; us += 10) {
		r = hg_rd(hg_gpu, ready);
		if ((r & mask) == mask) {
			break;
		}
		delay(10);
	}
	printf("HEROLTE GPU: power %s mask=%#x ready=%#x\n", name, mask, r);
	return (r & mask) == mask;
}

static bool
hg_cores_on(void)
{
	return hg_power(L2_PWRON_LO, L2_READY_LO, hg_rd(hg_gpu, L2_PRESENT_LO), "L2") &&
	       hg_power(SHADER_PWRON_LO, SHADER_READY_LO, 1, "shader core 0") &&
	       hg_power(TILER_PWRON_LO, TILER_READY_LO, hg_rd(hg_gpu, TILER_PRESENT_LO), "tiler");
}

static void
hg_cores_off(void)
{
	hg_wr(hg_gpu, TILER_PWROFF_LO, hg_rd(hg_gpu, TILER_READY_LO));
	hg_wr(hg_gpu, SHADER_PWROFF_LO, hg_rd(hg_gpu, SHADER_READY_LO));
	hg_wr(hg_gpu, L2_PWROFF_LO, hg_rd(hg_gpu, L2_READY_LO));
	printf("HEROLTE GPU: cores powered off\n");
}

static void
hg_zero(vm_offset_t va, size_t bytes)
{
	volatile uint64_t *q = (volatile uint64_t *)va;
	for (size_t i = 0; i < bytes / 8; i++) {
		q[i] = 0;
	}
}

/*
 * Fresh page tables: the 12 data pages at HG_TEST_VA, if asked the whole
 * panel at HG_PANEL_VA (8 level-3 tables of 2 MiB each), and src_n pages
 * of an iOS surface at HG_SRC_VA.
 */
static void
hg_tables(bool panel, const uint64_t *src, unsigned src_n)
{
	hg_zero(hg_mem, HG_TEST_BYTES);
	volatile uint64_t *l0 = (volatile uint64_t *)hg_mem;
	volatile uint64_t *l1 = (volatile uint64_t *)(hg_mem + 0x1000);
	volatile uint64_t *l2t = (volatile uint64_t *)(hg_mem + 0x2000);
	volatile uint64_t *l3 = (volatile uint64_t *)(hg_mem + 0x3000);
	l0[0] = (HG_TEST_PA + 0x1000) | 3;
	l1[0] = (HG_TEST_PA + 0x2000) | 3;
	l2t[(HG_TEST_VA >> 21) & 0x1ff] = (HG_TEST_PA + 0x3000) | 3;
	for (unsigned i = 0; i < HG_DATA_PAGES; i++) {
		/* Mali-LPAE leaf: read/write, outer shareable, noncached. */
		l3[i] = (HG_TEST_PA + HG_DATA_OFF + i * 0x1000) | 0x2c1;
	}
	if (panel) {
		unsigned pages = (HG_PANEL_BYTES + 0xfff) / 0x1000;
		for (unsigned t = 0; t * 512 < pages; t++) {
			uint32_t table = HG_PANEL_L3_OFF + t * 0x1000;
			volatile uint64_t *pl3 = (volatile uint64_t *)(hg_mem + table);
			l2t[((HG_PANEL_VA >> 21) & 0x1ff) + t] = (HG_TEST_PA + table) | 3;
			for (unsigned i = 0; i < 512 && t * 512 + i < pages; i++) {
				pl3[i] = (HG_FB_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x2c1;
			}
		}
	}
	for (unsigned t = 0; src && t * 512 < src_n && t < 8; t++) {
		uint32_t table = HG_SRC_L3_OFF + t * 0x1000;
		volatile uint64_t *sl3 = (volatile uint64_t *)(hg_mem + table);
		l2t[((HG_SRC_VA >> 21) & 0x1ff) + t] = (HG_TEST_PA + table) | 3;
		for (unsigned i = 0; i < 512 && t * 512 + i < src_n; i++) {
			sl3[i] = src[t * 512 + i] | 0x2c1;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Shader cores a job may use (JS_AFFINITY). */
static uint32_t hg_affinity = 1;
/* GPU VA of the job chain hg_run() starts, and of its header (for the log). */
static uint64_t hg_job_va = HG_TEST_VA;
static vm_offset_t hg_job_cpu;
/* Log only failed jobs (the per-frame GPU present). */
static bool hg_run_quiet;

/* Program AS0, start the job at HG_TEST_VA on slot js, wait (bounded). */
static bool
hg_run(unsigned js, const char *what)
{
	uint32_t slot = JS_BASE(js);
	if (hg_rd(hg_gpu, slot + 0x60) || hg_rd(hg_gpu, slot + 0x24) & 0xff) {
		printf("HEROLTE GPU: %s: slot %u busy\n", what, js);
	}
	hg_wr(hg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	hg_wr(hg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	uint64_t transtab = HG_TEST_PA | 0x7;
	hg_wr(hg_gpu, AS0 + 0x00, (uint32_t)transtab);
	hg_wr(hg_gpu, AS0 + 0x04, (uint32_t)(transtab >> 32));
	hg_wr(hg_gpu, AS0 + 0x08, 0x888d88);
	hg_wr(hg_gpu, AS0 + 0x0c, 0);
	hg_wr(hg_gpu, AS0 + 0x18, 1);           /* AS_COMMAND_UPDATE */
	for (unsigned i = 0; i < 10000 && (hg_rd(hg_gpu, AS0 + 0x28) & 1); i++) {
		delay(10);
	}
	if (hg_rd(hg_gpu, AS0 + 0x28) & 1) {
		printf("HEROLTE GPU: %s: MMU update timeout\n", what);
		return false;
	}
	hg_wr(hg_gpu, slot + 0x40, (uint32_t)hg_job_va);        /* HEAD_NEXT */
	hg_wr(hg_gpu, slot + 0x44, (uint32_t)(hg_job_va >> 32));
	hg_wr(hg_gpu, slot + 0x50, hg_affinity);                /* AFFINITY */
	hg_wr(hg_gpu, slot + 0x54, 0);
	hg_wr(hg_gpu, slot + 0x58, (8 << 16) | (3 << 8) | (3 << 12));
	hg_wr(hg_gpu, slot + 0x60, 1);                          /* START */

	uint32_t jirq = 0, mirq = 0, girq = 0;
	for (unsigned i = 0; i < 20000; i++) {
		jirq = hg_rd(hg_gpu, JOB_IRQ_RAWSTAT);
		mirq = hg_rd(hg_gpu, MMU_IRQ_RAWSTAT);
		girq = hg_rd(hg_gpu, GPU_IRQ_RAWSTAT);
		if ((jirq & ((1u << js) | (1u << (js + 16)))) || mirq || (girq & 0x81)) {
			break;
		}
		delay(10);
	}
	volatile uint32_t *hdr = (volatile uint32_t *)(hg_job_cpu ? hg_job_cpu : hg_mem + HG_DATA_OFF);
	bool ok = (jirq & (1u << js)) && !(jirq & (1u << (js + 16))) && !mirq && !(girq & 0x81);
	if (!ok || !hg_run_quiet) {
		printf("HEROLTE GPU: %s: irq=%#x mmu=%#x gpu=%#x js_status=%#x as_fault=%#x@%#x "
		    "exception=%#x fault=%#x:%#x gpu_fault=%#x@%#x\n", what, jirq, mirq, girq,
		    hg_rd(hg_gpu, slot + 0x24), hg_rd(hg_gpu, AS0 + 0x1c), hg_rd(hg_gpu, AS0 + 0x20),
		    hdr[0], hdr[3], hdr[2],
		    hg_rd(hg_gpu, GPU_FAULTSTATUS), hg_rd(hg_gpu, GPU_FAULTADDR_LO));
	}
	hg_wr(hg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	hg_wr(hg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	return ok;
}

static bool
hg_job_write_value(void)
{
	hg_tables(false, NULL, 0);
	volatile uint8_t *job = (volatile uint8_t *)(hg_mem + HG_DATA_OFF);
	job[16] = 1 | (2 << 1);                         /* 64-bit, WRITE_VALUE */
	*(volatile uint16_t *)(job + 18) = 1;           /* index */
	*(volatile uint64_t *)(job + 32) = HG_TEST_VA + HG_OUT_OFF;
	*(volatile uint64_t *)(job + 40) = 3;           /* zero 64 bits */
	volatile uint64_t *output = (volatile uint64_t *)(hg_mem + HG_DATA_OFF + HG_OUT_OFF);
	*output = 0xa5a5a5a55a5a5a5aull;
	__asm__ volatile ("dsb sy" ::: "memory");
	bool ok = hg_run(1, "WRITE_VALUE") && *output == 0;
	printf("HEROLTE GPU: WRITE_VALUE output=%#llx -> %s\n", *output,
	    ok ? "THE MALI-T880 RAN A JOB" : "failed or timed out");
	return ok;
}

/*
 * Fragment clear of a size x size RGBA8 target. 0227 sent the A5's
 * single-target descriptor: the T880 answered DATA_INVALID_FAULT (0x58) with
 * the fault pointer on it. Mesa's Midgard v5 layout (genxml/v5.xml), used
 * from the T760 on, is the multi-target one: Framebuffer = Local Storage
 * (+0, 32 B), Parameters (+32, 24 B), Tiler Context (+56, 40 B), Tiler
 * Weights (+96, 32 B), then one 64-byte Render Target (+128). The fragment
 * payload carries the bounds in 16x16 tiles and the framebuffer pointer
 * tagged with Type = 1 (MFBD), render target count - 1 = 0. RT values are
 * the A5 descriptor's (0xb84da201): RGBA8, swizzle 0x688, linear, dither,
 * clean pixel write.
 */
static bool hg_job_fragment_rt(uint64_t pixels_va, uint32_t w, uint32_t h, uint32_t stride,
    uint32_t color, uint64_t preload_va, uint32_t preload_stride, const char *what);

static bool
hg_job_fragment(uint64_t pixels_va, uint32_t w, uint32_t h, uint32_t stride, uint32_t color,
    bool panel, const char *what)
{
	hg_tables(panel, NULL, 0);
	return hg_job_fragment_rt(pixels_va, w, h, stride, color, 0, 0, what);
}

/*
 * Render target writeback at pixels_va. preload_va == 0: clear to color.
 * Otherwise each tile is first loaded from preload_va (same RGBA8 linear
 * format, preload_stride per row) and written back unchanged: a copy done
 * by the GPU's tile buffer, no shader involved (Midgard v5 RT words 2 and
 * 12-15; the preload buffer shares words 12-15 with the clear color).
 * The page tables must already be set up.
 */
static bool
hg_job_fragment_rt(uint64_t pixels_va, uint32_t w, uint32_t h, uint32_t stride, uint32_t color,
    uint64_t preload_va, uint32_t preload_stride, const char *what)
{
	uint64_t fb_va = HG_TEST_VA + HG_FBD_OFF;
	uint64_t poly_va = HG_TEST_VA + HG_POLY_OFF;
	uint32_t tiles_x = (w + 15) / 16, tiles_y = (h + 15) / 16;
	volatile uint32_t *hdr = (volatile uint32_t *)(hg_mem + HG_DATA_OFF);
	hdr[4] = 0x00010013;            /* 64-bit | FRAGMENT(9) << 1 | index 1 << 16 */
	hdr[8] = 0;                     /* bound min tile x, y */
	hdr[9] = ((tiles_y - 1) << 16) | (tiles_x - 1); /* bound max tile x, y */
	*(volatile uint64_t *)(hdr + 10) = fb_va | 1;   /* MFBD, 1 render target */

	volatile uint32_t *fb = (volatile uint32_t *)(hg_mem + HG_DATA_OFF + HG_FBD_OFF);
	fb[1] = 31;                     /* Local Storage: no workgroup memory */
	/* Parameters at +32 (word 8). */
	fb[8] = ((h - 1) << 16) | (w - 1);              /* width - 1, height - 1 */
	fb[9] = 0;                                      /* bound min x, y (pixels) */
	fb[10] = ((h - 1) << 16) | (w - 1);             /* bound max x, y */
	fb[11] = (2u << 6) |            /* tie-break MINUS_180_IN_0_OUT, as the A5 */
	    (8u << 9) |                 /* effective tile size log2(16 * 16) */
	    (1u << 24);                 /* color buffer allocation 1 KiB (shr 10) */
	fb[12] = 1u << 16;              /* Z internal format D24 (Mesa, no ZS) */
	fb[13] = 0;                     /* Z clear 0.0 */
	/* Tiler Context at +56 (word 14). Mesa pan_emit_midgard_tiler(), tiler
	 * disabled on a GPU with hierarchical tiling (the T880; 0228's A5 values
	 * 4095 + 4 are for the T720, which has none): mask DISABLED, 512-byte
	 * header only, heap start = end = the list. */
	fb[14] = 512;                   /* MALI_MIDGARD_TILER_MINIMUM_HEADER_SIZE */
	fb[15] = 4096;                  /* MALI_MIDGARD_TILER_DISABLED */
	*(volatile uint64_t *)(fb + 16) = poly_va;              /* polygon list */
	*(volatile uint64_t *)(fb + 18) = poly_va + 512;        /* body */
	*(volatile uint64_t *)(fb + 20) = poly_va;              /* heap start */
	*(volatile uint64_t *)(fb + 22) = poly_va;              /* heap end */
	/* Render Target at +128 (word 32). */
	volatile uint32_t *rt = fb + 32;
	rt[0] = 1u << 26;               /* internal format R8G8B8A8 */
	rt[1] = 1u |                    /* write enable */
	    (19u << 3) |                /* writeback format R8G8B8A8 */
	    (2u << 10) |                /* block format linear */
	    (1u << 15) |                /* dithering, as the A5 */
	    (0x688u << 16) |            /* identity swizzle */
	    (1u << 31);                 /* clean pixel write */
	*(volatile uint64_t *)(rt + 8) = pixels_va;     /* RGB base */
	rt[10] = stride;                                /* row stride */
	rt[11] = 0;                                     /* surface stride */
	if (preload_va) {
		rt[2] = 1u |            /* preload enable */
		    (19u << 3) |        /* preload format R8G8B8A8 */
		    (2u << 10);         /* preload block format linear */
		*(volatile uint64_t *)(rt + 12) = preload_va;   /* preload base */
		rt[14] = preload_stride;
		rt[15] = 0;
	} else {
		for (unsigned i = 0; i < 4; i++) {
			rt[12 + i] = color;     /* clear color */
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return hg_run(0, what);
}

/* SweetFramebuffer.cpp skips its CPU copies until this mach time. */
extern uint64_t sweet_fb_hold_until;
extern bool sweet_fb_last_surface(uint64_t *pages, unsigned max_pages, unsigned *npages,
    uint32_t *offset, uint32_t *stride, uint32_t *width, uint32_t *height, uintptr_t *kva);

static uint64_t
hg_us_since(uint64_t start)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / 1000;
}

/*
 * herogpu=7, step 1: the GPU copies the last full-screen iOS frame to the
 * panel, 256 rows lower so it is visibly not the CPU's copy, held 8 s.
 * iOS draws through the CPU caches and the GPU reads DRAM, so the surface
 * is cleaned to the point of coherency first (timed: if the GPU is not
 * IO-coherent, this clean is the price of every GPU copy).
 */
static void
hg_gpu_copy_test(uint64_t hold)
{
	static uint64_t pages[HG_SRC_MAX_PAGES];
	unsigned n = 0;
	uint32_t soff = 0, sstride = 0, sw = 0, sh = 0;
	uintptr_t kva = 0;
	if (!sweet_fb_last_surface(pages, HG_SRC_MAX_PAGES, &n, &soff, &sstride, &sw, &sh, &kva)) {
		printf("HEROLTE GPU: GPU COPY: no full-screen iOS surface yet\n");
		return;
	}
	printf("HEROLTE GPU: GPU COPY: surface %ux%u stride %u offset %#x, %u pages, first pa %#llx\n",
	    sw, sh, sstride, soff, n, pages[0]);
	uint64_t start = mach_absolute_time();
	uintptr_t end = kva + soff + (uintptr_t)sstride * sh;
	for (uintptr_t p = (kva + soff) & ~(uintptr_t)63; p < end; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uint64_t clean_us = hg_us_since(start);

	const uint32_t shift = 256;
	hg_tables(true, pages, n);
	sweet_fb_hold_until = mach_absolute_time() + hold;
	start = mach_absolute_time();
	bool ok = hg_job_fragment_rt(HG_PANEL_VA + (uint64_t)shift * HG_FB_STRIDE, HG_PANEL_W,
	    HG_PANEL_H - shift, HG_FB_STRIDE, 0, HG_SRC_VA + soff, sstride, "GPU COPY");
	printf("HEROLTE GPU: GPU COPY: cache clean %llu us, GPU copy %llu us (one core) -> %s\n",
	    clean_us, hg_us_since(start),
	    ok ? "THE SCREEN SHOWS THE iOS FRAME 256 PIXELS LOWER FOR 8 SECONDS" : "failed");
}

/*
 * herogpu=8: the first real draw. One triangle with a hand-assembled
 * Midgard fragment shader (tools/herolte-midgard-shader.py: imov r0 from the
 * embedded constants, iadd r1.w = r31.x + 0, extended br.write looping on
 * itself, as Mesa's scheduler emits the writeout bundle), no vertex shader:
 * the tiler reads screen-space positions directly. A tiler job on JS1 bins
 * the triangle into the polygon list; a fragment job on JS0 then clears the
 * panel dark blue and runs the shader on the covered pixels. Descriptor
 * layouts are Mesa's genxml/v5.xml (Renderer State + one Blend, Draw,
 * Primitive, Invocation, Viewport, Tiler Job, Framebuffer with a Midgard
 * tiler context: one hierarchy level of 256x256 bins, Mesa's
 * panfrost_tiler_*_size() formulas).
 *
 * Everything lives in a 2 MiB scratch block of the first DRAM bank
 * (0x8fe00000, below the 128 KiB test block), mapped at GPU VA 0x0a000000.
 */
#define HG_SCR_PA       0x8fe00000u
#define HG_SCR_BYTES    0x200000u
#define HG_SCR_VA       0x0a000000ull
#define HG_SCR_L3_OFF   0x1f000u        /* last surface L3 slot, unused here */
#define S_SHADER        0x0000u
#define S_RSD           0x0100u         /* Renderer State (64 B) + Blend (16 B) */
#define S_VIEWPORT      0x0200u
#define S_POS           0x0300u
#define S_TILER_JOB     0x0400u         /* 192 B */
#define S_FRAG_JOB      0x0600u
#define S_FBD           0x0800u         /* Framebuffer (128 B) + Render Target */
#define S_POLY          0x1000u
#define S_HEAP          0x10000u
#define S_HEAP_BYTES    0x40000u
#define HG_BIN_LEVEL    4u              /* 16 << 4 = 256-pixel bins */
/* herogpu=9 only: varyings, sampler and a small texture. */
#define S_VARY_BUF      0x0a00u         /* Attribute Buffer (16 B, align 32) */
#define S_VARY          0x0a40u         /* Attribute (8 B) */
#define S_SAMPLER       0x0a80u         /* Sampler (32 B) */
#define S_TEX_PTRS      0x0ac0u         /* one 64-bit texture descriptor pointer */
#define S_TEXDESC       0x0b00u         /* Texture (32 B) + Surface With Stride */
#define S_VARY_DATA     0x0c00u         /* 4 vertices x vec4 texel coordinates */
#define S_TEX           0x80000u
#define HG_TEX_W        180u            /* the panel / 8 */
#define HG_TEX_H        320u

/* Solid colour: one writeout bundle, constants read as fp32 (~0.008). */
static const uint32_t hg_midgard_shader[12] = {
	0x0822001e, 0x841f0358, 0x4000027b, 0x0240ffae, 0xc0a00000, 0xfffffa77,
	0x0000ffff, 0x00000000, 0x3c003c00, 0x3c003c00, 0x3c003c00, 0x3c003c00,
};
#define HG_SHADER_TAG   0xeu            /* TAG_ALU_12_WRITEOUT */
/* Textured: ld_vary_32 r0 / imov r28, r0 / tex r29 = texture(r28.xy) /
 * imov r0, r29 + writeout (tools/herolte-midgard-shader.py, textured). */
static const uint32_t hg_midgard_tex_shader[20] = {
	0xc9e09885, 0x01403939, 0x00000030, 0x00000000, 0x00020038, 0x027b7018,
	0xffae4000, 0x00000000, 0x210b12d3, 0x00e43d67, 0x00000000, 0x00000000,
	0x0822001d, 0x841f03b8, 0x4000027b, 0x0240ffae, 0xc0a00000, 0xfffffc6f,
	0x0000ffff, 0x00000000,
};
#define HG_TEX_SHADER_TAG 0x5u          /* TAG_LOAD_STORE_4 */

static void
hg_wr32(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static void
hg_wr64(vm_offset_t base, uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(base + off) = v;
}

/* IEEE-754 single bit patterns: the kernel is built without FP registers. */
#define F32_0           0x00000000u
#define F32_HALF        0x3f000000u
#define F32_1           0x3f800000u
#define F32_1440        0x44b40000u
#define F32_2560        0x45200000u

static uint32_t
hg_tiler_size(uint32_t bytes_per_tile)
{
	uint32_t bin = 16u << HG_BIN_LEVEL;
	uint32_t size = 0x40 + ((HG_PANEL_W + bin - 1) / bin) * ((HG_PANEL_H + bin - 1) / bin) *
	    bytes_per_tile;
	return (size + 0x1ff) & ~0x1ffu;
}

/*
 * herogpu=9 builds on the same draw: a 180x320 test image (gradient,
 * checkerboard, white frame) in the scratch block is the texture of a
 * full-panel quad, so the GPU scales it 8x with bilinear filtering. As in
 * Mesa's preload (blit) shader there is still no vertex shader: the varying
 * buffer holds unnormalized texel coordinates per vertex and the sampler
 * takes them as is.
 */
static void
hg_draw(uint64_t hold, bool tex)
{
	static vm_offset_t scr;
	if (!scr) {
		scr = ml_io_map(HG_SCR_PA, HG_SCR_BYTES);
	}
	if (!scr) {
		printf("HEROLTE GPU: DRAW: scratch mapping failed\n");
		return;
	}
	hg_tables(true, NULL, 0);
	/* Scratch block: one more L3 table, 512 pages at HG_SCR_VA. */
	volatile uint64_t *l2t = (volatile uint64_t *)(hg_mem + 0x2000);
	volatile uint64_t *sl3 = (volatile uint64_t *)(hg_mem + HG_SCR_L3_OFF);
	l2t[(HG_SCR_VA >> 21) & 0x1ff] = (HG_TEST_PA + HG_SCR_L3_OFF) | 3;
	for (unsigned i = 0; i < 512; i++) {
		sl3[i] = (HG_SCR_PA + i * 0x1000ull) | 0x2c1;
	}
	hg_zero(scr, S_HEAP + S_HEAP_BYTES);

	uint32_t header = hg_tiler_size(8), full = hg_tiler_size(0x200);
	uint64_t fbd_ptr = (HG_SCR_VA + S_FBD) | 1;     /* MFBD, one render target */

	/* Shader. */
	if (tex) {
		for (unsigned i = 0; i < 20; i++) {
			hg_wr32(scr, S_SHADER + i * 4, hg_midgard_tex_shader[i]);
		}
	} else {
		for (unsigned i = 0; i < 12; i++) {
			hg_wr32(scr, S_SHADER + i * 4, hg_midgard_shader[i]);
		}
	}

	/* Renderer State. */
	if (tex) {
		hg_wr64(scr, S_RSD + 0, (HG_SCR_VA + S_SHADER) | HG_TEX_SHADER_TAG);
		hg_wr32(scr, S_RSD + 8, 1u | (1u << 16));       /* 1 sampler, 1 texture */
		hg_wr32(scr, S_RSD + 12, 1u << 16);             /* 1 varying */
		/* As Mesa's preload RSD on v4/v5: 4 work registers, force early-z. */
		hg_wr32(scr, S_RSD + 16, (4u << 16) | (1u << 10));
	} else {
		hg_wr64(scr, S_RSD + 0, (HG_SCR_VA + S_SHADER) | HG_SHADER_TAG);
		hg_wr32(scr, S_RSD + 16, 2u << 16);     /* work register count 2 */
	}
	hg_wr32(scr, S_RSD + 32, 0xffffu | (7u << 24)); /* sample mask, depth ALWAYS */
	/* Stencil masks; Midgard's fixed-function alpha test must be ALWAYS
	 * (0 = NEVER kills every fragment). */
	hg_wr32(scr, S_RSD + 36, 0xffffu | (7u << 21));
	hg_wr32(scr, S_RSD + 40, (7u << 16) | (0xffu << 8));    /* stencil front ALWAYS */
	hg_wr32(scr, S_RSD + 44, (7u << 16) | (0xffu << 8));    /* stencil back */
	/* Blend for RT0 (v5: right after the renderer state): enabled, src
	 * replaces dst (A = 0, B = src, C = 1 - 0) for RGB and alpha, mask RGBA. */
	hg_wr32(scr, S_RSD + 64, 1u << 9);
	hg_wr32(scr, S_RSD + 72, 0xf0921921u);

	/* Viewport: the whole panel. */
	hg_wr32(scr, S_VIEWPORT + 0, F32_0);
	hg_wr32(scr, S_VIEWPORT + 4, F32_0);
	hg_wr32(scr, S_VIEWPORT + 8, F32_1440);
	hg_wr32(scr, S_VIEWPORT + 12, F32_2560);
	hg_wr32(scr, S_VIEWPORT + 16, F32_0);
	hg_wr32(scr, S_VIEWPORT + 20, F32_1);
	hg_wr32(scr, S_VIEWPORT + 24, 0);
	hg_wr32(scr, S_VIEWPORT + 28, ((HG_PANEL_H - 1) << 16) | (HG_PANEL_W - 1));

	/* Three screen-space vertices (x, y, z, w): (720, 600), (260, 1900),
	 * (1180, 1900) pixels. */
	static const uint32_t pos[3][2] = {
		{ 0x44340000u, 0x44160000u },
		{ 0x43820000u, 0x44ed8000u },
		{ 0x44938000u, 0x44ed8000u },
	};
	/* Textured: a triangle strip over the whole panel, (0,0) (W,0) (0,H)
	 * (W,H), with texel coordinates (0,0) .. (180,320) as the varying. */
	static const uint32_t quad[4][4] = {
		{ F32_0, F32_0, F32_0, F32_0 },
		{ F32_1440, F32_0, 0x43340000u /* 180 */, F32_0 },
		{ F32_0, F32_2560, F32_0, 0x43a00000u /* 320 */ },
		{ F32_1440, F32_2560, 0x43340000u, 0x43a00000u },
	};
	unsigned nverts = tex ? 4 : 3;
	for (unsigned v = 0; v < nverts; v++) {
		hg_wr32(scr, S_POS + v * 16 + 0, tex ? quad[v][0] : pos[v][0]);
		hg_wr32(scr, S_POS + v * 16 + 4, tex ? quad[v][1] : pos[v][1]);
		hg_wr32(scr, S_POS + v * 16 + 8, F32_HALF);
		hg_wr32(scr, S_POS + v * 16 + 12, F32_1);
		if (tex) {
			hg_wr32(scr, S_VARY_DATA + v * 16 + 0, quad[v][2]);
			hg_wr32(scr, S_VARY_DATA + v * 16 + 4, quad[v][3]);
			hg_wr32(scr, S_VARY_DATA + v * 16 + 8, F32_0);
			hg_wr32(scr, S_VARY_DATA + v * 16 + 12, F32_1);
		}
	}
	if (tex) {
		/* Test image, panel-native bytes (B, G, R, A): a gradient, red
		 * across and green down, a 20-texel checkerboard darkening every
		 * other square, a 2-texel white frame and a white diagonal. */
		for (uint32_t y = 0; y < HG_TEX_H; y++) {
			for (uint32_t x = 0; x < HG_TEX_W; x++) {
				uint32_t r = x * 255 / (HG_TEX_W - 1), g = y * 255 / (HG_TEX_H - 1);
				uint32_t b = 160;
				if (((x / 20) ^ (y / 20)) & 1) {
					r /= 2;
					g /= 2;
					b /= 2;
				}
				uint32_t c = 0xff000000u | (r << 16) | (g << 8) | b;
				if (x < 2 || y < 2 || x >= HG_TEX_W - 2 || y >= HG_TEX_H - 2 ||
				    x * HG_TEX_H / HG_TEX_W == y) {
					c = 0xffffffffu;
				}
				hg_wr32(scr, S_TEX + (y * HG_TEX_W + x) * 4, c);
			}
		}
		/* Varying 0: RGB32F (RGB1 swizzle) from buffer 0, offset 0. */
		hg_wr64(scr, S_VARY_BUF + 0, (HG_SCR_VA + S_VARY_DATA) | 1);    /* 1D */
		hg_wr32(scr, S_VARY_BUF + 8, 16);                               /* stride */
		hg_wr32(scr, S_VARY_BUF + 12, 16 * 4);                          /* size */
		hg_wr32(scr, S_VARY + 0, (1u << 9) | (((183u << 12) | 0xa88u) << 10));
		hg_wr32(scr, S_VARY + 4, 0);
		/* Sampler: bilinear, unnormalized coordinates, clamp to edge. */
		hg_wr32(scr, S_SAMPLER + 0, 0);
		hg_wr32(scr, S_SAMPLER + 8, 9u | (9u << 4) | (9u << 8));
		/* Texture: 180x320 RGBA8 UNORM, 2D, linear, 64-bit with strides,
		 * then its one surface. */
		hg_wr32(scr, S_TEXDESC + 0, (HG_TEX_W - 1) | ((HG_TEX_H - 1) << 16));
		hg_wr32(scr, S_TEXDESC + 8, ((187u << 12) | 0x688u) | (2u << 22) | (2u << 24) |
		    (3u << 28));
		hg_wr32(scr, S_TEXDESC + 16, 0x688u);                          /* swizzle */
		hg_wr64(scr, S_TEXDESC + 32, HG_SCR_VA + S_TEX);
		hg_wr32(scr, S_TEXDESC + 40, HG_TEX_W * 4);                     /* row stride */
		hg_wr32(scr, S_TEXDESC + 44, HG_TEX_W * 4 * HG_TEX_H);         /* surface stride */
		hg_wr64(scr, S_TEX_PTRS, HG_SCR_VA + S_TEXDESC);
	}

	/* Tiler job: header, Invocation (+32), Primitive (+40), Draw (+64). */
	uint32_t tj = S_TILER_JOB;
	hg_wr32(scr, tj + 16, 1u | (7u << 1) | (1u << 16));    /* 64-bit, TILER, index 1 */
	hg_wr32(scr, tj + 32, nverts - 1);              /* num_y - 1 (2 bits for 3 or 4) */
	hg_wr32(scr, tj + 36, (32u << 22) | (2u << 28));        /* z shift 32, split 2 */
	hg_wr32(scr, tj + 40, (tex ? 10u : 8u) |        /* triangle strip / triangles */
	    (1u << 15) | (1u << 16) | (1u << 17) |      /* first provoking, depth culls */
	    (6u << 26));                                /* job task split */
	hg_wr32(scr, tj + 52, nverts - 1);              /* index count - 1 */
	uint32_t d = tj + 64;
	hg_wr32(scr, d + 0, 0x7);                       /* 4 comps, 64-bit descriptors */
	hg_wr64(scr, d + 16, HG_SCR_VA + S_POS);        /* Position */
	if (tex) {
		hg_wr64(scr, d + 32, HG_SCR_VA + S_TEX_PTRS);   /* Textures */
		hg_wr64(scr, d + 40, HG_SCR_VA + S_SAMPLER);    /* Samplers */
		hg_wr64(scr, d + 80, HG_SCR_VA + S_VARY_BUF);   /* Varying buffers */
		hg_wr64(scr, d + 88, HG_SCR_VA + S_VARY);       /* Varyings */
	}
	hg_wr64(scr, d + 56, HG_SCR_VA + S_RSD);        /* State */
	hg_wr64(scr, d + 96, HG_SCR_VA + S_VIEWPORT);   /* Viewport */
	hg_wr64(scr, d + 112, fbd_ptr);                 /* FBD (v5 tiler: tagged) */

	/* Framebuffer: as hg_job_fragment_rt(), with a live tiler context. */
	uint32_t f = S_FBD;
	uint32_t tiles_x = (HG_PANEL_W + 15) / 16, tiles_y = (HG_PANEL_H + 15) / 16;
	hg_wr32(scr, f + 4, 31);
	hg_wr32(scr, f + 32, ((HG_PANEL_H - 1) << 16) | (HG_PANEL_W - 1));
	hg_wr32(scr, f + 40, ((HG_PANEL_H - 1) << 16) | (HG_PANEL_W - 1));
	hg_wr32(scr, f + 44, (2u << 6) | (8u << 9) | (1u << 24));
	hg_wr32(scr, f + 48, 1u << 16);
	hg_wr32(scr, f + 56, full);                     /* polygon list size */
	hg_wr32(scr, f + 60, 1u << HG_BIN_LEVEL);       /* hierarchy mask */
	hg_wr64(scr, f + 64, HG_SCR_VA + S_POLY);
	hg_wr64(scr, f + 72, HG_SCR_VA + S_POLY + header);
	hg_wr64(scr, f + 80, HG_SCR_VA + S_HEAP);
	hg_wr64(scr, f + 88, HG_SCR_VA + S_HEAP + S_HEAP_BYTES);
	uint32_t rt = f + 128;
	hg_wr32(scr, rt + 0, 1u << 26);
	hg_wr32(scr, rt + 4, 1u | (19u << 3) | (2u << 10) | (1u << 15) | (0x688u << 16) | (1u << 31));
	hg_wr64(scr, rt + 32, HG_PANEL_VA);
	hg_wr32(scr, rt + 40, HG_FB_STRIDE);
	for (unsigned i = 0; i < 4; i++) {
		/* The panel is BGRA: the RT's first byte (R) shows as blue. */
		hg_wr32(scr, rt + 48 + i * 4, 0xff000040u);     /* dark blue background */
	}

	/* Fragment job over the whole panel. */
	hg_wr32(scr, S_FRAG_JOB + 16, 0x00010013);
	hg_wr32(scr, S_FRAG_JOB + 32, 0);
	hg_wr32(scr, S_FRAG_JOB + 36, ((tiles_y - 1) << 16) | (tiles_x - 1));
	hg_wr64(scr, S_FRAG_JOB + 40, fbd_ptr);
	__asm__ volatile ("dsb sy" ::: "memory");

	printf("HEROLTE GPU: DRAW: polygon list header %#x full %#x, heap %#x bytes\n",
	    header, full, S_HEAP_BYTES);
	sweet_fb_hold_until = mach_absolute_time() + hold;
	uint64_t start = mach_absolute_time();
	hg_job_va = HG_SCR_VA + S_TILER_JOB;
	hg_job_cpu = scr + S_TILER_JOB;
	bool ok = hg_run(1, "DRAW tiler job");
	if (ok) {
		hg_job_va = HG_SCR_VA + S_FRAG_JOB;
		hg_job_cpu = scr + S_FRAG_JOB;
		ok = hg_run(0, "DRAW fragment job");
	}
	hg_job_va = HG_TEST_VA;
	hg_job_cpu = 0;
	printf("HEROLTE GPU: DRAW: %llu us -> %s\n", hg_us_since(start), !ok ? "failed" : tex ?
	    "A 180x320 TEST IMAGE SCALED 8x TO THE WHOLE SCREEN FOR 8 SECONDS" :
	    "A TRIANGLE ON A DARK BLUE SCREEN FOR 8 SECONDS (white or black)");
	volatile uint32_t *p = (volatile uint32_t *)(scr + S_POLY);
	printf("HEROLTE GPU: DRAW: polygon list %08x %08x %08x %08x %08x %08x %08x %08x\n",
	    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	/* Did the tiler bin anything? Count written words per region. */
	unsigned nz_head = 0, nz_body = 0, nz_heap = 0, first_body = ~0u;
	for (uint32_t o = 0; o < full; o += 4) {
		if (*(volatile uint32_t *)(scr + S_POLY + o)) {
			if (o < header) {
				nz_head++;
			} else {
				nz_body++;
				if (first_body == ~0u) {
					first_body = o;
				}
			}
		}
	}
	for (uint32_t o = 0; o < S_HEAP_BYTES; o += 4) {
		nz_heap += *(volatile uint32_t *)(scr + S_HEAP + o) != 0;
	}
	printf("HEROLTE GPU: DRAW: nonzero words header=%u body=%u (first %#x) heap=%u\n",
	    nz_head, nz_body, first_body, nz_heap);
	/* Read back the panel inside the triangle and in the background; for
	 * the texture, next to the texel each pixel should come from. */
	static const uint32_t probe[4][2] = { { 720, 1500 }, { 720, 800 }, { 400, 1850 }, { 720, 300 } };
	for (unsigned i = 0; i < 4; i++) {
		uint64_t pa = HG_FB_PA + (uint64_t)probe[i][1] * HG_FB_STRIDE + probe[i][0] * 4;
		vm_offset_t pg = ml_io_map(pa & ~0xfffull, 0x1000);
		if (!pg) {
			continue;
		}
		uint32_t px = *(volatile uint32_t *)(pg + (pa & 0xfff));
		if (tex) {
			uint32_t tx = probe[i][0] / 8, ty = probe[i][1] / 8;
			printf("HEROLTE GPU: DRAW: pixel (%u,%u) = %08x, texel (%u,%u) = %08x\n",
			    probe[i][0], probe[i][1], px, tx, ty,
			    *(volatile uint32_t *)(scr + S_TEX + (ty * HG_TEX_W + tx) * 4));
		} else {
			printf("HEROLTE GPU: DRAW: pixel (%u,%u) = %08x%s\n", probe[i][0], probe[i][1],
			    px, i == 3 ? " (background)" : "");
		}
	}
	if (!ok || !tex) {
		return;
	}

	/* The same scaled draw again on all 12 shader cores, timed: what a
	 * GPU upscale of every iOS frame would cost. Fresh polygon list and
	 * heap, job headers back to "not run". */
	uint32_t all = hg_rd(hg_gpu, SHADER_PRESENT_LO);
	if (!hg_power(SHADER_PWRON_LO, SHADER_READY_LO, all, "all shader cores")) {
		return;
	}
	hg_zero(scr + S_POLY, S_HEAP + S_HEAP_BYTES - S_POLY);
	for (unsigned i = 0; i < 4; i++) {
		hg_wr32(scr, S_TILER_JOB + i * 4, 0);
		hg_wr32(scr, S_FRAG_JOB + i * 4, 0);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	hg_affinity = all;
	start = mach_absolute_time();
	hg_job_va = HG_SCR_VA + S_TILER_JOB;
	hg_job_cpu = scr + S_TILER_JOB;
	ok = hg_run(1, "DRAW tiler job, all cores");
	if (ok) {
		hg_job_va = HG_SCR_VA + S_FRAG_JOB;
		hg_job_cpu = scr + S_FRAG_JOB;
		ok = hg_run(0, "DRAW fragment job, all cores");
	}
	hg_job_va = HG_TEST_VA;
	hg_job_cpu = 0;
	hg_affinity = 1;
	printf("HEROLTE GPU: DRAW (%#x): scaled image in %llu us -> %s\n", all, hg_us_since(start),
	    ok ? "ok" : "failed");
}

/*
 * GPU present (SweetFramebuffer.cpp, boot-args sweetfb_scale=1 sweetfb_gpu=1):
 * iOS draws a smaller frame (the ADT canvas, 960x1706) and every frame shown
 * is the herogpu=9 draw with that iOS surface as the texture: a full-panel
 * quad, bilinear, on all 12 shader cores, instead of a CPU copy.
 *
 * Set up once, on the first frame: page tables (the panel as in herogpu=6,
 * the scratch block with the shader and descriptors as in herogpu=8/9), all
 * cores powered and left on. iOS surfaces get GPU VAs in 8 MiB slots
 * (4 L3 tables each, in the scratch block); a surface is mapped the first
 * time it is shown and keeps its slot while its id and first page match.
 * A remapped slot takes a fresh VA and the MMU is flushed, so no stale TLB
 * entry can point at the old surface. Per frame the CPU writes the texture
 * size/address and texel coordinates, zeroes the polygon list header (Mesa
 * zeroes it with a WRITE_VALUE job per frame), resets the two job headers
 * and runs tiler then fragment. Run only with herogpu<=2 (no test jobs).
 */
#define HGP_SLOTS       4u
#define HGP_SLOT_TABLES 4u
#define HGP_SLOT_PAGES  (HGP_SLOT_TABLES * 512u)
#define HGP_L3_OFF      0x100000u       /* slot L3 tables, in the scratch block */
#define HGP_VA_BASE     0x10000000ull
#define HGP_VA_END      0x40000000ull   /* the one L2 table covers 1 GiB */

struct hgp_slot {
	uint32_t        id;
	uint64_t        first_pa;
	uint64_t        va;
};
static struct hgp_slot hgp_slots[HGP_SLOTS];
static unsigned hgp_victim;
static uint64_t hgp_next_va = HGP_VA_BASE;
static vm_offset_t hgp_scr;
static int hgp_state;                   /* 0 not set up, 1 ready, -1 failed */
static uint64_t hgp_frames;

/* Wait (bounded) for AS0 to finish its command. */
static bool
hgp_as_idle(void)
{
	for (unsigned i = 0; i < 10000 && (hg_rd(hg_gpu, AS0 + 0x28) & 1); i++) {
		delay(1);
	}
	return !(hg_rd(hg_gpu, AS0 + 0x28) & 1);
}

/* Lock a VA range and flush the MMU's page table caches for it, as kbase's
 * mmu_hw_do_operation(): LOCKADDR, LOCK, FLUSH_PT (which also unlocks). */
static bool
hgp_mmu_flush(uint64_t va, unsigned log2_size)
{
	uint64_t lock = (va & ~((1ull << log2_size) - 1)) | (log2_size - 1);
	if (!hgp_as_idle()) {
		return false;
	}
	hg_wr(hg_gpu, AS0 + 0x10, (uint32_t)lock);              /* AS_LOCKADDR */
	hg_wr(hg_gpu, AS0 + 0x14, (uint32_t)(lock >> 32));
	hg_wr(hg_gpu, AS0 + 0x18, 2);                           /* LOCK */
	if (!hgp_as_idle()) {
		return false;
	}
	hg_wr(hg_gpu, AS0 + 0x18, 4);                           /* FLUSH_PT */
	return hgp_as_idle();
}

static bool
hgp_setup(void)
{
	vm_offset_t pmu = ml_io_map(HG_PMU_PA, 0x8000);
	if (!pmu || !hg_powered(pmu)) {
		printf("HEROLTE GPU PRESENT: G3D domain off\n");
		return false;
	}
	if (!hg_gpu) {
		hg_gpu = ml_io_map(HG_GPU_PA, 0x5000);
	}
	if (!hg_mem) {
		hg_mem = ml_io_map(HG_TEST_PA, HG_TEST_BYTES);
	}
	hgp_scr = ml_io_map(HG_SCR_PA, HG_SCR_BYTES);
	if (!hg_gpu || !hg_mem || !hgp_scr) {
		printf("HEROLTE GPU PRESENT: mapping failed\n");
		return false;
	}
	uint32_t all = hg_rd(hg_gpu, SHADER_PRESENT_LO);
	if (!hg_power(L2_PWRON_LO, L2_READY_LO, hg_rd(hg_gpu, L2_PRESENT_LO), "L2") ||
	    !hg_power(SHADER_PWRON_LO, SHADER_READY_LO, all, "all shader cores") ||
	    !hg_power(TILER_PWRON_LO, TILER_READY_LO, hg_rd(hg_gpu, TILER_PRESENT_LO), "tiler")) {
		hg_cores_off();
		return false;
	}

	/* Page tables: test pages and the panel, then the scratch block. */
	hg_tables(true, NULL, 0);
	vm_offset_t scr = hgp_scr;
	volatile uint64_t *l2t = (volatile uint64_t *)(hg_mem + 0x2000);
	volatile uint64_t *sl3 = (volatile uint64_t *)(hg_mem + HG_SCR_L3_OFF);
	l2t[(HG_SCR_VA >> 21) & 0x1ff] = (HG_TEST_PA + HG_SCR_L3_OFF) | 3;
	for (unsigned i = 0; i < 512; i++) {
		sl3[i] = (HG_SCR_PA + i * 0x1000ull) | 0x2c1;
	}
	hg_zero(scr, S_HEAP + S_HEAP_BYTES);
	hg_zero(scr + HGP_L3_OFF, HGP_SLOTS * HGP_SLOT_TABLES * 0x1000);
	bzero(hgp_slots, sizeof(hgp_slots));

	/* Shader, Renderer State + Blend, Viewport: as herogpu=9. */
	for (unsigned i = 0; i < 20; i++) {
		hg_wr32(scr, S_SHADER + i * 4, hg_midgard_tex_shader[i]);
	}
	hg_wr64(scr, S_RSD + 0, (HG_SCR_VA + S_SHADER) | HG_TEX_SHADER_TAG);
	hg_wr32(scr, S_RSD + 8, 1u | (1u << 16));
	hg_wr32(scr, S_RSD + 12, 1u << 16);
	hg_wr32(scr, S_RSD + 16, (4u << 16) | (1u << 10));
	hg_wr32(scr, S_RSD + 32, 0xffffu | (7u << 24));
	hg_wr32(scr, S_RSD + 36, 0xffffu | (7u << 21));
	hg_wr32(scr, S_RSD + 40, (7u << 16) | (0xffu << 8));
	hg_wr32(scr, S_RSD + 44, (7u << 16) | (0xffu << 8));
	hg_wr32(scr, S_RSD + 64, 1u << 9);
	hg_wr32(scr, S_RSD + 72, 0xf0921921u);
	hg_wr32(scr, S_VIEWPORT + 8, F32_1440);
	hg_wr32(scr, S_VIEWPORT + 12, F32_2560);
	hg_wr32(scr, S_VIEWPORT + 20, F32_1);
	hg_wr32(scr, S_VIEWPORT + 28, ((HG_PANEL_H - 1) << 16) | (HG_PANEL_W - 1));

	/* Positions: the full-panel triangle strip. */
	static const uint32_t corner[4][2] = {
		{ F32_0, F32_0 }, { F32_1440, F32_0 }, { F32_0, F32_2560 }, { F32_1440, F32_2560 },
	};
	for (unsigned v = 0; v < 4; v++) {
		hg_wr32(scr, S_POS + v * 16 + 0, corner[v][0]);
		hg_wr32(scr, S_POS + v * 16 + 4, corner[v][1]);
		hg_wr32(scr, S_POS + v * 16 + 8, F32_HALF);
		hg_wr32(scr, S_POS + v * 16 + 12, F32_1);
		hg_wr32(scr, S_VARY_DATA + v * 16 + 12, F32_1);
	}
	hg_wr64(scr, S_VARY_BUF + 0, (HG_SCR_VA + S_VARY_DATA) | 1);
	hg_wr32(scr, S_VARY_BUF + 8, 16);
	hg_wr32(scr, S_VARY_BUF + 12, 16 * 4);
	hg_wr32(scr, S_VARY + 0, (1u << 9) | (((183u << 12) | 0xa88u) << 10));
	hg_wr32(scr, S_SAMPLER + 8, 9u | (9u << 4) | (9u << 8));
	hg_wr32(scr, S_TEXDESC + 8, ((187u << 12) | 0x688u) | (2u << 22) | (2u << 24) | (3u << 28));
	hg_wr32(scr, S_TEXDESC + 16, 0x688u);
	hg_wr64(scr, S_TEX_PTRS, HG_SCR_VA + S_TEXDESC);

	/* Tiler job + Draw. */
	uint32_t tj = S_TILER_JOB, d = S_TILER_JOB + 64;
	uint64_t fbd_ptr = (HG_SCR_VA + S_FBD) | 1;
	hg_wr32(scr, tj + 16, 1u | (7u << 1) | (1u << 16));
	hg_wr32(scr, tj + 32, 3);
	hg_wr32(scr, tj + 36, (32u << 22) | (2u << 28));
	hg_wr32(scr, tj + 40, 10u | (1u << 15) | (1u << 16) | (1u << 17) | (6u << 26));
	hg_wr32(scr, tj + 52, 3);
	hg_wr32(scr, d + 0, 0x7);
	hg_wr64(scr, d + 16, HG_SCR_VA + S_POS);
	hg_wr64(scr, d + 32, HG_SCR_VA + S_TEX_PTRS);
	hg_wr64(scr, d + 40, HG_SCR_VA + S_SAMPLER);
	hg_wr64(scr, d + 56, HG_SCR_VA + S_RSD);
	hg_wr64(scr, d + 80, HG_SCR_VA + S_VARY_BUF);
	hg_wr64(scr, d + 88, HG_SCR_VA + S_VARY);
	hg_wr64(scr, d + 96, HG_SCR_VA + S_VIEWPORT);
	hg_wr64(scr, d + 112, fbd_ptr);

	/* Framebuffer + render target (the panel) + fragment job. */
	uint32_t f = S_FBD, rt = S_FBD + 128;
	uint32_t header = hg_tiler_size(8), full = hg_tiler_size(0x200);
	hg_wr32(scr, f + 4, 31);
	hg_wr32(scr, f + 32, ((HG_PANEL_H - 1) << 16) | (HG_PANEL_W - 1));
	hg_wr32(scr, f + 40, ((HG_PANEL_H - 1) << 16) | (HG_PANEL_W - 1));
	hg_wr32(scr, f + 44, (2u << 6) | (8u << 9) | (1u << 24));
	hg_wr32(scr, f + 48, 1u << 16);
	hg_wr32(scr, f + 56, full);
	hg_wr32(scr, f + 60, 1u << HG_BIN_LEVEL);
	hg_wr64(scr, f + 64, HG_SCR_VA + S_POLY);
	hg_wr64(scr, f + 72, HG_SCR_VA + S_POLY + header);
	hg_wr64(scr, f + 80, HG_SCR_VA + S_HEAP);
	hg_wr64(scr, f + 88, HG_SCR_VA + S_HEAP + S_HEAP_BYTES);
	hg_wr32(scr, rt + 0, 1u << 26);
	hg_wr32(scr, rt + 4, 1u | (19u << 3) | (2u << 10) | (1u << 15) | (0x688u << 16) | (1u << 31));
	hg_wr64(scr, rt + 32, HG_PANEL_VA);
	hg_wr32(scr, rt + 40, HG_FB_STRIDE);
	for (unsigned i = 0; i < 4; i++) {
		hg_wr32(scr, rt + 48 + i * 4, 0xff000000u);     /* black bars */
	}
	uint32_t tiles_x = (HG_PANEL_W + 15) / 16, tiles_y = (HG_PANEL_H + 15) / 16;
	hg_wr32(scr, S_FRAG_JOB + 16, 0x00010013);
	hg_wr32(scr, S_FRAG_JOB + 36, ((tiles_y - 1) << 16) | (tiles_x - 1));
	hg_wr64(scr, S_FRAG_JOB + 40, fbd_ptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("HEROLTE GPU PRESENT: ready, cores %#x\n", all);
	return true;
}

/* IEEE-754 bits of an integer below 2^24 (exact; no FP in the kernel). */
static uint32_t
hgp_f32(uint32_t v)
{
	if (v == 0) {
		return 0;
	}
	uint32_t e = 31 - __builtin_clz(v);
	return ((127 + e) << 23) | ((v << (23 - e)) & 0x7fffff);
}

/* Is the surface (id, first page) mapped? Returns its slot or -1. */
int
herolte_gpu_present_slot(uint32_t id, uint64_t first_pa)
{
	for (unsigned i = 0; i < HGP_SLOTS; i++) {
		if (hgp_slots[i].id == id && hgp_slots[i].first_pa == first_pa && id != 0) {
			return (int)i;
		}
	}
	return -1;
}

/* Map n 4 KiB pages of a surface into a slot (round robin); slot or -1. */
int
herolte_gpu_present_map(uint32_t id, const uint64_t *pages, unsigned n)
{
	if (hgp_state == 0) {
		hgp_state = hgp_setup() ? 1 : -1;
	}
	if (hgp_state != 1 || n == 0 || n > HGP_SLOT_PAGES) {
		return -1;
	}
	unsigned s = hgp_victim;
	hgp_victim = (hgp_victim + 1) % HGP_SLOTS;
	if (hgp_next_va + HGP_SLOT_PAGES * 0x1000ull > HGP_VA_END) {
		hgp_next_va = HGP_VA_BASE;
	}
	uint64_t old_va = hgp_slots[s].va;
	uint64_t va = hgp_next_va;
	hgp_next_va += HGP_SLOT_PAGES * 0x1000ull;
	volatile uint64_t *l2t = (volatile uint64_t *)(hg_mem + 0x2000);
	if (old_va) {
		for (unsigned t = 0; t < HGP_SLOT_TABLES; t++) {
			l2t[((old_va >> 21) & 0x1ff) + t] = 0;
		}
	}
	for (unsigned t = 0; t < HGP_SLOT_TABLES; t++) {
		uint32_t table = HGP_L3_OFF + (s * HGP_SLOT_TABLES + t) * 0x1000;
		volatile uint64_t *l3 = (volatile uint64_t *)(hgp_scr + table);
		for (unsigned i = 0; i < 512; i++) {
			unsigned p = t * 512 + i;
			l3[i] = p < n ? (pages[p] | 0x2c1) : 0;
		}
		l2t[((va >> 21) & 0x1ff) + t] = (HG_SCR_PA + table) | 3;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!hgp_mmu_flush(HGP_VA_BASE, 30)) {
		printf("HEROLTE GPU PRESENT: MMU flush timeout\n");
	}
	hgp_slots[s].id = id;
	hgp_slots[s].first_pa = pages[0];
	hgp_slots[s].va = va;
	printf("HEROLTE GPU PRESENT: surface %u (%u pages, pa %#llx) in slot %u at VA %#llx\n",
	    id, n, pages[0], s, va);
	return (int)s;
}

/*
 * Draw the w x h rect at byte offset off (row stride bytes) of the surface
 * in slot s to the whole panel, scaled, bilinear. The caller has cleaned
 * the rect to the point of coherency. False if the GPU failed (the caller
 * falls back to the CPU copy).
 */
bool
herolte_gpu_present_draw(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h)
{
	if (hgp_state != 1 || s < 0 || s >= (int)HGP_SLOTS || w == 0 || h == 0 ||
	    w > 0x10000 || h > 0x10000) {
		return false;
	}
	vm_offset_t scr = hgp_scr;
	uint32_t fw = hgp_f32(w), fh = hgp_f32(h);
	/* Keep the aspect: a frame taller than the panel's 16:9 (the iPhone
	 * XR's 828x1792) is drawn full height, centered between black bars;
	 * a wider one full width. The bars are the render target's clear. */
	uint32_t x0 = 0, x1 = HG_PANEL_W, y0 = 0, y1 = HG_PANEL_H;
	if ((uint64_t)w * HG_PANEL_H < (uint64_t)h * HG_PANEL_W) {
		uint32_t dw = (uint32_t)((uint64_t)w * HG_PANEL_H / h);
		x0 = (HG_PANEL_W - dw) / 2;
		x1 = x0 + dw;
	} else if ((uint64_t)w * HG_PANEL_H > (uint64_t)h * HG_PANEL_W) {
		uint32_t dh = (uint32_t)((uint64_t)h * HG_PANEL_W / w);
		y0 = (HG_PANEL_H - dh) / 2;
		y1 = y0 + dh;
	}
	for (unsigned v = 0; v < 4; v++) {
		hg_wr32(scr, S_POS + v * 16 + 0, hgp_f32(v & 1 ? x1 : x0));
		hg_wr32(scr, S_POS + v * 16 + 4, hgp_f32(v & 2 ? y1 : y0));
	}
	hg_wr32(scr, S_VARY_DATA + 1 * 16 + 0, fw);
	hg_wr32(scr, S_VARY_DATA + 2 * 16 + 4, fh);
	hg_wr32(scr, S_VARY_DATA + 3 * 16 + 0, fw);
	hg_wr32(scr, S_VARY_DATA + 3 * 16 + 4, fh);
	hg_wr32(scr, S_TEXDESC + 0, (w - 1) | ((h - 1) << 16));
	hg_wr64(scr, S_TEXDESC + 32, hgp_slots[s].va + off);
	hg_wr32(scr, S_TEXDESC + 40, stride);
	hg_wr32(scr, S_TEXDESC + 44, stride * h);
	hg_zero(scr + S_POLY, hg_tiler_size(8));
	for (unsigned i = 0; i < 4; i++) {
		hg_wr32(scr, S_TILER_JOB + i * 4, 0);
		hg_wr32(scr, S_FRAG_JOB + i * 4, 0);
	}
	__asm__ volatile ("dsb sy" ::: "memory");

	hg_run_quiet = true;
	hg_affinity = hg_rd(hg_gpu, SHADER_PRESENT_LO);
	hg_job_va = HG_SCR_VA + S_TILER_JOB;
	hg_job_cpu = scr + S_TILER_JOB;
	bool ok = hg_run(1, "PRESENT tiler job");
	if (ok) {
		hg_job_va = HG_SCR_VA + S_FRAG_JOB;
		hg_job_cpu = scr + S_FRAG_JOB;
		ok = hg_run(0, "PRESENT fragment job");
	}
	hg_job_va = HG_TEST_VA;
	hg_job_cpu = 0;
	hg_affinity = 1;
	hg_run_quiet = false;
	if (++hgp_frames <= 3 || !ok) {
		printf("HEROLTE GPU PRESENT: frame %llu %ux%u stride %u slot %d -> %s\n",
		    hgp_frames, w, h, stride, s, ok ? "ok" : "failed");
	}
	return ok;
}

/* herogpu=7, step 2: all shader cores clear the whole panel, timed. */
static void
hg_all_cores_test(uint64_t hold)
{
	uint32_t all = hg_rd(hg_gpu, SHADER_PRESENT_LO);
	if (!hg_power(SHADER_PWRON_LO, SHADER_READY_LO, all, "all shader cores")) {
		printf("HEROLTE GPU: ALL CORES: not all cores came up\n");
		return;
	}
	hg_affinity = all;
	sweet_fb_hold_until = mach_absolute_time() + hold;
	uint64_t start = mach_absolute_time();
	bool ok = hg_job_fragment(HG_PANEL_VA, HG_PANEL_W, HG_PANEL_H, HG_FB_STRIDE, HG_RED, true,
	    "FRAGMENT whole panel, all cores");
	printf("HEROLTE GPU: ALL CORES (%#x): whole panel in %llu us -> %s\n", all,
	    hg_us_since(start), ok ? "THE SCREEN SHOULD BE BLUE FOR 8 SECONDS" : "failed");
	hg_affinity = 1;
}

static void
hg_jobs(vm_offset_t pmu, int mode)
{
	if (!hg_powered(pmu)) {
		printf("HEROLTE GPU: jobs: domain off\n");
		return;
	}
	if (!hg_gpu) {
		hg_gpu = ml_io_map(HG_GPU_PA, 0x5000);
	}
	if (!hg_mem) {
		hg_mem = ml_io_map(HG_TEST_PA, HG_TEST_BYTES);
	}
	if (!hg_gpu || !hg_mem) {
		printf("HEROLTE GPU: jobs: mapping failed\n");
		return;
	}
	uint32_t as0 = hg_rd(hg_gpu, AS0 + 0x28);
	if (!(hg_rd(hg_gpu, SHADER_PRESENT_LO) & 1) || (as0 & 1)) {
		printf("HEROLTE GPU: jobs: not idle (as0=%#x)\n", as0);
		return;
	}
	if (!hg_cores_on()) {
		printf("HEROLTE GPU: jobs: cores did not come up; nothing submitted\n");
		hg_cores_off();
		return;
	}
	if (!hg_job_write_value() || mode < 4) {
		hg_cores_off();
		return;
	}
	if (mode >= 8) {
		uint64_t hold8;
		nanoseconds_to_absolutetime(8ull * NSEC_PER_SEC, &hold8);
		hg_draw(hold8, mode >= 9);
		hg_cores_off();
		return;
	}

	/* Private 16x16 red clear: count the pixels. */
	volatile uint32_t *pix = (volatile uint32_t *)(hg_mem + HG_DATA_OFF + HG_PIX_OFF);
	bool ok = hg_job_fragment(HG_TEST_VA + HG_PIX_OFF, 16, 16, 64, HG_RED, false,
	    "FRAGMENT private 16x16");
	unsigned red = 0;
	for (unsigned i = 0; i < 256; i++) {
		red += pix[i] == HG_RED;
	}
	printf("HEROLTE GPU: FRAGMENT private: first=%#x last=%#x red=%u/256 -> %s\n",
	    pix[0], pix[255], red, ok && red == 256 ? "THE MALI-T880 RENDERED PIXELS" : "failed");
	if (!ok || red != 256 || mode < 5) {
		hg_cores_off();
		return;
	}

	/*
	 * The GPU writes the scanout buffer, but 0229's square was gone before
	 * anyone could see it: the CPU copy of the next iOS frame covers it.
	 * Hold those copies for 8 s around each panel job.
	 */
	uint64_t hold;
	nanoseconds_to_absolutetime(8ull * NSEC_PER_SEC, &hold);
	sweet_fb_hold_until = mach_absolute_time() + hold;

	/* 128x128 green square in the middle of the panel. */
	uint64_t off = (uint64_t)HG_SQ_Y * HG_FB_STRIDE + HG_SQ_X * 4;
	ok = hg_job_fragment(HG_PANEL_VA + off, HG_SQ_SIZE, HG_SQ_SIZE, HG_FB_STRIDE, HG_GREEN, true,
	    "FRAGMENT panel square");
	printf("HEROLTE GPU: FRAGMENT panel square at %u,%u -> %s\n", HG_SQ_X, HG_SQ_Y,
	    ok ? "done (screen held 8 s)" : "failed");
	if (ok && mode >= 6) {
		/* herogpu=6: then the whole panel green, held 8 s. */
		hg_msleep(3000);
		sweet_fb_hold_until = mach_absolute_time() + hold;
		uint64_t start = mach_absolute_time(), ns;
		ok = hg_job_fragment(HG_PANEL_VA, HG_PANEL_W, HG_PANEL_H, HG_FB_STRIDE, HG_GREEN, true,
		    "FRAGMENT whole panel");
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		printf("HEROLTE GPU: FRAGMENT whole panel 1440x2560 in %llu us (one core) -> %s\n",
		    ns / 1000, ok ? "THE SCREEN SHOULD BE GREEN FOR 8 SECONDS" : "failed");
	}
	if (ok && mode >= 7) {
		hg_msleep(10000);
		hg_gpu_copy_test(hold);
		hg_msleep(10000);
		hg_all_cores_test(hold);
	}
	hg_cores_off();
}

static void
hg_thread(void *arg, __unused wait_result_t wr)
{
	int mode = (int)(uintptr_t)arg;
	vm_offset_t pmu = ml_io_map(HG_PMU_PA, 0x8000);
	if (!pmu) {
		printf("HEROLTE GPU: PMU mapping failed\n");
		return;
	}
	/* Once early, once after the UI is up (in case something changed it). */
	hg_msleep(5000);
	hg_probe(pmu, "5s");
	hg_msleep(55000);
	hg_probe(pmu, "60s");
	if (mode >= 2) {
		hg_pmic_dump();
	}
	if (mode >= 3) {
		hg_jobs(pmu, mode);
	}
}

void
herolte_gpu_probe(void)
{
	int enabled = 1;
	thread_t thread;

	PE_parse_boot_argn("herogpu", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("HEROLTE GPU: probe disabled\n");
		return;
	}
	if (kernel_thread_start(hg_thread, (void *)(uintptr_t)enabled, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_HEROLTE */
