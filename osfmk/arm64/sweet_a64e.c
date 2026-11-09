/*
 * sweet: user-mode emulation of the arm64e (ARMv8.3+) instructions the S9's
 * cores lack (A55 is ARMv8.2, Mongoose M3 ARMv8.0), so iOS 26 userland, which
 * is arm64e only, can run. Pointers are never signed on this kernel, so the
 * pointer-authentication instructions reduce to their unauthenticated forms.
 * This is the safety net behind the host-side translator (which rewrites
 * these instructions in place) and covers code generated at run time (JIT).
 *
 *   PAC*, AUT*, XPAC* (register forms)    -> no change to the register
 *   RETAA/RETAB, BRA*, BLRA*              -> RET / BR / BLR
 *   LDRAA/LDRAB                           -> LDR (offset and pre-index forms)
 *   PACGA                                 -> deterministic 32-bit hash << 32 (sweetspeed&4: Xn & 0xffffffff00000000,
 *                                            what the cache rewrite of tools/ios27/speed leaves behind)
 *   FJCVTZS                               -> JavaScript ToInt32, Z = exact
 *   FCADD (vector, H/S/D)                 -> lane-wise FADD/FSUB, user FPCR
 *
 * And the ARMv8.4+ classes macOS (built for M1+) uses: MSR/MRS DIT, STLUR/LDAPUR (LRCPC2),
 *   FMLAL{2}/FMLSL{2} (FHM)               -> exact half->single widening, one FMADD, user FPCR
 *   EOR3, BCAX, RAX1, XAR (SHA3)          -> 64-bit integer lane operations
 *   SHA512H/H2/SU0/SU1 (SHA512)           -> the ARM ARM's SHA-512 round/schedule steps
 *   SB                                    -> nothing (the trap's ERET is context synchronizing)
 *
 * And for the Mongoose M3 (ARMv8.0: no LSE, no LRCPC), so iOS 26 runs on the big cores too:
 *   CAS*, CASP*, LD<op>*, ST<op>*, SWP*   -> compare-and-swap loop on the aligned 8 (16) bytes
 *                                            around the target (sweet_copy_cas64/128, copyio.c),
 *                                            full barriers (stronger than any A/L form)
 *   LDAPR{B,H,,}                          -> load + DMB ISH
 *   FMOV H, FCMP H (sweetspeed&2)         -> moves and compares in the thread's own state, instead of pinning it to an A55
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <mach/mach_types.h>
#include <mach/arm/thread_status.h>
#include <arm64/proc_reg.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <libkern/OSAtomic.h>
#include <arm/thread.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <os/atomic_private.h>

bool sweet_a64e_emulate(arm_saved_state_t *state, uint32_t instr);
/* sweet_a64e_fp.s: one FP add/sub of `size` (1 = H, 2 = S, 3 = D) under fpcr. */
extern uint64_t sweet_a64e_fp_addsub(uint64_t a, uint64_t b, uint32_t op_size, uint32_t fpcr,
    uint32_t *fpsr);

uint64_t sweet_a64e_counts[8];

/* Traps by process, for the STARLTE TOP report (bsd/dev/starlte_smp.c): a small open table. */
#define SWEET_A64E_PIDS 32
struct sweet_a64e_pid { int pid; uint64_t count; };
struct sweet_a64e_pid sweet_a64e_pids[SWEET_A64E_PIDS];
extern int proc_selfpid(void);

/*
 * Boot-arg sweetspeed=<mask> (default 0: none of it): the changes of the boot-time and trap-cost work of tools/ios27/speed
 * (docs/ios27/SPEED.md). Read once, when first asked, so the arguments are there.
 *    1  IOKitInitializeTime does not wait 30 s for the IORTC resource (iokit/Kernel/IOStartIOKit.cpp: nothing here publishes it)
 *    2  on a Mongoose M3, FMOV Hd,#imm / FMOV H<->W,X / FCMP,FCMPE H are emulated here instead of pinning the thread to an A55
 *    4  PACGA gives Xn & 0xffffffff00000000, as the cache's PACGA sites were rewritten ahead of time to AND Xd, Xn, #mask
 *       (tools/ios27/speed/a64e-rewrite2.py); a trapped one must agree with them, so set it only on a root rewritten that way
 *    8  "SBBOOT <ms> ms: <stage>" lines on the console from kernel_bootstrap, kernel_bootstrap_thread and bsd_init
 *   16  "STARLTE PIN: <program>[pid] ..." for each of the first 64 pins of a thread to an A55, then one every 10 s at most
 * The changes of the night run (patch 0427, docs/ios27/NIGHT.md), all off unless asked for:
 *   32  who ends what: "SWEET SIG" (kill, terminate_with_reason, psignal), "SWEET EXIT" per process exit, "SWEET SIGBT" (a kernel
 *       backtrace of every SIGKILL a process sends itself), "SWEET LIMITS" (launchd's spawn resource limits per exec), "SWEET RLIMIT"
 *   64  asynchronous GPU submits for StarbiteMali clients that ask (selectors 0x105, 0x106), and "HakiosWaitsForGPU" on the framebuffer
 *  128  "SWEET PREWARM": the GL stack and the UI daemons read into the page cache from a kernel thread once the root is mounted
 *  256  ignore the kqworkloop limits launchd sets at spawn (their hard-limit "fatal port" has no receiver here and kills the daemon)
 *  512  the S6E3HA8 panel driver (as panel=1) with iOS 27's swap brightness and setProperties brightness keys sent to it, and
 *       "SWEET FB ARGS" lines for the rarer IOMobileFramebuffer selectors
 * 1024  the framebuffer's 60 Hz timer keeps an absolute deadline (a present no longer delays the next tick)
 * 2048  CoreAnimation pacing aids (as sweetfb_cav=1: IOMFBDisplayRefresh, "SWEET FB CAV" lines)
 * 4096  scripted input for a repeatable frame-rate run (starlte_touch.c script 5)
 * 8192  scripted input that drags the brightness slider (script 6)
 * The optimisation run (patch 0429, docs/ios27/OPTIMISE.md):
 * 16384  SWEET PROF: a 1 ms per-CPU sampling profiler (osfmk/arm64/sweet_prof.c, report every 10 s with the STARLTE TOP report),
 *        the time inside the emulator, and the framebuffer's "A5XELTE FB gaps" histogram of vsync ticks between presented frames
 * 32768  the extra RAM canary pages (as extra_ram_canary=1; a bit for a command line that is full)
 * 65536  recommended cores 0xf1 (cpu0 + the four M3s) once the machine is up: the scheduler here is symmetric, so UI work else lands on
 *        the A55s; threads bound to an A55 (FP16 pins) still run there
 *131072  the A55s' governor floor 1456 MHz (measured: no change, kept for the record)
 * The Liquid Glass run (docs/ios27/LIQUID-GLASS.md):
 *262144  the GPU's app chains in a queue with a thread of their own (bsd/dev/starlte_gpu.c): a submit only appends, waits
 *        sleep, and the framebuffer shows a frame once the chains queued before its swap are done (needs bit 64)
 *524288  the GPU present cleans a frame out of the CPU caches only when its IOSurface seed moved (a CPU wrote it)
 * (1048576, 2097152 and 4194304 are the data-protection work's.)
 *8388608 an IOSurface that is a snapshot of its maker's unwireable memory is compared with it again at the maker's locks,
 *        use counts and release, and brought up to date (iokit/Kernel/arm/SweetSurface.cpp sweet_surface_resnap)
 * (16777216, 33554432 and 67108864 are the data-protection work's too.)
 *134217728 a cap on the GPU buffers' wired memory: all clients 35 % of the memory, one client half of that; a buffer beyond
 *        it is refused (iokit/Kernel/arm/StarbiteMali.cpp; sbm_cap_mb= and sbm_client_cap_mb= set the caps by hand)
 * The FP16 forms emulated under bit 2 now include FCCMP/FCCMPE/FCSEL H, FMOV/FABS/FNEG H and FABS/FNEG .4H/.8H (tools/ios27/perf/test/fp16t.c
 * checks them bit for bit); and, always on, a trap that was taken on an M3 but is handled after the thread migrated to an A55 is
 * retried (the A55 runs it) instead of ending in SIGILL.
 */
