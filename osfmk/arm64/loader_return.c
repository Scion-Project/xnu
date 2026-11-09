/*
 * Return-to-loader debug channel.
 *
 * On hardware without a serial port there is no way to get the kernel log
 * out. The XNU loader therefore leaves UEFI intact (no ExitBootServices) and
 * publishes the physical address of a return stub in /chosen/loader-return.
 * When the kernel has nothing more to do (panic, halt, timer) it collects its
 * message buffer, the panic buffer and any kprintf output, maps the stub 1:1
 * through TTBR0 and jumps to it with that log. The loader then restores the UEFI environment and writes
 * the log to storage.
 *
 * Only valid while the kernel does not drive hardware that UEFI also uses.
 */
#include <stdarg.h>
#include <mach/mach_types.h>
#include <machine/atomic.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm/machine_routines.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <pexpert/arm/protos.h>
#include <vm/vm_page.h>
#include <vm/vm_compressor.h>

/*
 * The console log handed back to the loader. A long session writes more than fits, and both
 * ends matter: the start of the boot is kept as it is, and of everything after it the most
 * recent part (a ring); room is left for what is appended on the way out (the kernel message
 * buffer, a panic's text). loader_log_linearize() puts the pieces in order before returning.
 */
#define LOADER_LOG_SIZE (1536 * 1024)
#define LOADER_LOG_HEAD (256 * 1024)
#define LOADER_LOG_RING (1024 * 1024)

#if defined(GENERIC_GIC_V3_IRQ)
extern void gic_v3_irq_quiesce(void);
extern void gic_v3_irq_cpu_shutdown(void);
#endif

void loader_log_putc(char c);
void loader_return(void);
extern void hakios_adreno_quiesce(void);
extern void hakios_adreno_stop(void);
extern void hakios_adreno_minute(void);
extern void hakios_wifi_stop(void);
extern void hakios_wifi_quiesce(void);
extern void hakios_adsp_stop(void);
extern void hakios_adsp_quiesce(void);
extern void hakios_mpss_stop(void);
extern void hakios_mpss_quiesce(void);
extern void hakios_ddr_quiesce(void);
extern void hakios_haptic_quiesce(void);
extern void hakios_torch_quiesce(void);
extern void hakios_camera_quiesce(void);
extern void hakios_mic_quiesce(void);
extern void hakios_max98512_quiesce(void);
extern void hakios_exynos_abox_quiesce(void);
extern void hakios_cs47l93_quiesce(void);
extern void hakios_exynos_camera_quiesce(void);
extern void hakios_exynos_iris_quiesce(void);
extern void hakios_s2mpb02_torch_quiesce(void);
extern void hakios_exynos_pcie_quiesce(void);
extern void hakios_ufs_quiesce(void);
extern void hakios_usb_gadget_quiesce(void);  /* drivers/usb/hakios.dwc3.c: the rest of the log out over USB, then the controller stopped */
extern void hakios_var_save_on_return(void);   /* drivers/storage/ufs/hakios.ufs.c: the data volume into the flash */
void loader_checkpoint(uint64_t n);

/* Also referenced from the early exception vector in start.s. */
char     loader_log[LOADER_LOG_SIZE];
uint32_t loader_log_len;
static bool     loader_return_entered;

/* One L1 table and one L2 table are enough to map a single 2 MB block. */
static uint64_t loader_l1[512] __attribute__((aligned(4096)));
static uint64_t loader_l2[512] __attribute__((aligned(4096)));

static uint32_t loader_log_ring_at;      /* where the next character goes in the ring */
static bool     loader_log_wrapped;
static bool     loader_log_linear;       /* the pieces have been put in order: only appending from now on */
static char     loader_log_scratch[LOADER_LOG_RING];

/*
 * Characters come from every processor, and from interrupt handlers on top of whatever was
 * writing one. The lock is of the simplest kind, good from the first line of C code to the
 * last of a panic; whoever cannot have it for a long while (its holder is gone: a panic
 * inside this very function) goes ahead without it.
 */
static uint32_t loader_log_lock;

static void loader_log_put_locked(char c);

/*
 * A second reader of every character the log gets, as it gets it: the USB gadget driver's (drivers/usb/hakios.dwc3.c),
 * which streams the log to a host. Called under the lock, interrupts off: it only copies the character.
 */
static void (*volatile loader_log_tap)(char c);

uint32_t loader_log_tap_set(void (*tap)(char c), void (*history)(const char *text, uint32_t len));

/*
 * Install (or with NULL remove) the tap. history, if given, first gets what the log has so far from its start (the part
 * kept as it was, the first LOADER_LOG_HEAD bytes), under the same lock, so that nothing is missed or sent twice.
 * Returns the log's length at that moment.
 */
