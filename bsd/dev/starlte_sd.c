/*
 * STARLTE (Galaxy S9, Exynos 9810) microSD card: a polling, read-only block
 * device on the card controller (dwmmc2, DesignWare MSHC 2.70a, Exynos7 type,
 * 0x11500000), ported step for step from the loader's reader
 * (shim/starlte/sd.c) and laid out like the UFS root (herolte_ufs.c): a BSD
 * devsw, no IOKit storage family (docs/XNU-MICROSD.md).
 *
 *   clocks  only the three gates GATE_CLKCMU_FSYS1_MMC_CARD 0x1a242070, FSYS1
 *           ACLK 0x11402030, SDCLKIN 0x11402034 are written; the CMU divider
 *           0x1a241864 is only read. MIF / INT are never touched.
 *   pins    GPF1-0..5 function 2, no pulls, drive 4 / 3 (pinctrl_fsys1)
 *   card    detect GPA1-5 active low; rails S2MPS18 LDO31 2.8 V + LDO2 3.3 V,
 *           power-cycled as the loader's hw_sd_rails (through starlte_touch.c's
 *           ACPM PMIC path)
 *   TMOUT   ~0, else every read ends in a data read timeout
 *   CLKSEL  read-modify-write of the timing fields only, as Linux
 *           dw_mci_exynos_set_clksel_timing; S-Boot's value is printed
 *   I/O     CMD17 / CMD18 with auto stop, the FIFO drained by the CPU, no DMA,
 *           no interrupts, every wait bounded
 *   errors  every failed read is followed by sd_recover() (Linux dw_mci_reset +
 *           send_stop_abort: controller and FIFO reset, clock reload, CMD12 with
 *           stop_abort, CMD13 until tran), then retried; nothing is left mid-CMD18
 *
 * The gates, divider, rails and card-detect pin are printed before the
 * controller is first read, and a gate that did not open stops the bring-up
 * before 0x11500000 is touched.
 *
 * Writes only with sdrw=1, and only inside the sdroot= range (every write is checked against it;
 * the range itself never covers p1's FAT / exFAT / NTFS or the store below its used end). Without
 * sdrw=1 the strategy refuses writes (EROFS). sdprobe=2 with sdrw=1 and sdroot= the card's last
 * 64 MiB runs a save / pattern / restore write test of 128 KiB there. Boot-args:
 *   sdprobe=1  bring the card up, print the MBR, the first blocks of p1 / p2
 *              (exFAT boot sector, SBSTORE header + directory CRCs), the
 *              sdroot= range head and a read throughput
 *   sdprobe=2  the same, then sdhold=S seconds (default 0) on screen and a
 *              deliberate panic carrying the summary (sweetreboot: to TWRP)
 *   rd=sdroot0 sdroot=pN | lba:START,COUNT   the root on the card (no default)
 *   sdfifo32=1 read the 64-bit FIFO as two 32-bit halves
 *   sdbench=M  MiB read by the probe's throughput test (default 16, 0 off)
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <libkern/crc.h>

/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
extern bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
extern void IOSleep(unsigned milliseconds);
extern void IODelay(unsigned microseconds);
extern void sweet_sample_start(void);

#define SD_PA           0x11500000UL
#define SD_SIZE         0x2000UL
#define CMU_PA          0x1a240000UL    /* gate +0x2070, divider +0x1864 */
#define FSYS1_PA        0x11400000UL    /* gates +0x2030, +0x2034 */
#define PINCTRL_PA      0x11430000UL    /* GPF1 CON +0, PUD +8, DRV +0xc */
#define ALIVE_PA        0x14050000UL    /* GPA1 CON +0x40, DAT +0x44 */

#undef CTRL                     /* sys/ttydefaults.h */
#define CTRL      0x00
#define PWREN     0x04
#define CLKDIV    0x08
#define CLKSRC    0x0c
#define CLKENA    0x10
#define TMOUT     0x14
#define CTYPE     0x18
#define BLKSIZ    0x1c
#define BYTCNT    0x20
#define INTMASK   0x24
#define CMDARG    0x28
#define CMD       0x2c
#define RESP0     0x30
#define RINTSTS   0x44
#define STATUS    0x48
#define FIFOTH    0x4c
#define VERID     0x6c
#define HCON      0x70
#define BMOD      0x80
#define CLKSEL64  0xa8
#define DATA      0x200

#define C_START    (1U << 31)
#define C_HOLD     (1U << 29)
#define C_UPD_CLK  (1U << 21)
#define C_INIT     (1U << 15)
#define C_STOP     (1U << 14)           /* stop_abort_cmd (Linux SDMMC_CMD_STOP) */
#define C_PRV_WAIT (1U << 13)
#define C_AUTOSTOP (1U << 12)
#define C_DATA     (1U << 9)
#define C_WRITE    (1U << 10)          /* read_write: 1 = write (dw_mmc CMD bit 10) */
#define C_CRC      (1U << 8)
#define C_LONG     (1U << 7)
#define C_RESP     (1U << 6)
#define R1         (C_RESP | C_CRC)

#define I_RE   (1U << 1)
#define I_CD   (1U << 2)
#define I_DTO  (1U << 3)
#define I_RCRC (1U << 6)
#define I_DCRC (1U << 7)
#define I_RTO  (1U << 8)
#define I_DRTO (1U << 9)
#define I_HTO  (1U << 10)
#define I_FRUN (1U << 11)
#define I_HLE  (1U << 12)
#define I_SBE  (1U << 13)
#define I_ACD  (1U << 14)
#define I_EBE  (1U << 15)
#define I_DERR (I_DCRC | I_DRTO | I_HTO | I_FRUN | I_SBE | I_EBE)

#define PLL_MMC_HZ      806000000ULL
#define SECTOR          512u
#define CHUNK           256u            /* sectors per CMD18, as sd.c */

static struct {
	vm_offset_t regs, cmu, fsys1, pinctrl, alive;
	int state;                      /* 0 not tried, 1 ready, -1 failed */
	const char *why;                /* the failed step */
	int err;                        /* its command / read error */
	bool fifo32;
	uint8_t l31, l2, l31_was, l2_was;
	uint32_t gpa1_con, gpa1_dat;
	uint32_t gate[3], cmu_div, verid, hcon, clksel, clksel_was;
	uint64_t bus_hz, card_hz;
	uint32_t r7, ocr, rca, cid[4], csd[4];
	bool high_capacity;
	uint64_t card_sectors;          /* from the CSD, 0 = unknown */
	uint32_t last_rintsts, last_status;
	uint64_t nreads, nsectors_read, nrecover;
	/* the root range, absolute card sectors */
	uint64_t first, count;
	uint64_t nwrites;
	char range[48];
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
	bool writable;                  /* sdrw=1 and the range passed sd_find_range */
} sd;

static LCK_GRP_DECLARE(starlte_sd_grp, "starlte_sd");
static LCK_MTX_DECLARE(starlte_sd_lock, &starlte_sd_grp);

static inline uint32_t
rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(sd.regs + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static inline void
wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(sd.regs + off) = v;
}

static inline uint32_t
mrd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
mwr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
}

static uint64_t
deadline_ms(unsigned ms)
{
	uint64_t t;
	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &t);
	return mach_absolute_time() + t;
}

static inline bool
expired(uint64_t t)
{
	return mach_absolute_time() > t;
}

static uint64_t
elapsed_us(uint64_t start)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / NSEC_PER_USEC;
}

/* Wait until (reg & mask) == want; 0 or -1 on timeout. */
static int
wait_reg(uint32_t off, uint32_t mask, uint32_t want, unsigned ms)
{
	uint64_t t = deadline_ms(ms);
	while ((rd(off) & mask) != want) {
		if (expired(t)) {
			return -1;
		}
	}
	return 0;
}

/* The CIU loads CLKDIV / CLKSRC / CLKENA. prv_wait: after the last data transfer (clock changes);
 * without it right after a controller reset, as Linux dw_mci_reset's SDMMC_CMD_UPD_CLK. */
static int
update_clock(bool prv_wait)
{
	wr(CMDARG, 0);
	wr(CMD, C_START | C_HOLD | C_UPD_CLK | (prv_wait ? C_PRV_WAIT : 0));
	return wait_reg(CMD, C_START, 0, 100);
}

static int
set_clock(uint64_t hz)
{
	uint64_t div = (sd.bus_hz + 2 * hz - 1) / (2 * hz);
	if (div > 255) {
		div = 255;
	}
	wr(CLKENA, 0);
	if (update_clock(true)) {
		return -1;
	}
	wr(CLKDIV, (uint32_t)div);
	wr(CLKSRC, 0);
	if (update_clock(true)) {
		return -1;
	}
	wr(CLKENA, 1);
	if (update_clock(true)) {
		return -1;
	}
	sd.card_hz = div ? sd.bus_hz / (2 * div) : sd.bus_hz;
	return 0;
}

/* One command; 0, -1 controller stuck, -2 no response, -3 response CRC, -4 response error.
 * A stop (C_STOP) goes out at once, as Linux sends it: no busy wait, no PRV_WAIT. */