#define SWEET_SPEED_IORTC   1
#define SWEET_SPEED_FP16    2
#define SWEET_SPEED_PACGA   4
#define SWEET_SPEED_BOOTLOG 8
#define SWEET_SPEED_PINLOG  16

uint32_t sweet_speed_flags(void);
static uint32_t sweet_speed_value;
static int sweet_speed_state;               /* 0 = boot-arg not read yet */

uint32_t
sweet_speed_flags(void)
{
	if (os_atomic_load(&sweet_speed_state, acquire) == 0) {
		uint32_t v = 0;
		PE_parse_boot_argn("sweetspeed", &v, sizeof(v));
		sweet_speed_value = v;
		os_atomic_store(&sweet_speed_state, 1, release);
	}
	return sweet_speed_value;
}

/* Where the non-LSE emulations come from (PAC*, PACGA, FJCVTZS, FCADD, DOT, DIT, LRCPC2, LDRA ...): an
 * open table of user PCs with the instruction word, racy counts like the LSE sites below, read and reset
 * by the STARLTE A64E hot report (bsd/dev/starlte_smp.c). */
#define SWEET_A64E_SITES 512
struct sweet_a64e_site { uint64_t pc, count; uint32_t insn; };
struct sweet_a64e_site sweet_a64e_sites[SWEET_A64E_SITES];

static void
a64e_site(uint64_t pc, uint32_t w)
{
	unsigned h = (unsigned)((pc >> 2) * 0x9E3779B1u) % SWEET_A64E_SITES;
	for (unsigned i = 0; i < 8; i++) {
		struct sweet_a64e_site *e = &sweet_a64e_sites[(h + i) % SWEET_A64E_SITES];
		if (e->pc == pc || e->pc == 0) {
			e->pc = pc;
			e->insn = w;
			e->count++;
			return;
		}
	}
}

static void
sweet_a64e_count_pid(void)
{
	int pid = proc_selfpid();
	unsigned h = (unsigned)pid % SWEET_A64E_PIDS;
	for (unsigned i = 0; i < SWEET_A64E_PIDS; i++) {
		struct sweet_a64e_pid *e = &sweet_a64e_pids[(h + i) % SWEET_A64E_PIDS];
		if (e->pid == pid || e->pid == 0) {
			e->pid = pid;
			e->count++;
			return;
		}
	}
}

/* r = 31 reads as XZR (sp31 false) or SP (sp31 true). */
static uint64_t
reg_get(arm_saved_state_t *ss, unsigned r, bool sp31)
{
	arm_saved_state64_t *s = saved_state64(ss);
	if (r < 29) {
		return s->x[r];
	}
	switch (r) {
	case 29: return s->fp;
	case 30: return s->lr;
	default: return sp31 ? s->sp : 0;
	}
}

static void
reg_set(arm_saved_state_t *ss, unsigned r, bool sp31, uint64_t v)
{
	arm_saved_state64_t *s = saved_state64(ss);
	if (r < 29) {
		s->x[r] = v;
	} else if (r == 29) {
		s->fp = v;
	} else if (r == 30) {
		s->lr = v;
	} else if (sp31) {
		s->sp = v;
	}
}

static uint64_t
pacga_hash(uint64_t a, uint64_t b)
{
	if (sweet_speed_flags() & SWEET_SPEED_PACGA) {
		/* the cache's PACGA sites are rewritten ahead of time to AND Xd, Xn, #0xffffffff00000000;
		 * trapped ones must give the same value */
		(void)b;
		return a & 0xffffffff00000000ULL;
	}
	uint64_t h = a * 0x9e3779b97f4a7c15ULL ^ (b + 0x632be59bd9b4e019ULL);
	h ^= h >> 31;
	h *= 0xbf58476d1ce4e5b9ULL;
	h ^= h >> 29;
	return h & 0xffffffff00000000ULL;
}

/* JavaScript ToInt32 of an IEEE double, integer arithmetic only. */
static uint32_t
fjcvtzs(uint64_t bits, bool *exact)
{
	int exp = (int)((bits >> 52) & 0x7ff);
	uint64_t mant = bits & ((1ULL << 52) - 1);
	bool neg = (bits >> 63) != 0;
	uint32_t r;

	*exact = false;
	if (exp == 0x7ff) {
		return 0;                       /* NaN, Inf */
	}
	if (exp == 0) {
		*exact = (mant == 0 && !neg);   /* +0 exact; -0 and denormals not */
		return 0;
	}
	int e = exp - 1075;                     /* value = (mant|1<<52) * 2^e */
	mant |= 1ULL << 52;
	bool frac = false;
	if (e <= -53) {
		frac = true;
		r = 0;
	} else if (e < 0) {
		frac = (mant & ((1ULL << -e) - 1)) != 0;
		r = (uint32_t)(mant >> -e);
	} else if (e < 32) {
		r = (uint32_t)(mant << e);
	} else {
		r = 0;                          /* multiple of 2^32 */
	}
	/* magnitude fits int32 exactly? (before the modulo) */
	bool in_range;
	if (e >= 32) {
		in_range = false;
	} else if (e >= 0) {
		in_range = e <= 11 && (mant << e) <= (neg ? 0x80000000ULL : 0x7fffffffULL);
	} else {
		uint64_t m = mant >> -e;
		in_range = m <= (neg ? 0x80000000ULL : 0x7fffffffULL);
	}
	if (neg) {
		r = (uint32_t)(0u - r);
	}
	*exact = in_range && !frac;
	return r;
}

static bool
emulate_fcadd(arm_saved_state_t *ss, uint32_t w)
{
	thread_t th = current_thread();
	if (th->machine.uNeon == NULL) {
		return false;
	}
	arm_neon_saved_state64_t *ns = &th->machine.uNeon->ns_64;
	unsigned size = (w >> 22) & 3, q = (w >> 30) & 1, rot270 = (w >> 12) & 1;
	unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
	if (size == 0 || (size == 3 && !q)) {
		return false;
	}
	/* The helper's FADD/FSUB H needs FP16 arithmetic, which the Mongoose M3 lacks: at EL1 that
	 * is an undefined kernel instruction (a panic). Refuse, so sleh.c pins the thread to an
	 * A55, where the FCADD traps again and is emulated. */
	if (size == 1 && (__builtin_arm_rsr64("MIDR_EL1") >> 24) == 0x53) {
		return false;
	}
	unsigned esize = 8u << size;            /* 16, 32, 64 bits */
	unsigned lanes = (q ? 128 : 64) / esize;
	uint64_t mask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
	uint8_t n[16], m[16], d[16];
	memcpy(n, &ns->v.q[rn], 16);
	memcpy(m, &ns->v.q[rm], 16);
	memset(d, 0, 16);
	uint32_t fpsr = 0;
	for (unsigned i = 0; i < lanes; i += 2) {
		uint64_t nre = 0, nim = 0, mre = 0, mim = 0;
		memcpy(&nre, n + i * esize / 8, esize / 8);
		memcpy(&nim, n + (i + 1) * esize / 8, esize / 8);
		memcpy(&mre, m + i * esize / 8, esize / 8);
		memcpy(&mim, m + (i + 1) * esize / 8, esize / 8);
		/* rot 90: re = n.re - m.im, im = n.im + m.re; rot 270: re = n.re + m.im, im = n.im - m.re */
		uint64_t re = sweet_a64e_fp_addsub(nre, mim, (rot270 ? 0 : 1) << 4 | size, ns->fpcr, &fpsr);
		uint64_t im = sweet_a64e_fp_addsub(nim, mre, (rot270 ? 1 : 0) << 4 | size, ns->fpcr, &fpsr);
		re &= mask;
		im &= mask;
		memcpy(d + i * esize / 8, &re, esize / 8);
		memcpy(d + (i + 1) * esize / 8, &im, esize / 8);
	}
	memcpy(&ns->v.q[rd], d, 16);            /* 64-bit form clears the top half */
	ns->fpsr |= fpsr;
	(void)ss;
	return true;
}