uint32_t
loader_log_tap_set(void (*tap)(char c), void (*history)(const char *text, uint32_t len))
{
	uint64_t daif;
	bool locked = false;
	uint32_t len;

	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	len = loader_log_len;
	if (history != NULL) {
		history(loader_log, len < LOADER_LOG_HEAD || loader_log_linear || !loader_log_wrapped ? len : LOADER_LOG_HEAD);
	}
	loader_log_tap = tap;
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
	return len;
}

void
loader_log_putc(char c)
{
	uint64_t daif;
	bool locked = false;

	if (c == '\r') {
		return;
	}
	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	loader_log_put_locked(c);
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

/*
 * A whole line at once, under the lock: lines written from several processors at the same time
 * (tools/latency.py's "tl:" lines, a frame's on one and a touch's on another) are not mixed up.
 */
void loader_log_line(const char *line);

void
loader_log_line(const char *line)
{
	uint64_t daif;
	bool locked = false;

	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	for (; *line != 0; line++) {
		if (*line != '\r') {
			loader_log_put_locked(*line);
		}
	}
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

/*
 * esslog (Galaxy S9): every character of the log also into Samsung's exynos-snapshot kernel log buffer
 * (log_kernel, PA 0xFDA10000, 2 MB; header at ess_setup 0xFDA00000, last write position at +0x200), so that
 * after a restart TWRP's kernel offers this run's whole log as /proc/last_kmsg (drivers/trace/exynos-ss.c,
 * drivers/staging/samsung/sec_debug_last_kmsg.c). boot-arg esslog=1.
 */
#define ESSLOG_HDR_PA   0xFDA00000ULL
#define ESSLOG_BUF_PA   0xFDA10000ULL
#define ESSLOG_SIZE     (2u * 1024 * 1024)
#define ESSLOG_MAGIC    0x0000000a6c6c7546ULL
static volatile uint8_t *esslog_hdr, *esslog_buf;
static uint32_t esslog_at;
static void
esslog_put(char c)
{
	esslog_buf[esslog_at++] = (uint8_t)c;
	if (esslog_at >= ESSLOG_SIZE - 8) {
		*(volatile uint64_t *)(esslog_buf + ESSLOG_SIZE - 8) = ESSLOG_MAGIC;
		esslog_at = 0;
	}
	if (c == '\n') {
		*(volatile uint32_t *)(esslog_hdr + 0x200) = (uint32_t)(ESSLOG_BUF_PA + esslog_at);
	}
}
static void
loader_log_put_locked(char c)
{
	void (*tap)(char) = loader_log_tap;

	if (tap != NULL) {
		tap(c);
	}
	if (esslog_buf != NULL) {
		esslog_put(c);
	}
	if (loader_log_linear || loader_log_len < LOADER_LOG_HEAD) {
		if (loader_log_len < LOADER_LOG_SIZE) {
			loader_log[loader_log_len++] = c;
		}
	} else {
		loader_log[LOADER_LOG_HEAD + loader_log_ring_at] = c;
		if (++loader_log_ring_at == LOADER_LOG_RING) {
			loader_log_ring_at = 0;
			loader_log_wrapped = true;
		}
		if (!loader_log_wrapped) {
			loader_log_len = LOADER_LOG_HEAD + loader_log_ring_at;
		}
	}
}

/* Oldest first: the head, a note about what was dropped, then the ring from its oldest character. */
static void
loader_log_linearize(void)
{
	static const char note[] = "\n\n[... the log between the first 256 KB and the last 1 MB was dropped ...]\n\n";
	uint32_t older = LOADER_LOG_RING - loader_log_ring_at, n = LOADER_LOG_HEAD;

	if (loader_log_linear) {
		return;
	}
	loader_log_linear = true;
	if (!loader_log_wrapped) {
		return;
	}
	__builtin_memcpy(loader_log_scratch, loader_log + LOADER_LOG_HEAD + loader_log_ring_at, older);
	__builtin_memcpy(loader_log_scratch + older, loader_log + LOADER_LOG_HEAD, loader_log_ring_at);
	/* the note takes the place of the oldest characters of the ring */
	__builtin_memcpy(loader_log + n, note, sizeof(note) - 1);
	n += sizeof(note) - 1;
	__builtin_memcpy(loader_log + n, loader_log_scratch + sizeof(note) - 1, LOADER_LOG_RING - (sizeof(note) - 1));
	loader_log_len = LOADER_LOG_HEAD + LOADER_LOG_RING;
}

/* Patched by the loader: { magic, PA of its return stub, checkpoint to stop at, PA - VA }. See start.s. */
extern uint64_t loader_stub_slot[4];

/*
 * For a loader that watches from another processor core while the kernel runs (it starts the
 * kernel on a core of its choice and stays on the first one): a sign of life, and where the log
 * is, so that the log of a kernel that hangs -- or dies before it has said a word through any
 * other channel -- is not lost with it. The loader finds this by its first word in the kernel
 * it has loaded. The addresses are the kernel's virtual ones, filled in by the linker and so
 * valid from the first instruction; the loader knows the distance to the physical ones.
 */
struct loader_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every HV_WDT_PET_SECONDS while the kernel is alive */
	const char *log;                /* loader_log */
	const uint32_t *log_len;        /* how much of it is in use */
	const uint32_t *log_ring_at;    /* where the ring part is being written */
	const bool *log_wrapped;        /* the ring has gone round */
	uint64_t log_head, log_ring;    /* sizes of the two parts */
	const uint64_t *recording;      /* loader_recording: { physical address, length } of a screen recording */
	volatile uint64_t stop;         /* the loader's to write: the host has asked for the run to end */
	uint64_t magic2;                /* "XNULIVE2": what follows is there (a loader may meet an older kernel) */
	const uint64_t *sound;          /* loader_sound: the same for a recording of the sound */
};

/*
 * A screen recording (the display driver makes one with boot-arg rec_secs): where it is, for the
 * loader to hand to the host once the kernel is back. { physical address, bytes, kernel address }.
 */
uint64_t loader_recording[3];
/* ... and of the sound (/dev/hakios-audio makes one with boot-arg rec_sound). */
uint64_t loader_sound[3];

struct loader_live loader_live = {
	.magic = 0x214556494c554e58ULL, .log = loader_log, .log_len = &loader_log_len, .log_ring_at = &loader_log_ring_at,
	.log_wrapped = &loader_log_wrapped, .log_head = LOADER_LOG_HEAD, .log_ring = LOADER_LOG_RING,
	.recording = loader_recording,
	.magic2 = 0x324556494c554e58ULL, .sound = loader_sound,
};

/* Valid from the first instruction of C code: does not depend on gVirtBase/gPhysBase being set up. */
static inline uint64_t
static_kvtophys(const void *va)
{
	return (uint64_t)va + loader_stub_slot[3];
}

/*
 * Jump to the loader's return stub. reason: 0 = orderly end, 2 = checkpoint.
 * Returns only if no loader is listening.
 */
static void
loader_jump(uint64_t reason, uint64_t code)
{
#if ARM_PGSHIFT == 12
	uint64_t stub = loader_stub_slot[1];
	uint64_t tcr, t0sz;
	uint32_t own_core = 0;

	if (stub == 0 || loader_return_entered) {
		return;
	}

	tcr = __builtin_arm_rsr64("TCR_EL1");
	t0sz = tcr & 0x3f;
	/* The TTBR0 walk must start at L1 (1 GB per entry) and reach the stub. */
	if (t0sz < 25 || t0sz > 33 || (stub >> (64 - t0sz)) != 0) {
		return;
	}
	loader_return_entered = true;
#if defined(GENERIC_GIC_V3_IRQ)
	gic_v3_irq_quiesce();
	/*
	 * With a core of its own (bootcpu=) the kernel does not go back into the loader's code on
	 * this processor: the stub leaves the log where the loader's core finds it and has the
	 * firmware switch this one off. It must go as the other cores do (psci_smp_cpu_off()), with
	 * nothing pending: after a panic the "stop" signal and the timer are.
	 */
	if (PE_parse_boot_argn("bootcpu", &own_core, sizeof(own_core))) {
		__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
		gic_v3_irq_cpu_shutdown();
	}
#endif

	loader_l1[stub >> 30] = static_kvtophys(loader_l2) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
	loader_l2[(stub >> 21) & 0x1ff] = (stub & ~0x1fffffULL) | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
	    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_AP(AP_RWNA) |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);

	/*
	 * The stub turns the MMU off, and on real hardware dirty cache lines do
	 * not survive that: write the log back to RAM first.
	 */
	for (uintptr_t p = (uintptr_t)loader_log & ~63UL; p < (uintptr_t)loader_log + loader_log_len; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	/* ... and a screen recording, and the note of where it is */
	for (uintptr_t p = loader_recording[2] & ~63UL; p < loader_recording[2] + loader_recording[1]; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_recording));
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_recording + 16));
	for (uintptr_t p = loader_sound[2] & ~63UL; p < loader_sound[2] + loader_sound[1]; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_sound));
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_sound + 16));
	__asm__ volatile ("dsb sy");

	register uint64_t r_ttbr0 __asm__("x0") = static_kvtophys(loader_l1);
	register uint64_t r_log __asm__("x2") = static_kvtophys(loader_log);
	register uint64_t r_len __asm__("x3") = loader_log_len;
	register uint64_t r_tcr __asm__("x4") = tcr;
	register uint64_t r_stub __asm__("x5") = stub;
	register uint64_t r_reason __asm__("x6") = reason;
	register uint64_t r_code __asm__("x7") = code;

	__asm__ volatile (
                "msr DAIFSet, #0xf\n"
                "bic x4, x4, #0x80\n"           /* TCR_EL1.EPD0: allow TTBR0 walks */
                "msr TCR_EL1, x4\n"
                "msr TTBR0_EL1, x0\n"
                "isb\n"
                "tlbi vmalle1\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, x2\n"
                "mov x1, x3\n"
                "mov x2, x6\n"
                "mov x3, x7\n"
                "br x5\n"
                : : "r" (r_ttbr0), "r" (r_log), "r" (r_len), "r" (r_tcr), "r" (r_stub), "r" (r_reason), "r" (r_code));
	__builtin_unreachable();
