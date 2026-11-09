/*
 * HEROLTE (Galaxy S7, Exynos 8890) internal UFS root device.
 *
 * Derived from the SWEET driver (bsd/dev/sweet_ufs.c). S-Boot leaves the
 * UFSHCI host enabled with the link up after loading BOOT from it; this
 * driver reuses that state with polled SCSI commands on LUN 0, slot 0.
 * Exynos host quirks, from the Samsung kernel (ufs-exynos.c, ufshcd.c):
 *  - UTRD response UPIU and PRDT offsets/lengths are in bytes, not DWORDs
 *    (UFSHCI_QUIRK_BROKEN_DWORD_UTRD);
 *  - UTRLCLR takes a 1 to clear a slot (UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR);
 *  - the vendor HCI_UTRL_NEXUS_TYPE bit of the slot marks a SCSI command;
 *  - PRDT entries describe at most 4 KiB (PRDT_SET_SIZE(12)).
 *
 * Boot-arg ufsprobe=1 only reads: it prints host registers and the GPT of
 * LUN 0. rd=ufsroot0 exposes the GPT partition ufsroot= (default
 * "USERDATA") as ufsroot0; writes need ufsrw=1 and stay inside it.
 */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/msgbuf.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>     /* 11417: kmem_alloc_contig, KMA_* */
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <machine/atomic.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* osfmk/arm/cpu_data.h, not exported to the BSD layer */
extern int get_preemption_level(void);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;

#define REG_CAP         0x00
#define REG_IS          0x20
#define REG_IE          0x24
#define REG_HCS         0x30
#define REG_HCE         0x34
#define REG_UTRLBA      0x50
#define REG_UTRLBAU     0x54
#define REG_UTRLDBR     0x58
#define REG_UTRLCLR     0x5c
#define REG_UTRLRSR     0x60
#define REG_VER         0x08
/* Exynos vendor HCI registers, 0x100 above the standard block. */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* Exynos 9810: vendor HCI at 0x11121100 (+0x1100), same layout as 8890. */
#define VS_BASE                 0x1100
#define HEROLTE_UFS_MAP         0x1300
/* Multi-block commands: 16 entries (64 KiB) per command, the size the
 * exynos9810-woa Windows driver runs with on this controller. */
#define STARLTE_MULTI           16u
/* The 9810 vendor block (vs_hci) starts AT 0x11121100: TXPRDT +0x00,
 * RXPRDT +0x04, UTRL_NEXUS +0x40, DATA_REORDER +0x60, AXIDMA_RWDATA_BURST
 * +0x6C (Linux ufs-exynos.c, Exynos9810Ufs.h, Star2LteUfsHcDxe). The 8890
 * offsets below (+0x100) landed on unrelated registers at 0x11121200. */
#define VS_TXPRDT_ENTRY_SIZE    (VS_BASE + 0x00)
#define VS_RXPRDT_ENTRY_SIZE    (VS_BASE + 0x04)
#define VS_UTRL_NEXUS_TYPE      (VS_BASE + 0x40)
#define VS_DATA_REORDER         (VS_BASE + 0x60)
#define VS_AXIDMA_BURST         (VS_BASE + 0x6c)
#else
#define VS_BASE                 0x0
#define HEROLTE_UFS_MAP         0x300
#define VS_TXPRDT_ENTRY_SIZE    (VS_BASE + 0x100)
#define VS_RXPRDT_ENTRY_SIZE    (VS_BASE + 0x104)
#define VS_UTRL_NEXUS_TYPE      (VS_BASE + 0x140)
#endif

#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define CAP_64AS        (1u << 24)              /* 64-bit DMA addressing */

#define UFS_BLOCK       4096u
#define BOUNCE_SIZE     (1u << 20)
#define PRDT_CHUNK      4096u                   /* Exynos PRDT_SET_SIZE(12) */
#define PRDT_MAX        (BOUNCE_SIZE / PRDT_CHUNK)
#define CMD_TIMEOUT_NS  (5ull * NSEC_PER_SEC)
#define CMD_SPIN_NS     (50ull * NSEC_PER_USEC)
#define CMD_SLEEP_NS    (200ull * NSEC_PER_USEC)

struct utrd {
	uint32_t dw0, dw1, dw2, dw3;
	uint32_t ucdba, ucdbau;
	uint32_t resp;          /* [31:16] response UPIU offset, [15:0] length, BYTES */
	uint32_t prdt;          /* [31:16] PRDT offset, [15:0] PRDT length, BYTES */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;
	} prdt[PRDT_MAX];
};

/* Layout of the contiguous DMA allocation. */
#define DMA_UTRL        0x0000                  /* 1 KiB aligned */
#define DMA_UCD         0x1000                  /* 128 byte aligned, 5 KiB */
#define DMA_BOUNCE      0x4000
#define DMA_SIZE        (DMA_BOUNCE + BOUNCE_SIZE)

static struct {
	vm_offset_t regs;
	vm_offset_t dma;                        /* kernel VA of the DMA allocation */
	uint64_t dma_pa;
	uint8_t lun;
	uint8_t sense_key, asc, ascq;     /* last CHECK CONDITION */
	uint8_t tag;
	uint32_t bounce_off;                    /* data offset in the bounce buffer */
	uint32_t prdt_stride;                   /* STARLTE: 0 = one 4 KiB block per command */
	uint64_t ncmds, last_lba;               /* STARLTE heartbeat */
	uint32_t busy;                          /* a command is in flight */
	uint64_t first_block, nblocks;          /* partition, in 4 KiB device blocks */
	uint32_t secsize;                       /* logical sector size for the BSD layer */
	bool writable;                          /* boot-arg ufsrw=1 */
	bool cache;                             /* boot-arg ufscache=1 */
	bool wait;                              /* boot-arg ufswait=1 */
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
} su;

static LCK_GRP_DECLARE(herolte_ufs_grp, "herolte_ufs");
static LCK_MTX_DECLARE(herolte_ufs_lock, &herolte_ufs_grp);

static inline uint32_t
ufs_rd(uint32_t off)
{
	return *(volatile uint32_t *)(su.regs + off);
}

static inline void
ufs_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(su.regs + off) = v;
}

static void
dma_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
}

static void
dma_clean(uint32_t off, uint32_t len)
{
	clean_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
}

/*
 * One SCSI command in slot 0, data (if any) to/from the bounce buffer
 * (to the device when write is set). Returns 0 or an errno.
 */