static int
cmd(unsigned idx, uint32_t arg, uint32_t flags)
{
	bool stop = (flags & C_STOP) != 0;
	if (!stop && wait_reg(STATUS, 1U << 9, 0, 500)) {      /* card busy (R1b of the last command) */
		return -1;
	}
	wr(RINTSTS, 0xffffffffU);
	wr(CMDARG, arg);
	wr(CMD, C_START | C_HOLD | (stop ? 0 : C_PRV_WAIT) | flags | idx);
	if (wait_reg(CMD, C_START, 0, 100)) {
		return -1;
	}
	uint64_t t = deadline_ms(500);
	uint32_t s;
	while (!((s = rd(RINTSTS)) & (I_CD | I_HLE))) {
		if (expired(t)) {
			return -1;
		}
	}
	sd.last_rintsts = s;
	if (s & I_HLE) {
		return -1;
	}
	if (s & I_RTO) {
		return -2;
	}
	if ((flags & C_CRC) && (s & I_RCRC)) {
		return -3;
	}
	if (s & I_RE) {
		return -4;
	}
	return 0;
}

static int
acmd(unsigned idx, uint32_t arg, uint32_t flags)
{
	int r = cmd(55, sd.rca << 16, R1);
	return r ? r : cmd(idx, arg, flags);
}

static int
fail(const char *why, int err)
{
	sd.why = why;
	sd.err = err;
	sd.state = -1;
	sd.last_status = rd(STATUS);
	printf("STARBITE SD: %s failed (%d, rintsts 0x%x status 0x%x)\n", why, err,
	    sd.last_rintsts, sd.last_status);
	return ENXIO;
}

/* Card capacity in sectors from the CSD (RESP3..RESP0 = CSD[127:0], as Linux reads it). */
static uint64_t
csd_sectors(const uint32_t c[4])
{
	uint32_t structure = c[3] >> 30;
	if (structure == 1) {                   /* SDHC / SDXC: (C_SIZE + 1) * 512 KiB */
		uint32_t c_size = ((c[2] & 0x3fU) << 16) | (c[1] >> 16);
		return ((uint64_t)c_size + 1) * 1024;
	}
	if (structure == 0) {                   /* SDSC */
		uint32_t c_size = ((c[2] & 0x3ffU) << 2) | (c[1] >> 30);         /* [73:62] */
		uint32_t mult = (c[1] >> 15) & 7U;                                 /* [49:47] */
		uint32_t bl_len = (c[2] >> 16) & 0xfU;                             /* [83:80] */
		return (((uint64_t)c_size + 1) << (mult + 2)) << bl_len >> 9;
	}
	return 0;
}

/* docs/XNU-MICROSD.md 2.3: sd_up() of shim/starlte/sd.c, step for step (plus CMD9). Lock held. */
static int
sd_hw_up(void)
{
	if (sd.state) {
		return sd.state > 0 ? 0 : ENXIO;
	}
	sd.state = -1;
	uint32_t f32 = 0;
	PE_parse_boot_argn("sdfifo32", &f32, sizeof(f32));
	sd.fifo32 = (f32 != 0);

	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	vm_offset_t pa = SD_PA;
	if (SecureDTLookupEntry(NULL, "/starbite-sd", &node) == kSuccess &&
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) == kSuccess &&
	    size >= 2 * sizeof(uintptr_t)) {
		pa = reg[0];
	}
	if (pa != SD_PA) {
		sd.why = "controller address (ADT /starbite-sd is not 0x11500000)";
		printf("STARBITE SD: %s\n", sd.why);
		return ENXIO;
	}
	sd.regs = ml_io_map(SD_PA, SD_SIZE);
	sd.cmu = ml_io_map(CMU_PA, 0x4000);
	sd.fsys1 = ml_io_map(FSYS1_PA, 0x4000);
	sd.pinctrl = ml_io_map(PINCTRL_PA, 0x1000);
	sd.alive = ml_io_map(ALIVE_PA, 0x1000);
	if (!sd.regs || !sd.cmu || !sd.fsys1 || !sd.pinctrl || !sd.alive) {
		sd.why = "register mappings";
		return ENXIO;
	}

	/* 1 card present: GPA1-5 (CON bits 23:20, 0 input / 0xf EINT) reads low */
	sd.gpa1_con = mrd(sd.alive, 0x40);
	sd.gpa1_dat = mrd(sd.alive, 0x44);
	if (sd.gpa1_dat & (1U << 5)) {
		sd.why = "no card (detect GPA1-5 high)";
		printf("STARBITE SD: %s: gpa1 con 0x%08x (pin 5 function 0x%x) dat 0x%02x\n", sd.why,
		    sd.gpa1_con, (sd.gpa1_con >> 20) & 0xfU, sd.gpa1_dat & 0xffU);
		return ENXIO;
	}
	/* 2 the three gates, manual + on; 3 the divider: S-Boot leaves DIV_CLKCMU_FSYS1_MMC_CARD at
	 * /16 (a 12.6 MHz bus, 6.3 MHz card clock, measured 2026-11-03); Linux writes it to /1
	 * (tools/linux/mk-starlte-dts.py, a 201.5 MHz bus), so do the same unless sddiv=0. This is
	 * the card's own divider in CMU top, not a MIF / INT clock. */
	static const uint32_t gate_off[3] = { 0x2070, 0x2030, 0x2034 };
	for (unsigned i = 0; i < 3; i++) {
		vm_offset_t b = i == 0 ? sd.cmu : sd.fsys1;
		mwr(b, gate_off[i], mrd(b, gate_off[i]) | (1U << 20) | (1U << 21));
		sd.gate[i] = mrd(b, gate_off[i]);
	}
	sd.cmu_div = mrd(sd.cmu, 0x1864);
	{
		uint32_t setdiv = 1;
		PE_parse_boot_argn("sddiv", &setdiv, sizeof(setdiv));
		if (setdiv && (sd.cmu_div & 0xfU)) {
			uint32_t was = sd.cmu_div;
			mwr(sd.cmu, 0x1864, sd.cmu_div & ~0xfU);
			for (unsigned i = 0; i < 1000 && (mrd(sd.cmu, 0x1864) & (1U << 16)); i++) {
				IODelay(1);     /* the divider's busy bit while it switches */
			}
			sd.cmu_div = mrd(sd.cmu, 0x1864);
			printf("STARBITE SD: cmu div 0x%x -> 0x%x (/1, as Linux)\n", was, sd.cmu_div);
		}
	}
	sd.bus_hz = PLL_MMC_HZ / ((sd.cmu_div & 0xfU) + 1U) / 4U;
	/* Printed before the controller is first read: a read of an unclocked 0x11500000 can stall
	 * the bus or take an SError, and then this line is the last evidence. */
	printf("STARBITE SD: gates 0x%x 0x%x 0x%x, cmu div 0x%x -> bus %llu Hz, gpa1 con 0x%08x dat 0x%02x\n",
	    sd.gate[0], sd.gate[1], sd.gate[2], sd.cmu_div, sd.bus_hz, sd.gpa1_con, sd.gpa1_dat & 0xffU);
	for (unsigned i = 0; i < 3; i++) {
		if ((sd.gate[i] & (3U << 20)) != (3U << 20)) {
			sd.why = "clock gates did not open";
			printf("STARBITE SD: %s (gate %u reads 0x%x): the controller is not touched\n", sd.why, i,
			    sd.gate[i]);
			return ENXIO;
		}
	}
	/* 4 pins */
	mwr(sd.pinctrl, 0x0, (mrd(sd.pinctrl, 0x0) & 0xff000000U) | 0x00222222U);
	mwr(sd.pinctrl, 0x8, mrd(sd.pinctrl, 0x8) & 0xff000000U);
	mwr(sd.pinctrl, 0xc, (mrd(sd.pinctrl, 0xc) & 0xff000000U) | 0x00333334U);
	/* 5 rails: hw_sd_rails (menu.c), the card power-cycled */
	if (!starlte_pmic_read(1, 0x68, &sd.l31) || !starlte_pmic_read(1, 0x4a, &sd.l2)) {
		sd.why = "card power (ACPM read)";
		printf("STARBITE SD: %s failed\n", sd.why);
		return ENXIO;
	}
	sd.l31_was = sd.l31;
	sd.l2_was = sd.l2;
	unsigned v = sd.l31 & 0x3fU;
	if (v != 0 && (v < 0x24U || v > 0x3cU)) {
		sd.why = "card power (LDO31 vsel outside 2.7-3.3 V, refused)";
		printf("STARBITE SD: %s: ldo31 0x%x ldo2 0x%x\n", sd.why, sd.l31, sd.l2);
		return ENXIO;
	}
	bool ok = starlte_pmic_write(1, 0x68, 0x28) && starlte_pmic_write(1, 0x4a, (uint8_t)(sd.l2 & 0x3fU));
	delay(20 * 1000);
	ok = ok && starlte_pmic_write(1, 0x68, 0xe8) && starlte_pmic_write(1, 0x4a, 0xfc);
	delay(5 * 1000);
	(void)starlte_pmic_read(1, 0x68, &sd.l31);
	(void)starlte_pmic_read(1, 0x4a, &sd.l2);
	printf("STARBITE SD: rails ldo31 0x%x -> 0x%x, ldo2 0x%x -> 0x%x%s\n", sd.l31_was, sd.l31, sd.l2_was,
	    sd.l2, ok ? "" : " (an ACPM write failed)");
	if (!ok) {
		sd.why = "card power (ACPM write)";
		printf("STARBITE SD: %s failed\n", sd.why);
		return ENXIO;
	}
	/* 6 the controller */
	delay(10 * 1000);
	sd.verid = rd(VERID);
	sd.hcon = rd(HCON);
	sd.clksel_was = rd(CLKSEL64);
	printf("STARBITE SD: verid 0x%x hcon 0x%x, clksel 0x%x (as S-Boot left it)\n", sd.verid, sd.hcon,
	    sd.clksel_was);
	if ((sd.verid & 0xffffU) != 0x270aU) {
		return fail("controller not answering (VERID)", 0);
	}
	if (((sd.hcon >> 7) & 7U) != 2U) {
		return fail("FIFO is not 64 bit (HCON)", 0);
	}
	/* 7-12 reset, PIO, no interrupts, TMOUT ~0, FIFO thresholds, CLKSEL, 1 bit, power */
	wr(CTRL, rd(CTRL) | 7U);
	if (wait_reg(CTRL, 7U, 0, 100)) {
		return fail("controller reset", 0);
	}
	wr(BMOD, 0);
	wr(CTRL, rd(CTRL) & ~((1U << 25) | (1U << 4)));
	wr(INTMASK, 0);
	wr(RINTSTS, 0xffffffffU);
	wr(TMOUT, 0xffffffffU);
	wr(FIFOTH, (2U << 28) | (31U << 16) | 32U);
	/* ciu div 3, drive phase 2, sample phase 0 (Linux SDMMC_CLKSEL_TIMING(0, 2, 3) from the DT's
	 * <0 2> and ciu-div 3); the other bits stay as they are (SDMMC_CLKSEL_TIMING_MASK) */
	uint32_t clksel = rd(CLKSEL64);
	wr(CLKSEL64, (clksel & ~0x07070007U) | (3U << 24) | (2U << 16));
	sd.clksel = rd(CLKSEL64);
	wr(CTYPE, 0);
	wr(PWREN, 1);
	delay(5 * 1000);
	/* 13 400 kHz */
	if (set_clock(400000)) {
		return fail("clock update (400 kHz)", 0);
	}
	delay(2 * 1000);

	/* 14-21 SD init */
	int r;
	sd.rca = 0;
	if ((r = cmd(0, 0, C_INIT))) {
		return fail("CMD0 (go idle)", r);
	}
	r = cmd(8, 0x1aa, R1);
	sd.r7 = r ? 0 : rd(RESP0);
	if (r || (sd.r7 & 0xfffU) != 0x1aaU) {
		return fail("CMD8 (not an SD 2.0 card at 3.3 V)", r);
	}
	uint64_t t = deadline_ms(1500);
	for (;;) {
		r = acmd(41, 0x40ff8000U, C_RESP);      /* HCS, 2.7-3.6 V; R3 has no CRC */
		sd.ocr = r ? 0 : rd(RESP0);
		if (!r && (sd.ocr & (1U << 31))) {
			break;
		}
		if (expired(t)) {
			return fail("ACMD41 (card never ready)", r);
		}
		delay(10 * 1000);
	}
	sd.high_capacity = (sd.ocr >> 30) & 1U;
	if ((r = cmd(2, 0, C_RESP | C_LONG | C_CRC))) {
		return fail("CMD2 (CID)", r);
	}
	for (unsigned i = 0; i < 4; i++) {
		sd.cid[i] = rd(RESP0 + 4 * i);
	}
	if ((r = cmd(3, 0, R1))) {
		return fail("CMD3 (address)", r);
	}
	sd.rca = rd(RESP0) >> 16;
	/* Not in sd.c: CMD9 (CSD, stand-by state) for the card size; a failure only loses the size. */
	if (cmd(9, sd.rca << 16, C_RESP | C_LONG | C_CRC) == 0) {
		for (unsigned i = 0; i < 4; i++) {
			sd.csd[i] = rd(RESP0 + 4 * i);
		}
		sd.card_sectors = csd_sectors(sd.csd);
	} else {
		printf("STARBITE SD: CMD9 (CSD) failed: card size unknown\n");
	}
	if ((r = cmd(7, sd.rca << 16, R1))) {
		return fail("CMD7 (select)", r);
	}
	if (!sd.high_capacity && (r = cmd(16, SECTOR, R1))) {
		return fail("CMD16 (block length)", r);
	}
	if ((r = acmd(6, 2, R1))) {
		return fail("ACMD6 (4-bit bus)", r);
	}
	wr(CTYPE, 1);
	if (set_clock(25000000)) {
		return fail("clock update (25 MHz)", 0);
	}
	wr(BLKSIZ, SECTOR);
	char pnm[6];
	for (unsigned i = 0; i < 5; i++) {      /* CID PNM [103:64]: RESP3[7:0], RESP2[31:0] */
		uint32_t w = i == 0 ? sd.cid[3] : sd.cid[2];
		char ch = (char)(i == 0 ? w & 0xff : (w >> (8 * (4 - i))) & 0xff);
		pnm[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '?';
	}
	pnm[5] = 0;
	printf("STARBITE SD: card %s \"%s\" mid 0x%02x rca 0x%04x ocr 0x%08x, %llu sectors (%llu MiB), "
	    "clksel 0x%x (was 0x%x), %llu kHz 4 bit, fifo %s\n", sd.high_capacity ? "SDHC/SDXC" : "SDSC", pnm,
	    sd.cid[3] >> 24, sd.rca, sd.ocr, sd.card_sectors, sd.card_sectors >> 11, sd.clksel,
	    sd.clksel_was, sd.card_hz / 1000, sd.fifo32 ? "2x32" : "64");
	sd.why = NULL;
	sd.state = 1;
	return 0;
}