#else
	(void)reason; (void)code;
#endif /* ARM_PGSHIFT == 12 */
}

extern char *debug_buf_base, *debug_buf_ptr;

/* bsd/sys/msgbuf.h (not includable from osfmk) */
struct msgbuf {
	int   msg_magic;
	int   msg_size;
	int   msg_bufx;         /* write pointer */
	int   msg_bufr;         /* read pointer */
	char *msg_bufc;         /* buffer */
};
extern struct msgbuf *msgbufp;

static void
loader_log_hex(uint64_t v)
{
	loader_log_putc('0');
	loader_log_putc('x');
	for (int shift = 60; shift >= 0; shift -= 4) {
		loader_log_putc("0123456789abcdef"[(v >> shift) & 0xf]);
	}
}

static void
loader_log_puts(const char *s)
{
	for (; *s; s++) {
		loader_log_putc(*s);
	}
}

/*
 * A new phone's first runs, said in full in the log the loader gets back. A panic's own text, the registers of the
 * last exception and the calls that led to it are always written here: before arm_vm_init there is no panic buffer,
 * and on a phone without a serial port such a panic otherwise comes back as nothing but loader_return()'s caller.
 * Boot-arg early_log=1 adds every kprintf from PE_init_platform on (the drivers' and the VM's bring-up lines, which
 * no console shows that early) and what the kernel takes the processors to be.
 */