static int
ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len, bool write)
{
	struct utrd *utrd = (struct utrd *)(su.dma + DMA_UTRL);
	struct ucd *ucd = (struct ucd *)(su.dma + DMA_UCD);
	uint64_t ucd_pa = su.dma_pa + DMA_UCD, bounce_pa = su.dma_pa + DMA_BOUNCE + su.bounce_off;

	assert(su.bounce_off + len <= BOUNCE_SIZE);
	bzero(utrd, sizeof(*utrd));
	bzero(ucd, sizeof(*ucd));

	ucd->cmd[0] = 0x01;                     /* COMMAND UPIU */
	ucd->cmd[1] = len ? (write ? 0x20 : 0x40) : 0x00;      /* W: data out, R: data in */
	ucd->cmd[2] = su.lun;
	ucd->cmd[3] = ++su.tag;
	ucd->cmd[12] = (uint8_t)(len >> 24);
	ucd->cmd[13] = (uint8_t)(len >> 16);
	ucd->cmd[14] = (uint8_t)(len >> 8);
	ucd->cmd[15] = (uint8_t)len;
	memcpy(&ucd->cmd[16], cdb, cdb_len);

	/* DD: 1 = memory to device, 2 = device to memory. */
	utrd->dw0 = (1u << 28) | ((len ? (write ? 1u : 2u) : 0u) << 25) | (1u << 24);
	utrd->dw2 = 0x0f;                       /* OCS invalid until updated */
	utrd->ucdba = (uint32_t)ucd_pa;
	utrd->ucdbau = (uint32_t)(ucd_pa >> 32);
	utrd->resp = 512u << 16 | 512u;
	if (len) {
		uint32_t n = 0;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		uint32_t stride = su.prdt_stride ? su.prdt_stride : (uint32_t)sizeof(ucd->prdt[0]);
		uint8_t *pt = (uint8_t *)ucd->prdt;
		for (uint32_t done = 0; done < len; done += PRDT_CHUNK, n++) {
			uint32_t chunk = MIN(PRDT_CHUNK, len - done);
			uint32_t *e = (uint32_t *)(pt + n * stride);
			memset(e, 0, stride);           /* FMP words 2, 4..31 = 0: bypass */
			e[0] = (uint32_t)(bounce_pa + done);
			e[1] = (uint32_t)((bounce_pa + done) >> 32);
			e[3] = (chunk - 1) & 0x3ffffu;
		}
		utrd->prdt = 1024u << 16 | (n * stride);
#else
		for (uint32_t done = 0; done < len; done += PRDT_CHUNK, n++) {
			uint32_t chunk = MIN(PRDT_CHUNK, len - done);
			ucd->prdt[n].addr_lo = (uint32_t)(bounce_pa + done);
			ucd->prdt[n].addr_hi = (uint32_t)((bounce_pa + done) >> 32);
			ucd->prdt[n].dbc = chunk - 1;
		}
		utrd->prdt = 1024u << 16 | (n * (uint32_t)sizeof(ucd->prdt[0]));
#endif
	}

	if (su.cache) {
		dma_clean(DMA_UTRL, sizeof(*utrd));
		dma_clean(DMA_UCD, 16 + cdb_len);
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
		if (len) {
			dma_clean(DMA_UCD + offsetof(struct ucd, prdt),
			    utrd->prdt & 0xffff);
		}
	} else {
		dma_sync(DMA_UTRL, sizeof(*utrd));
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len) {
		if (su.cache && write) {
			dma_clean(DMA_BOUNCE + su.bounce_off, len);
		} else {
			dma_sync(DMA_BOUNCE + su.bounce_off, len);
		}
	}
	__builtin_arm_dsb(0xf);

	ufs_wr(VS_UTRL_NEXUS_TYPE, ufs_rd(VS_UTRL_NEXUS_TYPE) | 1u);   /* slot 0: SCSI */
	su.busy = cdb[0];
	if (cdb[0] == 0x28 || cdb[0] == 0x2a) {
		su.last_lba = (uint64_t)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
	}
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t deadline;
	nanoseconds_to_absolutetime(CMD_TIMEOUT_NS, &deadline);
	deadline += mach_absolute_time();
	bool sleepable = su.wait && su.ready && ml_get_interrupts_enabled() &&
	    !ml_at_interrupt_context() && get_preemption_level() == 0;
	uint64_t spin_until = 0, sleep_interval = 0;
	if (sleepable) {
		nanoseconds_to_absolutetime(CMD_SPIN_NS, &spin_until);
		spin_until += mach_absolute_time();
		nanoseconds_to_absolutetime(CMD_SLEEP_NS, &sleep_interval);
	}
	while (ufs_rd(REG_UTRLDBR) & 1) {
		uint64_t now = mach_absolute_time();
		if (now > deadline) {
			ufs_wr(REG_UTRLCLR, 1u);   /* BROKEN_REQ_LIST_CLR */
			printf("HEROLTE UFS: command 0x%02x timed out\n", cdb[0]);
			return ETIMEDOUT;
		}
		if (sleepable && now >= spin_until) {
			uint64_t wake = now + MIN(sleep_interval, deadline - now);
			/* Keep the mutex: slot 0 and the bounce buffer are still owned. */
			if (assert_wait_deadline_with_leeway((event_t)&su.wait, THREAD_UNINT,
			    TIMEOUT_URGENCY_SYS_CRITICAL, wake, 0) == THREAD_WAITING) {
				thread_block(THREAD_CONTINUE_NULL);
			}
		} else {
			delay(1);
		}
	}
	__builtin_arm_dsb(0xf);
	su.busy = 0;
	su.ncmds++;
	ufs_wr(REG_IS, ufs_rd(REG_IS));         /* acknowledge completion status */

	dma_sync(DMA_UTRL, sizeof(*utrd));
	if (su.cache) {
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
	} else {
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len && !(su.cache && write)) {
		dma_sync(DMA_BOUNCE + su.bounce_off, len);
	}

	uint32_t ocs = utrd->dw2 & 0xff;
	if (ocs || ucd->resp[0] != 0x21 || ucd->resp[6] || ucd->resp[7]) {
		const uint8_t *s = &ucd->resp[34];
		unsigned slen = (unsigned)ucd->resp[32] << 8 | ucd->resp[33];
		su.sense_key = slen >= 3 ? s[2] & 0xf : 0xff;
		su.asc = slen >= 13 ? s[12] : 0xff;
		su.ascq = slen >= 14 ? s[13] : 0xff;
		printf("HEROLTE UFS: lun %u cmd 0x%02x failed: ocs 0x%x upiu 0x%x resp 0x%x status 0x%x"
		    " sense len %u key 0x%x asc 0x%x ascq 0x%x\n",
		    su.lun, cdb[0], ocs, ucd->resp[0], ucd->resp[6], ucd->resp[7],
		    slen, su.sense_key, su.asc, su.ascq);
		return EIO;
	}
	return 0;
}