/* ---- FEAT_DotProd: SDOT / UDOT (vector and by element). macOS 26's opendirectoryd and others use
 * them; on this SoC they trap on every core (2026-11-04: opendirectoryd SIGILL 0x6e819440 with no
 * M3 pin). Integer only: d.s[i] += sum of n.b[4i+j] * m.b[4i+j] (or m.b[4*index+j]), wrapping. */
static bool
emulate_dot(uint32_t w)
{
	bool vec = (w & 0xBFA0FC00) == 0x0E809400 || (w & 0xBFA0FC00) == 0x2E809400;   /* size 10 */
	bool elem = (w & 0xBFC0F400) == 0x0F80E000 || (w & 0xBFC0F400) == 0x2F80E000;
	if (!vec && !elem) {
		return false;
	}
	thread_t th = current_thread();
	if (th->machine.uNeon == NULL) {
		return false;
	}
	arm_neon_saved_state64_t *ns = &th->machine.uNeon->ns_64;
	unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, rd = w & 31, rn = (w >> 5) & 31;
	unsigned rm, index = 0;
	if (vec) {
		rm = (w >> 16) & 31;
	} else {
		rm = (w >> 16) & 31;                    /* M:Rm (size 10: M is bit 20, part of Rm) */
		index = ((w >> 11) & 1) << 1 | ((w >> 21) & 1);
	}
	uint8_t n[16], m[16];
	uint32_t d[4];
	memcpy(n, &ns->v.q[rn], 16);
	memcpy(m, &ns->v.q[rm], 16);
	memcpy(d, &ns->v.q[rd], 16);
	unsigned lanes = q ? 4 : 2;
	for (unsigned i = 0; i < lanes; i++) {
		int64_t acc = 0;
		for (unsigned j = 0; j < 4; j++) {
			unsigned mi = vec ? 4 * i + j : 4 * index + j;
			acc += u ? (int64_t)n[4 * i + j] * m[mi] : (int64_t)(int8_t)n[4 * i + j] * (int8_t)m[mi];
		}
		d[i] = (uint32_t)(d[i] + (uint32_t)acc);
	}
	if (!q) {
		d[2] = d[3] = 0;
	}
	memcpy(&ns->v.q[rd], d, 16);
	return true;
}

/* ---- FEAT_FHM: FMLAL, FMLAL2, FMLSL, FMLSL2 (vector and by element) ------------------------ */

/* sweet_a64e_fp.s: a + n * m in single precision, one rounding, under fpcr. */
extern uint32_t sweet_a64e_fp_fmadd_s(uint32_t a, uint32_t n, uint32_t m, uint32_t fpcr, uint32_t *fpsr);

#define SWEET_FPCR_FZ16 (1u << 19)
#define SWEET_FPCR_FZ   (1u << 24)
#define SWEET_FPCR_DN   (1u << 25)
#define SWEET_FPSR_IOC  (1u << 0)
#define SWEET_FPSR_IDC  (1u << 7)

static bool h_nan(uint16_t h)  { return (h & 0x7c00) == 0x7c00 && (h & 0x3ff) != 0; }
static bool h_snan(uint16_t h) { return h_nan(h) && (h & 0x200) == 0; }
static bool h_inf(uint16_t h)  { return (h & 0x7fff) == 0x7c00; }
static bool h_zero(uint16_t h) { return (h & 0x7fff) == 0; }
static bool s_nan(uint32_t s)  { return (s & 0x7f800000) == 0x7f800000 && (s & 0x7fffff) != 0; }
static bool s_snan(uint32_t s) { return s_nan(s) && (s & 0x400000) == 0; }

/* An IEEE half (not a NaN) as the single with the same value: every half is exact in single. */
static uint32_t
h_to_s(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
	if (exp == 0x1f) {
		return sign | 0x7f800000;               /* infinity */
	}
	if (exp == 0) {
		if (frac == 0) {
			return sign;
		}
		int e = -14;                            /* subnormal 0.frac * 2^-14: normalize */
		while ((frac & 0x400) == 0) {
			frac <<= 1;
			e--;
		}
		return sign | (uint32_t)(e + 127) << 23 | (frac & 0x3ff) << 13;
	}
	return sign | (exp - 15 + 127) << 23 | frac << 13;
}

/*
 * One lane of FMLAL/FMLSL: FPMulAddH(a, n, m) of the ARM ARM (n already negated for FMLSL).
 * The halves are unpacked with AHP = 0 and flushed by FZ16 (not FZ, and without IDC), the
 * addend by FZ (with IDC) even when a NaN wins; NaNs are picked as FPProcessNaNs3H does:
 * signalling first, in the order addend, n, m, and a half NaN is widened by FPConvertNaN. Without NaNs, n * m of two halves is exact in single precision
 * (22-bit significand, exponent -48..32), so a single FMADD of the widened values rounds exactly
 * once, like FPMulAddH, with the same infinity, zero-sign, FZ (addend and result) and flag rules.
 */
static uint32_t
fmlal_lane(uint32_t a, uint16_t n, uint16_t m, uint32_t fpcr, uint32_t *fpsr)
{
	if (fpcr & SWEET_FPCR_FZ16) {
		if ((n & 0x7c00) == 0) {
			n &= 0x8000;
		}
		if ((m & 0x7c00) == 0) {
			m &= 0x8000;
		}
	}
	if (s_nan(a) || h_nan(n) || h_nan(m)) {
		if ((fpcr & SWEET_FPCR_FZ) && (a & 0x7f800000) == 0 && (a & 0x7fffff) != 0) {
			*fpsr |= SWEET_FPSR_IDC;        /* FPUnpack flushed the addend before the NaN pick */
		}
		unsigned pick = s_snan(a) ? 0 : h_snan(n) ? 1 : h_snan(m) ? 2 : s_nan(a) ? 0 : h_nan(n) ? 1 : 2;
		bool snan = pick == 0 ? s_snan(a) : h_snan(pick == 1 ? n : m);
		if (snan) {
			*fpsr |= SWEET_FPSR_IOC;
		}
		/* addend a quiet NaN and n * m = 0 * inf: the default NaN, Invalid */
		if (!snan && pick == 0 && ((h_inf(n) && h_zero(m)) || (h_zero(n) && h_inf(m)))) {
			*fpsr |= SWEET_FPSR_IOC;
			return 0x7fc00000;
		}
		if (fpcr & SWEET_FPCR_DN) {
			return 0x7fc00000;
		}
		if (pick == 0) {
			return a | 0x400000;
		}
		uint16_t h = pick == 1 ? n : m;
		return (uint32_t)(h & 0x8000) << 16 | 0x7fc00000 | (uint32_t)(h & 0x1ff) << 13;
	}
	return sweet_a64e_fp_fmadd_s(a, h_to_s(n), h_to_s(m), fpcr, fpsr);
}

/*
 * FMLAL{2} / FMLSL{2} Vd.2S|4S, Vn.2H|4H, Vm.2H|4H   (vector)
 * FMLAL{2} / FMLSL{2} Vd.2S|4S, Vn.2H|4H, Vm.H[i]    (by element, Vm in V0-V15, i = H:L:M)
 * Each single lane d[e] += n[e] * m[e] (or m[i]), with n negated for FMLSL. The 2 forms take
 * the upper half of the Vn (and Vm vector) halfwords that the plain forms take the lower half of.
 */
static bool
emulate_fmlal(uint32_t w)
{
	thread_t th = current_thread();
	if (th->machine.uNeon == NULL) {
		return false;
	}
	arm_neon_saved_state64_t *ns = &th->machine.uNeon->ns_64;
	bool q = (w >> 30) & 1, upper = (w >> 29) & 1, elem = (w >> 24) & 1;
	unsigned rd = w & 31, rn = (w >> 5) & 31;
	unsigned lanes = q ? 4 : 2, base = upper ? lanes : 0;
	bool sub;
	uint16_t n[8], m[8];
	uint32_t d[4], r[4] = { 0, 0, 0, 0 };

	memcpy(n, &ns->v.q[rn], 16);
	memcpy(d, &ns->v.q[rd], 16);
	if (elem) {
		unsigned rm = (w >> 16) & 15;
		unsigned idx = ((w >> 11) & 1) << 2 | ((w >> 21) & 1) << 1 | ((w >> 20) & 1);
		uint16_t v[8];
		memcpy(v, &ns->v.q[rm], 16);
		for (unsigned e = 0; e < 8; e++) {
			m[e] = v[idx];
		}
		sub = (w >> 14) & 1;
	} else {
		memcpy(m, &ns->v.q[(w >> 16) & 31], 16);
		sub = (w >> 23) & 1;
	}
	uint32_t fpsr = 0;
	for (unsigned e = 0; e < lanes; e++) {
		uint16_t a = n[base + e];
		if (sub) {
			a ^= 0x8000;                    /* FPNeg, before the NaN checks (FEAT_AFP absent) */
		}
		r[e] = fmlal_lane(d[e], a, elem ? m[0] : m[base + e], ns->fpcr, &fpsr);
	}
	memcpy(&ns->v.q[rd], r, 16);            /* the 2S form clears the top half */
	ns->fpsr |= fpsr;
	return true;
}