bool loader_early_log;

void loader_early_log_init(void);
void loader_log_vprintf(const char *fmt, va_list *ap);
void loader_log_panic(const char *fmt, va_list *ap, unsigned long caller);
void loader_log_cpu_facts(void);
extern uint32_t ml_mpidr_mt_aff1;

void
loader_early_log_init(void)
{
	uint32_t on = 0;

	if (PE_parse_boot_argn("early_log", &on, sizeof(on)) && on != 0) {
		loader_early_log = true;
	}
}

void
loader_log_vprintf(const char *fmt, va_list *ap)
{
	va_list copy;

	va_copy(copy, *ap);
	_doprnt(fmt, &copy, loader_log_putc, 16);
	va_end(copy);
}

static void
loader_log_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	_doprnt(fmt, &ap, loader_log_putc, 16);
	va_end(ap);
}

static void
loader_log_frame(uint64_t pc)
{
	loader_log_puts("  ");
	loader_log_hex(pc);
	if (vm_kernel_slide != 0) {
		loader_log_puts(" unslid ");
		loader_log_hex(pc - vm_kernel_slide);
	}
	loader_log_putc('\n');
}

/* Called first thing by panic_trap_to_debugger() (patch 1503), on whichever processor panicked */
void
loader_log_panic(const char *fmt, va_list *ap, unsigned long caller)
{
	static uint32_t panics;
	uint64_t *fp = __builtin_frame_address(0);

	if (os_atomic_inc(&panics, relaxed) > 2) {
		return;                 /* a panic inside the panic, over and over: the first two say enough */
	}
	loader_log_puts("\n--- panic on MPIDR ");
	loader_log_hex(__builtin_arm_rsr64("MPIDR_EL1") & 0xffffff);
	loader_log_puts(": ");
	if (fmt != NULL && ap != NULL) {
		loader_log_vprintf(fmt, ap);
	} else if (fmt != NULL) {
		loader_log_puts(fmt);
	}
	loader_log_puts("\n--- the last exception: ESR_EL1 ");
	loader_log_hex(__builtin_arm_rsr64("ESR_EL1"));
	loader_log_puts(" FAR_EL1 ");
	loader_log_hex(__builtin_arm_rsr64("FAR_EL1"));
	loader_log_puts(" ELR_EL1 ");
	loader_log_hex(__builtin_arm_rsr64("ELR_EL1"));
	loader_log_puts("\n--- panic() called from\n");
	loader_log_frame(caller);
	loader_log_puts("--- frames (nm on the unstripped kernel names them)\n");
	/* the frame records: [fp] the caller's fp, [fp + 8] the return address; on the stack, each above the last */
	for (int i = 0; i < 32; i++) {
		uint64_t *next;

		if (fp == NULL || ((uintptr_t)fp & 0xf) != 0 || (uintptr_t)fp < VM_MIN_KERNEL_AND_KEXT_ADDRESS) {
			break;
		}
		loader_log_frame(fp[1]);
		next = (uint64_t *)fp[0];
		if (next <= fp || (uintptr_t)next - (uintptr_t)fp > 64 * 1024) {
			break;
		}
		fp = next;
	}
	loader_log_puts("---\n");
}