static inline uint64_t
fifo_word(void)
{
	if (sd.fifo32) {
		uint64_t lo = *(volatile uint32_t *)(sd.regs + DATA);
		uint64_t hi = *(volatile uint32_t *)(sd.regs + DATA + 4);
		return lo | hi << 32;
	}
	return *(volatile uint64_t *)(sd.regs + DATA);
}

/* Up to 256 sectors with one CMD17/18 (auto stop) into dst (8-byte aligned). sd.c read_chunk. */
static int
read_chunk(uint64_t sector, uint32_t n, uint64_t *dst)
{
	wr(CTRL, rd(CTRL) | 2U);                        /* FIFO reset */
	if (wait_reg(CTRL, 2U, 0, 50)) {
		return -1;
	}
	wr(BLKSIZ, SECTOR);
	wr(BYTCNT, n * SECTOR);
	uint32_t arg = (uint32_t)(sd.high_capacity ? sector : sector * SECTOR);
	int r = cmd(n > 1 ? 18 : 17, arg, R1 | C_DATA | (n > 1 ? C_AUTOSTOP : 0));
	if (r) {
		return r;
	}
	uint64_t want = (uint64_t)n * (SECTOR / 8), got = 0;
	uint64_t t = deadline_ms(200 + n * 2U);
	for (;;) {
		uint32_t s = rd(RINTSTS);
		uint32_t c = (rd(STATUS) >> 17) & 0x1fffU;
		while (c-- && got < want) {
			dst[got++] = fifo_word();
		}
		if (s & I_DERR) {
			sd.last_rintsts = s;
			return -5;
		}
		if ((s & I_DTO) && got >= want) {
			break;
		}
		if ((s & I_DTO) && !((rd(STATUS) >> 17) & 0x1fffU)) {
			sd.last_rintsts = s;
			return -6;                      /* transfer over, data short */
		}
		if (expired(t)) {
			sd.last_rintsts = s;
			return -7;
		}
	}
	if (n > 1 && wait_reg(RINTSTS, I_ACD, I_ACD, 100)) {
		return -8;
	}
	return 0;
}

/*
 * After a failed read_chunk the CIU can still be inside the CMD18: read_chunk returned on the
 * error bit or its deadline without draining the FIFO, and once the FIFO is full flow control
 * stops the card clock. The next command (PRV_WAIT) would then wait for that transfer forever.
 * So, as Linux does after a data error (dw_mci_data_complete -> dw_mci_reset, plus
 * send_stop_abort):
 *   controller + FIFO reset (CTRL bits 0, 1), RINTSTS cleared, UPD_CLK so the CIU reloads the
 *   clock registers; then CMD12 with stop_abort (no busy wait, no PRV_WAIT): a card still in
 *   "data" goes back to "tran"; a card already in tran does not answer (illegal command, RTO,
 *   ignored). Then CMD13 until the card says tran (4), at most 250 ms, with one more CMD12 when
 *   it still says data (5). Returns the last CMD13 state, or -1 when CMD13 got no answer. Lock held.
 */