/* READ(10) of count device blocks at lba into the bounce buffer. */
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
	if (lba > UINT32_MAX || count > BOUNCE_SIZE / UFS_BLOCK) {
		return EINVAL;
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (lba + count - 1 > UINT32_MAX) {
		return EINVAL;
	}
	int error = 0;
	uint32_t per = su.prdt_stride ? STARLTE_MULTI : 1u;
	for (uint32_t i = 0; i < count && !error; i += per) {
		uint64_t b = lba + i;
		uint32_t n = MIN(per, count - i);
		uint8_t cdb[10] = {
			0x28, 0,
			(uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b,
			0, (uint8_t)(n >> 8), (uint8_t)n, 0
		};
		su.bounce_off = i * UFS_BLOCK;
		error = ufs_scsi(cdb, sizeof(cdb), n * UFS_BLOCK, false);
	}
	su.bounce_off = 0;
	return error;
#else
	uint8_t cdb[10] = {
		0x28, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);
#endif
}

/*
 * WRITE(10) of count device blocks from the bounce buffer to lba. The range
 * must lie inside the partition: this is the only place data goes out.
 */
static int
ufs_write_blocks(uint64_t lba, uint32_t count)
{
	if (!su.writable || !su.nblocks || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK ||
	    lba < su.first_block || lba - su.first_block >= su.nblocks ||
	    count > su.nblocks - (lba - su.first_block) || lba + count - 1 > UINT32_MAX) {
		printf("HEROLTE UFS: refusing write of %u blocks at 0x%llx\n", count, lba);
		return EPERM;
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	int error = 0;
	uint32_t per = su.prdt_stride ? STARLTE_MULTI : 1u;
	for (uint32_t i = 0; i < count && !error; i += per) {
		uint64_t b = lba + i;
		uint32_t n = MIN(per, count - i);
		uint8_t cdb[10] = {
			0x2a, 0,
			(uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b,
			0, (uint8_t)(n >> 8), (uint8_t)n, 0
		};
		su.bounce_off = i * UFS_BLOCK;
		error = ufs_scsi(cdb, sizeof(cdb), n * UFS_BLOCK, true);
	}
	su.bounce_off = 0;
	return error;
#else
	uint8_t cdb[10] = {
		0x2a, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);
#endif
}

static int
ufs_sync_cache(void)
{
	static const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */
	return ufs_scsi(cdb, sizeof(cdb), 0, false);
}

static int
ufs_find_partition(const char *want)
{
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	int error = ufs_read_blocks(1, 1);
	if (error) {
		return error;
	}
	if (memcmp(b, "EFI PART", 8) != 0) {
		printf("HEROLTE UFS: LUN %u has no GPT\n", su.lun);
		return ENXIO;
	}
	uint64_t entry_lba;
	uint32_t nent, esz;
	memcpy(&entry_lba, b + 72, 8);
	memcpy(&nent, b + 80, 4);
	memcpy(&esz, b + 84, 4);
	if (esz != 128 || nent == 0 || nent * esz > BOUNCE_SIZE) {
		printf("HEROLTE UFS: unsupported GPT entries %u x %u\n", nent, esz);
		return ENXIO;
	}
	error = ufs_read_blocks(entry_lba, (nent * esz + UFS_BLOCK - 1) / UFS_BLOCK);
	if (error) {
		return error;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = b + i * esz;
		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
			if (ch == 0 || ch > 0x7f) {
				break;
			}
			name[c] = (char)ch;
		}
		name[c] = 0;
		if (strcmp(name, want) != 0) {
			continue;
		}
		uint64_t first, last;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (last < first) {
			return ENXIO;
		}
		su.first_block = first;
		su.nblocks = last - first + 1;
		/* ufsrootoff=<MiB> [ufsrootlen=<MiB>] use only that window of the partition
		 * (root image at its start, the console log ring at its end), so another system's data before it survives */
		uint32_t off_mb = 0, len_mb = 0;
		PE_parse_boot_argn("ufsrootoff", &off_mb, sizeof(off_mb));
		PE_parse_boot_argn("ufsrootlen", &len_mb, sizeof(len_mb));
		uint64_t off = ((uint64_t)off_mb << 20) / UFS_BLOCK, len = ((uint64_t)len_mb << 20) / UFS_BLOCK;
		if (off && off < su.nblocks) {
			su.first_block += off;
			su.nblocks -= off;
		}
		if (len && len < su.nblocks) {
			su.nblocks = len;
		}
		if (off || len) {
			printf("HEROLTE UFS: window: %u MiB in, %llu MiB long (first block 0x%llx)\n",
			    off_mb, su.nblocks * UFS_BLOCK >> 20, su.first_block);
		}
		return 0;
	}
	printf("HEROLTE UFS: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}

/*
 * Bring the device to the Active power mode: clear the unit attention of the
 * UFS Device well-known LUN, then START STOP UNIT with power condition 1
 * (ACTIVE), as ufshcd_set_dev_pwr_mode() does. Leaves su.lun at 0.
 */
static int
ufs_wake_device(void)
{
	static const uint8_t tur[6] = { 0x00 };
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	int error = 0;

	su.lun = 0xd0;                          /* UFS Device W-LUN */
	for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
	}
	printf("HEROLTE UFS: device W-LUN TEST UNIT READY %d\n", error);
	for (int i = 0; i < 3 && (error = ufs_scsi(ssu, sizeof(ssu), 0, false)); i++) {
	}
	printf("HEROLTE UFS: START STOP UNIT (ACTIVE) %d\n", error);
	su.lun = 0;
	return error;
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
#define STARLTE_UFSP_PA         0x11130000UL
static void
starlte_ufs_peek(const char *what, uint64_t lba)
{
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	int error = ufs_read_blocks(lba, 1);
	printf("HEROLTE UFS: peek %s lba 0x%llx: %d [0] %02x%02x%02x%02x%02x%02x%02x%02x [1024] %02x%02x%02x%02x\n",
	    what, lba, error, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
	    b[1024], b[1025], b[1026], b[1027]);
}

static void
starlte_ufsp_open(void)
{
	static vm_offset_t ufsp;
	if (!ufsp) {
		ufsp = ml_io_map(STARLTE_UFSP_PA, PAGE_SIZE);
	}
	volatile uint32_t *r = (volatile uint32_t *)ufsp;
	printf("HEROLTE UFS: ufsp rctrl 0x%x rsecurity 0x%x\n", r[0x000 / 4], r[0x010 / 4]);
	for (unsigned n = 0; n < 8; n++) {
		unsigned o = (0x200 + n * 0x10) / 4;
		printf("HEROLTE UFS: ufsp region %u begin 0x%x end 0x%x lun 0x%x ctrl 0x%x\n",
		    n, r[o], r[o + 1], r[o + 2], r[o + 3]);
	}
	/* GPT: BOOT p10 at 19458, USERDATA p25 at 1698432 (4 KiB blocks). */
	starlte_ufs_peek("BOOT", 19458);
	starlte_ufs_peek("USERDATA", 1698432);
	for (unsigned n = 0; n < 8; n++) {
		unsigned o = (0x200 + n * 0x10) / 4;
		r[o] = 0;
		r[o + 1] = 0xffffffffu;
		r[o + 2] = 0xff;
		r[o + 3] = 0xf1;
	}
	__builtin_arm_dsb(0xf);
	printf("HEROLTE UFS: ufsp regions opened, region 0 ctrl now 0x%x\n", r[0x20c / 4]);
	/* SYSTEM p18 at 68736 (ext4: magic 53ef at 1024+56) as a known-good
	 * control, then USERDATA deeper in: is the whole root missing (erased /
	 * discarded) or only its start? HFSX root is 16 GiB = 4194304 blocks. */
	starlte_ufs_peek("SYSTEM", 68736);
	static const uint32_t off[] = { 1, 16, 256, 4096, 65536, 262144, 1048576, 2097152, 4194303 };
	for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
		char what[24] = "USERDATA+";
		unsigned n = 9;
		for (uint32_t v = off[i], d = 1000000000u; d; d /= 10) {
			if (v >= d || n > 9 || d == 1) {
				what[n++] = (char)('0' + (v / d) % 10);
			}
		}
		what[n] = 0;
		starlte_ufs_peek(what, 1698432ull + off[i]);
	}
}
#endif

static int
ufs_take_over_host(void)
{
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* Exynos 9810 after S-Boot: HCE reads 0 while HCS reports the device
	 * present and the lists ready, with S-Boot's list still running. Go by
	 * HCS; a host that is really off only times out the commands. */
	if (!(hce & 1)) {
		printf("HEROLTE UFS: HCE 0x%x with HCS 0x%x, trying anyway\n", hce, hcs);
	}
	if (!(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
#else
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
#endif
		printf("HEROLTE UFS: host not ready (hce 0x%x hcs 0x%x)\n", hce, hcs);
		return ENXIO;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		printf("HEROLTE UFS: requests outstanding from the bootloader\n");
		return EBUSY;
	}
	/* With RAM above 4 GiB (mkadt.py --highmem) the DMA memory can be there too. */
	if (((su.dma_pa + DMA_SIZE - 1) >> 32) && !(ufs_rd(REG_CAP) & CAP_64AS)) {
		printf("HEROLTE UFS: DMA memory at 0x%llx but the host has no 64-bit addressing\n",
		    su.dma_pa);
		return ENXIO;
	}
	ufs_wr(REG_IE, 0);                      /* polled operation */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* Auto-hibernate drops completions after idle on Exynos 9810
	 * (exynos9810-woa Star2LteUfsHcDxe): AHIT (0x18) = 0. */
	ufs_wr(0x18, 0);
#endif
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_wr(REG_UTRLRSR, 0);
	uint64_t list = su.dma_pa + DMA_UTRL;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	/* Never ring a doorbell unless the host uses our list. */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		printf("HEROLTE UFS: host did not take our request list\n");
		return EIO;
	}
	return 0;
}

static int
herolte_ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!su.ready) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !su.writable) ? EROFS : 0;
}