/* ---- FEAT_SHA3 (EOR3, BCAX, RAX1, XAR) and FEAT_SHA512 ---------------------------------------- */

static uint64_t
ror64(uint64_t x, unsigned r)
{
	return r == 0 ? x : (x >> r) | (x << (64 - r));
}

/*
 * Integer operations on whole vectors, so they run on either core. EOR3/BCAX/XAR also come
 * from the compiler (three-way XOR, and-not, xor-rotate patterns with +sha3: libsystem_m,
 * Foundation, CoreGraphics, BoringSSL), not only from crypto code.
 */
static bool
emulate_sha3_sha512(uint32_t w)
{
	thread_t th = current_thread();
	if (th->machine.uNeon == NULL) {
		return false;
	}
	arm_neon_saved_state64_t *ns = &th->machine.uNeon->ns_64;
	unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31, ra = (w >> 10) & 31;
	uint64_t n[2], m[2], d[2], a[2], r[2];
	memcpy(n, &ns->v.q[rn], 16);
	memcpy(m, &ns->v.q[rm], 16);
	memcpy(d, &ns->v.q[rd], 16);
	memcpy(a, &ns->v.q[ra], 16);
#define SHA512_S0(x)    (ror64(x, 28) ^ ror64(x, 34) ^ ror64(x, 39))
#define SHA512_S1(x)    (ror64(x, 14) ^ ror64(x, 18) ^ ror64(x, 41))
#define SHA512_s0(x)    (ror64(x, 1) ^ ror64(x, 8) ^ ((x) >> 7))
#define SHA512_s1(x)    (ror64(x, 19) ^ ror64(x, 61) ^ ((x) >> 6))
#define SHA512_CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA512_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
	if ((w & 0xFFE08000) == 0xCE000000) {                   /* EOR3 Vd.16B, Vn, Vm, Va */
		r[0] = n[0] ^ m[0] ^ a[0];
		r[1] = n[1] ^ m[1] ^ a[1];
	} else if ((w & 0xFFE08000) == 0xCE200000) {            /* BCAX Vd.16B, Vn, Vm, Va: n ^ (m & ~a) */
		r[0] = n[0] ^ (m[0] & ~a[0]);
		r[1] = n[1] ^ (m[1] & ~a[1]);
	} else if ((w & 0xFFE0FC00) == 0xCE608C00) {            /* RAX1 Vd.2D, Vn, Vm: n ^ rol(m, 1) */
		r[0] = n[0] ^ ror64(m[0], 63);
		r[1] = n[1] ^ ror64(m[1], 63);
	} else if ((w & 0xFFE00000) == 0xCE800000) {            /* XAR Vd.2D, Vn, Vm, #imm6: ror(n ^ m) */
		unsigned imm = (w >> 10) & 63;
		r[0] = ror64(n[0] ^ m[0], imm);
		r[1] = ror64(n[1] ^ m[1], imm);
	} else if ((w & 0xFFE0FC00) == 0xCE608000) {            /* SHA512H Qd, Qn, Vm.2D */
		r[1] = d[1] + SHA512_S1(m[1]) + SHA512_CH(m[1], n[0], n[1]);
		uint64_t t = r[1] + m[0];
		r[0] = d[0] + SHA512_S1(t) + SHA512_CH(t, m[1], n[0]);
	} else if ((w & 0xFFE0FC00) == 0xCE608400) {            /* SHA512H2 Qd, Qn, Vm.2D */
		r[1] = d[1] + SHA512_S0(m[0]) + SHA512_MAJ(n[0], m[1], m[0]);
		r[0] = d[0] + SHA512_S0(r[1]) + SHA512_MAJ(r[1], m[0], m[1]);
	} else if ((w & 0xFFE0FC00) == 0xCE608800) {            /* SHA512SU1 Vd.2D, Vn.2D, Vm.2D */
		r[0] = d[0] + SHA512_s1(n[0]) + m[0];
		r[1] = d[1] + SHA512_s1(n[1]) + m[1];
	} else if ((w & 0xFFFFFC00) == 0xCEC08000) {            /* SHA512SU0 Vd.2D, Vn.2D */
		r[0] = d[0] + SHA512_s0(d[1]);
		r[1] = d[1] + SHA512_s0(n[0]);
	} else {
		return false;
	}
#undef SHA512_S0
#undef SHA512_S1
#undef SHA512_s0
#undef SHA512_s1
#undef SHA512_CH
#undef SHA512_MAJ
	memcpy(&ns->v.q[rd], r, 16);
	return true;
}

/* ---- LSE and LRCPC for the Mongoose M3 ------------------------------------------------------ */

extern int sweet_copy_cas64(user_addr_t user_addr, uint64_t expected, uint64_t desired, uint64_t *old);
extern int sweet_copy_cas128(user_addr_t user_addr, const uint64_t expected[2], const uint64_t desired[2],
    uint64_t old[2]);
extern int copyin_atomic64(const user_addr_t user_addr, uint64_t *kernel_addr);

uint64_t sweet_lse_counts[4];   /* CAS, CASP, LD<op>/SWP, LDAPR */

/*
 * ios27-opt: instructions the A55 implements (ARMv8.2 FP16, DotProd, RDM) and the Mongoose M3 lacks. If one traps on an
 * M3 and the handler finds itself on an A55 (sleh_synchronous enables interrupts, so the thread can be preempted and
 * migrated before the MIDR checks), retrying it is right: the A55 runs it. Without this the late MIDR check says "not
 * an M3" and the process gets a SIGILL (iconservicesagent, CoreGraphics RGBAf16_mark_inner FCMP H).
 */
uint64_t sweet_a55_retries;
bool sweet_a55_native_m3_lacks(uint32_t w);
bool sweet_a55_retry_ok(uint32_t w, thread_t th, uint64_t pc);
bool
sweet_a55_native_m3_lacks(uint32_t w)
{
	unsigned ftype = (w >> 22) & 3;
	if ((w & 0x5F200000) == 0x1E200000 && ftype == 3) {     /* scalar FP data-processing, half */
		return true;
	}
	if ((w & 0x5F000000) == 0x1F000000 && ftype == 3) {     /* FP 3-source, half */
		return true;
	}
	if ((w & 0x9F60C400) == 0x0E400400 || (w & 0xDF60C400) == 0x5E400400) {        /* (scalar) three same FP16 */
		return true;
	}
	if ((w & 0x9F7E0C00) == 0x0E780800 || (w & 0xDF7E0C00) == 0x5E780800) {        /* (scalar) two-reg misc FP16 */
		return true;
	}
	if ((w & 0x9FC00400) == 0x0F000000 || (w & 0xDFC00400) == 0x5F000000) {        /* by element, size 00 = half */
		unsigned op = (w >> 12) & 0xF;
		if (op == 1 || op == 5 || op == 9) {
			return true;
		}
	}
	if ((w & 0x9FBFFC00) == 0x0E30C800 || (w & 0x9FBFFC00) == 0x0E30F800 ||       /* FMAXNMV/FMINNMV/FMAXV/FMINV .H */
	    (w & 0xFFBE0C00) == 0x5E300800 ||                                            /* scalar pairwise .H */
	    (w & 0x9FF8FC00) == 0x0F00FC00) {                                            /* FMOV Vd.nH, #imm */
		return true;
	}
	if ((w & 0xBF20FC00) == 0x0E009400 || (w & 0xBF00F400) == 0x0F00E000) {        /* SDOT/UDOT */
		return true;
	}
	if ((w & 0xBF20F400) == 0x2E008400 || (w & 0xFF20F400) == 0x7E008400 ||       /* SQRDMLAH/SQRDMLSH */
	    (w & 0xBF00D400) == 0x2F00D000 || (w & 0xFF00D400) == 0x7F00D000) {
		return true;
	}
	return false;
}

