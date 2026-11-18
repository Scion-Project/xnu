/*
 * STARLTE (Galaxy S9) iOS 27 line: acknowledging the Scion loader's boot-loop guard. NOT RUN ON A PHONE.
 *
 * The second-stage loader (github Scion-Project/scion-boot-exynos9810: include/boot.h struct scion_guard_rec,
 * src/lib/verify.c scion_guard_valid/scion_guard_seal, loader/main.c add_loader_tokens) keeps a 32-byte record in RAM
 * that survives a warm reset (on the S9 a slot of the exynos-ss page, 0xfed17fc0) and adds one to its strikes before
 * every automatic start; after three unacknowledged automatic starts the next one defaults to recovery. It puts the
 * record's address on XNU's line as scion_guard=0x<pa>. This file acknowledges a good boot the way the loader's
 * boot.h says: strikes = 0 with a matching crc32.
 *
 * "Booted" here is: SpringBoard has been exec'd and the same process is still running SCION_GUARD_DELAY_S seconds
 * later. Kernel start is not enough (a boot that panics or hangs in launchd, a userspace that never brings up the UI, or
 * a SpringBoard that dies at once and is restarted over and over are exactly the loops the guard is for). The hook is in
 * set_proc_name (bsd/kern/kern_exec.c), which every exec passes; each later SpringBoard exec re-arms the wait, so a
 * SpringBoard that crash-loops faster than the delay is never acknowledged. One acknowledgement per boot.
 *
 * Without scion_guard= on the line nothing of this runs and nothing is mapped. The address is accepted only when it is
 * 8-byte aligned and the whole record lies in 0xfed17000..0xfed17fe0 (the exynos-ss page below the boot-next mailbox at
 * 0xfed17fe0, docs/ios27/USB-FASTBOOT.md): anything else is refused and nothing is written anywhere. The page is not in
 * XNU's RAM (tools/ios27/adt-extra-ram.py stops below 0xf0000000) and is mapped as herolte_ufs.c maps the pmsg ring at
 * 0xfed14000 (ml_io_map). The record is validated exactly as the loader does (magic, version, size, crc32 of the 32
 * bytes with the crc field 0); one that does not validate is left alone (the loader reads it as no strikes).
 * The acknowledgement keeps last_entry, clears strikes, flags (SCION_GUARD_F_SAFE_TRIED) and _reserved, recomputes
 * the crc and writes the words with 32-bit stores (the mapping is Device memory), crc last, then a dsb.
 *
 * Lines start with "SCION GUARD:". Built with -DSCION_GUARD_HOST the kernel part is left out and the record code can be
 * tested on a host (the test: scion-kernel tools, or any C file that includes this one).
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <stdint.h>
#include <stdbool.h>

#define SCION_GUARD_MAGIC       0x474E4353U    /* 'SCNG' */
#define SCION_GUARD_VERSION     1U
#define SCION_GUARD_SIZE        32U
#define SCION_GUARD_WINDOW_LO   0xfed17000ULL
#define SCION_GUARD_WINDOW_HI   0xfed17fe0ULL  /* the boot-next mailbox (SBNEXT2!) starts here: never touched */
#define SCION_GUARD_DELAY_S     20U

/* The record as 8 little-endian words: magic, version | size << 16, strikes, last_entry, flags, crc32, _reserved (2). */
enum { SG_MAGIC, SG_VS, SG_STRIKES, SG_LAST, SG_FLAGS, SG_CRC, SG_RES0, SG_RES1, SG_WORDS };

/* IEEE CRC32 (zlib's crc32) of the 32 bytes with the crc word taken as 0. */
static uint32_t
scion_guard_crc(const uint32_t w[SG_WORDS])
{
	uint32_t c = 0xffffffffU;
	for (unsigned i = 0; i < SG_WORDS; i++) {
		uint32_t v = (i == SG_CRC) ? 0 : w[i];
		for (unsigned b = 0; b < 4; b++) {
			c ^= (v >> (8 * b)) & 0xffU;
			for (unsigned k = 0; k < 8; k++) {
				c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
			}
		}
	}
	return ~c;
}

static bool
scion_guard_valid_words(const uint32_t w[SG_WORDS])
{
	return w[SG_MAGIC] == SCION_GUARD_MAGIC && (w[SG_VS] & 0xffffU) == SCION_GUARD_VERSION &&
	       (w[SG_VS] >> 16) == SCION_GUARD_SIZE && scion_guard_crc(w) == w[SG_CRC];
}