static int
herolte_ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void
herolte_ufs_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * su.secsize;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = su.nblocks * UFS_BLOCK;

	bool rd = buf_flags(bp) & B_READ;

	buf_setresid(bp, count);
	if (!rd && !su.writable) {
		error = EROFS;
		goto done;
	}
	if (off > part_bytes || count > part_bytes - off) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}

	lck_mtx_lock(&herolte_ufs_lock);
	for (uint32_t done = 0; done < count;) {
		uint64_t pos = off + done;
		uint64_t lba = pos / UFS_BLOCK;
		uint32_t skip = (uint32_t)(pos % UFS_BLOCK);
		uint32_t want = MIN(count - done, BOUNCE_SIZE - skip);
		uint32_t blocks = (skip + want + UFS_BLOCK - 1) / UFS_BLOCK;
		void *bounce = (void *)(su.dma + DMA_BOUNCE);
		if (rd) {
			error = ufs_read_blocks(su.first_block + lba, blocks);
			if (!error) {
				memcpy(vaddr + done, (const char *)bounce + skip, want);
			}
		} else {
			/* Partial device blocks at either end: read, merge, write back. */
			if (skip || (want % UFS_BLOCK)) {
				error = ufs_read_blocks(su.first_block + lba, blocks);
			}
			if (!error) {
				memcpy((char *)bounce + skip, vaddr + done, want);
				error = ufs_write_blocks(su.first_block + lba, blocks);
			}
		}
		if (error) {
			break;
		}
		done += want;
		buf_setresid(bp, count - done);
	}
	lck_mtx_unlock(&herolte_ufs_lock);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
herolte_ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)data;
	uint64_t *o = (uint64_t *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = su.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) {
			return ENODEV;
		}
		if (*f < DEV_BSIZE || *f > UFS_BLOCK || (*f & (*f - 1))) {
			return EINVAL;
		}
		su.secsize = *f;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = UFS_BLOCK;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = su.nblocks * UFS_BLOCK / su.secsize;
		return 0;
	case DKIOCISWRITABLE:
		*f = su.writable;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE: {
		if (!su.writable) {
			return 0;
		}
		lck_mtx_lock(&herolte_ufs_lock);
		int error = ufs_sync_cache();
		lck_mtx_unlock(&herolte_ufs_lock);
		return error;
	}
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = BOUNCE_SIZE / su.secsize;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = su.writable ? BOUNCE_SIZE / su.secsize : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
herolte_ufs_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return herolte_ufs_ioctl(dev, cmd, data, flag, p, 0);
}

static int
herolte_ufs_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return herolte_ufs_ioctl(dev, cmd, data, flag, p, 1);
}

static int
herolte_ufs_psize(__unused dev_t dev)
{
	return su.ready ? (int)MIN(su.nblocks * UFS_BLOCK / su.secsize, INT_MAX) : -1;
}

static int
herolte_ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(herolte_ufs_strategy, NULL, dev, B_READ, minphys, uio, (int)su.secsize);
}

static int
herolte_ufs_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (!su.writable) {
		return EROFS;
	}
	return physio(herolte_ufs_strategy, NULL, dev, B_WRITE, minphys, uio, (int)su.secsize);
}

