/*
 * STARLTE (Galaxy S9 SM-G960F, Exynos 9810) touchscreen: Samsung S6SY761
 * ("sec,sec_ts", fw y761_star1) at 0x48 on HSI2C 0x104B0000 (USI03,
 * "usi-i2c-v2"; pins GPP1-4/5 function 2), INT on GPA1-0 (active low),
 * from the starlte DTB. Polled in its own thread like herolte_touch.c,
 * whose HSI2C transaction code this reuses.
 *
 * Bus bring-up (CMU_PERIC0 gates, SYSREG_PERIC0 USI mode, pin mux,
 * USI_CON) follows exynos9810-woa drivers/Exynos9810Hsi2c, validated on the
 * S9+. Supplies tsp_io/tsp_avdd (S2MPS18 LDO35/LDO43) are regulator-boot-on
 * and left on by S-Boot; nothing here touches the PMIC.
 *
 * Protocol is the S6SY761 one (Linux s6sy761.c, exynos9810-woa
 * S6SY761Touch), not the older sec_ts of the S7: BOOT_STATUS 0x55 (0x20 =
 * application), PANEL_INFO 0x23 (max x/y), SENSE_ON 0x10, CLEAR_EVENT_STACK
 * 0x62, READ_ONE_EVENT 0x60 (8 bytes, byte 0 = 0 when the stack is empty):
 * byte 0 EID [1:0] (0 = coordinate), TID [5:2] - 1, state [7:6] (1 press,
 * 2 move, 3 release); x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0xf.
 * Boot-arg startouch=0 disables everything here; sweetswipe=1 plays scripted
 * input (st_script) for unattended test boots.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* when the scripted input (sweetspeed&4096) saw the first frame, 0 before: read by the burst sampler */
unsigned long long starlte_touch_script_t0;
extern unsigned long long sweet_fb_frames_shown;       /* iokit/Kernel/arm/SweetFramebuffer.cpp: frames presented since boot */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
/* the CPU governor's touch boost (bsd/dev/hakios.exynos_cpufreq.c): nothing unless cpufreq_gov=1 started it */
extern void hakios_exynos_cpufreq_touch(void);
extern void hakios_exynos_cpufreq_fingers(uint32_t down);
void starlte_touch_probe(void);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);

/*
 * Scion's touch findings (HakiOS hakios.s6sy761.c, xnu-8796), behind boot-args here (default: as before):
 *   startouch=N      (N > 1) the polling thread's priority (startouch=0: no touch, 1: as before). Scion runs it at 92 (BASEPRI_PREEMPT, as its display presenter):
 *                    at the kernel's default it lost the processors to the window server under load and a finger's
 *                    moves came in bursts (scrolling sticky, long presses by mistake; seen on the S9, 1 November).
 *   startouch_ms=N   the polling period, 2..20 ms (Scion: 4; here 5 by default).
 */
static unsigned st_poll_ms = 5;

#define ST_CMU_PERIC0_PA        0x10400000u     /* gates +0x2018/0x206c/0x20dc/0x20e0, QCH +0x3038 */
#define ST_SYSREG_PERIC0_PA     0x10411000u     /* +0x1c USI03 mode: 4 = I2C */
#define ST_GPIO_PERIC0_PA       0x10430000u     /* GPP1 CON +0x20, PUD +0x28, DRV +0x2c */
#define ST_HSI2C_PA             0x104b0000u
#define ST_ALIVE_PA             0x14050000u     /* GPA1 CON +0x40, DAT +0x44 */
#define ST_ADDR                 0x48u
#define ST_PANEL_W              1440
#define ST_PANEL_H              2960
#define ST_FINGERS              10

#define HSI2C_CTL               0x00
#define HSI2C_FIFO_CTL          0x04
#define HSI2C_TRAILING          0x08
#define HSI2C_INT_EN            0x20
#define HSI2C_INT_STAT          0x24
#define HSI2C_FIFO_STAT         0x30
#define HSI2C_TX_DATA           0x34
#define HSI2C_RX_DATA           0x38
#define HSI2C_CONF              0x40
#define HSI2C_AUTO_CONF         0x44
#define HSI2C_TIMEOUT           0x48
#define HSI2C_TRANS             0x50
#define HSI2C_FS1               0x60
#define HSI2C_FS2               0x64
#define HSI2C_FS3               0x68
#define HSI2C_SLA               0x6c
#define HSI2C_ADDR              0x70
#define HSI2C_USI_CON           0xc4

#define S6_SENSE_ON             0x10
#define S6_PANEL_INFO           0x23
#define S6_DEVICE_ID            0x52
#define S6_BOOT_STATUS          0x55
#define S6_READ_ONE_EVENT       0x60
#define S6_CLEAR_EVENT_STACK    0x62

static vm_offset_t st_i2c, st_alive;
static int st_sleep;
static unsigned st_max_x = 4096, st_max_y = 4096;
static bool st_down[ST_FINGERS];
static int st_x[ST_FINGERS], st_y[ST_FINGERS];
static unsigned st_events, st_retries, st_lost;

static inline uint32_t
st_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static inline void
st_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
}

static void
st_msleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&st_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * Touch supplies through ACPM. After S-Boot tsp_io (S2MPS18 LDO35) and
 * tsp_avdd (LDO43) are off: SCL, SDA and INT read 0. The PMIC sits on
 * SPEEDY (0x141C0000), owned by the APM firmware; like the vendor
 * acpm_mfd.c we ask ACPM (IPC channel 2) to write the LDO registers.
 * Protocol from the vendor drivers/soc/samsung/acpm (acpm_ipc.c
 * acpm_ipc_send_data, fw_header/framework.h, acpm_mfd.c):
 *   SRAM 0x02039000, framework at +0x7f00: +0x08 channel array offset,
 *   +0x18 ipc_ap_max; channels are 0x48 bytes: +0x00 id, +0x10/+0x14
 *   AP->APM rear/front, +0x18 AP->APM base, +0x28/+0x2c APM->AP
 *   rear/front, +0x30 APM->AP base, +0x34 q_len, +0x38 q_elem_size (all
 *   SRAM offsets). Send: slot at base + size * front, front = next, then
 *   INTGR0 (mailbox +0x08) = (1 << id) << 16. Reply: APM->AP queue entry
 *   whose word 0 bits 21:16 match the sequence; consume by advancing rear,
 *   INTCR1 (+0x20) = 1 << id when empty.
 *   PMIC command: w0 = type << 8 | reg (type 1 = PMIC, 0 = TOP), w1 =
 *   function (0 read, 1 write | value << 8); reply w1 [15:8] value,
 *   [31:24] result (0 ok).
 *   LDOxxCTRL: [7:6] enable (3 = on), [5:0] voltage, 25 mV steps:
 *   LDO35 (0x6c) 0.7 V base -> 1.8 V = 0x2c; LDO43 (0x74) 1.8 V base ->
 *   3.0 V = 0x30 (s2mps18.c, s2mps18-private.h; tsp_avdd is 3000000 uV
 *   in the starlte DTB, and S-Boot leaves exactly 0x30 programmed).
 */
#define ACPM_MBOX_PA            0x14100000u
#define ACPM_SRAM_PA            0x02039000u
#define ACPM_SRAM_SIZE          0x30000u
#define ACPM_INITDATA           0x7f00u
#define ACPM_PMIC_CH            2u