static int
sd_recover(void)
{
	sd.nrecover++;
	wr(CTRL, rd(CTRL) | 3U);
	(void)wait_reg(CTRL, 3U, 0, 100);
	wr(RINTSTS, 0xffffffffU);
	(void)update_clock(false);
	(void)cmd(12, 0, R1 | C_STOP);
	(void)wait_reg(STATUS, 1U << 9, 0, 500);        /* R1b busy */
	int state = -1;
	bool restopped = false;
	uint64_t t = deadline_ms(250);
	for (;;) {
		if (cmd(13, sd.rca << 16, R1) == 0) {
			state = (int)((rd(RESP0) >> 9) & 0xfU);
			if (state == 4) {
				break;
			}
			if (state == 5 && !restopped) {
				restopped = true;
				(void)cmd(12, 0, R1 | C_STOP);
				(void)wait_reg(STATUS, 1U << 9, 0, 500);
			}
		}
		if (expired(t)) {
			break;
		}
		delay(1000);
	}
	wr(RINTSTS, 0xffffffffU);
	return state;
}

/* count sectors from absolute card sector `sector` to dst (8-byte aligned); 0 or a read error. Lock held. */
static int
sd_read_sectors(uint64_t sector, uint32_t count, void *dst)
{
	if (sd.state != 1) {
		return -1;
	}
	if (((uintptr_t)dst & 7) || (sd.card_sectors && (sector >= sd.card_sectors ||
	    count > sd.card_sectors - sector)) || sector + count > 0xffffffffULL) {
		return -9;
	}
	uint8_t *p = dst;
	while (count) {
		uint32_t n = MIN(count, CHUNK);
		int r = -1;
		for (int a = 0; a < 3 && r; a++) {
			r = read_chunk(sector, n, (uint64_t *)(void *)p);
			if (r) {
				/* every failure is recovered, the last one too, so the next read starts clean */
				uint32_t ri = sd.last_rintsts, st = rd(STATUS);
				int cs = sd_recover();
				printf("STARBITE SD: read %llu+%u: %d (rintsts 0x%x status 0x%x), reset + CMD12: card "
				    "state %d%s, %s\n", sector, n, r, ri, st, cs, cs == 4 ? " (tran)" : "",
				    a < 2 ? "retry" : "giving up");
				sd.last_rintsts = ri;
				sd.last_status = st;
			}
		}
		if (r) {
			return r;
		}
		sd.nreads++;
		sd.nsectors_read += n;
		sector += n;
		count -= n;
		p += (size_t)n * SECTOR;
	}
	return 0;
}

/*
 * Writes (sdrw=1 only). The dw_mmc FIFO is 64 entries of 64 bits (FIFOTH TX_WMark 32 = depth / 2),
 * filled by the CPU as the card drains it; CMD24 / CMD25 + auto CMD12; then the card's
 * programming busy on DAT0 (STATUS bit 9) and a CMD13 that must read tran with no error bits.
 */
#define FIFO_DEPTH 64u
#define R1_ERRORS  0xFDF90000U          /* OUT_OF_RANGE .. CSD_OVERWRITE, not CARD_IS_LOCKED */

static void
fifo_put(uint64_t v)
{
	if (sd.fifo32) {
		*(volatile uint32_t *)(sd.regs + DATA) = (uint32_t)v;
		*(volatile uint32_t *)(sd.regs + DATA + 4) = (uint32_t)(v >> 32);
		return;
	}
	*(volatile uint64_t *)(sd.regs + DATA) = v;
}

static int
write_chunk(uint64_t sector, uint32_t n, const uint64_t *src)
{
	wr(CTRL, rd(CTRL) | 2U);                        /* FIFO reset */
	if (wait_reg(CTRL, 2U, 0, 50)) {
		return -1;
	}
	wr(BLKSIZ, SECTOR);
	wr(BYTCNT, n * SECTOR);
	uint32_t arg = (uint32_t)(sd.high_capacity ? sector : sector * SECTOR);
	int r = cmd(n > 1 ? 25 : 24, arg, R1 | C_DATA | C_WRITE | (n > 1 ? C_AUTOSTOP : 0));
	if (r) {
		return r;
	}
	uint64_t want = (uint64_t)n * (SECTOR / 8), put = 0;
	uint64_t t = deadline_ms(500 + n * 4U);
	for (;;) {
		uint32_t s = rd(RINTSTS);
		if (s & I_DERR) {
			sd.last_rintsts = s;
			return -5;
		}
		if (put < want) {
			uint32_t c = (rd(STATUS) >> 17) & 0x1fffU;
			uint32_t room = c < FIFO_DEPTH ? FIFO_DEPTH - c : 0;
			while (room-- && put < want) {
				fifo_put(src[put++]);
			}
		} else if (s & I_DTO) {
			break;
		}
		if (expired(t)) {
			sd.last_rintsts = s;
			return -7;
		}
	}
	if (n > 1 && wait_reg(RINTSTS, I_ACD, I_ACD, 100)) {
		return -8;
	}
	if (wait_reg(STATUS, 1U << 9, 0, 1000)) {        /* programming (SDXC: up to 500 ms) */
		return -10;
	}
	if ((r = cmd(13, sd.rca << 16, R1))) {
		return r;
	}
	uint32_t st = rd(RESP0);
	if ((st & R1_ERRORS) || ((st >> 9) & 0xfU) != 4) {
		printf("STARBITE SD: write %llu+%u: card status 0x%08x\n", sector, n, st);
		return -11;
	}
	return 0;
}

/* count sectors from src (8-byte aligned) to absolute card sector `sector`, only inside the
 * sdroot range and only when writable. Lock held. */
/* Which 64 MiB chunks of the root this boot wrote (a bit each, 256 chunks = 16 GiB of root at most):
 * saved in the sdlog header at the reset, so the host rewrites exactly those from its image. macOS's
 * writes into blocks the host image still holds as allocated (a file it replaced and then reused the
 * space of) corrupted the card's copies of unchanged files (boot 171811: the user record). */
static uint64_t sd_dirty[4];

static int
sd_write_sectors(uint64_t sector, uint32_t count, const void *src)
{
	if (sd.state != 1 || !sd.writable) {
		return -1;
	}
	if (((uintptr_t)src & 7) || sector < sd.first || count > sd.count ||
	    sector - sd.first > sd.count - count || sector + count > 0xffffffffULL) {
		printf("STARBITE SD: write %llu+%u outside the root range %llu+%llu: refused\n", sector, count,
		    sd.first, sd.count);
		return -9;
	}
	for (uint64_t c = (sector - sd.first) / 131072; c <= (sector + count - 1 - sd.first) / 131072 && c < 256; c++) {
		sd_dirty[c / 64] |= 1ULL << (c % 64);
	}
	const uint8_t *p = src;
	while (count) {
		uint32_t n = MIN(count, CHUNK);
		int r = -1;
		for (int a = 0; a < 3 && r; a++) {
			r = write_chunk(sector, n, (const uint64_t *)(const void *)p);
			if (r) {
				uint32_t ri = sd.last_rintsts, st = rd(STATUS);
				int cs = sd_recover();
				printf("STARBITE SD: write %llu+%u: %d (rintsts 0x%x status 0x%x), reset + CMD12: card "
				    "state %d, %s\n", sector, n, r, ri, st, cs, a < 2 ? "retry" : "giving up");
			}
		}
		if (r) {
			return r;
		}
		sd.nwrites++;
		sector += n;
		count -= n;
		p += (size_t)n * SECTOR;
	}
	return 0;
}

/*
 * The kernel log to the card (sdlog=1): the msgbuf ring, written at the sweetreboot reset into the
 * card's last 64 MiB (the test range, p2's tail, 249868288 + 131072 on the 128 GB card), so TWRP
 * reads megabytes of log instead of pmsg's 16 KiB tail. Block 0: "SBLOG001", ring size, write
 * index, length; then the ring. Only on the 128 GB card (249999360 sectors), only that range.
 */