static const struct bdevsw herolte_ufs_bdevsw = {
	.d_open     = herolte_ufs_open,
	.d_close    = herolte_ufs_close,
	.d_strategy = herolte_ufs_strategy,
	.d_ioctl    = herolte_ufs_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = herolte_ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw herolte_ufs_cdevsw = {
	.d_open       = herolte_ufs_open,
	.d_close      = herolte_ufs_close,
	.d_read       = herolte_ufs_read,
	.d_write      = herolte_ufs_write,
	.d_ioctl      = herolte_ufs_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/*
 * Persistent console log ("herolog"): the console ring (a5xelte_emmc.c) is
 * copied every 10 s to the last 4 MiB + 4 KiB of the root partition, beyond
 * the HFSX volume, and the device cache is flushed. Block 0 of that area:
 *   "HEROLOG1" | u32 version 1 | u32 ring size | u64 total bytes | u64 seq
 * ring offset of byte n = n % ring size. Read it from TWRP with
 * tools/herolte-log-pull.py. Needs ufsrw=1; boot-arg herolog=0 disables it.
 */
extern void a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;
extern void sweet_sample_start(void);

struct herolte_log_header {
	char magic[8];
	uint32_t version, size;
	uint64_t total, seq;
};

static int herolte_log_chan;
static uint64_t herolte_log_first, herolte_log_flushed, herolte_log_seq;
static bool herolte_log_started;      /* herolte_log_start() decided to log */

/* Copy what the console ring gained since the last flush, then the header. */
static int
herolte_log_write(void)
{
	const char *ring;
	uint32_t rsize;
	uint64_t total;
	uint8_t *bounce = (uint8_t *)(su.dma + DMA_BOUNCE);
	a5xelte_log_snapshot(&ring, &rsize, &total);
	if (total == herolte_log_flushed) {
		return 0;
	}
	uint64_t first = herolte_log_first;
	uint64_t pos = total - herolte_log_flushed >= rsize ? total - rsize : herolte_log_flushed;
	uint64_t left = total - pos;
	int error = 0;
	lck_mtx_lock(&herolte_ufs_lock);
	while (left && !error) {
		uint32_t blk = (uint32_t)((pos % rsize) / UFS_BLOCK);
		uint32_t skip = (uint32_t)(pos % UFS_BLOCK);
		uint32_t n = (uint32_t)MIN((uint64_t)(BOUNCE_SIZE / UFS_BLOCK),
		    MIN((uint64_t)(rsize / UFS_BLOCK - blk), (left + skip + UFS_BLOCK - 1) / UFS_BLOCK));
		memcpy(bounce, ring + (size_t)blk * UFS_BLOCK, (size_t)n * UFS_BLOCK);
		error = ufs_write_blocks(first + 1 + blk, n);
		uint64_t adv = (uint64_t)n * UFS_BLOCK - skip;
		pos += adv;
		left -= MIN(adv, left);
	}
	if (!error) {
		struct herolte_log_header h = {
			.magic = { 'H', 'E', 'R', 'O', 'L', 'O', 'G', '1' },
			.version = 1, .size = rsize, .total = total, .seq = ++herolte_log_seq,
		};
		bzero(bounce, UFS_BLOCK);
		memcpy(bounce, &h, sizeof(h));
		error = ufs_write_blocks(first, 1);
	}
	if (!error) {
		error = ufs_sync_cache();
	}
	lck_mtx_unlock(&herolte_ufs_lock);
	if (!error) {
		herolte_log_flushed = total;
	}
	return error;
}

/*
 * Synchronous flush for proc_handle_critical_exit (kern_exit.c): launchd's
 * death panics within ~30 s of boot, before the 10 s thread's first flush
 * landed (xnu-11417 bring-up). Thread context, before the panic.
 */
static void
herolte_log_setup(void)
{
	const char *ring;
	uint32_t rsize;
	uint64_t total;
	a5xelte_log_snapshot(&ring, &rsize, &total);
	herolte_log_first = su.first_block + su.nblocks - (1 + rsize / UFS_BLOCK);
}

void herolte_log_flush_now(void);
void
herolte_log_flush_now(void)
{
	/* An init that dies within moments of the root mount can beat the log
	 * thread's start: set the area up here then (needs the writable root). */
	if (herolte_log_first == 0 && su.writable && herolte_log_started) {
		herolte_log_setup();
	}
	if (herolte_log_first == 0) {
		printf("HEROLTE LOG: no flash log to flush\n");
		return;
	}
	int error = herolte_log_write();
	printf("HEROLTE LOG: flushed before the critical-exit panic (%d)\n", error);
	(void)herolte_log_write();      /* the line above */
}

/*
 * Boot-arg sweetdumpfile=<path>: right before the sweetreboot reset, print
 * the last 128 KiB of that file line by line into the console (and so the
 * persistent log).  Daemons whose os_log is mirrored to a file with
 * ACTIVITY_LOG_STDERR=1 + StandardErrorPath become readable this way.
 */
static void
starlte_dump_file(void)
{
	static char path[128];
	vnode_t vp = NULLVP;
	vfs_context_t ctx = vfs_context_kernel();
	off_t size = 0, off;
	char *buf, line[400];
	size_t ln = 0;

	if (!PE_parse_boot_argn("sweetdumpfile", path, sizeof(path))) {
		return;
	}
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
		printf("SWEET DUMP: cannot open %s\n", path);
		return;
	}
	(void)vnode_size(vp, &size, ctx);
	off = size > 131072 ? size - 131072 : 0;
	printf("SWEET DUMP: %s, %lld bytes, from offset %lld\n", path, size, off);
	buf = kalloc_data(4096, Z_WAITOK | Z_ZERO);
	for (; buf != NULL && off < size; off += 4096) {
		int resid = 0;
		int n = (int)MIN(4096, size - off);
		if (vn_rdwr(UIO_READ, vp, buf, n, off, UIO_SYSSPACE, 0,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) != 0) {
			break;
		}
		for (int i = 0; i < n - resid; i++) {
			char c = buf[i];
			if (c == '\n' || ln == sizeof(line) - 1) {
				line[ln] = '\0';
				printf("DUMP| %s\n", line);
				ln = 0;
				if (c == '\n') {
					continue;
				}
			}
			line[ln++] = (c >= 0x20 && c < 0x7f) ? c : '.';
		}
	}
	if (ln) {
		line[ln] = '\0';
		printf("DUMP| %s\n", line);
	}
	if (buf) {
		kfree_data(buf, 4096);
	}
	(void)vnode_close(vp, FREAD, ctx);
	printf("SWEET DUMP: end\n");
}

/*
 * Boot-arg sweetreboot=N: N seconds after the log thread starts, flush the
 * persistent log and reset into recovery (TWRP), like the loader's menu does:
 * PMU INFORM2 = 0x12345678, INFORM3 = 0x12345674 (recovery), SWRESET.
 * Lets a test boot run unattended: flash, auto-boot, log, back to TWRP.
 * A kernel panic in such a boot does the same (sweet_panic_hook, called from
 * panic_spin_forever): the panic log's tail goes to the ramoops pmsg ring at
 * 0xFED14000 ("DBGC" sig/start/size, as the loader and the woa UEFI write it),
 * which TWRP shows as /sys/fs/pstore/pmsg-ramoops-0, then the reset. Both
 * mappings are made at boot: nothing may be mapped inside a panic.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#define STARLTE_PMU_PA          0x14060000
#define STARLTE_PMSG_PA         0xFED14000
#define STARLTE_PMSG_DATA       0x3F00u         /* the loader's cap, clear of the UEFI latches */
static volatile uint32_t *starlte_pmu_va;
static volatile uint8_t *starlte_pmsg_va;
extern void (*sweet_panic_hook)(void);
extern char *debug_buf_base;
extern char *debug_buf_ptr;

static void
starlte_reset_to_recovery(void)
{
	volatile uint32_t *pmu = starlte_pmu_va;
	if (pmu == NULL) {
		return;
	}
	(void)ml_set_interrupts_enabled(FALSE);
	pmu[0x808 / 4] = 0x12345678U;
	pmu[0x80c / 4] = 0x12345674U;
	__asm__ volatile ("dsb sy" ::: "memory");
	pmu[0x400 / 4] = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (;;) {
		__asm__ volatile ("wfe");
	}
}

static void
starlte_pmsg_put(volatile uint8_t *d, uint32_t *o, const char *s, uint32_t n)
{
	while (n-- > 0 && *o < STARLTE_PMSG_DATA) {
		d[(*o)++] = (uint8_t)*s++;
	}
}

/*
 * The panic log's head (panic string, panicked thread, backtrace; its tail is
 * the binary stackshot) and the console's last lines before the panic, into
 * the ramoops pmsg ring that TWRP shows as /sys/fs/pstore/pmsg-ramoops-0.
 */
static void
starlte_panic_to_recovery(void)
{
	static const char hdr[] = "STARBITE: XNU panic (sweetreboot), panic log head:\n";
	static const char mid[] = "\n--- console before the panic:\n";
	volatile uint32_t *h = (volatile uint32_t *)(void *)starlte_pmsg_va;
	volatile uint8_t *d;
	const char *b = debug_buf_base, *e = debug_buf_ptr;
	const char *ring;
	uint32_t rsize, o = 0;
	uint64_t total;

	if (h != NULL) {
		d = starlte_pmsg_va + 12;
		starlte_pmsg_put(d, &o, hdr, sizeof(hdr) - 1);
		if (b != NULL && e > b) {
			starlte_pmsg_put(d, &o, b, (uint32_t)MIN(e - b, 11 * 1024));
		}
		starlte_pmsg_put(d, &o, mid, sizeof(mid) - 1);
		a5xelte_log_snapshot(&ring, &rsize, &total);
		uint64_t n = MIN(total, MIN((uint64_t)rsize, (uint64_t)(STARLTE_PMSG_DATA - o)));
		for (uint64_t k = total - n; k < total; k++) {
			starlte_pmsg_put(d, &o, &ring[k % rsize], 1);
		}
		h[0] = 0x43474244U;                     /* "DBGC" */
		h[1] = o % STARLTE_PMSG_DATA;
		h[2] = o;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	starlte_reset_to_recovery();
}

static void
starlte_reboot_setup(void)
{
	starlte_pmu_va = (volatile uint32_t *)ml_io_map(STARLTE_PMU_PA, PAGE_SIZE);
	starlte_pmsg_va = (volatile uint8_t *)ml_io_map(STARLTE_PMSG_PA, 0x4000);
	if (starlte_pmu_va != NULL && starlte_pmsg_va != NULL) {
		sweet_panic_hook = starlte_panic_to_recovery;
	}
	printf("HEROLTE LOG: sweetreboot armed (PMU %p, pmsg %p, panics reset too)\n",
	    starlte_pmu_va, starlte_pmsg_va);
}

/* Armed as soon as kmem works, so early panics reach TWRP too. */
static void
starlte_reboot_early(void)
{
	uint32_t secs = 0;
	if (PE_parse_boot_argn("sweetreboot", &secs, sizeof(secs)) && secs != 0) {
		starlte_reboot_setup();
	}
}
STARTUP(KMEM, STARTUP_RANK_LAST, starlte_reboot_early);

/*
 * sweetreboot=N with no UFS log (rd=sdroot0, rd=md0, a read-only UFS root):
 * herolte_log_start() never starts the log thread there, so its N-second reset
 * never came and a hang needed the buttons (macOS t0 boot, 2026-11-03). This
 * thread does it N seconds after boot unless the UFS log thread took over:
 * the console tail goes to the ramoops pmsg ring, then the reset into TWRP.
 */
static void
starlte_reboot_timer_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	uint32_t secs = 0;
	PE_parse_boot_argn("sweetreboot", &secs, sizeof(secs));
	(void)tsleep(&chan, PZERO, "sweetreboot", (int)(secs * hz));
	if (herolte_log_started) {
		/* herolte_log_thread flushes and resets itself; iOS 27 on STARLTE: if it
		 * is stuck (UFS write hung, or it stopped on a write error), reset anyway
		 * 60 s later with the console tail in pmsg. */
		(void)tsleep(&chan, PZERO, "sweetreboot", (int)(60 * hz));
	}
	printf("HEROLTE LOG: sweetreboot=%u reached, log thread %s: console tail to pmsg, reset to recovery\n",
	    secs, herolte_log_started ? "did not reset in 60 s" : "never started");
	{
		/* sdlog=1: the whole kernel log ring to the card's test range too (starlte_sd.c) */
		extern struct msgbuf *msgbufp;
		extern int starlte_sd_dump_log(const void *, uint32_t, uint32_t);
		int lr = starlte_sd_dump_log(msgbufp->msg_bufc, (uint32_t)msgbufp->msg_size, (uint32_t)msgbufp->msg_bufx);
		printf("HEROLTE LOG: kernel log to card: %d\n", lr);
	}
	starlte_dump_file();
	starlte_panic_to_recovery();    /* no panic log: header + console tail */
}

static void
starlte_reboot_timer(void)
{
	uint32_t secs = 0;
	thread_t th;
	if (PE_parse_boot_argn("sweetreboot", &secs, sizeof(secs)) && secs > 1 && starlte_pmu_va != NULL &&
	    kernel_thread_start(starlte_reboot_timer_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		printf("HEROLTE LOG: sweetreboot timer %u s armed (independent of the UFS log)\n", secs);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, starlte_reboot_timer);

/*
 * Boot-arg sweetwdog=SECONDS (0 or absent: off; at least 60): a watchdog for hard hangs. A phone that stops with no
 * panic (9 November: dead 66 s into a boot, the console gone, nothing in pstore) never comes back by itself.
 *
 * Every 2 s a timer call (so: from the timer interrupt, with no thread to schedule and no lock taken) looks at two signs
 * of life. The kernel's: a thread of ordinary priority that wakes every second and notes the time. User space's: a
 * system call or Mach trap from EL0 since the last look (sweet_wdog_mark, osfmk/arm64/sleh.c). The watchdog is armed
 * by the first system call it sees, so a boot that never reaches user space is the panic paths' business, not its.
 * When either sign is older than SECONDS it writes one line (which sign, both ages, the uptime) and the console's
 * tail into the ramoops pmsg ring, as a panic does (TWRP: /sys/fs/pstore/pmsg-ramoops-0), and resets into recovery
 * through the PMU. It cannot see a hang in which no CPU takes timer interrupts any more (that needs the SoC's own
 * watchdog timer, which this kernel does not drive), nor one on the single CPU its timer call lives on.
 *
 * kern.sweetwdog_test (the USB console: "sysctl kern.sweetwdog_test=N") provokes it without hanging anything:
 * 1 = signs of life from user space are not counted any more, 2 = the kernel thread stops noting the time.
 * kern.sweetwdog_ages reads "kernel-age user-age" in ms as the last look saw them.
 */
#include <kern/timer_call.h>
#include <sys/sysctl.h>
extern volatile uint32_t sweet_wdog_mark;
static uint32_t starlte_wdog_secs, starlte_wdog_test, starlte_wdog_armed;
static volatile uint64_t starlte_wdog_kbeat, starlte_wdog_ubeat;
static uint64_t starlte_wdog_limit, starlte_wdog_step, starlte_wdog_kage_ms, starlte_wdog_uage_ms;
static timer_call_data_t starlte_wdog_tc;
static char starlte_wdog_why[160];
SYSCTL_UINT(_kern, OID_AUTO, sweetwdog_test, CTLFLAG_RW | CTLFLAG_LOCKED, &starlte_wdog_test, 0,
    "hang watchdog test: 1 = ignore user space's signs of life, 2 = stop the kernel thread's");
SYSCTL_QUAD(_kern, OID_AUTO, sweetwdog_kernel_age_ms, CTLFLAG_RD | CTLFLAG_LOCKED, &starlte_wdog_kage_ms,
    "hang watchdog: ms since the kernel thread's last sign of life, at the last look");
SYSCTL_QUAD(_kern, OID_AUTO, sweetwdog_user_age_ms, CTLFLAG_RD | CTLFLAG_LOCKED, &starlte_wdog_uage_ms,
    "hang watchdog: ms since user space's last system call, at the last look");

/* From the timer interrupt: the reason and the console's tail into the pmsg ring, then the reset. No lock, no allocation. */
static void
starlte_wdog_fire(void)
{
	static const char mid[] = "--- console before the hang:\n";
	volatile uint32_t *h = (volatile uint32_t *)(void *)starlte_pmsg_va;
	const char *ring;
	uint32_t rsize, o = 0;
	uint64_t total;

	if (h != NULL) {
		volatile uint8_t *d = starlte_pmsg_va + 12;
		starlte_pmsg_put(d, &o, starlte_wdog_why, (uint32_t)strlen(starlte_wdog_why));
		starlte_pmsg_put(d, &o, mid, sizeof(mid) - 1);
		a5xelte_log_snapshot(&ring, &rsize, &total);
		uint64_t n = MIN(total, MIN((uint64_t)rsize, (uint64_t)(STARLTE_PMSG_DATA - o)));
		for (uint64_t k = total - n; k < total; k++) {
			starlte_pmsg_put(d, &o, &ring[k % rsize], 1);
		}
		h[0] = 0x43474244U;                     /* "DBGC" */
		h[1] = o % STARLTE_PMSG_DATA;
		h[2] = o;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	starlte_reset_to_recovery();
}

static void
starlte_wdog_tick(__unused timer_call_param_t p0, __unused timer_call_param_t p1)
{
	uint64_t now = mach_absolute_time(), kage = 0, uage = 0;

	if (sweet_wdog_mark != 0) {
		if (starlte_wdog_test != 1) {
			starlte_wdog_ubeat = now;
			if (!starlte_wdog_armed) {
				starlte_wdog_armed = 1;         /* (the first system call: user space is up) */
				starlte_wdog_kbeat = now;
			}
		}
		sweet_wdog_mark = 0;
	}
	if (starlte_wdog_armed) {
		kage = now - starlte_wdog_kbeat;
		uage = now - starlte_wdog_ubeat;
		absolutetime_to_nanoseconds(kage, &starlte_wdog_kage_ms);
		absolutetime_to_nanoseconds(uage, &starlte_wdog_uage_ms);
		starlte_wdog_kage_ms /= NSEC_PER_MSEC;
		starlte_wdog_uage_ms /= NSEC_PER_MSEC;
		if (kage > starlte_wdog_limit || uage > starlte_wdog_limit) {
			uint64_t up = 0;
			absolutetime_to_nanoseconds(now, &up);
			snprintf(starlte_wdog_why, sizeof(starlte_wdog_why),
			    "STARBITE: HANG WATCHDOG (sweetwdog=%u): no sign of life from %s; kernel thread %llu ms ago, user space %llu ms ago, "
			    "up %llu s, test %u\n", starlte_wdog_secs, kage > starlte_wdog_limit ? "the kernel's thread" : "user space",
			    starlte_wdog_kage_ms, starlte_wdog_uage_ms, up / NSEC_PER_SEC, starlte_wdog_test);
			starlte_wdog_fire();
		}
	}
	timer_call_enter(&starlte_wdog_tc, now + starlte_wdog_step, TIMER_CALL_SYS_CRITICAL);
}

static void
starlte_wdog_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	/* armed from here, a running thread, not from the start-up hook: the timer calls are surely up */
	timer_call_setup(&starlte_wdog_tc, starlte_wdog_tick, NULL);
	timer_call_enter(&starlte_wdog_tc, mach_absolute_time() + starlte_wdog_step, TIMER_CALL_SYS_CRITICAL);
	for (;;) {
		if (starlte_wdog_test != 2) {
			starlte_wdog_kbeat = mach_absolute_time();
		}
		(void)tsleep(&chan, PZERO, "sweetwdog", hz);
	}
}

static void
starlte_wdog_start(void)
{
	uint32_t secs = 0;
	thread_t th;
	if (!PE_parse_boot_argn("sweetwdog", &secs, sizeof(secs)) || secs == 0) {
		return;
	}
	if (secs < 60) {
		secs = 60;
	}
	if (starlte_pmu_va == NULL || starlte_pmsg_va == NULL) {
		starlte_reboot_setup();                 /* (no sweetreboot on the line: the reset's mappings are still needed) */
	}
	if (starlte_pmu_va == NULL) {
		printf("HEROLTE LOG: sweetwdog: not armed (no PMU mapping)\n");
		return;
	}
	starlte_wdog_secs = secs;
	nanoseconds_to_absolutetime((uint64_t)secs * NSEC_PER_SEC, &starlte_wdog_limit);
	nanoseconds_to_absolutetime(2ull * NSEC_PER_SEC, &starlte_wdog_step);
	starlte_wdog_kbeat = starlte_wdog_ubeat = mach_absolute_time();
	sweet_wdog_mark = 0;                            /* the first system call arms it */
	if (kernel_thread_start(starlte_wdog_thread, NULL, &th) != KERN_SUCCESS) {
		sweet_wdog_mark = 1;
		printf("HEROLTE LOG: sweetwdog: not armed (no thread)\n");
		return;
	}
	thread_deallocate(th);
	printf("HEROLTE LOG: sweetwdog=%u: the hang watchdog starts with the first system call (looks every 2 s; "
	    "kern.sweetwdog_test provokes it)\n", secs);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, starlte_wdog_start);
#endif

static void
starlte_reboot_recovery(void)
{
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	starlte_reset_to_recovery();
	printf("HEROLTE LOG: PMU mapping failed, no reboot\n");
#else
	printf("HEROLTE LOG: sweetreboot is STARLTE only\n");
#endif
}

/* iOS 27 on STARLTE: the USB console's "return" (bsd/dev/starlte_usbcon.c loader_return):
 * the dump file, the log flushed, then the reset into TWRP, as sweetreboot does. */
void starlte_return_to_recovery(void);
void
starlte_return_to_recovery(void)
{
	printf("HEROLTE LOG: return asked on the USB console: flushing and rebooting to recovery\n");
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	starlte_dump_file();
#endif
	herolte_log_flush_now();
	starlte_reboot_recovery();
}

static void
herolte_log_thread(__unused void *arg, __unused wait_result_t wr)
{
	const char *ring;
	uint32_t rsize;
	uint64_t total;
	uint32_t ticks = 0;
	a5xelte_log_snapshot(&ring, &rsize, &total);
	if (herolte_log_first == 0) {
		herolte_log_setup();
	}
	printf("HEROLTE LOG: %u KiB ring at partition block %llu\n",
	    rsize >> 10, herolte_log_first - su.first_block);
	uint32_t reboot_s = 0;
	PE_parse_boot_argn("sweetreboot", &reboot_s, sizeof(reboot_s));
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (reboot_s != 0 && starlte_pmu_va == NULL) {
		starlte_reboot_setup();
	}
#endif
	for (;;) {
		(void)tsleep(&herolte_log_chan, PZERO, "herolog", hz * 10);
		/* Heartbeat: gaps between these lines show CPU starvation. */
		if (++ticks % 6 == 0) {
			printf("HEROLTE LOG: alive %u s\n", ticks * 10);
		}
		if (reboot_s != 0 && ticks * 10 >= reboot_s) {
			printf("HEROLTE LOG: sweetreboot=%u reached, flushing and rebooting to recovery\n", reboot_s);
			starlte_dump_file();
			(void)herolte_log_write();
			starlte_reboot_recovery();
		}
		int error = herolte_log_write();
		if (error) {
			printf("HEROLTE LOG: write failed (%d), stopping\n", error);
			herolte_log_first = 0;
			return;
		}
	}
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* Every 15 s for 10 minutes: is the root still being read? */
static void
starlte_ufs_heartbeat(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	for (int i = 0; i < 40; i++) {
		(void)tsleep(&chan, PZERO, "ufshb", 15 * hz);
		printf("HEROLTE UFS: hb %d: %llu cmds, last lba 0x%llx, busy 0x%x, dbr 0x%x\n",
		    i, su.ncmds, su.last_lba, su.busy, ufs_rd(REG_UTRLDBR));
		extern void starlte_brand_banner(bool full);
		starlte_brand_banner(false);
	}
}
#endif

static void
herolte_log_start(void)
{
	uint32_t on = 1;
	thread_t thread;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (kernel_thread_start(starlte_ufs_heartbeat, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (!su.writable) {
		/* Read-only root: no flash log, but sample the processes named
		 * in boot-arg sweetsample (bsd/dev/sweet_sample.c). */
		sweet_sample_start();
		return;
	}
#endif
	PE_parse_boot_argn("herolog", &on, sizeof(on));
	if (!on || !su.writable || su.nblocks < 2 * (1 + (4u << 20) / UFS_BLOCK)) {
		return;
	}
	herolte_log_started = true;
	if (kernel_thread_start(herolte_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	/* Thread backtraces of SpringBoard/backboardd (bsd/dev/sweet_sample.c). */
	sweet_sample_start();
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/*
 * Multi-block commands (boot-arg ufsmulti=1 only). Earlier tries wrote the
 * PRDT size registers at the 8890 offsets (+0x100 too high on the 9810) and
 * left DATA_REORDER / AXI DMA as S-Boot had them. This one programs the four
 * vendor registers exactly like Linux and the exynos9810-woa UEFI
 * (TXPRDT/RXPRDT = PRDT_PREFECT_EN | PRDT_SET_SIZE(12), DATA_REORDER = 0xa,
 * AXIDMA = WLU_EN | BURST_LEN(3)), checks a single-block read still returns
 * the same data, then compares one 16-entry read (one entry per 4 KiB) with
 * it. Entry size: the FMP descriptor type (UFSPRSECURITY DESCTYPE, secure
 * only) picks it. S-Boot leaves DESCTYPE 0 = standard 16-byte UFSHCI entries
 * (the woa UEFI ran 512 KiB multi-entry reads like that); Windows uses 128-byte
 * entries only after its UEFI set DESCTYPE 3. 128-byte entries on DESCTYPE 0
 * returned wrong data with OCS 0 and left the host failing every command
 * (2026-10-27). Reads only; any mismatch restores the registers and stays at
 * one block per command.
 */
static void
starlte_ufs_pick_stride(void)
{
	static const uint64_t probe = 1;                /* GPT header + entries */
	const uint32_t nb = STARLTE_MULTI;
	uint8_t *b = (uint8_t *)(su.dma + DMA_BOUNCE);
	const uint32_t ref = 512u << 10;                /* reference copy, upper half */
	uint32_t tx0 = ufs_rd(VS_TXPRDT_ENTRY_SIZE), rx0 = ufs_rd(VS_RXPRDT_ENTRY_SIZE);
	uint32_t ro0 = ufs_rd(VS_DATA_REORDER), ax0 = ufs_rd(VS_AXIDMA_BURST);

	printf("HEROLTE UFS: vs_hci tx 0x%x rx 0x%x nexus 0x%x reorder 0x%x axidma 0x%x\n",
	    tx0, rx0, ufs_rd(VS_UTRL_NEXUS_TYPE), ro0, ax0);
	su.prdt_stride = 0;
	int e0 = ufs_read_blocks(probe, nb);
	memcpy(b + ref, b, nb * UFS_BLOCK);
	ufs_wr(VS_TXPRDT_ENTRY_SIZE, 0x8000000cu);
	ufs_wr(VS_RXPRDT_ENTRY_SIZE, 0x8000000cu);
	ufs_wr(VS_DATA_REORDER, 0xau);
	ufs_wr(VS_AXIDMA_BURST, 0x98000003u);
	memset(b, 0xa5, nb * UFS_BLOCK);
	int e1 = ufs_read_blocks(probe, nb);
	bool single_ok = !e0 && !e1 && memcmp(b, b + ref, nb * UFS_BLOCK) == 0;
	printf("HEROLTE UFS: vs_hci now tx 0x%x rx 0x%x reorder 0x%x axidma 0x%x: single-block %d/%d %s\n",
	    ufs_rd(VS_TXPRDT_ENTRY_SIZE), ufs_rd(VS_RXPRDT_ENTRY_SIZE), ufs_rd(VS_DATA_REORDER),
	    ufs_rd(VS_AXIDMA_BURST), e0, e1, single_ok ? "same" : "DIFFERENT");
	if (!single_ok) {
		goto restore;
	}
	memset(b, 0xa5, nb * UFS_BLOCK);
	su.prdt_stride = 16;
	uint8_t cdb[10] = { 0x28, 0, 0, 0, 0, (uint8_t)probe, 0, 0, (uint8_t)nb, 0 };
	int e2 = ufs_scsi(cdb, sizeof(cdb), nb * UFS_BLOCK, false);
	bool same = !e2 && memcmp(b, b + ref, nb * UFS_BLOCK) == 0;
	printf("HEROLTE UFS: %u x 16-byte entries: %d %s\n", nb, e2, same ? "MATCH" : "differ");
	if (same) {
		printf("HEROLTE UFS: multi-block commands on, %u blocks per command\n", STARLTE_MULTI);
		return;
	}
	su.prdt_stride = 0;
restore:
	ufs_wr(VS_TXPRDT_ENTRY_SIZE, tx0);
	ufs_wr(VS_RXPRDT_ENTRY_SIZE, rx0);
	ufs_wr(VS_DATA_REORDER, ro0);
	ufs_wr(VS_AXIDMA_BURST, ax0);
	printf("HEROLTE UFS: multi-block self-test failed, registers restored, one block per command\n");
}
#endif

static int
herolte_ufs_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	char want[64] = "USERDATA";
	int error;

	if (SecureDTLookupEntry(NULL, "/herolte-ufs", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("HEROLTE UFS: no /herolte-ufs node in the device tree\n");
		return ENXIO;
	}
	PE_parse_boot_argn("ufsroot", want, sizeof(want));
	uint32_t rw = 0;
	PE_parse_boot_argn("ufsrw", &rw, sizeof(rw));
	uint32_t cache = 0, wait = 0;
	PE_parse_boot_argn("ufscache", &cache, sizeof(cache));
	PE_parse_boot_argn("ufswait", &wait, sizeof(wait));
	su.cache = (cache == 1);
	su.wait = (wait == 1);

	su.regs = ml_io_map(reg[0], MAX(reg[1], (uintptr_t)HEROLTE_UFS_MAP));
	if (kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("HEROLTE UFS: no contiguous DMA memory\n");
		return ENOMEM;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)su.dma);
	if (!pn) {
		return ENOMEM;
	}
	su.dma_pa = ptoa_64(pn);
	su.lun = 0;
	su.secsize = DEV_BSIZE;

	lck_mtx_lock(&herolte_ufs_lock);
	error = ufs_take_over_host();
	if (!error) {
		(void)ufs_wake_device();
	}
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention. */
		for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
		}
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (!error) {
		starlte_ufsp_open();
		uint32_t multi = 0;     /* experimental: a failed test can stop the host */
		PE_parse_boot_argn("ufsmulti", &multi, sizeof(multi));
		if (multi) {
			starlte_ufs_pick_stride();
		}
	}
#endif
	if (!error) {
		error = ufs_find_partition(want);
	}
	lck_mtx_unlock(&herolte_ufs_lock);
	if (error) {
		return error;
	}

	su.bmajor = bdevsw_add(-1, &herolte_ufs_bdevsw);
	if (su.bmajor < 0) {
		return ENXIO;
	}
	su.cmajor = cdevsw_add_with_bdev(-1, &herolte_ufs_cdevsw, su.bmajor);
	if (su.cmajor < 0) {
		return ENXIO;
	}
	su.bdev = makedev(su.bmajor, 0);
	devfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
	devfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
	/* Only now, with the partition bounds known, can writes be allowed. */
	su.writable = (rw != 0);
	su.ready = true;
	printf("HEROLTE UFS: \"%s\" on LUN %u: first block 0x%llx, %llu MiB, %s, DMA at 0x%llx\n",
	    want, su.lun, su.first_block, su.nblocks * UFS_BLOCK >> 20,
	    su.writable ? "read-write" : "read-only", su.dma_pa);
	herolte_log_start();
	return 0;
}

/* Boot-arg ufsprobe=1: print the host state and the LUN 0 GPT. Read only. */
void herolte_ufs_probe(void);

void
herolte_ufs_probe(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, "/herolte-ufs", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("HEROLTE UFS: probe: no /herolte-ufs node\n");
		return;
	}
	if (!su.regs) {
		su.regs = ml_io_map(reg[0], MAX(reg[1], (uintptr_t)HEROLTE_UFS_MAP));
	}
	printf("HEROLTE UFS: probe: cap 0x%x ver 0x%x hcs 0x%x hce 0x%x is 0x%x utrlba 0x%x:%x dbr 0x%x rsr 0x%x\n",
	    ufs_rd(REG_CAP), ufs_rd(REG_VER), ufs_rd(REG_HCS), ufs_rd(REG_HCE), ufs_rd(REG_IS),
	    ufs_rd(REG_UTRLBAU), ufs_rd(REG_UTRLBA), ufs_rd(REG_UTRLDBR), ufs_rd(REG_UTRLRSR));
	printf("HEROLTE UFS: probe: txprdt 0x%x rxprdt 0x%x nexus 0x%x\n",
	    ufs_rd(VS_TXPRDT_ENTRY_SIZE), ufs_rd(VS_RXPRDT_ENTRY_SIZE), ufs_rd(VS_UTRL_NEXUS_TYPE));
	if (!su.dma && kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("HEROLTE UFS: probe: no contiguous DMA memory\n");
		return;
	}
	su.dma_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)su.dma));
	su.lun = 0;

	lck_mtx_lock(&herolte_ufs_lock);
	int error = ufs_take_over_host();
	if (!error) {
		(void)ufs_wake_device();
	}
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		for (unsigned lun = 0; lun < 4; lun++) {
			int e = 0;
			su.lun = (uint8_t)lun;
			for (int i = 0; i < 3 && (e = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
			}
			printf("HEROLTE UFS: probe: lun %u TEST UNIT READY %d\n", lun, e);
		}
		su.lun = 0;
	}
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	if (!error) {
		error = ufs_read_blocks(1, 1);
		printf("HEROLTE UFS: probe: READ(10) LBA1 %d, signature %.8s\n", error,
		    error ? "-" : (const char *)b);
	}
	if (!error && memcmp(b, "EFI PART", 8) == 0) {
		uint64_t entry_lba;
		uint32_t nent, esz;
		memcpy(&entry_lba, b + 72, 8);
		memcpy(&nent, b + 80, 4);
		memcpy(&esz, b + 84, 4);
		printf("HEROLTE UFS: probe: GPT entries %u x %u at LBA %llu\n", nent, esz, entry_lba);
		if (esz == 128 && nent && nent * esz <= BOUNCE_SIZE &&
		    !ufs_read_blocks(entry_lba, (nent * esz + UFS_BLOCK - 1) / UFS_BLOCK)) {
			for (uint32_t i = 0; i < nent; i++) {
				const uint8_t *e = b + i * esz;
				uint64_t first, last;
				char name[37];
				unsigned c;
				memcpy(&first, e + 32, 8);
				memcpy(&last, e + 40, 8);
				if (!first && !last) {
					continue;
				}
				for (c = 0; c < 36; c++) {
					uint16_t ch = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
					if (ch == 0 || ch > 0x7f) {
						break;
					}
					name[c] = (char)ch;
				}
				name[c] = 0;
				printf("HEROLTE UFS: p%u %s %llu+%llu (%llu MiB)\n", i + 1, name,
				    first, last - first + 1, (last - first + 1) * UFS_BLOCK >> 20);
			}
		}
	}
	lck_mtx_unlock(&herolte_ufs_lock);
	printf("HEROLTE UFS: probe done (%d)\n", error);
}

/* Root device for rd=ufsroot0 (IOFindBSDRoot). */
int herolte_ufs_rootdev(dev_t *dev);

int
herolte_ufs_rootdev(dev_t *dev)
{
	if (!su.ready) {
		int error = herolte_ufs_init();
		if (error) {
			return error;
		}
	}
	*dev = su.bdev;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_HEROLTE */