/*
 * The retry above is only right for an instruction the A55 really runs. The patterns are classes, and an unallocated encoding
 * inside one would trap on the A55 too and be retried for ever: at most 4 retries in a row of the same pc of the same thread
 * (a small direct-mapped table, racy on purpose: a lost entry only means one more try), then the SIGILL goes out as before.
 */
bool
sweet_a55_retry_ok(uint32_t w, thread_t th, uint64_t pc)
{
	static struct { thread_t th; uint64_t pc; uint32_t n; } tab[64];
	if (!sweet_a55_native_m3_lacks(w)) {
		return false;
	}
	unsigned i = (unsigned)(((uintptr_t)th >> 6) ^ (pc >> 2)) % 64;
	if (tab[i].th == th && tab[i].pc == pc) {
		if (tab[i].n >= 4) {
			return false;
		}
		tab[i].n++;
	} else {
		tab[i].th = th;
		tab[i].pc = pc;
		tab[i].n = 1;
	}
	sweet_a55_retries++;
	return true;
}

uint64_t sweet_m3_pins;          /* threads pinned to an A55 (sleh.c) */
uint64_t sweet_m3_unpins;        /* soft pins released (sweetm3unpin) */

/*
 * Per-class residual counters: each time an M3 still runs an instruction it architecturally lacks
 * (emulated here, pinned to an A55, or A55-retried in sleh.c), classify the word and bump a bucket.
 * After the ahead-of-time rewrite every bucket should be 0 and "STARLTE M3 RESIDUAL" should vanish.
 * Racy increments are fine (diagnostic only).
 */
enum { SM3R_LDOP, SM3R_CAS, SM3R_CASP, SM3R_SWP, SM3R_LDAPR, SM3R_FP16, SM3R_FCADD, SM3R_FCMLA,
       SM3R_DOT, SM3R_RDM, SM3R_I8MM, SM3R_BF16, SM3R_SHA3, SM3R_JSCVT, SM3R_FHM, SM3R_OTHER, SM3R_N };
uint64_t sweet_m3_residual[SM3R_N];
const char * const sweet_m3_residual_name[SM3R_N] = {
    "ldop", "cas", "casp", "swp", "ldapr", "fp16", "fcadd", "fcmla",
    "dot", "rdm", "i8mm", "bf16", "sha3", "jscvt", "fhm", "other" };

static int
sweet_m3_classify(uint32_t w)
{
	if ((w & 0x3FA07C00) == 0x08A07C00) { return SM3R_CAS; }
	if (((w & 0xBFA07C00) == 0x08207C00) && (((w >> 10) & 31) == 31)) { return SM3R_CASP; }
	if ((w & 0x3F200C00) == 0x38200000) {
		unsigned o3 = (w >> 15) & 1, opc = (w >> 12) & 7;
		if (o3 == 0) { return SM3R_LDOP; }
		if (opc == 0) { return SM3R_SWP; }
		if (opc == 4 && (((w >> 16) & 31) == 31) && (((w >> 22) & 3) == 2)) { return SM3R_LDAPR; }
	}
	if ((w & 0x3FFFFC00) == 0x38BFC000) { return SM3R_LDAPR; }
	if ((w & 0xFFFFFC00) == 0x1E7E0000) { return SM3R_JSCVT; }
	if ((w & 0xBF20EC00) == 0x2E00E400) { return SM3R_FCADD; }
	if (((w & 0xBF20E400) == 0x2E00C400) || ((w & 0xBF009400) == 0x2F001000)) { return SM3R_FCMLA; }
	if (((w & 0xBF20FC00) == 0x0E009400) || ((w & 0xBF00F400) == 0x0F00E000)) { return SM3R_DOT; }
	if (((w & 0xBF20F400) == 0x2E008400) || ((w & 0xFF20F400) == 0x7E008400) ||
	    ((w & 0xBF00D400) == 0x2F00D000) || ((w & 0xFF00D400) == 0x7F00D000)) { return SM3R_RDM; }
	if (((w & 0xBF20FC00) == 0x0E809C00) || ((w & 0xBFE0FC00) == 0x4E80A400) ||
	    ((w & 0xBF20FC00) == 0x0E80A400)) { return SM3R_I8MM; }
	if (((w & 0x9FA0FC00) == 0x0E20EC00) || ((w & 0x9FA0FC00) == 0x2E20CC00)) { return SM3R_FHM; }
	if (((w & 0xFFE08000) == 0xCE000000) || ((w & 0xFFE0FC00) == 0xCE608400) ||
	    ((w & 0xFFFFFC00) == 0xCEC08000) || ((w & 0xFFE0FC00) == 0xCE800000) ||
	    ((w & 0xFFE0FC00) == 0xCE808400)) { return SM3R_SHA3; }
	if ((w & 0xFFFFFC00) == 0x1E634000) { return SM3R_BF16; }
	{
		unsigned ftype = (w >> 22) & 3;
		if (((w & 0x5F200000) == 0x1E200000) && ftype == 3) { return SM3R_FP16; }
		if (((w & 0x5F000000) == 0x1F000000) && ftype == 3) { return SM3R_FP16; }
		if (((w & 0x9F60C400) == 0x0E400400) || ((w & 0xDF60C400) == 0x5E400400)) { return SM3R_FP16; }
		if (((w & 0x9F7E0C00) == 0x0E780800) || ((w & 0xDF7E0C00) == 0x5E780800)) { return SM3R_FP16; }
	}
	return SM3R_OTHER;
}

void sweet_m3_residual_note(uint32_t w);
void
sweet_m3_residual_note(uint32_t w)
{
	sweet_m3_residual[sweet_m3_classify(w)]++;
}


/*
 * Soft M3 pins (boot-arg sweetm3unpin=N, milliseconds; default 0 = off = pins are permanent):
 * sleh.c binds a thread that ran an instruction the M3 lacks (FP16, DotProd, RDM) to an A55 and
 * notes it here; at the thread's first synchronous exception from EL0 (syscall, fault, trap) at
 * least N ms later the binding is dropped, so it can run on the big cores again (and is pinned
 * again, one trap later, if it meets such an instruction on an M3 once more). Pins that do not fit
 * the table stay permanent, as before. Entries carry the thread id, so a recycled thread_t that
 * matches the pointer but not the id is never unbound.
 */
#define SWEET_PIN_SLOTS 256
static struct { thread_t th; uint64_t tid; uint64_t until; } sweet_pin_slots[SWEET_PIN_SLOTS];
static uint64_t sweet_unpin_abs;
static int sweet_unpin_state;            /* 0 = boot-arg not read yet, 1 = off, 2 = on */

static bool
sweet_unpin_enabled(void)
{
	if (sweet_unpin_state == 0) {
		uint32_t ms = 0;
		PE_parse_boot_argn("sweetm3unpin", &ms, sizeof(ms));
		if (ms != 0) {
			nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &sweet_unpin_abs);
		}
		sweet_unpin_state = ms != 0 ? 2 : 1;
	}
	return sweet_unpin_state == 2;
}

static unsigned
sweet_pin_hash(thread_t th)
{
	return (unsigned)((((uintptr_t)th) >> 4) * 0x9E3779B1u) % SWEET_PIN_SLOTS;
}

void sweet_m3_pin_note(thread_t th);
void sweet_m3_unpin_check(thread_t th);