static vm_offset_t acpm_mbox, acpm_sram;
static uint32_t acpm_ch;                /* SRAM offset of channel 2 */
static uint32_t acpm_chans, acpm_ap_max;
static uint32_t acpm_seq;
/* One ACPM command at a time: touch power and DVFS here, vdd_g3d from
 * bsd/dev/starlte_gpu.c (starlte_pmic_read/write below). */
static LCK_GRP_DECLARE(acpm_lck_grp, "starlte_acpm");
static LCK_MTX_DECLARE(acpm_lock, &acpm_lck_grp);

static inline uint32_t
acpm_rd(uint32_t off)
{
	return st_rd(acpm_sram, off);
}

static inline void
acpm_wr(uint32_t off, uint32_t v)
{
	st_wr(acpm_sram, off, v);
}

static bool
acpm_init(void)
{
	acpm_mbox = ml_io_map(ACPM_MBOX_PA, 0x1000);
	acpm_sram = ml_io_map(ACPM_SRAM_PA, ACPM_SRAM_SIZE);
	uint32_t chans = acpm_rd(ACPM_INITDATA + 0x08);
	uint32_t ap_max = acpm_rd(ACPM_INITDATA + 0x18);
	char build[26];
	for (unsigned i = 0; i < 24; i += 4) {
		uint32_t w = acpm_rd(ACPM_INITDATA + 0x60 + i);
		for (unsigned b = 0; b < 4; b++) {
			char c = (char)(w >> (8 * b));
			build[i + b] = (c >= 32 && c < 127) ? c : '.';
		}
	}
	build[24] = 0;
	printf("STARLTE ACPM: channels at %#x, ap_max %u, build \"%s\"\n", chans, ap_max, build);
	if (!ap_max || ap_max > 32 || chans >= ACPM_SRAM_SIZE - ap_max * 0x48) {
		return false;
	}
	acpm_chans = chans;
	acpm_ap_max = ap_max;
	for (uint32_t i = 0; i < ap_max; i++) {
		uint32_t c = chans + i * 0x48;
		if (acpm_rd(c) == ACPM_PMIC_CH) {
			acpm_ch = c;
			printf("STARLTE ACPM: ch %u at %#x type %u q_len %u elem %u tx base %#x rx base %#x poll %u\n",
			    ACPM_PMIC_CH, c, acpm_rd(c + 0x0c), acpm_rd(c + 0x34), acpm_rd(c + 0x38),
			    acpm_rd(c + 0x18), acpm_rd(c + 0x30), acpm_rd(c + 0x44));
			uint32_t qlen = acpm_rd(c + 0x34), esz = acpm_rd(c + 0x38);
			if (!qlen || qlen > 64 || esz < 16 || esz > 64 || (esz & 3)) {
				return false;
			}
			return true;
		}
	}
	printf("STARLTE ACPM: no channel %u\n", ACPM_PMIC_CH);
	return false;
}

/* SRAM offset of the descriptor for channel id, or 0. */
static uint32_t
acpm_find(uint32_t id)
{
	for (uint32_t i = 0; i < acpm_ap_max; i++) {
		uint32_t c = acpm_chans + i * 0x48;
		if (acpm_rd(c) == id) {
			uint32_t qlen = acpm_rd(c + 0x34), esz = acpm_rd(c + 0x38);
			return (qlen && qlen <= 64 && esz >= 16 && esz <= 64 && !(esz & 3)) ? c : 0;
		}
	}
	return 0;
}

/* One command on channel id (descriptor c), polled; reply copied back. */
static bool
acpm_send_on_locked(uint32_t c, uint32_t id, uint32_t cmd[4])
{
	uint32_t qlen = acpm_rd(c + 0x34), esz = acpm_rd(c + 0x38);
	uint32_t tx_rear = acpm_rd(c + 0x10), tx_front = acpm_rd(c + 0x14), tx_base = acpm_rd(c + 0x18);
	uint32_t rx_rear = acpm_rd(c + 0x28), rx_front = acpm_rd(c + 0x2c), rx_base = acpm_rd(c + 0x30);

	uint32_t front = acpm_rd(tx_front), next = (front + 1) % qlen;
	for (unsigned us = 0; next == acpm_rd(tx_rear); us++) {
		if (us > 100000) {
			printf("STARLTE ACPM: tx queue full\n");
			return false;
		}
		delay(1);
	}
	if (++acpm_seq >= 64) {
		acpm_seq = 1;
	}
	uint32_t seq = acpm_seq;
	cmd[0] = (cmd[0] & ~(0x3fu << 16)) | (seq << 16);
	uint32_t slot = tx_base + esz * front;
	for (uint32_t i = 0; i < esz / 4; i++) {
		acpm_wr(slot + 4 * i, i < 4 ? cmd[i] : 0);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	acpm_wr(tx_front, next);
	__asm__ volatile ("dsb sy" ::: "memory");
	st_wr(acpm_mbox, 0x08, (1u << id) << 16);    /* INTGR0 */

	for (unsigned us = 0; us < 100000; us++) {
		uint32_t rear = acpm_rd(rx_rear), rfront = acpm_rd(rx_front);
		for (uint32_t i = rear; i != rfront; i = (i + 1) % qlen) {
			uint32_t e = rx_base + esz * i;
			if (((acpm_rd(e) >> 16) & 0x3fu) != seq) {
				continue;
			}
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = acpm_rd(e + 4 * w);
			}
			if (i != rear) {
				/* Keep the queue packed: move the rear entry here. */
				for (uint32_t w = 0; w < esz / 4; w++) {
					acpm_wr(e + 4 * w, acpm_rd(rx_base + esz * rear + 4 * w));
				}
			}
			rear = (rear + 1) % qlen;
			acpm_wr(rx_rear, rear);
			__asm__ volatile ("dsb sy" ::: "memory");
			if (rear == acpm_rd(rx_front)) {
				st_wr(acpm_mbox, 0x20, 1u << id);    /* INTCR1 */
			}
			return true;
		}
		delay(1);
	}
	printf("STARLTE ACPM: ch %u: no reply to seq %u (rx rear %u front %u, intsr1 %#x)\n", id, seq,
	    acpm_rd(rx_rear), acpm_rd(rx_front), st_rd(acpm_mbox, 0x28));
	return false;
}

static bool
acpm_send_on(uint32_t c, uint32_t id, uint32_t cmd[4])
{
	lck_mtx_lock(&acpm_lock);
	bool ok = acpm_send_on_locked(c, id, cmd);
	lck_mtx_unlock(&acpm_lock);
	return ok;
}

static bool
acpm_send(uint32_t cmd[4])
{
	return acpm_send_on(acpm_ch, ACPM_PMIC_CH, cmd);
}