/* early_log: right after ml_parse_cpu_topology(), what the kernel will match each processor's MPIDR against */
void
loader_log_cpu_facts(void)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");

	if (!loader_early_log) {
		return;
	}
	loader_log_printf("\nearly_log: MPIDR %llx MIDR %llx CurrentEL %llu CLIDR %llx CTR %llx ID_AA64PFR0 %llx "
	    "ID_AA64MMFR0 %llx ID_AA64ISAR0 %llx\n", mpidr, __builtin_arm_rsr64("MIDR_EL1"),
	    (__builtin_arm_rsr64("CurrentEL") >> 2) & 3, __builtin_arm_rsr64("CLIDR_EL1"), __builtin_arm_rsr64("CTR_EL0"),
	    __builtin_arm_rsr64("ID_AA64PFR0_EL1"), __builtin_arm_rsr64("ID_AA64MMFR0_EL1"),
	    __builtin_arm_rsr64("ID_AA64ISAR0_EL1"));
	if (t == NULL || t->cpus == NULL) {
		loader_log_puts("early_log: no topology\n");
		return;
	}
	loader_log_printf("early_log: topology: %u cpus (max id %u), %u clusters; boot cpu id %d phys %x; "
	    "this processor by its MPIDR: phys %x, cpu %d%s\n", t->num_cpus, t->max_cpu_id, t->num_clusters,
	    t->boot_cpu != NULL ? (int)t->boot_cpu->cpu_id : -1, t->boot_cpu != NULL ? t->boot_cpu->phys_id : 0,
	    ml_phys_id_from_mpidr(mpidr), ml_get_cpu_number(ml_phys_id_from_mpidr(mpidr)),
	    ml_mpidr_mt_aff1 ? " (MT: the device tree numbers the cores one affinity field lower)" : "");
	for (unsigned i = 0; i < t->num_cpus; i++) {
		loader_log_printf("early_log:   cpu %u phys %x cluster %u type %d\n", t->cpus[i].cpu_id,
		    t->cpus[i].phys_id, t->cpus[i].cluster_id, (int)t->cpus[i].cluster_type);
	}
}

/*
 * Everything printf()/IOLog()/os_log wrote ends up in the kernel message
 * buffer (what dmesg shows), whether or not a console displayed it.
 */
static void
loader_log_append_msgbuf(void)
{
	struct msgbuf *mb = msgbufp;

	if (mb == NULL || mb->msg_bufc == NULL || mb->msg_size <= 0 || mb->msg_bufx < 0 || mb->msg_bufx > mb->msg_size) {
		return;
	}
	loader_log_puts("\n--- kernel message buffer ---\n");
	/* oldest data first: [bufx, size) is only meaningful once the buffer has wrapped */
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? mb->msg_bufx : 0;
		int to = pass == 0 ? mb->msg_size : mb->msg_bufx;
		for (int i = from; i < to; i++) {
			if (mb->msg_bufc[i] != 0) {
				loader_log_putc(mb->msg_bufc[i]);
			}
		}
	}
}

/*
 * No loader to return to (avfboot under crosvm/AVF leaves the return slot 0): end the machine
 * through PSCI instead, so that the VM stops and its owner sees it stop. A reboot asks for
 * SYSTEM_RESET, a halt or a panic for SYSTEM_OFF; a panic's first line is said once more on the
 * console just before, so that it is the last thing there. boot-arg avf_exit=0: stay, as before.
 */
static bool loader_exit_reset;

void
loader_exit_wanted(bool reset)
{
	loader_exit_reset = reset;
}

static void
avf_console_puts(const char *s, const char *end)
{
	for (; *s && (end == NULL || s < end); s++) {
		if (*s == '\n') {
			uart_putc('\r');
		}
		uart_putc(*s);
	}
}