void
sweet_m3_pin_note(thread_t th)
{
	if (!sweet_unpin_enabled()) {
		return;
	}
	unsigned h = sweet_pin_hash(th);
	uint64_t tid = thread_tid(th);
	for (unsigned i = 0; i < 16; i++) {
		unsigned k = (h + i) % SWEET_PIN_SLOTS;
		thread_t cur = os_atomic_load(&sweet_pin_slots[k].th, acquire);
		if (cur == th || (cur == THREAD_NULL &&
		    os_atomic_cmpxchg(&sweet_pin_slots[k].th, THREAD_NULL, th, acq_rel))) {
			sweet_pin_slots[k].tid = tid;
			sweet_pin_slots[k].until = mach_absolute_time() + sweet_unpin_abs;
			return;
		}
	}
}

void
sweet_m3_unpin_check(thread_t th)
{
	if (sweet_unpin_state != 2) {
		return;
	}
	unsigned h = sweet_pin_hash(th);
	for (unsigned i = 0; i < 16; i++) {
		unsigned k = (h + i) % SWEET_PIN_SLOTS;
		if (os_atomic_load(&sweet_pin_slots[k].th, acquire) != th) {
			continue;
		}
		if (sweet_pin_slots[k].tid != thread_tid(th)) {
			/* a recycled thread_t: the old thread's slot is stale; free it, leave the binding */
			os_atomic_store(&sweet_pin_slots[k].th, THREAD_NULL, release);
			return;
		}
		if (mach_absolute_time() >= sweet_pin_slots[k].until) {
			os_atomic_store(&sweet_pin_slots[k].th, THREAD_NULL, release);
			thread_bind(PROCESSOR_NULL);
			sweet_m3_unpins++;
		}
		return;
	}
}

/* Where the traps come from: an open table of user PCs (racy counts are fine), read and reset by the
 * STARLTE TOP report (bsd/dev/starlte_smp.c). Shared-cache PCs are the same in every process. */
#define SWEET_LSE_SITES 512
struct sweet_lse_site { uint64_t pc, count; };
struct sweet_lse_site sweet_lse_sites[SWEET_LSE_SITES];

static void
lse_site(uint64_t pc)
{
	unsigned h = (unsigned)((pc >> 2) * 0x9E3779B1u) % SWEET_LSE_SITES;
	for (unsigned i = 0; i < 8; i++) {
		struct sweet_lse_site *e = &sweet_lse_sites[(h + i) % SWEET_LSE_SITES];
		if (e->pc == pc || e->pc == 0) {
			e->pc = pc;
			e->count++;
			return;
		}
	}
}

static int64_t
sext(uint64_t v, unsigned bits)
{
	return bits == 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* An atomic op on the `bytes`-wide field at addr: *old = the field before; false = fault/misaligned. */
static bool
lse_rmw(user_addr_t addr, unsigned bytes, unsigned op, uint64_t operand, uint64_t compare, uint64_t *old)
{
	if (addr & (bytes - 1)) {
		return false;
	}
	unsigned bits = bytes * 8;
	user_addr_t base = addr & ~7ULL;
	unsigned shift = (unsigned)(addr & 7) * 8;
	uint64_t mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1) << shift;
	uint64_t word;
	if (copyin_atomic64(base, &word) != 0) {
		return false;
	}
	for (int tries = 0; tries < 100000; tries++) {
		uint64_t field = (word & mask) >> shift, next;
		switch (op) {
		case 0: next = field + operand; break;                                   /* ADD */
		case 1: next = field & ~operand; break;                                  /* CLR */
		case 2: next = field ^ operand; break;                                   /* EOR */
		case 3: next = field | operand; break;                                   /* SET */
		case 4: next = sext(field, bits) > sext(operand, bits) ? field : operand; break; /* SMAX */
		case 5: next = sext(field, bits) < sext(operand, bits) ? field : operand; break; /* SMIN */
		case 6: next = field > (operand & (mask >> shift)) ? field : operand; break;     /* UMAX */
		case 7: next = field < (operand & (mask >> shift)) ? field : operand; break;     /* UMIN */
		case 8: next = operand; break;                                           /* SWP */
		default:                                                                 /* CAS */
			if (field != (compare & (mask >> shift))) {
				/* no store, but the same ordering as a successful one */
				__asm__ volatile ("dmb ish" ::: "memory");
				*old = field;
				return true;
			}
			next = operand;
			break;
		}
		uint64_t desired = (word & ~mask) | ((next << shift) & mask), seen = 0;
		if (sweet_copy_cas64(base, word, desired, &seen) != 0) {
			return false;
		}
		if (seen == word) {
			*old = field;
			return true;
		}
		word = seen;
	}
	return false;
}

static bool
sweet_lse_emulate(arm_saved_state_t *ss, uint32_t w)
{
	uint64_t pc = get_saved_state_pc(ss);
	unsigned rt = w & 31, rn = (w >> 5) & 31, rs = (w >> 16) & 31;
	if ((w & 0x3fa07c00) == 0x08a07c00 || (w & 0xbfa07c00) == 0x08207c00 || (w & 0x3f200c00) == 0x38200000) {
		lse_site(pc);
	}
	user_addr_t addr = (user_addr_t)reg_get(ss, rn, true);

	if ((w & 0x3fa07c00) == 0x08a07c00) {                   /* CAS{A,L,AL}{B,H,,} */
		unsigned bytes = 1u << (w >> 30);
		uint64_t old;
		if (bytes == 8) {                                   /* the common case: one CAS on the word itself */
			if ((addr & 7) || sweet_copy_cas64(addr, reg_get(ss, rs, false), reg_get(ss, rt, false), &old) != 0) {
				return false;
			}
			reg_set(ss, rs, false, old);
			sweet_lse_counts[0]++;
			set_saved_state_pc(ss, pc + 4);
			return true;
		}
		if (!lse_rmw(addr, bytes, 9, reg_get(ss, rt, false), reg_get(ss, rs, false), &old)) {
			return false;
		}
		reg_set(ss, rs, false, old);
		sweet_lse_counts[0]++;
	} else if ((w & 0xbfa07c00) == 0x08207c00) {            /* CASP{A,L,AL} Ws/Xs pairs */
		if ((rs & 1) || (rt & 1)) {
			return false;
		}
		bool x = (w >> 30) & 1;
		if (!x) {
			uint64_t cmp = (reg_get(ss, rs, false) & 0xffffffffULL) | reg_get(ss, rs + 1, false) << 32;
			uint64_t nv = (reg_get(ss, rt, false) & 0xffffffffULL) | reg_get(ss, rt + 1, false) << 32;
			uint64_t old;
			if (!lse_rmw(addr, 8, 9, nv, cmp, &old)) {
				return false;
			}
			reg_set(ss, rs, false, old & 0xffffffffULL);
			reg_set(ss, rs + 1, false, old >> 32);
		} else {
			uint64_t cmp[2] = { reg_get(ss, rs, false), reg_get(ss, rs + 1, false) };
			uint64_t nv[2] = { reg_get(ss, rt, false), reg_get(ss, rt + 1, false) };
			uint64_t old[2];
			if (sweet_copy_cas128(addr, cmp, nv, old) != 0) {
				return false;
			}
			reg_set(ss, rs, false, old[0]);
			reg_set(ss, rs + 1, false, old[1]);
		}
		sweet_lse_counts[1]++;
	} else if ((w & 0x3f200c00) == 0x38200000) {            /* LD<op>, SWP, LDAPR */
		unsigned bytes = 1u << (w >> 30), o3 = (w >> 15) & 1, opc = (w >> 12) & 7;
		uint64_t old;
		if (o3 && opc == 4 && rs == 31) {                   /* LDAPR{B,H,,} */
			uint64_t word;
			if ((addr & (bytes - 1)) || copyin_atomic64(addr & ~7ULL, &word) != 0) {
				return false;
			}
			__asm__ volatile ("dmb ish" ::: "memory");
			unsigned shift = (unsigned)(addr & 7) * 8;
			old = bytes == 8 ? word : (word >> shift) & ((1ULL << (bytes * 8)) - 1);
			reg_set(ss, rt, false, old);
			sweet_lse_counts[3]++;
			set_saved_state_pc(ss, pc + 4);
			return true;
		}
		if (o3 && opc != 0) {
			return false;
		}
		if (!lse_rmw(addr, bytes, o3 ? 8 : opc, reg_get(ss, rs, false), 0, &old)) {
			return false;
		}
		reg_set(ss, rt, false, old);                        /* Rt = 31: ST<op>, the result goes to XZR */
		sweet_lse_counts[2]++;
	} else {
		return false;
	}
	set_saved_state_pc(ss, pc + 4);
	return true;
}