/*
 * CPU clocks through ACPM DVFS (vendor cal-if/acpm_dvfs.c): cmd = { domain, kHz, 0 = FREQ_REQ |
 * 1 = FREQ_GET, 0 }; ACPM sets the voltage itself. Domains (acpm_dvfs_exynos9810.h): cpucl0 = 2 (the
 * Cortex-A55s), cpucl1 = 3 (the Mongoose M3s). Linux sends these on ipc channel *index* 0 (its
 * acpm_dvfs.ch_num is never set), the DT names channel 5. S-Boot leaves both clusters at ~1.05 GHz
 * (measured, starlte_smp.c), where Settings' scrolling is CPU-bound (sbca's profile, boot65). So
 * by default: A55s 1690000 kHz, M3s 1794000 kHz, levels of exynos9810.dtsi (the M3s' limit with all
 * four busy and no OCP, max-freq-wo-ocp; there is no thermal control here, so not the 2.7 GHz
 * single-core level). Boot-arg starcpu=0: leave them; starcpu=LITTLE[,BIG] in kHz: those.
 * FREQ_GET answered 0 kHz on channel 0 earlier, so the result is judged by the clock survey.
 */
extern void starlte_cpu_clock_report(void);

static void
st_cpu_dvfs(void)
{
	char arg[32] = "";
	uint32_t little = 1690000, big = 1794000;
	if (PE_parse_boot_argn("starcpu", arg, sizeof(arg))) {
		if (arg[0] == '0' && arg[1] == 0) {
			printf("STARLTE DVFS: starcpu=0: CPU clocks left as S-Boot set them\n");
			return;
		}
		char *end = arg;
		little = (uint32_t)strtoul(arg, &end, 10);
		big = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 10) : big;
	}
	if (little > 1794000 || big > 1794000) {
		printf("STARLTE DVFS: %u/%u kHz above 1794000: not without thermal control\n", little, big);
		return;
	}
	/* channel index 0 as Linux, then the descriptor whose id is 5 as the DT says */
	uint32_t descs[2] = { acpm_chans, acpm_find(5) };
	for (unsigned d = 0; d < 2; d++) {
		uint32_t c = descs[d];
		if (c == 0) {
			continue;
		}
		uint32_t id = acpm_rd(c);
		uint32_t set0[4] = { 2, little, 0, 0 }, set1[4] = { 3, big, 0, 0 };
		bool ok0 = acpm_send_on(c, id, set0), ok1 = ok0 && acpm_send_on(c, id, set1);
		uint32_t get0[4] = { 2, 0, 1, 0 }, get1[4] = { 3, 0, 1, 0 };
		if (ok1) {
			(void)acpm_send_on(c, id, get0);
			(void)acpm_send_on(c, id, get1);
		}
		printf("STARLTE DVFS: channel id %u: cpucl0 %u kHz %s (reply %#x %#x %#x), cpucl1 %u kHz %s (reply %#x %#x "
		    "%#x); FREQ_GET says %u / %u kHz\n", id, little, ok0 ? "sent" : "no reply", set0[0], set0[1], set0[2], big,
		    ok1 ? "sent" : "no reply", set1[0], set1[1], set1[2], get0[1], get1[1]);
		if (ok1) {
			break;
		}
	}
	/*
	 * Not the buses: asking ACPM for MIF 2093000 / INT 667000 kHz here (DT levels) froze the phone
	 * at once, the display going green (boot69): the memory clock changes under the display's DMA
	 * with nothing coordinating it. Left as S-Boot set them.
	 */
}

static bool
pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
	uint32_t cmd[4] = { (type & 0xfu) << 8 | (reg & 0xffu), 0, 0, 0 };
	if (!acpm_send(cmd) || (cmd[1] >> 24)) {
		printf("STARLTE ACPM: read %u:%#x failed (result %u)\n", type, reg, cmd[1] >> 24);
		return false;
	}
	*val = (uint8_t)(cmd[1] >> 8);
	return true;
}

static bool
pmic_write(uint32_t type, uint32_t reg, uint8_t val)
{
	uint32_t cmd[4] = { (type & 0xfu) << 8 | (reg & 0xffu), 1u | (uint32_t)val << 8, 0, 0 };
	if (!acpm_send(cmd) || (cmd[1] >> 24)) {
		printf("STARLTE ACPM: write %u:%#x=%#x failed (result %u)\n", type, reg, val, cmd[1] >> 24);
		return false;
	}
	return true;
}

/*
 * PMIC registers for other drivers (type 1 = PMIC, 0 = TOP), through the
 * same ACPM channel; the channel is looked up on first use if the touch
 * thread has not done it yet.
 */
bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);

static bool
acpm_ready(void)
{
	lck_mtx_lock(&acpm_lock);
	bool ok = acpm_ch || acpm_init();
	lck_mtx_unlock(&acpm_lock);
	return ok;
}

bool
starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
	return acpm_ready() && pmic_read(type, reg, val);
}

bool
starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val)
{
	return acpm_ready() && pmic_write(type, reg, val);
}

/*
 * One ACPM DVFS request (cmd { domain, kHz, FREQ_REQ, 0 } on ipc channel index 0, as st_cpu_dvfs()):
 * for other drivers, the GPU's clock (domain g3d = 4) from bsd/dev/starlte_gpu.c.
 */
bool starlte_acpm_dvfs(uint32_t domain, uint32_t khz);

bool
starlte_acpm_dvfs(uint32_t domain, uint32_t khz)
{
	if (!acpm_ready() || acpm_chans == 0) {
		return false;
	}
	uint32_t cmd[4] = { domain, khz, 0, 0 };
	bool ok = acpm_send_on(acpm_chans, acpm_rd(acpm_chans), cmd);
	printf("STARLTE DVFS: domain %u %u kHz %s (reply %#x %#x %#x)\n", domain, khz, ok ? "sent" : "no reply",
	    cmd[0], cmd[1], cmd[2]);
	return ok;
}

/*
 * The CPU governor's way onto this client (bsd/dev/hakios.exynos_acpm_dvfs.c, for hakios.exynos_cpufreq.c, cpufreq_gov=1):
 * one DVFS request on the same descriptor as starlte_acpm_dvfs() (channel index 0), under this file's lock and sequence
 * numbers, so that the APM's mailbox has one client; no log line (the governor logs once a second). And the SRAM read, 32
 * bits, for the APM's fvmap: only above its faulting first pages (0x3000), read only.
 */
bool starlte_acpm_dvfs_ready(void);
bool starlte_acpm_dvfs_send(uint32_t cmd[4]);
bool starlte_acpm_sram_read(uint32_t off, uint32_t *value);

bool
starlte_acpm_dvfs_ready(void)
{
	return acpm_ready() && acpm_chans != 0;
}

bool
starlte_acpm_dvfs_send(uint32_t cmd[4])
{
	if (!starlte_acpm_dvfs_ready()) {
		return false;
	}
	return acpm_send_on(acpm_chans, acpm_rd(acpm_chans), cmd);
}

bool
starlte_acpm_sram_read(uint32_t off, uint32_t *value)
{
	if (acpm_sram == 0 || (off & 3) || off < 0x3000u || off > ACPM_SRAM_SIZE - 4) {
		return false;
	}
	*value = acpm_rd(off);
	return true;
}