static void
loader_exit_without_loader(void)
{
	uint32_t stay_on = 1;
	const char *panic_line = NULL, *line_end = NULL;

	if (loader_stub_slot[1] != 0) {
		return;                         /* a loader is there; it just would not have us back */
	}
	if (PE_parse_boot_argn("avf_exit", &stay_on, sizeof(stay_on)) && stay_on == 0) {
		return;
	}
	/* the panic's own first line, from the panic buffer */
	if (debug_buf_base != NULL && debug_buf_ptr > debug_buf_base &&
	    (size_t)(debug_buf_ptr - debug_buf_base) < 256 * 1024) {
		for (const char *c = debug_buf_base; c + 6 <= debug_buf_ptr; c++) {
			if (__builtin_memcmp(c, "panic(", 6) == 0) {
				panic_line = c;
				for (line_end = c; line_end < debug_buf_ptr && *line_end != '\n' && *line_end; line_end++) {
				}
				break;
			}
		}
	}
	if (panic_line != NULL) {
		avf_console_puts("\navf: kernel panic: ", NULL);
		avf_console_puts(panic_line, line_end);
		avf_console_puts("\navf: powering off (PSCI SYSTEM_OFF)\n", NULL);
		(void)psci_system_off(false);
	} else if (loader_exit_reset) {
		avf_console_puts("\navf: reboot (PSCI SYSTEM_RESET)\n", NULL);
		(void)psci_system_off(true);
	} else {
		avf_console_puts("\navf: halt (PSCI SYSTEM_OFF)\n", NULL);
		(void)psci_system_off(false);
	}
	avf_console_puts("avf: the firmware refused to end the machine\n", NULL);
}

void
loader_return(void)
{
#if defined(GENERIC_PSCI_SMP)
	/*
	 * With more than one processor: the others switch themselves off first, and the rest of
	 * this is done by the boot processor, the one the loader gave us. This call comes back
	 * on that processor only (on another one it has it call loader_return() in its turn).
	 */
	psci_smp_stop_others();
#endif
	hakios_ufs_quiesce();          /* this kernel's requests to the storage done, UEFI's list as it was, for the log */
	hakios_adreno_quiesce();       /* the GPU block, if it was started, has tables in memory that is about to be reused */
	hakios_wifi_quiesce();         /* the Wi-Fi DSP, if it still runs (a panic), is not left to itself */
	hakios_adsp_quiesce();         /* nor the audio DSP */
	hakios_mpss_quiesce();         /* nor the modem */
	hakios_ddr_quiesce();          /* and the vote for the memory's bandwidth taken away */
	hakios_haptic_quiesce();       /* the vibration motor's chip back into reset, if it was let out */
	hakios_torch_quiesce();        /* the flashlight off, if it was lit */
	hakios_camera_quiesce();       /* the camera powered down, if it was powered up */
	hakios_mic_quiesce();          /* (what the microphones' path did, said again: it happens in the log's middle) */
	hakios_max98512_quiesce();     /* the S9's speaker amplifiers shut down, while UAIF1's clocks still run */
	hakios_exynos_abox_quiesce();  /* then ABOX's RDMA0 and UAIF1: no more reading of memory the loader reuses */
	hakios_cs47l93_quiesce();      /* the S9's audio codec held in reset */
	hakios_s2mpb02_torch_quiesce(); /* the S9's flashlight off */
	hakios_exynos_camera_quiesce(); /* the S9's front camera powered down */
	hakios_exynos_iris_quiesce();   /* the S9's iris camera powered down, its sensor (and so its IR LED) stopped */
	hakios_exynos_pcie_quiesce();  /* the S9's Wi-Fi chip held in reset, unpowered: no DMA into the loader's memory */
	/*
	 * A panic's text is first collected in the panic buffer and only later
	 * flushed to the console; if the console itself is what failed, that
	 * never happens. Append the buffer so the loader gets it either way.
	 */
	loader_log_linearize();
	/*
	 * Who asked for the return: on a new phone a kernel that came back before its message buffer existed said nothing
	 * else. Unslid by vm_kernel_slide, nm on the unstripped kernel names the function.
	 */
	{
		uint64_t caller = (uint64_t)__builtin_return_address(0);

		loader_log_puts("\n--- loader_return() from ");
		loader_log_hex(caller);
		loader_log_puts(", unslid ");
		loader_log_hex(caller - vm_kernel_slide);
		loader_log_puts(" ---\n");
	}
	if (!loader_return_entered) {
		loader_log_append_msgbuf();
	}
	if (!loader_return_entered && debug_buf_base != NULL && debug_buf_ptr > debug_buf_base &&
	    (size_t)(debug_buf_ptr - debug_buf_base) < 256 * 1024) {
		loader_log_puts("\n--- panic buffer ---\n");
		for (const char *c = debug_buf_base; c < debug_buf_ptr; c++) {
			loader_log_putc(*c);
		}
	}
	hakios_usb_gadget_quiesce();   /* last: the panic text above goes out over USB too, before the controller stops */
	loader_jump(0, 0);
	loader_exit_without_loader();
}