int starlte_sd_dump_log(const void *ring, uint32_t size, uint32_t bufx);
int
starlte_sd_dump_log(const void *ring, uint32_t size, uint32_t bufx)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sdlog", &on, sizeof(on));
	if (!on || sd.state != 1 || sd.card_sectors != 249999360ULL || ((uintptr_t)ring & 7) ||
	    size == 0 || size > (64u << 20) - SECTOR) {
		return -1;
	}
	/* a busy desktop is often mid-transfer at the reset (boot 150628: -2, no log): wait up to
	 * 500 ms for the lock, then write anyway; the interrupted transfer is lost to the reset regardless */
	bool locked = false;
	for (int i = 0; i < 500 && !(locked = lck_mtx_try_lock(&starlte_sd_lock)); i++) {
		delay(1000);
	}
	const uint64_t first = 249868288ULL, count = 131072ULL;
	uint64_t hdr[SECTOR / 8] = { 0 };
	memcpy(hdr, "SBLOG001", 8);
	hdr[1] = size;
	hdr[2] = bufx;
	hdr[3] = mach_absolute_time();
	memcpy(&hdr[4], "SBDIRTY1", 8);     /* then the root's written-chunk bitmap, 4 words */
	hdr[5] = sd_dirty[0];
	hdr[6] = sd_dirty[1];
	hdr[7] = sd_dirty[2];
	hdr[8] = sd_dirty[3];
	int r = write_chunk(first, 1, hdr);
	{
		/* the clock for the next boot (0393) */
		clock_sec_t secs = 0;
		clock_usec_t us = 0;
		clock_get_calendar_microtime(&secs, &us);
		if (secs > 1700000000) {
			uint64_t t[SECTOR / 8] = { 0 };
			memcpy(t, "SBTIME01", 8);
			t[1] = (uint64_t)secs;
			(void)write_chunk(249999359ULL, 1, t);
		}
	}
	uint32_t sectors = (size + SECTOR - 1) / SECTOR;
	const uint8_t *p = ring;
	for (uint32_t done = 0; !r && done < sectors;) {
		uint32_t n = MIN(sectors - done, CHUNK);
		if (1 + done + n > count) {
			r = -9;
			break;
		}
		/* the ring's last chunk may be short: whole sectors only, the tail is ignored by the reader */
		if ((uint64_t)(done + n) * SECTOR > size) {
			n = (size - done * SECTOR) / SECTOR;
			if (n == 0) {
				break;
			}
		}
		r = write_chunk(first + 1 + done, n, (const uint64_t *)(const void *)(p + (size_t)done * SECTOR));
		done += n;
	}
	if (locked) {
		lck_mtx_unlock(&starlte_sd_lock);
	}
	return r;
}

/*
 * SD high speed (default, sdhs=0 turns it off): CMD6 switch to function group 1 = 1 (Linux core/sd_ops.c
 * mmc_sd_switch: arg 0x80fffff1, a 64-byte status block; core/sd.c: the result nibble
 * status[16] & 0xf must read 1), then the card clock to 50 MHz (bus / 4 = 50.375 MHz from the
 * 201.5 MHz bus, as Linux runs it). Read only: CMD6 mode 1 only selects a bus timing. On any
 * failure the card stays at 25 MHz. Lock held.
 */
static int sd_hs;

static void
sd_high_speed(void)
{
	uint32_t want = 1;                      /* default on since 2026-11-03: 23.2 MB/s measured; sdhs=0 off */
	PE_parse_boot_argn("sdhs", &want, sizeof(want));
	if (!want || sd.state != 1 || sd.bus_hz > 202000000ULL || sd.bus_hz < 196000000ULL) {
		return;
	}
	uint64_t st[8];
	wr(CTRL, rd(CTRL) | 2U);                        /* FIFO reset */
	if (wait_reg(CTRL, 2U, 0, 50)) {
		return;
	}
	wr(BLKSIZ, 64);
	wr(BYTCNT, 64);
	int r = cmd(6, 0x80fffff1U, R1 | C_DATA);
	unsigned got = 0;
	if (!r) {
		uint64_t t = deadline_ms(200);
		for (;;) {
			uint32_t s = rd(RINTSTS);
			uint32_t c = (rd(STATUS) >> 17) & 0x1fffU;
			while (c-- && got < 8) {
				st[got++] = fifo_word();
			}
			if (s & I_DERR) {
				r = -5;
				break;
			}
			if ((s & I_DTO) && got >= 8) {
				break;
			}
			if (expired(t)) {
				r = -7;
				break;
			}
		}
	}
	wr(BLKSIZ, SECTOR);
	if (r || got < 8) {
		printf("STARBITE SD: high speed: CMD6 failed (%d, %u words), staying at %llu kHz\n", r, got,
		    sd.card_hz / 1000);
		(void)sd_recover();
		return;
	}
	const uint8_t *b = (const uint8_t *)st;         /* the status is big-endian, byte 16 = [383:376] */
	unsigned fn = b[16] & 0xfU;
	if (fn != 1) {
		printf("STARBITE SD: high speed: card answered function %u, staying at %llu kHz\n", fn,
		    sd.card_hz / 1000);
		return;
	}
	IODelay(1000);
	if (set_clock(sd.bus_hz / 4)) {
		printf("STARBITE SD: high speed: clock update failed\n");
		(void)set_clock(25000000);
		return;
	}
	sd_hs = 1;
	printf("STARBITE SD: high speed on: %llu kHz\n", sd.card_hz / 1000);
}

static int
sd_up_locked(void)
{
	lck_mtx_lock(&starlte_sd_lock);
	bool first = sd.state != 1;
	int e = sd_hw_up();
	if (!e && first) {
		sd_high_speed();
	}
	lck_mtx_unlock(&starlte_sd_lock);
	return e;
}

/*
 * The clock (0393): the phone gives macOS no time source, so the calendar started at 1970 ("System clock
 * is incorrect"). The 128 GB card's last sector (the sdlog range's end, never reached by the ring) holds
 * "SBTIME01" + Unix seconds: the host writes it before each test boot (boot-next.sh), and the kernel
 * writes the current time back at the sweetreboot reset, so a boot from the menu starts close to the
 * truth too. Read once, at the first card read.
 */
#define SD_TIME_SECTOR 249999359ULL
static void
sd_time_restore(void)
{
	static bool done;
	if (done || sd.card_sectors != 249999360ULL) {
		return;
	}
	done = true;
	uint64_t sec[SECTOR / 8];
	lck_mtx_lock(&starlte_sd_lock);
	int r = sd_read_sectors(SD_TIME_SECTOR, 1, sec);
	lck_mtx_unlock(&starlte_sd_lock);
	if (r == 0 && !memcmp(sec, "SBTIME01", 8) && sec[1] > 1700000000ULL && sec[1] < 4102444800ULL) {
		clock_set_calendar_microtime((clock_sec_t)sec[1], 0);
		printf("STARBITE SD: calendar set to %llu from the card\n", sec[1]);
	}
}

/* 0395: the clock saved every 10 minutes too, so a boot after a hard reset (not the sweetreboot one) starts
 * close to the truth: one sector, "SBTIME01" + Unix seconds */