/* ios27-opt: condition codes as the architecture's ConditionHolds() */
static bool
fp16_cond_holds(uint32_t cond, uint32_t cpsr)
{
	bool n = (cpsr >> 31) & 1, z = (cpsr >> 30) & 1, c = (cpsr >> 29) & 1, v = (cpsr >> 28) & 1, r;
	switch (cond >> 1) {
	case 0: r = z; break;
	case 1: r = c; break;
	case 2: r = n; break;
	case 3: r = v; break;
	case 4: r = c && !z; break;
	case 5: r = n == v; break;
	case 6: r = n == v && !z; break;
	default: r = true; break;
	}
	return ((cond & 1) && cond != 15) ? !r : r;
}

static uint32_t
fp16_compare_nzcv(uint16_t a, uint16_t b)
{
	bool an = (a & 0x7c00) == 0x7c00 && (a & 0x3ff), bn = (b & 0x7c00) == 0x7c00 && (b & 0x3ff);
	if (an || bn) {
		return 0x3;                                     /* unordered: C V */
	}
	int ka = (a & 0x8000) ? -(int)(a & 0x7fff) : (int)(a & 0x7fff);
	int kb = (b & 0x8000) ? -(int)(b & 0x7fff) : (int)(b & 0x7fff);
	return ka == kb ? 0x6 : ka < kb ? 0x8 : 0x2;
}

static bool
emulate_fp16_m3_more(arm_saved_state_t *ss, arm_neon_saved_state64_t *ns, uint32_t w)
{
	unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
	uint32_t cpsr = get_saved_state_cpsr(ss);
	if ((w & 0xFFE00C00) == 0x1EE00400) {                   /* FCCMP/FCCMPE Hn, Hm, #nzcv, cond */
		uint32_t nzcv = w & 0xf;
		if (fp16_cond_holds((w >> 12) & 0xf, cpsr)) {
			uint16_t a, b;
			memcpy(&a, &ns->v.q[rn], 2);
			memcpy(&b, &ns->v.q[rm], 2);
			nzcv = fp16_compare_nzcv(a, b);
		}
		set_saved_state_cpsr(ss, (cpsr & ~PSR64_NZCV_MASK) | (nzcv << 28));
		return true;
	}
	if ((w & 0xFFE00C00) == 0x1EE00C00) {                   /* FCSEL Hd, Hn, Hm, cond */
		uint16_t h;
		memcpy(&h, &ns->v.q[fp16_cond_holds((w >> 12) & 0xf, cpsr) ? rn : rm], 2);
		uint8_t d[16];
		memset(d, 0, 16);
		memcpy(d, &h, 2);
		memcpy(&ns->v.q[rd], d, 16);
		return true;
	}
	if ((w & 0xFFFE7C00) == 0x1EE04000 && ((w >> 15) & 3) != 3) {  /* FMOV/FABS/FNEG Hd, Hn */
		uint16_t h;
		memcpy(&h, &ns->v.q[rn], 2);
		unsigned op = (w >> 15) & 3;
		h = op == 1 ? (uint16_t)(h & 0x7fff) : op == 2 ? (uint16_t)(h ^ 0x8000) : h;
		uint8_t d[16];
		memset(d, 0, 16);
		memcpy(d, &h, 2);
		memcpy(&ns->v.q[rd], d, 16);
		return true;
	}
	if ((w & 0x9FFFFC00) == 0x0EF8F800) {                   /* FABS / FNEG Vd.4H|8H, Vn */
		bool q = (w >> 30) & 1, neg = (w >> 29) & 1;
		uint16_t lanes[8];
		memcpy(lanes, &ns->v.q[rn], 16);
		for (unsigned i = 0; i < 8; i++) {
			lanes[i] = (i >= 4 && !q) ? 0 : neg ? (uint16_t)(lanes[i] ^ 0x8000) : (uint16_t)(lanes[i] & 0x7fff);
		}
		memcpy(&ns->v.q[rd], lanes, 16);
		return true;
	}
	return false;
}

/*
 * The few scalar FP16 instructions iOS 27's UI code runs all the time (FMOV H #imm, FMOV between H and W/X,
 * FCMP/FCMPE H), emulated on the Mongoose M3 instead of pinning the thread to an A55 (sleh.c) (sweetspeed&2).
 * Exact: no arithmetic, only moves and compares. Anything else still pins.
 */
static bool
emulate_fp16_m3(arm_saved_state_t *ss, uint32_t w)
{
	thread_t th = current_thread();
	if (th->machine.uNeon == NULL) {
		return false;
	}
	arm_neon_saved_state64_t *ns = &th->machine.uNeon->ns_64;
	unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
	if ((w & 0xFFE01FE0) == 0x1EE01000) {                   /* FMOV Hd, #imm */
		unsigned imm8 = (w >> 13) & 0xff, b6 = (imm8 >> 6) & 1;
		uint16_t h = (uint16_t)(((imm8 >> 7) << 15) |
		    ((((b6 ^ 1) << 4) | (b6 ? 0xc : 0) | ((imm8 >> 4) & 3)) << 10) | ((imm8 & 0xf) << 6));
		uint8_t d[16];
		memset(d, 0, 16);
		memcpy(d, &h, 2);
		memcpy(&ns->v.q[rd], d, 16);
		return true;
	}
	if ((w & 0x7FFFFC00) == 0x1EE60000) {                   /* FMOV Wd|Xd, Hn */
		uint16_t h;
		memcpy(&h, &ns->v.q[rn], 2);
		reg_set(ss, rd, false, h);
		return true;
	}
	if ((w & 0x7FFFFC00) == 0x1EE70000) {                   /* FMOV Hd, Wn|Xn */
		uint16_t h = (uint16_t)reg_get(ss, rn, false);
		uint8_t d[16];
		memset(d, 0, 16);
		memcpy(d, &h, 2);
		memcpy(&ns->v.q[rd], d, 16);
		return true;
	}
	if ((w & 0xFFE0FC07) == 0x1EE02000) {                   /* FCMP/FCMPE Hn, Hm|#0.0 */
		uint16_t a, b = 0;
		memcpy(&a, &ns->v.q[rn], 2);
		if (w & 0x08) {
			if (rm != 0) {
				return false;
			}
		} else {
			memcpy(&b, &ns->v.q[rm], 2);
		}
		uint32_t nzcv;
		bool an = (a & 0x7c00) == 0x7c00 && (a & 0x3ff), bn = (b & 0x7c00) == 0x7c00 && (b & 0x3ff);
		if (an || bn) {
			nzcv = 0x3;                             /* unordered: C V */
		} else {
			int ka = (a & 0x8000) ? -(int)(a & 0x7fff) : (int)(a & 0x7fff);
			int kb = (b & 0x8000) ? -(int)(b & 0x7fff) : (int)(b & 0x7fff);
			nzcv = ka == kb ? 0x6 : ka < kb ? 0x8 : 0x2;
		}
		uint32_t cpsr = get_saved_state_cpsr(ss) & ~PSR64_NZCV_MASK;
		set_saved_state_cpsr(ss, cpsr | (nzcv << 28));
		return true;
	}
	return emulate_fp16_m3_more(ss, ns, w);
}

static bool sweet_a64e_emulate_inner(arm_saved_state_t *ss, uint32_t w);
extern uint64_t sweet_emu_n[2], sweet_emu_abs[2];
bool
sweet_a64e_emulate(arm_saved_state_t *ss, uint32_t w)
{
	/* time spent emulating, LSE/LRCPC vs the rest (the SWEET PROF emu line): only while the profiler is on (sweetspeed&16384) */
	extern uint32_t sweet_prof_on;
	if (__probable(!os_atomic_load(&sweet_prof_on, relaxed))) {
		return sweet_a64e_emulate_inner(ss, w);
	}
	uint64_t t0 = mach_absolute_time();
	bool ok = sweet_a64e_emulate_inner(ss, w);
	unsigned c = ((w & 0x3f200c00) == 0x38200000 || (w & 0x3fa07c00) == 0x08a07c00 || (w & 0xbfa07c00) == 0x08207c00) ? 0 : 1;
	os_atomic_inc(&sweet_emu_n[c], relaxed);
	os_atomic_add(&sweet_emu_abs[c], mach_absolute_time() - t0, relaxed);
	return ok;
}