/* Returns true once both touch rails are on (or were already). */
static bool
st_power_on(void)
{
	uint8_t id = 0, l35 = 0, l43 = 0;
	if (!acpm_ready() || !pmic_read(0, 0x00, &id)) {
		return false;
	}
	(void)pmic_read(1, 0x6c, &l35);
	(void)pmic_read(1, 0x74, &l43);
	printf("STARLTE ACPM: PMICID %#x, LDO35CTRL %#x, LDO43CTRL %#x\n", id, l35, l43);
	/* Only switch rails whose programmed voltage is the expected one, or
	 * that are unset (0), as herolte_touch.c does on the S7. */
	if ((l35 & 0x3fu) != 0x2cu && (l35 & 0x3fu) != 0) {
		printf("STARLTE ACPM: LDO35 voltage %#x unexpected, not touching\n", l35 & 0x3fu);
		return false;
	}
	/* tsp_avdd is 3.0 V (0x30) in the DTB. Once the S9 came up with 0x3c
	 * (3.3 V) programmed: within the S6SY761's AVDD range, so re-program the
	 * DTB's 3.0 V rather than give up; anything outside 2.8-3.3 V is left. */
	if ((l43 & 0x3fu) != 0 && ((l43 & 0x3fu) < 0x28u || (l43 & 0x3fu) > 0x3cu)) {
		printf("STARLTE ACPM: LDO43 voltage %#x unexpected, not touching\n", l43 & 0x3fu);
		return false;
	}
	bool ok = pmic_write(1, 0x6c, 0xec) && pmic_write(1, 0x74, 0xf0);
	(void)pmic_read(1, 0x6c, &l35);
	(void)pmic_read(1, 0x74, &l43);
	printf("STARLTE ACPM: after enable: LDO35CTRL %#x, LDO43CTRL %#x (%s)\n", l35, l43, ok ? "ok" : "FAILED");
	return ok;
}

/* Clocks, USI mode and pins before any HSI2C register access. */
static void
st_platform_init(void)
{
	vm_offset_t cmu = ml_io_map(ST_CMU_PERIC0_PA, 0x4000);
	vm_offset_t sysreg = ml_io_map(ST_SYSREG_PERIC0_PA, 0x1000);
	vm_offset_t gpio = ml_io_map(ST_GPIO_PERIC0_PA, 0x1000);
	static const uint32_t gates[] = { 0x2018, 0x206c, 0x20dc, 0x20e0 };

	/* Bus pull-ups and INT are on the touch IC's tsp_io rail (LDO35): all
	 * three read low if S-Boot left it off (as herolte found on the S7). */
	uint32_t dat = st_rd(gpio, 0x24);
	printf("STARLTE TOUCH: before mux: gpp1 con %#x dat %#x pud %#x -> SCL %u SDA %u, INT %u\n",
	    st_rd(gpio, 0x20), dat, st_rd(gpio, 0x28), (dat >> 4) & 1u, (dat >> 5) & 1u,
	    st_rd(st_alive, 0x44) & 1u);

	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		st_wr(cmu, gates[i], st_rd(cmu, gates[i]) | 0x00300000u);
	}
	st_wr(cmu, 0x3038, st_rd(cmu, 0x3038) | 0x3u);
	st_wr(sysreg, 0x1c, 0x4u);
	/* GPP1-4 SCL, GPP1-5 SDA: function 2, no pull, lowest drive. */
	st_wr(gpio, 0x20, (st_rd(gpio, 0x20) & ~0x00ff0000u) | 0x00220000u);
	st_wr(gpio, 0x28, st_rd(gpio, 0x28) & ~0x00ff0000u);
	st_wr(gpio, 0x2c, st_rd(gpio, 0x2c) & ~0x00ff0000u);
	__asm__ volatile ("dsb sy" ::: "memory");
	dat = st_rd(gpio, 0x24);
	printf("STARLTE TOUCH: cmu 0x2018=%#x 0x3038=%#x usi=%#x gpp1 con=%#x dat %#x -> SCL %u SDA %u\n",
	    st_rd(cmu, 0x2018), st_rd(cmu, 0x3038), st_rd(sysreg, 0x1c), st_rd(gpio, 0x20), dat,
	    (dat >> 4) & 1u, (dat >> 5) & 1u);
}

/*
 * Exynos 9810 USI HSI2C, as the vendor i2c-exynos5.c (exynos_usi_init,
 * exynos5_i2c_reset, exynos5_i2c_set_timing): release the USI reset, soft
 * reset, then the 9810 timing fields (FS3[23:16] clock divider, FS2[7:0]
 * SCL-high mask, FS1[23:16] start hold), master + AUTO mode. Timing values
 * for 400 kHz from the 200 MHz source are exynos9810-woa's (divider 33).
 */
static void
st_bus_init(void)
{
	st_wr(st_i2c, HSI2C_INT_EN, 0);
	st_wr(st_i2c, HSI2C_USI_CON, 0);         /* USI_RESET is active high */
	st_wr(st_i2c, HSI2C_CTL, st_rd(st_i2c, HSI2C_CTL) | 0x80000000u);
	st_wr(st_i2c, HSI2C_CTL, st_rd(st_i2c, HSI2C_CTL) & ~0x80000000u);
	st_wr(st_i2c, HSI2C_FS3, (st_rd(st_i2c, HSI2C_FS3) & ~0x00ff0000u) | (0x21u << 16));
	st_wr(st_i2c, HSI2C_FS2, (st_rd(st_i2c, HSI2C_FS2) & ~0x000000ffu) | 0xe0u);
	st_wr(st_i2c, HSI2C_FS1, (st_rd(st_i2c, HSI2C_FS1) & ~0x00ff0000u) | (0xf0u << 16));
	st_wr(st_i2c, HSI2C_CTL, 0x8u);          /* master */
	st_wr(st_i2c, HSI2C_TRAILING, 0x00ffffffu);
	st_wr(st_i2c, HSI2C_TIMEOUT, st_rd(st_i2c, HSI2C_TIMEOUT) & ~0x80000000u);
	st_wr(st_i2c, HSI2C_CONF, st_rd(st_i2c, HSI2C_CONF) | 0x80000000u);  /* AUTO mode */
	st_wr(st_i2c, HSI2C_INT_STAT, st_rd(st_i2c, HSI2C_INT_STAT) & 0xfffu);
}

/* One message with STOP; count <= 32 (herolte_touch.c ht_message). */
static bool
st_message(bool receive, uint8_t *bytes, unsigned count)
{
	uint32_t last = 0;
	bool xfer_done = false;
	unsigned done = 0;

	if (count == 0 || count > 32) {
		return false;
	}
	/* DTB: samsung,reset-before-trans and stop-after-trans on this bus. */
	st_bus_init();
	unsigned trig = count < 8 ? count : 8;
	st_wr(st_i2c, HSI2C_FIFO_CTL, 3u | (trig << 4) | (trig << 16));
	st_wr(st_i2c, HSI2C_ADDR, ST_ADDR << 10);
	st_wr(st_i2c, HSI2C_CTL, 0x8u | (receive ? 0x40u : 0x80u));
	st_wr(st_i2c, HSI2C_INT_EN, 0x80u);     /* polled: TRANSFER_DONE only */
	st_wr(st_i2c, HSI2C_AUTO_CONF, count | (receive ? 0x10000u : 0) | 0x20000u);
	st_wr(st_i2c, HSI2C_AUTO_CONF, st_rd(st_i2c, HSI2C_AUTO_CONF) | 0x80000000u);
	for (unsigned us = 0; us < 50000; us++) {
		uint32_t irq = st_rd(st_i2c, HSI2C_INT_STAT);
		if (irq) {
			last |= irq;
			st_wr(st_i2c, HSI2C_INT_STAT, irq);
		}
		if (irq & 0xf00u) {
			break;
		}
		if (irq & 0x80u) {
			xfer_done = true;
		}
		if (receive) {
			while (done < count && !(st_rd(st_i2c, HSI2C_FIFO_STAT) & (1u << 24))) {
				bytes[done++] = (uint8_t)st_rd(st_i2c, HSI2C_RX_DATA);
			}
		} else {
			while (done < count && !(st_rd(st_i2c, HSI2C_FIFO_STAT) & (1u << 7))) {
				st_wr(st_i2c, HSI2C_TX_DATA, bytes[done++]);
			}
		}
		if (done == count && (xfer_done || receive)) {
			st_wr(st_i2c, HSI2C_INT_EN, 0);
			for (unsigned n = 0; n < 10000; n++) {
				if (!(st_rd(st_i2c, HSI2C_TRANS) & (1u << 17))) {
					return true;
				}
				delay(1);
			}
			break;
		}
		delay(1);
	}
	static unsigned errors_logged;
	if (errors_logged++ < 12) {
		printf("STARLTE TOUCH: %s %u/%u failed int=%#x trans=%#x fifo=%#x\n",
		    receive ? "rx" : "tx", done, count, last,
		    st_rd(st_i2c, HSI2C_TRANS), st_rd(st_i2c, HSI2C_FIFO_STAT));
	}
	st_wr(st_i2c, HSI2C_INT_EN, 0);
	st_bus_init();
	return false;
}