static thread_call_t sd_time_call;
static void
sd_time_save(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	clock_sec_t secs = 0;
	clock_usec_t us = 0;
	clock_get_calendar_microtime(&secs, &us);
	if (secs > 1700000000 && sd.state == 1 && sd.card_sectors == 249999360ULL) {
		uint64_t t[SECTOR / 8] = { 0 };
		memcpy(t, "SBTIME01", 8);
		t[1] = (uint64_t)secs;
		lck_mtx_lock(&starlte_sd_lock);
		(void)write_chunk(SD_TIME_SECTOR, 1, t);
		lck_mtx_unlock(&starlte_sd_lock);
	}
	uint64_t deadline;
	clock_interval_to_deadline(600, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(sd_time_call, deadline);
}

static int
sd_read(uint64_t sector, uint32_t count, void *dst)
{
	sd_time_restore();
	if (sd_time_call == NULL && sd.card_sectors == 249999360ULL) {
		uint32_t rw = 0;
		PE_parse_boot_argn("sdrw", &rw, sizeof(rw));
		if (rw) {
			sd_time_call = thread_call_allocate(sd_time_save, NULL);
			uint64_t deadline;
			clock_interval_to_deadline(600, NSEC_PER_SEC, &deadline);
			thread_call_enter_delayed(sd_time_call, deadline);
		}
	}
	lck_mtx_lock(&starlte_sd_lock);
	int r = sd_read_sectors(sector, count, dst);
	lck_mtx_unlock(&starlte_sd_lock);
	return r;
}

/* ---- MBR and the sdroot= range (docs/XNU-MICROSD.md 6) ---- */

struct sd_part {
	uint8_t type;
	uint64_t start, count;
};

static inline uint32_t
le32(const uint8_t *b)
{
	return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline uint64_t
le64(const uint8_t *b)
{
	return le32(b) | (uint64_t)le32(b + 4) << 32;
}

static bool
mbr_entries(const uint8_t *sec, struct sd_part p[4])
{
	if (sec[510] != 0x55 || sec[511] != 0xaa) {
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		const uint8_t *e = sec + 446 + 16 * i;
		p[i].type = e[4];
		p[i].start = le32(e + 8);
		p[i].count = le32(e + 12);
	}
	return true;
}

static inline bool
type_extended(uint8_t t)
{
	return t == 0x05 || t == 0x0f || t == 0x85;
}

/* FAT12/16/32, exFAT, NTFS: the user's data (vold mounts these). */
static inline bool
type_data(uint8_t t)
{
	return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x07 || t == 0x0b || t == 0x0c || t == 0x0e;
}

/* Types sdroot=pN never exposes: empty, extended containers, the user's data, the OS store (0xDA). */
static inline bool
type_refused(uint8_t t)
{
	return t == 0 || type_extended(t) || type_data(t) || t == 0xda;
}

#define SD_MAXPARTS 36                  /* p1..p4 + 32 logical */

/*
 * The partition table: p[0..3] the primary entries, p[4..] the logical partitions of the first
 * extended entry (p5.., the EBR chain, relative LBAs, at most 32 hops, inside the container).
 * *np = the entries found. 0, or EIO (MBR / an EBR unreadable or not 55aa) or EINVAL (a logical
 * partition outside its container); the entries found before the error are kept.
 */
static int
sd_partitions(struct sd_part p[SD_MAXPARTS], unsigned *np, uint8_t *sec)
{
	*np = 0;
	if (sd_read(0, 1, sec) || !mbr_entries(sec, p)) {
		return EIO;
	}
	*np = 4;
	unsigned x;
	for (x = 0; x < 4 && !type_extended(p[x].type); x++) {
	}
	if (x == 4) {
		return 0;
	}
	uint64_t base = p[x].start, end = p[x].start + p[x].count, ebr = base;
	for (unsigned hop = 0; hop < SD_MAXPARTS - 4; hop++) {
		struct sd_part l[4];
		if (ebr < base || ebr >= end || sd_read(ebr, 1, sec) || !mbr_entries(sec, l)) {
			return EIO;
		}
		struct sd_part *o = &p[*np];
		o->type = l[0].type;
		o->start = ebr + l[0].start;
		o->count = l[0].count;
		if (o->start < base || o->start + o->count > end) {
			return EINVAL;
		}
		(*np)++;
		if (!type_extended(l[1].type) || !l[1].start) {
			return 0;
		}
		ebr = base + l[1].start;
	}
	return 0;
}

/* Partition N (1..4 primary, 5.. logical through the EBR chain). 0 or an errno. */
static int
find_part(unsigned n, struct sd_part *out, uint8_t *sec)
{
	struct sd_part p[SD_MAXPARTS];
	unsigned np = 0;
	int e = sd_partitions(p, &np, sec);
	if (n >= 1 && n <= np) {
		*out = p[n - 1];
		return 0;
	}
	return e ? e : ENOENT;
}

/*
 * sdroot=lba: inside the OS store (an 0xDA entry): only at or past the store's used end, read from
 * the SBSTORE2 header (entry block 1: u64 capacity at 16, u64 used blocks at 24, CRC32 of bytes
 * 0..47 at 48; shim/starlte/store.h), so a range never covers an entry the loader boots. An entry
 * with no store in it (neither SBSTORE1 at block 0 nor SBSTORE2 at block 1) is allowed. 0 or EPERM
 * / EIO, reason printed.
 */
static int
sd_range_vs_store(const char *spec, unsigned idx, const struct sd_part *e, uint64_t s, uint64_t c,
    uint8_t *sec)
{
	if (s < e->start || s + c > e->start + e->count) {
		printf("STARBITE SD: sdroot=%s: crosses the bounds of p%u (type 0xda, the OS store), refused\n",
		    spec, idx);
		return EPERM;
	}
	if (sd_read(e->start + 8, 1, sec)) {
		printf("STARBITE SD: sdroot=%s: p%u store header unreadable, refused\n", spec, idx);
		return EIO;
	}
	if (memcmp(sec, "SBSTORE2", 8) == 0) {
		uint64_t cap = le64(sec + 16), used = le64(sec + 24);
		if (crc32(0, sec, 48) != le32(sec + 48) || used > cap || cap * 8 > e->count) {
			printf("STARBITE SD: sdroot=%s: p%u SBSTORE2 header bad (crc, capacity %llu, used %llu), "
			    "refused\n", spec, idx, cap, used);
			return EPERM;
		}
		uint64_t used_end = e->start + used * 8;
		if (s < used_end) {
			printf("STARBITE SD: sdroot=%s: inside p%u's store below its used end (sector %llu, block %llu), "
			    "refused\n", spec, idx, used_end, used);
			return EPERM;
		}
		printf("STARBITE SD: sdroot=%s: inside p%u (type 0xda), past the store's used end (block %llu of %llu)\n",
		    spec, idx, used, cap);
		return 0;
	}
	if (sd_read(e->start, 1, sec)) {
		printf("STARBITE SD: sdroot=%s: p%u block 0 unreadable, refused\n", spec, idx);
		return EIO;
	}
	if (memcmp(sec, "SBSTORE1", 8) == 0) {
		printf("STARBITE SD: sdroot=%s: p%u has a store with no SBSTORE2 header (used end unknown), refused\n",
		    spec, idx);
		return EPERM;
	}
	printf("STARBITE SD: sdroot=%s: inside p%u (type 0xda), which holds no store\n", spec, idx);
	return 0;
}

/* boot-arg sdroot=: pN or lba:START,COUNT into sd.first / sd.count. 0 or an errno, reason printed. */
static int
sd_find_range(void)
{
	char spec[48] = "";
	int error = 0;
	uint8_t *sec = kalloc_data(SECTOR, Z_WAITOK | Z_ZERO);
	if (!sec) {
		return ENOMEM;
	}
	if (!PE_parse_boot_argn("sdroot", spec, sizeof(spec)) || !spec[0]) {
		printf("STARBITE SD: no sdroot= (pN or lba:START,COUNT): no root range\n");
		error = EINVAL;
		goto out;
	}
	strlcpy(sd.range, spec, sizeof(sd.range));
	if (!sd.card_sectors) {
		printf("STARBITE SD: sdroot=%s: card size unknown, refused\n", spec);
		error = ENXIO;
		goto out;
	}
	if (spec[0] == 'p') {
		char *end = NULL;
		unsigned long n = strtoul(spec + 1, &end, 10);
		struct sd_part part = { 0 };
		if (!end || *end || n < 1 || n > 36 || (error = find_part((unsigned)n, &part, sec))) {
			printf("STARBITE SD: sdroot=%s: no such partition (%d)\n", spec, error);
			error = error ? error : EINVAL;
			goto out;
		}
		if (type_refused(part.type) || part.count < 64 ||
		    part.start + part.count > sd.card_sectors) {
			printf("STARBITE SD: sdroot=%s: type 0x%02x %llu+%llu refused\n", spec, part.type,
			    part.start, part.count);
			error = EPERM;
			goto out;
		}
		sd.first = part.start;
		sd.count = part.count;
	} else if (strncmp(spec, "lba:", 4) == 0) {
		char *end = NULL;
		uint64_t s = strtouq(spec + 4, &end, 10), c = 0;
		if (end && *end == ',') {
			c = strtouq(end + 1, &end, 10);
		}
		if (!end || *end || !c || (s & 7) || (c & 7) || s + c > sd.card_sectors || s + c < s) {
			printf("STARBITE SD: sdroot=%s: bad range (4 KiB aligned, inside %llu sectors)\n", spec,
			    sd.card_sectors);
			error = EINVAL;
			goto out;
		}
		/* every partition, primary and logical, is checked: the whole table must read */
		struct sd_part p[SD_MAXPARTS];
		unsigned np = 0;
		int pe = sd_partitions(p, &np, sec);
		if (pe) {
			printf("STARBITE SD: sdroot=%s: partition table unreadable (%d, %u entries read), refused\n",
			    spec, pe, np);
			error = pe;
			goto out;
		}
		for (unsigned i = 0; i < np; i++) {
			if (type_data(p[i].type) && s < p[i].start + p[i].count && p[i].start < s + c) {
				printf("STARBITE SD: sdroot=%s: overlaps p%u (type 0x%02x, the user's data), refused\n",
				    spec, i + 1, p[i].type);
				error = EPERM;
				goto out;
			}
		}
		for (unsigned i = 0; i < np; i++) {
			if (p[i].type == 0xda && s < p[i].start + p[i].count && p[i].start < s + c &&
			    (error = sd_range_vs_store(spec, i + 1, &p[i], s, c, sec))) {
				goto out;
			}
		}
		sd.first = s;
		sd.count = c;
	} else {
		printf("STARBITE SD: sdroot=%s: want pN or lba:START,COUNT\n", spec);
		error = EINVAL;
	}
out:
	kfree_data(sec, SECTOR);
	return error;
}

/* The HFS+ / HFSX volume header at byte 1024 of the range: "H+" / "HX", or NULL. */
static const char *
hfs_signature(uint64_t first, uint8_t *sec)
{
	if (sd_read(first + 2, 1, sec)) {
		return NULL;
	}
	if (sec[0] == 'H' && (sec[1] == '+' || sec[1] == 'X')) {
		return sec[1] == '+' ? "H+" : "HX";
	}
	return NULL;
}

/* ---- the block device ---- */

static int
starlte_sd_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!sd.ready) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !sd.writable) ? EROFS : 0;
}