/* The acknowledged record in place of a valid one (last_entry kept). */
static void
scion_guard_ack_words(uint32_t w[SG_WORDS])
{
	w[SG_STRIKES] = 0;
	w[SG_FLAGS] = 0;
	w[SG_RES0] = 0;
	w[SG_RES1] = 0;
	w[SG_CRC] = scion_guard_crc(w);
}

static bool
scion_guard_pa_ok(uint64_t pa)
{
	return (pa & 7U) == 0 && pa >= SCION_GUARD_WINDOW_LO && pa + SCION_GUARD_SIZE <= SCION_GUARD_WINDOW_HI;
}

#if !defined(SCION_GUARD_HOST)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/atomic.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
void scion_guard_exec(const char *comm, int pid);   /* bsd/kern/kern_exec.c set_proc_name */

static uint64_t scion_guard_pa;
static volatile uint32_t *scion_guard_va;
static thread_call_t scion_guard_call;
static int scion_guard_pid;
static uint32_t scion_guard_done;                   /* 1 once the boot is acknowledged (or the record refused) */

static void
scion_guard_tick(__unused thread_call_param_t a, __unused thread_call_param_t b)
{
	char name[32];
	int pid = os_atomic_load(&scion_guard_pid, relaxed);
	uint32_t w[SG_WORDS];

	proc_name(pid, name, sizeof(name));
	if (strcmp(name, "SpringBoard") != 0) {
		printf("SCION GUARD: SpringBoard[%d] did not run %u s: not acknowledged (waiting for the next one)\n", pid,
		    SCION_GUARD_DELAY_S);
		return;
	}
	if (!os_atomic_cmpxchg(&scion_guard_done, 0U, 1U, relaxed)) {
		return;
	}
	for (unsigned i = 0; i < SG_WORDS; i++) {
		w[i] = scion_guard_va[i];
	}
	if (!scion_guard_valid_words(w)) {
		printf("SCION GUARD: no valid record at %#llx (magic %#x): nothing written\n", scion_guard_pa, w[SG_MAGIC]);
		return;
	}
	uint32_t before = w[SG_STRIKES];
	scion_guard_ack_words(w);
	for (unsigned i = 0; i < SG_WORDS; i++) {
		if (i != SG_CRC) {
			scion_guard_va[i] = w[i];
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	scion_guard_va[SG_CRC] = w[SG_CRC];
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned i = 0; i < SG_WORDS; i++) {
		w[i] = scion_guard_va[i];
	}
	if (!scion_guard_valid_words(w) || w[SG_STRIKES] != 0) {
		printf("SCION GUARD: written at %#llx but it does not read back valid\n", scion_guard_pa);
		return;
	}
	printf("SCION GUARD: acknowledged (%u strikes before) at %#llx\n", before, scion_guard_pa);
}

/* From set_proc_name: every exec. Cheap when off (one load). */
void
scion_guard_exec(const char *comm, int pid)
{
	if (scion_guard_call == NULL || os_atomic_load(&scion_guard_done, relaxed) != 0 ||
	    strncmp(comm, "SpringBoard", MAXCOMLEN) != 0) {
		return;
	}
	uint64_t deadline;
	os_atomic_store(&scion_guard_pid, pid, relaxed);
	clock_interval_to_deadline(SCION_GUARD_DELAY_S, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(scion_guard_call, deadline);   /* a later SpringBoard moves the deadline */
}

static void
scion_guard_setup(void)
{
	uint64_t pa = 0;
	if (!PE_parse_boot_argn("scion_guard", &pa, sizeof(pa))) {
		return;
	}
	if (!scion_guard_pa_ok(pa)) {
		printf("SCION GUARD: scion_guard=%#llx refused (8-byte aligned, inside %#llx..%#llx only): nothing mapped\n", pa,
		    SCION_GUARD_WINDOW_LO, SCION_GUARD_WINDOW_HI);
		return;
	}
	scion_guard_va = (volatile uint32_t *)ml_io_map((vm_offset_t)pa, SCION_GUARD_SIZE);
	if (scion_guard_va == NULL) {
		printf("SCION GUARD: %#llx could not be mapped: nothing written\n", pa);
		return;
	}
	scion_guard_pa = pa;
	scion_guard_call = thread_call_allocate_with_options(scion_guard_tick, NULL, THREAD_CALL_PRIORITY_KERNEL,
	    THREAD_CALL_OPTIONS_ONCE);
	printf("SCION GUARD: record at %#llx; acknowledged %u s after SpringBoard starts\n", pa, SCION_GUARD_DELAY_S);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, scion_guard_setup);
#endif /* !SCION_GUARD_HOST */
#endif /* ARM64_BOARD_CONFIG_STARLTE */