static bool
st_cmd(uint8_t reg)
{
	for (unsigned attempt = 0; attempt < 10; attempt++) {
		if (attempt) {
			st_retries++;
			delay(200);
		}
		if (st_message(false, &reg, 1)) {
			return true;
		}
	}
	return false;
}

/* The S6SY761 may NACK while scanning: retry like Linux's i2c path. */
static bool
st_read(uint8_t reg, uint8_t *data, unsigned len)
{
	for (unsigned attempt = 0; attempt < 10; attempt++) {
		if (attempt) {
			st_retries++;
			delay(200);
		}
		if (st_message(false, &reg, 1) && st_message(true, data, len)) {
			return true;
		}
	}
	st_lost++;
	return false;
}

static bool
st_int_low(void)
{
	return !(st_rd(st_alive, 0x44) & 1u);
}

/* the governor's boosts (no-ops without cpufreq_gov=1): a finger event, and the count down when it changed */
static void
st_gov_event(bool changed)
{
	hakios_exynos_cpufreq_touch();
	if (changed) {
		uint32_t down = 0;
		for (unsigned f = 0; f < ST_FINGERS; f++) {
			down += st_down[f] ? 1 : 0;
		}
		hakios_exynos_cpufreq_fingers(down);
	}
}

static void
st_release_all(void)
{
	for (int f = 0; f < ST_FINGERS; f++) {
		if (st_down[f]) {
			st_down[f] = false;
			sweet_hid_touch(f, 0, st_x[f], st_y[f]);
		}
	}
	hakios_exynos_cpufreq_fingers(0);
}

/* One 8-byte event; returns false when the stack is empty or on error. */
static bool
st_event(void)
{
	uint8_t e[8];
	if (!st_read(S6_READ_ONE_EVENT, e, sizeof(e)) || e[0] == 0) {
		return false;
	}
	st_events++;
	if ((e[0] & 0x3u) != 0) {
		return true;            /* status event */
	}
	int tid = (int)((e[0] >> 2) & 0xfu) - 1;
	unsigned state = e[0] >> 6;
	if (tid < 0 || tid >= ST_FINGERS) {
		return true;
	}
	unsigned rx = (unsigned)e[1] << 4 | e[3] >> 4;
	unsigned ry = (unsigned)e[2] << 4 | (e[3] & 0xfu);
	int x = (int)((uint64_t)rx * ST_PANEL_W / st_max_x);
	int y = (int)((uint64_t)ry * ST_PANEL_H / st_max_y);
	if (st_events <= 12) {
		printf("STARLTE TOUCH: ev %02x%02x%02x%02x%02x%02x%02x%02x tid %d state %u raw %u,%u -> %d,%d\n",
		    e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], tid, state, rx, ry, x, y);
	}
	st_x[tid] = x;
	st_y[tid] = y;
	if (state == 3) {
		bool was = st_down[tid];
		st_down[tid] = false;
		sweet_hid_touch(tid, 0, x, y);
		st_gov_event(was);
	} else if (state == 1 || state == 2) {
		bool was = st_down[tid];
		st_down[tid] = true;
		sweet_hid_touch(tid, 1, x, y);
		st_gov_event(!was);
	}
	return true;
}

static void
st_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint8_t b[16];

	st_msleep(2000);
	uint32_t pwr = 1;
	PE_parse_boot_argn("startouchpwr", &pwr, sizeof(pwr));
	if (pwr) {
		bool on = st_power_on();
		uint32_t gov = 0;
		PE_parse_boot_argn("cpufreq_gov", &gov, sizeof(gov));
		if (acpm_ap_max && !gov) {
			st_cpu_dvfs();
		} else if (gov) {
			printf("STARLTE DVFS: cpufreq_gov=%u: no fixed CPU rates; the governor sets them\n", gov);
		}
		/* Supplies up, then the S6SY761 boots its firmware (~100 ms). */
		st_msleep(150);
		printf("STARLTE TOUCH: power %s, INT %u\n", on ? "on" : "NOT on", st_rd(st_alive, 0x44) & 1u);
	}
	st_platform_init();
	st_i2c = ml_io_map(ST_HSI2C_PA, 0x1000);
	st_bus_init();
	printf("STARLTE TOUCH: hsi2c ctl %#x conf %#x fs1 %#x fs2 %#x fs3 %#x usi %#x/%#x INT pin %s\n",
	    st_rd(st_i2c, HSI2C_CTL), st_rd(st_i2c, HSI2C_CONF), st_rd(st_i2c, HSI2C_FS1),
	    st_rd(st_i2c, HSI2C_FS2), st_rd(st_i2c, HSI2C_FS3), st_rd(st_i2c, HSI2C_USI_CON),
	    st_rd(st_i2c, 0xc8), st_int_low() ? "low" : "high");

	bzero(b, sizeof(b));
	bool ok = st_read(S6_BOOT_STATUS, b, 1);
	printf("STARLTE TOUCH: boot status %s 0x%02x (0x20 = application)\n", ok ? "read" : "FAILED", b[0]);
	if (!ok) {
		return;
	}
	if (st_read(S6_DEVICE_ID, b, 3)) {
		printf("STARLTE TOUCH: device id %02x %02x %02x\n", b[0], b[1], b[2]);
	}
	if (st_read(S6_PANEL_INFO, b, 11)) {
		unsigned mx = (unsigned)b[0] << 8 | b[1], my = (unsigned)b[2] << 8 | b[3];
		printf("STARLTE TOUCH: panel max %u x %u, channels %u x %u\n", mx, my, b[8], b[9]);
		if (mx >= 1000 && my >= 1000) {
			st_max_x = mx;
			st_max_y = my;
		}
	}
	bool sense = st_cmd(S6_SENSE_ON);
	st_msleep(20);
	bool clear = st_cmd(S6_CLEAR_EVENT_STACK);
	printf("STARLTE TOUCH: sense on %d, clear %d; polling every %u ms\n", sense, clear, st_poll_ms);

	unsigned stuck = 0;
	for (unsigned tick = 0;; tick++) {
		if (st_int_low()) {
			unsigned n = 0;
			while (n < 32 && st_event()) {
				n++;
			}
			if (n == 32 || (n == 0 && ++stuck >= 200)) {
				st_release_all();
				(void)st_cmd(S6_CLEAR_EVENT_STACK);
				stuck = 0;
			} else if (n) {
				stuck = 0;
			}
		} else {
			stuck = 0;
		}
		if (tick % (30000 / st_poll_ms) == (30000 / st_poll_ms) - 1) {
			printf("STARLTE TOUCH: %u events, %u retries, %u lost\n", st_events, st_retries, st_lost);
		}
		st_msleep(st_poll_ms);
	}
}