static int
starlte_sd_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void
starlte_sd_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * SECTOR;
	uint32_t count = buf_count(bp);
	uint64_t bytes = sd.count * SECTOR;

	buf_setresid(bp, count);
	bool wr_ = !(buf_flags(bp) & B_READ);
	if (wr_ && !sd.writable) {
		error = EROFS;                  /* sdrw=1 not given */
		goto done;
	}
	if (!sd.ready || off > bytes || count > bytes - off) {
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
	uint64_t bounce[SECTOR / 8];
	lck_mtx_lock(&starlte_sd_lock);
	for (uint32_t done = 0; done < count;) {
		uint64_t lba = sd.first + (off + done) / SECTOR;
		char *to = vaddr + done;
		uint32_t left = count - done;
		uint32_t want;
		int r;
		if (!((uintptr_t)to & 7) && left >= SECTOR) {
			uint32_t n = MIN(left / SECTOR, CHUNK);
			want = n * SECTOR;
			r = wr_ ? sd_write_sectors(lba, n, to) : sd_read_sectors(lba, n, to);
		} else if (wr_) {
			/* unaligned or partial: read-modify-write of one sector through the stack */
			want = MIN(left, SECTOR);
			r = want < SECTOR ? sd_read_sectors(lba, 1, bounce) : 0;
			if (!r) {
				memcpy(bounce, to, want);
				r = sd_write_sectors(lba, 1, bounce);
			}
		} else {
			/* unaligned buffer or a partial sector: one sector through the stack */
			want = MIN(left, SECTOR);
			r = sd_read_sectors(lba, 1, bounce);
			if (!r) {
				memcpy(to, bounce, want);
			}
		}
		if (r) {
			printf("STARBITE SD: %s of sector %llu failed (%d)\n", wr_ ? "write" : "read", lba, r);
			error = EIO;
			break;
		}
		done += want;
		buf_setresid(bp, count - done);
	}
	lck_mtx_unlock(&starlte_sd_lock);
	buf_unmap(bp);
done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
starlte_sd_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, __unused int is_char)
{
	uint32_t *f = (uint32_t *)(void *)data;
	uint64_t *o = (uint64_t *)(void *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *f == SECTOR ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*o = sd.count;
		return 0;
	case DKIOCISWRITABLE:
		*f = sd.writable ? 1 : 0;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = CHUNK;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = sd.writable ? CHUNK : 0;
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
starlte_sd_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return starlte_sd_ioctl(dev, cmd, data, flag, p, 0);
}

static int
starlte_sd_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return starlte_sd_ioctl(dev, cmd, data, flag, p, 1);
}

static int
starlte_sd_psize(__unused dev_t dev)
{
	return sd.ready ? (int)MIN(sd.count, INT_MAX) : -1;
}

static int
starlte_sd_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(starlte_sd_strategy, NULL, dev, B_READ, minphys, uio, SECTOR);
}

static int
starlte_sd_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (!sd.writable) {
		return EROFS;
	}
	return physio(starlte_sd_strategy, NULL, dev, B_WRITE, minphys, uio, SECTOR);
}