static bool
sweet_a64e_emulate_inner(arm_saved_state_t *ss, uint32_t w)
{
	/* LSE/LRCPC (the M3s, millions a second) first: they are not arm64e and not counted per pid */
	if (((w & 0x3f200c00) == 0x38200000 || (w & 0x3fa07c00) == 0x08a07c00 || (w & 0xbfa07c00) == 0x08207c00) &&
	    sweet_lse_emulate(ss, w)) {
		return true;
	}
	sweet_a64e_count_pid();
	uint64_t pc = get_saved_state_pc(ss);
	a64e_site(pc, w);

	if ((w & 0xFFFFFBFF) == 0xD65F0BFF) {                   /* RETAA, RETAB */
		set_saved_state_pc(ss, reg_get(ss, 30, false));
		sweet_a64e_counts[0]++;
		return true;
	}
	if ((w & 0xFE9FF800) == 0xD61F0800) {                   /* BRA*, BLRA* (Z and register forms) */
		unsigned op = (w >> 21) & 3;
		uint64_t target = reg_get(ss, (w >> 5) & 31, false);
		if (op == 1) {
			reg_set(ss, 30, false, pc + 4);
		} else if (op != 0) {
			return false;
		}
		set_saved_state_pc(ss, target);
		sweet_a64e_counts[1]++;
		return true;
	}
	if ((w & 0xFF200400) == 0xF8200400) {                   /* LDRAA, LDRAB */
		unsigned rt = w & 31, rn = (w >> 5) & 31;
		int64_t imm = (int64_t)(((w >> 12) & 0x1ff) | ((w >> 22) & 1) << 9);
		if (imm & 0x200) {
			imm -= 0x400;
		}
		uint64_t addr = reg_get(ss, rn, true) + (uint64_t)(imm * 8);
		uint64_t val;
		if (copyin((user_addr_t)addr, &val, sizeof(val)) != 0) {
			return false;                           /* let the fault become a crash */
		}
		if (w & (1u << 11)) {
			reg_set(ss, rn, true, addr);
		}
		reg_set(ss, rt, false, val);
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[2]++;
		return true;
	}
	if ((w & 0xFFFF8000) == 0xDAC10000 && ((w >> 10) & 0x3f) <= 17) {
		/* PACIA..AUTDB, PACIZA..AUTDZB, XPACI, XPACD: no signature to add or check */
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[3]++;
		return true;
	}
	if ((w & 0xFFE0FC00) == 0x9AC03000) {                   /* PACGA Xd, Xn, Xm|SP */
		uint64_t v = pacga_hash(reg_get(ss, (w >> 5) & 31, false), reg_get(ss, (w >> 16) & 31, true));
		reg_set(ss, w & 31, false, v);
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[4]++;
		return true;
	}
	if ((w & 0xFFFFFC00) == 0x1E7E0000) {                   /* FJCVTZS Wd, Dn */
		thread_t th = current_thread();
		if (th->machine.uNeon == NULL) {
			return false;
		}
		uint64_t bits;
		memcpy(&bits, &th->machine.uNeon->ns_64.v.q[(w >> 5) & 31], sizeof(bits));
		bool exact;
		uint32_t r = fjcvtzs(bits, &exact);
		reg_set(ss, w & 31, false, r);
		uint32_t cpsr = get_saved_state_cpsr(ss) & ~PSR64_NZCV_MASK;
		set_saved_state_cpsr(ss, cpsr | (exact ? PSR64_Z : 0));
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[5]++;
		return true;
	}
	if (emulate_dot(w)) {                                   /* SDOT / UDOT */
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	if ((sweet_speed_flags() & SWEET_SPEED_FP16) && (__builtin_arm_rsr64("MIDR_EL1") >> 24) == 0x53 &&
	    emulate_fp16_m3(ss, w)) {
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	if ((w & 0xBF20EC00) == 0x2E00E400) {                   /* FCADD (vector) */
		if (!emulate_fcadd(ss, w)) {
			return false;
		}
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[6]++;
		return true;
	}
	/* FEAT_DIT (ARMv8.4; macOS's corecrypto sets and restores it around every operation): data-
	 * independent timing is a hint, so MSR DIT is dropped and MRS DIT reads 0. */
	if ((w & 0xFFFFF0FF) == 0xD503405F ||                   /* MSR DIT, #imm */
	    (w & 0xFFFFFFE0) == 0xD51B42A0) {                   /* MSR DIT, Xt */
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	if ((w & 0xFFFFFFE0) == 0xD53B42A0) {                   /* MRS Xt, DIT */
		reg_set(ss, w & 31, false, 0);
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	/* FEAT_LRCPC2 (ARMv8.4): STLUR, LDAPUR, LDAPURS* (unscaled signed 9-bit offset). One access
	 * through copyin / copyout with a full barrier on the release / acquire side. */
	if ((w & 0x3F200C00) == 0x19000000) {
		unsigned size = w >> 30, opc = (w >> 22) & 3, rt = w & 31, rn = (w >> 5) & 31;
		unsigned bytes = 1U << size;
		if (opc >= 2 && size == 3) {
			return false;                           /* unallocated */
		}
		int64_t imm = (int64_t)((w >> 12) & 0x1ff);
		if (imm & 0x100) {
			imm -= 0x200;
		}
		uint64_t addr = reg_get(ss, rn, true) + (uint64_t)imm;
		uint64_t v = 0;
		if (opc == 0) {                                 /* STLUR: release */
			v = reg_get(ss, rt, false);
			__asm__ volatile ("dmb ish" ::: "memory");
			if (copyout(&v, (user_addr_t)addr, bytes) != 0) {
				return false;
			}
		} else {                                        /* LDAPUR / LDAPURS: acquire */
			if (copyin((user_addr_t)addr, &v, bytes) != 0) {
				return false;
			}
			__asm__ volatile ("dmb ish" ::: "memory");
			if (opc >= 2 && bytes < 8) {            /* sign extend to 64 (opc 2) or 32 (opc 3) bits */
				unsigned sh = 64 - 8 * bytes;
				v = (uint64_t)((int64_t)(v << sh) >> sh);
				if (opc == 3) {
					v &= 0xffffffffULL;
				}
			}
			reg_set(ss, rt, false, v);
		}
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	/* FEAT_FHM (ARMv8.2 optional, every Apple M1+): FMLAL/FMLAL2/FMLSL/FMLSL2 */
	if ((w & 0xBF60FC00) == 0x0E20EC00 ||                   /* FMLAL, FMLSL (vector) */
	    (w & 0xBF60FC00) == 0x2E20CC00 ||                   /* FMLAL2, FMLSL2 (vector) */
	    (w & 0xBFC0B400) == 0x0F800000 ||                   /* FMLAL, FMLSL (by element) */
	    (w & 0xBFC0B400) == 0x2F808000) {                   /* FMLAL2, FMLSL2 (by element) */
		if (!emulate_fmlal(w)) {
			return false;
		}
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	/* FEAT_SHA3 / FEAT_SHA512 (ARMv8.2 optional, every Apple M1+; both S9 cores lack them) */
	if ((w & 0xFFC08000) == 0xCE000000 ||                   /* EOR3, BCAX */
	    (w & 0xFFE0F000) == 0xCE608000 ||                   /* SHA512H, SHA512H2, SHA512SU1, RAX1 */
	    (w & 0xFFE00000) == 0xCE800000 ||                   /* XAR */
	    (w & 0xFFFFFC00) == 0xCEC08000) {                   /* SHA512SU0 */
		if (!emulate_sha3_sha512(w)) {
			return false;
		}
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	/* FEAT_SB (ARMv8.5): the trap and its exception return already synchronize the context */
	if (w == 0xD50330FF) {
		set_saved_state_pc(ss, pc + 4);
		sweet_a64e_counts[7]++;
		return true;
	}
	return false;
}
#endif /* SWEET || A5XELTE */