/*
 * Boot-arg sweetswipe=1: scripted input for unattended test boots (screenshots of what is past the
 * lock screen). Control Center opens with a swipe down from the top right corner, as on the iPhone X
 * (a swipe up from the bottom edge does nothing, boot57; the user's own swipe down worked, boot58),
 * Home closes it and unlocks (single presses 5 s apart, not a double press), a swipe left shows the
 * applications' page, a tap opens Settings, which is scrolled up and down for 20 s (frames for
 * sbca's profiler), Home and the swipe down give Control Center over the home screen. Times count from the first frame on the panel
 * (sweet_fb_first_present, SweetFramebuffer.cpp: 70-85 s after boot in boot55-56, when absolute
 * times had most of the script run before there was a UI). Swipes use finger 9 in panel pixels
 * through sweet_hid_touch(), like the real ones; Home goes through sweet_hid_key().
 */
extern void sweet_hid_key(int key, int down);
extern uint64_t sweet_fb_first_present;
/* for sbca (backboardd), whose screenshots are to be timed like this script */
SYSCTL_QUAD(_kern, OID_AUTO, sweet_first_present, CTLFLAG_RD | CTLFLAG_LOCKED, &sweet_fb_first_present,
    "mach_absolute_time() of the first frame on the panel, 0 before");

static const struct {
	unsigned at_s;
	int home;                       /* 1: press Home; 0: swipe x0,y0 -> x1,y1 */
	int x0, y0, x1, y1;
} st_script[] = {
	{ 15, 0, 1380, 6, 1380, 1500 },         /* Control Center over the lock screen (sbca shots at 17-25 s) */
	{ 30, 1, 0, 0, 0, 0 },                  /* closes it */
	{ 35, 1, 0, 0, 0, 0 },                  /* unlocks */
	{ 45, 0, 1300, 1500, 150, 1500 },       /* home screen page 2, the applications (shot at 55 s) */
	{ 60, 0, 534, 1790, 534, 1790 },        /* a tap on Settings (shot at 68 s, while it scrolls) */
	{ 64, 0, 720, 2300, 720, 1000 },         /* scrolling Settings up and down: frames to profile */
	{ 66, 0, 720, 1000, 720, 2300 },
	{ 68, 0, 720, 2300, 720, 1000 },
	{ 70, 0, 720, 1000, 720, 2300 },
	{ 72, 0, 720, 2300, 720, 1000 },
	{ 74, 0, 720, 1000, 720, 2300 },
	{ 76, 0, 720, 2300, 720, 1000 },
	{ 78, 0, 720, 1000, 720, 2300 },
	{ 80, 0, 720, 2300, 720, 1000 },
	{ 82, 0, 720, 1000, 720, 2300 },
	{ 84, 0, 720, 2300, 720, 1000 },
	{ 88, 1, 0, 0, 0, 0 },                  /* back to the home screen */
	{ 92, 0, 1380, 6, 1380, 1500 },         /* Control Center over the home screen (shots at 97, 103 s) */
};

/* sweetswipe=2: iOS 26 (iPhone 11 UI, no Home button): edge swipes and a tap, for touch debugging. */
static const struct {
	unsigned at_s;
	int home;
	int x0, y0, x1, y1;
} st_script_phone[] = {
	{ 20, 0, 720, 2955, 720, 1500 },        /* swipe up from the bottom edge: unlock */
	{ 30, 0, 720, 1480, 720, 1480 },        /* a tap in the middle */
	{ 40, 0, 720, 2880, 720, 1200 },        /* swipe up from just above the edge */
	{ 50, 0, 1380, 6, 1380, 1500 },         /* Control Center */
	{ 60, 0, 720, 2955, 720, 1500 },        /* swipe up again */
};
/* sweetswipe=3: unlock with Home, then touches on the home screen (iOS 26 touch debugging). */
struct st_step {
	unsigned at_s;
	int home;
	int x0, y0, x1, y1;
};
static const struct st_step st_script_home[] = {
	{ 15, 1, 0, 0, 0, 0 },                  /* Home: unlock to the home screen */
	{ 25, 0, 380, 500, 380, 500 },          /* tap the first widget */
	{ 35, 1, 0, 0, 0, 0 },                  /* Home: back */
	{ 40, 0, 1300, 1500, 150, 1500 },       /* swipe to the next page */
	{ 50, 0, 380, 500, 380, 500 },          /* tap */
};
/*
 * sweetswipe=4: an iOS 26 tour for unattended screenshot boots, timed against sbca's shots (every
 * 30 s from 60 s after the first frame). Panel pixels: the 828x1792 canvas is drawn at x 36..1403,
 * full height, so canvas (x, y) is panel (36 + 1.652 x, 1.652 y). Icons from boot s1's home screen.
 */
static const struct st_step st_script_tour[] = {
	{ 50, 1, 0, 0, 0, 0 },                  /* Home: unlock (shots 60, 90: home screen) */
	{ 95, 0, 1380, 6, 1380, 1500 },         /* Control Center (shot 120) */
	{ 125, 1, 0, 0, 0, 0 },                 /* closes it */
	{ 135, 0, 556, 1406, 556, 1406 },       /* tap Settings (shot 150) */
	{ 165, 0, 720, 2300, 720, 1200 },       /* scroll: the blurred header (shot 180) */
	{ 195, 1, 0, 0, 0, 0 },                 /* home screen (shot 210) */
	{ 215, 0, 1300, 1500, 150, 1500 },      /* next page (shot 240) */
	{ 245, 1, 0, 0, 0, 0 },                 /* back to the first page */
	{ 255, 0, 248, 1406, 248, 1406 },       /* tap Clock: its glass tab bar (shot 270) */
	{ 285, 1, 0, 0, 0, 0 },                 /* home (shot 300) */
	{ 305, 0, 868, 1008, 868, 1008 },       /* tap Notes (shot 330) */
	{ 335, 1, 0, 0, 0, 0 },                 /* home (shot 360) */
};

/*
 * boot-arg sweetspeed&4096: the same input every boot, to compare frame rates: unlock,
 * 20 home screen page swipes (60-90 s), Settings scrolled up and down (100-130 s), Home.
 */