/*
 * Bring-up aid: stop here if the loader asked for checkpoint n. Every checkpoint passed is also written to the log the
 * loader gets back ("[c10][c11]..."), so a kernel that ends before its first line still says how far it came.
 */
void
loader_checkpoint(uint64_t n)
{
	char digits[20];
	uint64_t v = n;
	int i = 0;

	do {
		digits[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0 && i < (int)sizeof(digits));
	loader_log_putc('[');
	loader_log_putc('c');
	while (i > 0) {
		loader_log_putc(digits[--i]);
	}
	loader_log_putc(']');
	if (loader_stub_slot[2] == n) {
		loader_jump(2, n);
	}
}

/*
 * boot-arg loader_return_after=<seconds>: hand control back to the loader
 * after that long, whatever the kernel is doing. This is how the log gets out
 * when the kernel does not panic but simply waits (e.g. for a root device).
 */
/*
 * For trying the ways out when there is more than one processor: boot-arg return_cpu=<n> has
 * that processor make the timed return, from an interrupt handler -- any but the boot processor
 * has to hand it over (psci_smp_stop_others()) --, and return_panic=1 makes a panic of it.
 */
static void
loader_return_test(void *as_panic)
{
	if (as_panic != (void *)1) {
		panic("return_panic: the panic that was asked for, on cpu %d", cpu_number());
	}
	loader_return();
}

static void
loader_return_timer(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static const char banner[] = "\n--- loader_return_after expired ---\n";
	uint32_t cpu = 0, as_panic = 0;

	for (const char *c = banner; *c; c++) {
		loader_log_putc(*c);
	}
	PE_parse_boot_argn("return_panic", &as_panic, sizeof(as_panic));
	if (!as_panic) {
		hakios_var_save_on_return();   /* the data volume kept, while the system is still whole */
		hakios_adreno_stop();          /* a GPU kept for the system is given back while there is a thread to wait in */
		hakios_wifi_stop();            /* and the Wi-Fi DSP, if it runs, shut down */
		hakios_adsp_stop();            /* and the audio DSP */
		hakios_mpss_stop();            /* and the modem */
	}
	if (PE_parse_boot_argn("return_cpu", &cpu, sizeof(cpu)) && cpu <= (uint32_t)ml_get_max_cpu_number() &&
	    cpu_xcall((int)cpu, loader_return_test, (void *)(uintptr_t)(as_panic ? 2 : 1)) == KERN_SUCCESS) {
		for (;;) {
			delay_for_interval(1000, NSEC_PER_MSEC);        /* that processor ends it */
		}
	}
	loader_return_test((void *)(uintptr_t)(as_panic ? 2 : 1));
}

__startup_func
static void
loader_return_timer_init(void)
{
	uint32_t seconds = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	thread_call_t call = thread_call_allocate(loader_return_timer, NULL);
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_return_timer_init);

/*
 * A loader that watches from another core also listens to the host, and passes on a request to
 * end the run ("xl stop") by setting loader_live.stop. Look once a second.
 */
static thread_call_t loader_stop_call;

static void
loader_stop_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (loader_live.stop) {
		static const char banner[] = "\n--- the host asked for the end of the run: back to the loader ---\n";
		for (const char *c = banner; *c; c++) {
			loader_log_putc(*c);
		}
		hakios_var_save_on_return();
		hakios_adreno_stop();
		hakios_wifi_stop();
		hakios_adsp_stop();
		loader_return();
	}
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(loader_stop_call, deadline);
}