static const struct bdevsw starlte_sd_bdevsw = {
	.d_open     = starlte_sd_open,
	.d_close    = starlte_sd_close,
	.d_strategy = starlte_sd_strategy,
	.d_ioctl    = starlte_sd_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = starlte_sd_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw starlte_sd_cdevsw = {
	.d_open       = starlte_sd_open,
	.d_close      = starlte_sd_close,
	.d_read       = starlte_sd_read,
	.d_write      = starlte_sd_write,
	.d_ioctl      = starlte_sd_cioctl,
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

/* Root device for rd=sdroot0 (IOFindBSDRoot): the sdroot= range, read-only. */
int starlte_sd_rootdev(dev_t *dev);

int
starlte_sd_rootdev(dev_t *dev)
{
	if (sd.ready) {
		/*
		 * IOFindBSDRoot runs again only after vfs_mountroot failed (bsd_init's mount loop). This
		 * path skips IOFindBSDRoot's 10-attempt check (and wdt=-1 turns it off), so returning the
		 * same dev would loop forever with no panic, no pmsg and no reset into TWRP. Panic instead:
		 * bsd_init's "cannot mount root, errno = N" is in the console tail sweetreboot saves.
		 */
		panic("STARBITE SD: HFS did not mount sdroot=%s (lba %llu +%llu)", sd.range, sd.first, sd.count);
	}
	uint32_t rw = 0;
	(void)PE_parse_boot_argn("sdrw", &rw, sizeof(rw));
	int error = sd_up_locked();
	if (!error) {
		error = sd_find_range();
	}
	if (error) {
		return error;
	}
	uint8_t *sec = kalloc_data(SECTOR, Z_WAITOK | Z_ZERO);
	const char *sig = sec ? hfs_signature(sd.first, sec) : NULL;
	if (sec) {
		kfree_data(sec, SECTOR);
	}
	if (!sig) {
		printf("STARBITE SD: sdroot=%s: no HFS+ / HFSX volume header at sector %llu\n", sd.range,
		    sd.first + 2);
		return EFTYPE;
	}
	/* writes only into this range (sd_write_sectors checks every one against sd.first / sd.count);
	 * sd_find_range already refused p1's FAT / exFAT / NTFS and the store below its used end */
	sd.writable = rw != 0;
	sd.bmajor = bdevsw_add(-1, &starlte_sd_bdevsw);
	if (sd.bmajor < 0) {
		return ENXIO;
	}
	sd.cmajor = cdevsw_add_with_bdev(-1, &starlte_sd_cdevsw, sd.bmajor);
	if (sd.cmajor < 0) {
		return ENXIO;
	}
	sd.bdev = makedev(sd.bmajor, 0);
	devfs_make_node(sd.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "sdroot0");
	devfs_make_node(makedev(sd.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rsdroot0");
	sd.ready = true;
	printf("STARBITE SD: root sdroot=%s: lba %llu +%llu (%llu MiB), %s, %s, %llu kHz\n",
	    sd.range, sd.first, sd.count, sd.count >> 11, sig, sd.writable ? "read-write (sdrw=1)" : "read-only",
	    sd.card_hz / 1000);
	/* As the UFS root does on a read-only root: the process sampler (bsd/dev/sweet_sample.c). */
	sweet_sample_start();
	*dev = sd.bdev;
	return 0;
}

/* ---- boot-arg sdprobe=1 / 2: read only ---- */

static void
probe_store(uint64_t p2, uint8_t *buf, char *sum, size_t sumlen)
{
	/* p2 block 0 "SBSTORE1", block 1 "SBSTORE2" header, blocks 3..6 directory (store.h) */
	if (sd_read(p2, 16, buf)) {
		printf("STARBITE SD: p2: blocks 0-1 unreadable\n");
		strlcat(sum, " p2 unreadable", sumlen);
		return;
	}
	const uint8_t *h = buf + 4096;
	char m0[9], m1[9];
	for (unsigned i = 0; i < 8; i++) {
		m0[i] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : '.';
		m1[i] = (h[i] >= 0x20 && h[i] < 0x7f) ? (char)h[i] : '.';
	}
	m0[8] = m1[8] = 0;
	if (memcmp(h, "SBSTORE2", 8) != 0) {
		printf("STARBITE SD: p2: block 0 \"%s\", block 1 \"%s\": no SBSTORE2 header\n", m0, m1);
		snprintf(sum + strlen(sum), sumlen - strlen(sum), " p2 '%s' no store", m1);
		return;
	}
	uint32_t ver = le32(h + 8), n = le32(h + 12), dcrc = le32(h + 44), hcrc = le32(h + 48);
	uint64_t cap = le64(h + 16), used = le64(h + 24);
	uint32_t dblk = le32(h + 32), dblks = le32(h + 36);
	bool hok = crc32(0, h, 48) == hcrc;
	printf("STARBITE SD: p2: block 0 \"%s\", SBSTORE2 v%u, %u entries, capacity %llu used %llu blocks, "
	    "dir %u+%u, header crc %s\n", m0, ver, n, cap, used, dblk, dblks, hok ? "ok" : "BAD");
	bool dok = false;
	if (hok && dblk == 3 && dblks == 4 && n <= 256 && !sd_read(p2 + 8 * dblk, 8 * dblks, buf)) {
		dok = crc32(0, buf, 64 * (size_t)n) == dcrc;
		printf("STARBITE SD: p2: directory (one 32-sector CMD18) crc %s\n", dok ? "ok" : "BAD");
		for (uint32_t i = 0; i < n && i < 12; i++) {
			const uint8_t *e = buf + 64 * i;
			char name[33];
			memcpy(name, e, 32);
			name[32] = 0;
			printf("STARBITE SD: p2:   %-24s block %7llu size %10llu type %u\n", name, le64(e + 32),
			    le64(e + 40), e[52] | e[53] << 8);
		}
	}
	snprintf(sum + strlen(sum), sumlen - strlen(sum), " p2 SBSTORE2 %u ent used %llu hdr %s dir %s", n, used,
	    hok ? "ok" : "BAD", dok ? "ok" : "BAD");
}

void starlte_sd_probe(void);

void
starlte_sd_probe(void)
{
	uint32_t mode = 0, hold = 0, bench = 16;
	PE_parse_boot_argn("sdprobe", &mode, sizeof(mode));
	PE_parse_boot_argn("sdhold", &hold, sizeof(hold));
	PE_parse_boot_argn("sdbench", &bench, sizeof(bench));
	bench = MIN(bench, 256);
	char sum[320] = "";
	const size_t BUF = 128 * 1024;
	uint8_t *buf = kalloc_data(BUF, Z_WAITOK | Z_ZERO);
	uint8_t *buf2 = kalloc_data(BUF, Z_WAITOK | Z_ZERO);
	struct sd_part p[4] = { };

	printf("STARBITE SD: probe (read only): microSD controller 0x%lx\n", SD_PA);
	uint64_t t0 = mach_absolute_time();
	int e = sd_up_locked();
	if (e) {
		snprintf(sum, sizeof(sum), "FAIL at %s (%d, rintsts 0x%x status 0x%x) verid 0x%x ldo31 0x%x "
		    "gates %x %x %x gpa1 %x", sd.why ? sd.why : "?", sd.err, sd.last_rintsts, sd.last_status,
		    sd.verid, sd.l31, sd.gate[0], sd.gate[1], sd.gate[2], sd.gpa1_con);
		goto done;
	}
	snprintf(sum, sizeof(sum), "card up in %llu ms, %llu MiB, %llu kHz, rca %04x;",
	    elapsed_us(t0) / 1000, sd.card_sectors >> 11, sd.card_hz / 1000, sd.rca);
	if (!buf || !buf2) {
		strlcat(sum, " no memory", sizeof(sum));
		goto done;
	}
	/* MBR */
	int r = sd_read(0, 1, buf);
	if (r || !mbr_entries(buf, p)) {
		printf("STARBITE SD: MBR: read %d, signature %02x%02x\n", r, buf[510], buf[511]);
		snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " MBR %s (%d)", r ? "unreadable" : "no 55aa", r);
		goto done;
	}
	printf("STARBITE SD: MBR: disk id 0x%08x, 55aa ok\n", le32(buf + 440));
	for (unsigned i = 0; i < 4; i++) {
		if (p[i].type || p[i].count) {
			printf("STARBITE SD: MBR p%u type 0x%02x boot 0x%02x lba %llu +%llu (%llu MiB, end %llu)\n",
			    i + 1, p[i].type, buf[446 + 16 * i], p[i].start, p[i].count, p[i].count >> 11,
			    p[i].start + p[i].count - 1);
			snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " p%u %02x %llu+%llu", i + 1,
			    p[i].type, p[i].start, p[i].count);
		}
	}
	strlcat(sum, ";", sizeof(sum));
	/* p1: its boot sector (exFAT "EXFAT   " at 3, volume length at 72) */
	if (p[0].type && !sd_read(p[0].start, 1, buf)) {
		char oem[9];
		for (unsigned i = 0; i < 8; i++) {
			oem[i] = (buf[3 + i] >= 0x20 && buf[3 + i] < 0x7f) ? (char)buf[3 + i] : '.';
		}
		oem[8] = 0;
		printf("STARBITE SD: p1 sector 0: oem \"%s\" sig %02x%02x", oem, buf[510], buf[511]);
		if (memcmp(buf + 3, "EXFAT   ", 8) == 0) {
			printf(", exFAT volume %llu sectors, 2^%u bytes/sector, 2^%u sectors/cluster, serial %08x",
			    le64(buf + 72), buf[108], buf[109], le32(buf + 100));
		}
		printf("\nSTARBITE SD: p1 sector 0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
		    buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
		snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " p1 '%s'", oem);
	}
	/* p2: the OS store (type 0xDA) */
	for (unsigned i = 0; i < 4; i++) {
		if (p[i].type == 0xda && p[i].count >= 64) {
			probe_store(p[i].start, buf, sum, sizeof(sum));
			break;
		}
	}
	strlcat(sum, ";", sizeof(sum));
	/* the sdroot= range, when given */
	char spec[48];
	if (PE_parse_boot_argn("sdroot", spec, sizeof(spec)) && spec[0]) {
		int fe = sd_find_range();
		const char *sig = fe ? NULL : hfs_signature(sd.first, buf);
		printf("STARBITE SD: sdroot=%s: %d, lba %llu +%llu, volume header %s\n", spec, fe, sd.first,
		    sd.count, sig ? sig : "none");
		snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " sdroot %s %s;", fe ? "refused" : "ok",
		    sig ? sig : "no HFS");
	}
	/* write self-test (sdrw=1, only when sdroot= is the card's last 64 MiB test range): save the
	 * range's last 128 KiB, write a pattern, read it back, write the saved data back, read back */
	uint32_t wtest = 0;
	PE_parse_boot_argn("sdrw", &wtest, sizeof(wtest));
	if (wtest && sd_find_range() == 0 && sd.card_sectors && sd.first + sd.count == sd.card_sectors &&
	    sd.count <= 131072 && sd.count >= CHUNK) {
		uint64_t at = sd.first + sd.count - CHUNK;
		const char *res = "ok";
		sd.writable = true;
		uint64_t t = 0, wus = 0;
		if (sd_read(at, CHUNK, buf2)) {
			res = "save read failed";
		} else {
			for (size_t i = 0; i < BUF / 8; i++) {
				((uint64_t *)(void *)buf)[i] = 0x5342575445535400ULL ^ (i * 0x9e3779b97f4a7c15ULL);
			}
			lck_mtx_lock(&starlte_sd_lock);
			t = mach_absolute_time();
			int w1 = sd_write_sectors(at, CHUNK, buf);
			wus = elapsed_us(t);
			lck_mtx_unlock(&starlte_sd_lock);
			uint8_t *chk = kalloc_data(BUF, Z_WAITOK | Z_ZERO);
			if (w1 || !chk) {
				res = "pattern write failed";
			} else if (sd_read(at, CHUNK, chk) || memcmp(chk, buf, BUF)) {
				res = "pattern readback DIFF";
			}
			lck_mtx_lock(&starlte_sd_lock);
			int w2 = sd_write_sectors(at, CHUNK, buf2);
			lck_mtx_unlock(&starlte_sd_lock);
			if (w2) {
				res = "RESTORE WRITE FAILED";
			} else if (chk && (sd_read(at, CHUNK, chk) || memcmp(chk, buf2, BUF))) {
				res = "RESTORE readback DIFF";
			}
			if (chk) {
				kfree_data(chk, BUF);
			}
		}
		sd.writable = false;
		uint64_t wkbps = wus ? (uint64_t)128 * 1000000 / wus : 0;
		printf("STARBITE SD: write test at %llu (128 KiB, test range): %s, %llu us, %llu KB/s\n", at, res,
		    wus, wkbps);
		snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " write %s %llu KB/s;", res, wkbps);
	}
	/* throughput: bench MiB from p2's start (else sector 0) in 128 KiB reads, read only;
	 * the first chunk is read twice and compared */
	uint64_t from = 0;
	for (unsigned i = 0; i < 4; i++) {
		if (p[i].type == 0xda) {
			from = p[i].start;
		}
	}
	if (bench) {
		uint32_t chunks = bench * 8, k;
		uint64_t t = mach_absolute_time();
		for (k = 0; k < chunks; k++) {
			if ((r = sd_read(from + (uint64_t)k * CHUNK, CHUNK, k ? buf : buf2))) {
				break;
			}
		}
		uint64_t us = elapsed_us(t);
		/* the reread needs the first chunk (k > 0); a mismatch and a failed reread are told apart */
		char reread[16];
		int rr = 0;
		if (k == 0) {
			strlcpy(reread, "skipped", sizeof(reread));
		} else if ((rr = sd_read(from, CHUNK, buf))) {
			snprintf(reread, sizeof(reread), "err %d", rr);
		} else {
			strlcpy(reread, memcmp(buf, buf2, BUF) == 0 ? "same" : "DIFF", sizeof(reread));
		}
		uint64_t kbps = us ? (uint64_t)k * 128 * 1000000 / us : 0;
		printf("STARBITE SD: read %u x 128 KiB from %llu: %d, %llu us, %llu KB/s, reread %s, %llu reads, "
		    "%llu recoveries\n", k, from, r, us, kbps, reread, sd.nreads, sd.nrecover);
		snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " %u MiB %llu KB/s reread %s", k / 8,
		    kbps, reread);
		if (r) {
			snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " err %d", r);
		}
		if (sd.nrecover) {
			snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), " rec %llu", sd.nrecover);
		}
	}
done:
	if (buf) {
		kfree_data(buf, BUF);
	}
	if (buf2) {
		kfree_data(buf2, BUF);
	}
	printf("STARBITE SD: probe done: %s\n", sum);
	if (mode != 2) {
		return;
	}
	if (hold) {
		printf("STARBITE SD: holding %u s, then a deliberate panic (photograph the screen now)\n", hold);
		IOSleep(hold * 1000);
	}
	panic("STARBITE SD: probe done (sdprobe=2): %s", sum);
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