static const struct st_step st_script_bench[] = {
	{ 45, 1, 0, 0, 0, 0 },                  /* Home: unlock */
	{ 55, 1, 0, 0, 0, 0 },                  /* Home again: first page of the home screen */
	{ 60, 0, 1300, 1500, 150, 1500 }, { 62, 0, 150, 1500, 1300, 1500 },
	{ 64, 0, 1300, 1500, 150, 1500 }, { 66, 0, 150, 1500, 1300, 1500 },
	{ 68, 0, 1300, 1500, 150, 1500 }, { 70, 0, 150, 1500, 1300, 1500 },
	{ 72, 0, 1300, 1500, 150, 1500 }, { 74, 0, 150, 1500, 1300, 1500 },
	{ 76, 0, 1300, 1500, 150, 1500 }, { 78, 0, 150, 1500, 1300, 1500 },
	{ 80, 0, 1300, 1500, 150, 1500 }, { 82, 0, 150, 1500, 1300, 1500 },
	{ 84, 0, 1300, 1500, 150, 1500 }, { 86, 0, 150, 1500, 1300, 1500 },
	{ 88, 0, 1300, 1500, 150, 1500 }, { 90, 0, 150, 1500, 1300, 1500 },
	{ 95, 0, 556, 1406, 556, 1406 },        /* tap Settings */
	{ 100, 0, 720, 2300, 720, 900 }, { 102, 0, 720, 900, 720, 2300 },
	{ 104, 0, 720, 2300, 720, 900 }, { 106, 0, 720, 900, 720, 2300 },
	{ 108, 0, 720, 2300, 720, 900 }, { 110, 0, 720, 900, 720, 2300 },
	{ 112, 0, 720, 2300, 720, 900 }, { 114, 0, 720, 900, 720, 2300 },
	{ 116, 0, 720, 2300, 720, 900 }, { 118, 0, 720, 900, 720, 2300 },
	{ 120, 0, 720, 2300, 720, 900 }, { 122, 0, 720, 900, 720, 2300 },
	{ 124, 0, 720, 2300, 720, 900 }, { 126, 0, 720, 900, 720, 2300 },
	{ 128, 0, 720, 2300, 720, 900 }, { 130, 0, 720, 900, 720, 2300 },
	{ 135, 1, 0, 0, 0, 0 },                 /* home screen */
};

/*
 * boot-arg sweetspeed&8192: the brightness slider, blind. Unlock, Control Center (swipe down from the top right),
 * vertical drags down then up at four columns of the middle band (one of them should be the brightness slider),
 * ending bright; Home closes it. Canvas (x, y) -> panel (36 + 1.652 x, 1.652 y). Look for "SWEET FB: swap brightness".
 */
static const struct st_step st_script_bright[] = {
	{ 45, 1, 0, 0, 0, 0 },                  /* Home: unlock */
	{ 55, 1, 0, 0, 0, 0 },                  /* Home: first page */
	{ 60, 0, 1380, 6, 1380, 1500 },         /* Control Center */
	{ 66, 0, 812, 1156, 812, 1817 }, { 69, 0, 812, 1817, 812, 1156 },
	{ 72, 0, 961, 1156, 961, 1817 }, { 75, 0, 961, 1817, 961, 1156 },
	{ 78, 0, 1110, 1156, 1110, 1817 }, { 81, 0, 1110, 1817, 1110, 1156 },
	{ 84, 0, 1258, 1156, 1258, 1817 }, { 87, 0, 1258, 1817, 1258, 1156 },
	{ 90, 0, 664, 1156, 664, 1817 }, { 93, 0, 664, 1817, 664, 1156 },
	{ 98, 1, 0, 0, 0, 0 },                  /* Home: closes it */
};

static void
st_script_thread(void *arg, __unused wait_result_t wr)
{
	/* 50 samples 6 ms apart, eased in and out (smoothstep): 24 even steps of 60 px were not taken
	 * for a swipe from the top edge (boot59), where the user's own swipe was */
	const int f = ST_FINGERS - 1, steps = 50;

	while (sweet_fb_first_present == 0) {
		st_msleep(250);
	}
	uint64_t start_ns;
	absolutetime_to_nanoseconds(sweet_fb_first_present, &start_ns);
	starlte_touch_script_t0 = mach_absolute_time();      /* (bsd/dev/sweet_sample.c: the burst sampler's window) */
	printf("STARLTE TOUCH: first frame on the panel; scripted input starts\n");
	const struct st_step *script;
	unsigned count;
	switch ((uintptr_t)arg) {
	case 2: script = (const struct st_step *)st_script_phone; count = sizeof(st_script_phone) / sizeof(st_script_phone[0]); break;
	case 3: script = st_script_home; count = sizeof(st_script_home) / sizeof(st_script_home[0]); break;
	case 4: script = st_script_tour; count = sizeof(st_script_tour) / sizeof(st_script_tour[0]); break;
	case 5: script = st_script_bench; count = sizeof(st_script_bench) / sizeof(st_script_bench[0]); break;
	case 6: script = st_script_bright; count = sizeof(st_script_bright) / sizeof(st_script_bright[0]); break;
	default: script = (const struct st_step *)st_script; count = sizeof(st_script) / sizeof(st_script[0]); break;
	}
	for (unsigned i = 0; i < count; i++) {
		unsigned at_s = script[i].at_s;
		int home = script[i].home;
		int x0 = script[i].x0, y0 = script[i].y0, x1 = script[i].x1, y1 = script[i].y1;
		uint64_t now_ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
		uint64_t at_ns = start_ns + (uint64_t)at_s * NSEC_PER_SEC;
		if (now_ns < at_ns) {
			st_msleep((unsigned)((at_ns - now_ns) / NSEC_PER_MSEC));
		}
		/* the frames shown since the step before this one: the same input every boot, so the counts compare */
		static unsigned long long shown_before;
		unsigned long long shown = sweet_fb_frames_shown;
		printf("STARLTE TOUCH: step %u at %u s: %llu frames shown since the step before\n", i, at_s, shown - shown_before);
		shown_before = shown;
		if (home) {
			printf("STARLTE TOUCH: scripted Home press\n");
			sweet_hid_key(3, 1);
			st_msleep(80);
			sweet_hid_key(3, 0);
			continue;
		}
		printf("STARLTE TOUCH: scripted swipe %u: %d,%d -> %d,%d\n", i, x0, y0,
		    x1, y1);
		for (int k = 0; k <= steps; k++) {
			int t = k * 1000 / steps;                       /* 0..1000 */
			int e = t * t / 1000 * (3000 - 2 * t) / 1000;   /* smoothstep, 0..1000 */
			int x = x0 + (x1 - x0) * e / 1000;
			int y = y0 + (y1 - y0) * e / 1000;
			sweet_hid_touch(f, 1, x, y);
			st_msleep(6);
		}
		sweet_hid_touch(f, 0, x1, y1);
	}
	printf("STARLTE TOUCH: scripted input done\n");
}

/*
 * The USB console's "touch X Y [X2 Y2 [MS]]" and "button lock|home|up|down [MS]" (drivers/usb/hakios.dwc3.c): a tap, a
 * swipe or a key press from the host, so that a flow can be driven and looked at ("shot") with nobody at the phone.
 * X and Y are thousandths of the screen (0 0 top left, 1000 1000 bottom right) and go to panel pixels as the picture
 * does: the 828 x 1792 canvas is drawn 1368 wide from x = 36, full height. A tap holds for 70 ms; a swipe takes MS
 * (default 300, 50 to 5000) with the eased samples of the scripted input above, finger 9, through sweet_hid_touch()
 * like a real one; a button is down for MS (default 80, up to 3000: a long press). One at a time; each runs on a
 * thread of its own and says "STARLTE TOUCH: console ..." when it starts and when it is done.
 */