__startup_func
static void
loader_stop_poll_init(void)
{
	loader_stop_call = thread_call_allocate(loader_stop_poll, NULL);
	loader_stop_poll(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_stop_poll_init);

/*
 * boot-arg hv_wdt_pet=1: the loader has armed the Qualcomm hypervisor's
 * virtual watchdog (it resets the phone when a kernel hangs, so that test
 * runs need nobody at the device). Its timeout is at most 65 seconds; feed
 * it from a timer so that a healthy kernel can run for as long as it likes.
 */
#define VIRT_WDT_PET            0x86000007u
#define HV_WDT_PET_SECONDS      10

static thread_call_t hv_wdt_call;
static uint32_t hv_wdt_enabled;

#define MEMORY_REPORT_BEATS     6               /* a line about memory every so many heartbeats */

extern unsigned int memorystatus_available_pages, memorystatus_available_pages_critical,
    memorystatus_available_pages_critical_base;
extern uint32_t c_segment_pages_compressed;
extern _Atomic uint64_t ramfs_pages_in_use;
extern void MiniIOSurfaceUsage(uint32_t *count, uint64_t *bytes);

/*
 * Where the memory is, in megabytes: for reading a session's log afterwards. "available" is what
 * the kernel's own count says (free and file-backed): below the first threshold it kills idle
 * daemons, and once none is left and it is below the second, whatever is next in line -- the
 * window server included.
 */
void hakios_memory_line(char *buf, size_t len);

/* The line itself (also kern.hakios_memory, for a job list to keep: bsd/miscfs/apfs/apfs_vfsops.c) */
void
hakios_memory_line(char *buf, size_t len)
{
#define MB(pages)       ((unsigned int)(((uint64_t)(pages) * PAGE_SIZE) >> 20))
	uint32_t surfaces = 0;
	uint64_t surface_bytes = 0;

	MiniIOSurfaceUsage(&surfaces, &surface_bytes);
	snprintf(buf, len, "memory: free %u, files %u, programs %u, wired %u (%u surfaces %u, ramfs %u), compressor %u "
	    "(holding %u) MB; available %u, kills below %u (the idle) and %u (anybody)\n",
	    MB(vm_page_free_count), MB(vm_page_pageable_external_count), MB(vm_page_pageable_internal_count),
	    MB(vm_page_wire_count), surfaces, (unsigned int)(surface_bytes >> 20),
	    MB(os_atomic_load(&ramfs_pages_in_use, relaxed)),
	    MB(VM_PAGE_COMPRESSOR_COUNT), MB(c_segment_pages_compressed),
	    MB(memorystatus_available_pages),
	    MB(memorystatus_available_pages_critical), MB(memorystatus_available_pages_critical_base));
#undef MB
}

static void
memory_report(void)
{
	char line[256];

	hakios_memory_line(line, sizeof(line));
	printf("%s", line);
	hakios_adreno_minute();                /* (and how warm the GPU is, if it is in use) */
}

/* Every HV_WDT_PET_SECONDS: feed the watchdog if there is one, and show any watching loader a sign of life. */
static void
hv_wdt_pet(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (hv_wdt_enabled) {
		register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
		register uint64_t x1 __asm__("x1") = 0;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	if (++loader_live.heartbeat % MEMORY_REPORT_BEATS == 0) {
		memory_report();
	}

	clock_interval_to_deadline(HV_WDT_PET_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(hv_wdt_call, deadline);
}

__startup_func
static void
hv_wdt_pet_init(void)
{
	PE_parse_boot_argn("hv_wdt_pet", &hv_wdt_enabled, sizeof(hv_wdt_enabled));
	hv_wdt_call = thread_call_allocate(hv_wdt_pet, NULL);
	hv_wdt_pet(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hv_wdt_pet_init);

void hakios_esslog_start(void);
void
hakios_esslog_start(void)
{
	uint32_t on = 0, len;
	uint64_t daif;
	bool locked = false;
	volatile uint8_t *hdr, *buf;

	if (esslog_buf != NULL || !PE_parse_boot_argn("esslog", &on, sizeof(on)) || on == 0) {
		return;
	}
	hdr = (volatile uint8_t *)ml_io_map(ESSLOG_HDR_PA, 0x1000);
	buf = (volatile uint8_t *)ml_io_map(ESSLOG_BUF_PA, ESSLOG_SIZE);     /* uncached: a hard hang must not lose what sat in a write-combining buffer */
	if (hdr == NULL || buf == NULL) {
		printf("esslog: could not map\n");
		return;
	}
	*(volatile uint64_t *)(buf + ESSLOG_SIZE - 8) = 0;
	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	len = loader_log_len < LOADER_LOG_HEAD || loader_log_linear || !loader_log_wrapped ? loader_log_len : LOADER_LOG_HEAD;
	if (len > ESSLOG_SIZE - 64) {
		len = ESSLOG_SIZE - 64;
	}
	for (uint32_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)loader_log[i];
	}
	esslog_at = len;
	esslog_hdr = hdr;
	*(volatile uint32_t *)(hdr + 0x200) = (uint32_t)(ESSLOG_BUF_PA + esslog_at);
	esslog_buf = buf;
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
	printf("esslog: the log goes to exynos-snapshot's log_kernel too (%u bytes so far): /proc/last_kmsg in TWRP\n", len);
}