static struct {
	int key;                        /* >= 0: a button (sweet_hid_key's order: power, volume up, volume down, home) */
	int x0, y0, x1, y1;
	unsigned ms;
} st_con_job;
static volatile int st_con_busy;

static void
st_con_thread(__unused void *arg, __unused wait_result_t wr)
{
	const int f = ST_FINGERS - 1;
	if (st_con_job.key >= 0) {
		sweet_hid_key(st_con_job.key, 1);
		st_msleep(st_con_job.ms);
		sweet_hid_key(st_con_job.key, 0);
	} else if (st_con_job.x0 == st_con_job.x1 && st_con_job.y0 == st_con_job.y1) {
		for (unsigned t = 0; t < 70; t += 6) {
			sweet_hid_touch(f, 1, st_con_job.x0, st_con_job.y0);
			st_msleep(6);
		}
		sweet_hid_touch(f, 0, st_con_job.x0, st_con_job.y0);
	} else {
		int steps = (int)(st_con_job.ms / 6);
		for (int k = 0; k <= steps; k++) {
			int t = k * 1000 / steps;                       /* 0..1000 */
			int e = t * t / 1000 * (3000 - 2 * t) / 1000;   /* smoothstep, 0..1000 */
			sweet_hid_touch(f, 1, st_con_job.x0 + (st_con_job.x1 - st_con_job.x0) * e / 1000,
			    st_con_job.y0 + (st_con_job.y1 - st_con_job.y0) * e / 1000);
			st_msleep(6);
		}
		/* (held a moment at the end: a drag that is let go of while it moves is taken for a flick) */
		for (unsigned t = 0; t < 60; t += 6) {
			sweet_hid_touch(f, 1, st_con_job.x1, st_con_job.y1);
			st_msleep(6);
		}
		sweet_hid_touch(f, 0, st_con_job.x1, st_con_job.y1);
	}
	printf("STARLTE TOUCH: console input done\n");
	st_con_busy = 0;
}

static bool
st_con_start(void)
{
	thread_t thread;
	if (kernel_thread_start(st_con_thread, NULL, &thread) != KERN_SUCCESS) {
		st_con_busy = 0;
		printf("usb: touch: no thread\n");
		return false;
	}
	thread_deallocate(thread);
	return true;
}

/* up to `most` decimal numbers of `arg` into v[]: how many were there */
static unsigned
st_con_numbers(const char *arg, int *v, unsigned most)
{
	unsigned n = 0;
	while (n < most) {
		while (*arg == ' ') {
			arg++;
		}
		if (*arg < '0' || *arg > '9') {
			break;
		}
		int x = 0;
		for (; *arg >= '0' && *arg <= '9' && x < 100000; arg++) {
			x = x * 10 + (*arg - '0');
		}
		v[n++] = x;
	}
	return n;
}

void hakios_input_console_touch(const char *arg);
void
hakios_input_console_touch(const char *arg)
{
	int v[5] = { 0, 0, 0, 0, 300 };
	unsigned n = st_con_numbers(arg, v, 5);
	if (n != 2 && n != 4 && n != 5) {
		printf("usb: touch X Y [X2 Y2 [MS]]: thousandths of the screen; two numbers are a tap, four a swipe (MS 50 to 5000, 300)\n");
		return;
	}
	if (n == 2) {
		v[2] = v[0];
		v[3] = v[1];
	}
	for (unsigned i = 0; i < 4; i++) {
		if (v[i] > 1000) {
			v[i] = 1000;
		}
	}
	if (!__sync_bool_compare_and_swap(&st_con_busy, 0, 1)) {
		printf("usb: touch: one is going on\n");
		return;
	}
	st_con_job.key = -1;
	st_con_job.x0 = 36 + v[0] * 1368 / 1000;
	st_con_job.y0 = v[1] * 2959 / 1000;
	st_con_job.x1 = 36 + v[2] * 1368 / 1000;
	st_con_job.y1 = v[3] * 2959 / 1000;
	st_con_job.ms = v[4] < 50 ? 50 : v[4] > 5000 ? 5000 : (unsigned)v[4];
	printf("STARLTE TOUCH: console %s %d,%d -> %d,%d (panel pixels) in %u ms\n", n == 2 ? "tap" : "swipe", st_con_job.x0,
	    st_con_job.y0, st_con_job.x1, st_con_job.y1, n == 2 ? 70 : st_con_job.ms);
	(void)st_con_start();
}

void hakios_input_console_button(const char *arg);
void
hakios_input_console_button(const char *arg)
{
	static const char *const names[] = { "lock", "up", "down", "home" };
	int key = -1;
	while (*arg == ' ') {
		arg++;
	}
	for (int i = 0; i < 4; i++) {
		size_t len = strlen(names[i]);
		if (strncmp(arg, names[i], len) == 0 && (arg[len] == 0 || arg[len] == ' ')) {
			key = i;
			arg += len;
		}
	}
	if (key < 0) {
		printf("usb: button lock|home|up|down [MS]: a press (80 ms; MS up to 3000 for a long one)\n");
		return;
	}
	int ms = 80;
	(void)st_con_numbers(arg, &ms, 1);
	if (!__sync_bool_compare_and_swap(&st_con_busy, 0, 1)) {
		printf("usb: button: an input is going on\n");
		return;
	}
	st_con_job.key = key;
	st_con_job.ms = ms < 20 ? 20 : ms > 3000 ? 3000 : (unsigned)ms;
	printf("STARLTE TOUCH: console button %s for %u ms\n", names[key], st_con_job.ms);
	(void)st_con_start();
}

void
starlte_touch_probe(void)
{
	uint32_t on = 1;
	thread_t thread;

	extern uint32_t sweet_speed_flags(void);
	uint32_t script = 0, sbx = sweet_speed_flags();
	if ((sbx & 4096) != 0) {
		script = 5;                     /* the bench input */
	}
	if ((sbx & 8192) != 0) {
		script = 6;                     /* the brightness slider test */
	}
	if ((script != 0 || PE_parse_boot_argn("sweetswipe", &script, sizeof(script))) && script &&
	    kernel_thread_start(st_script_thread, (void *)(uintptr_t)script, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	PE_parse_boot_argn("startouch", &on, sizeof(on));
	if (!on) {
		return;
	}
	st_alive = ml_io_map(ST_ALIVE_PA, 0x1000);
	printf("STARLTE TOUCH: INT pin %s at probe\n", st_int_low() ? "low" : "high");
	uint32_t pri = on, ms = 0;      /* startouch=N, N > 1: the thread's priority (short: the command line is full) */
	if (PE_parse_boot_argn("startouch_ms", &ms, sizeof(ms)) && ms >= 2 && ms <= 20) {
		st_poll_ms = ms;
	}
	kern_return_t kr;
	if (pri >= 2 && pri <= 95) {
		kr = kernel_thread_start_priority((thread_continue_t)st_thread, NULL, (integer_t)pri, &thread);
		printf("STARLTE TOUCH: thread at priority %u, every %u ms\n", pri, st_poll_ms);
	} else {
		kr = kernel_thread_start(st_thread, NULL, &thread);
	}
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
