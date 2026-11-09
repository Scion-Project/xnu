#define HAKIOS_SOC_FIXED
/*
 * The Galaxy S9's panel brightness (starlte, Exynos 9810): a Samsung S6E3HA8 AMOLED in MIPI command mode (4 lanes, DSC)
 * on DSIM0 (0x16080000), sent its brightness through the DSIM's command FIFO while the DECON keeps showing what the
 * boot loader set up. Only with boot-arg panel=1 (and, to blank the panel for a halt that powers the phone off, halt_off=N):
 * every address below belongs to this phone.
 *
 * The panel has no DCS "set brightness" (0x51) in Samsung's tables. Its brightness is Samsung's own: a gamma (CA), the
 * AOR (B1, the part of each frame the pixels are dark) and ELVSS, behind a level-2 key and taken by F7 03. The gamma of
 * each of its 74 luminances, 2 to 400 nits, is worked out from the panel's own factory calibration, its MTP (C8), which
 * this driver reads at start; hakios.s6e3ha8_gamma.h works it out as Samsung's kernel does, and has Samsung's tables of
 * which luminance and which AOR go with each level of 0-255. A brightness is then
 *   F0 5A 5A        the level-2 key on
 *   CA + 34 bytes   the gamma of the level's row
 *   B1 hi lo        the level's AOR (0x0B76 at 0 ... 0x000C at 255)
 *   F7 03           take them
 *   F0 A5 A5        the key off
 * the first three and the last two packets of Samsung's set-bl sequence (s6e3ha8_star_a3_s0_panel.h, _s1_:
 * star_a3_s0_set_bl_cmdtbl), with the bytes Android would send for that level. These are the panel's working state:
 * a restart clears them.
 *
 * Why each register sent is safe.
 *   F0           the level-2 key: opens Samsung's registers and closes them again. Samsung sends it around every change
 *                of brightness, and this driver did already.
 *   28, 10       display off and sleep-in, only for a halt that powers the phone off (halt_off=N: hakios_s6e3ha8_blank(),
 *                drivers/power/exynos/hakios.exynos_poweroff.c), each between the level-1 key as Samsung sends them; the
 *                panel's supplies then go with PS_HOLD.
 *   9F, FC       the level-1 and level-3 keys, only around the reads at start (and 9F around the halt's 28 and 10):
 *                Samsung's resource-init sequence opens all three for the same reads at every start of the panel
 *                (star_a3_s0_res_init_cmdtbl). A key changes nothing that is shown, and each is closed again whatever
 *                the reads gave.
 *   CA           the gamma: each value is a step of one of the panel's voltage dividers, between voltages the panel
 *                makes itself (VREGOUT, VT, the next point), and is added to the panel's own calibration: it sets how
 *                bright a gray is, not a supply. Only rows worked out from this panel's MTP are sent, each value kept
 *                within the register's range as Samsung keeps it (0-255; 0-511 at gray 255). Whatever the MTP, the
 *                400-nit row comes out as the panel's calibrated gamma itself. Android sends one of these rows at
 *                every change of brightness.
 *   B1           the AOR, two bytes from Samsung's table, within the range this driver sent before (0x000C-0x0B76).
 *   F7 03        takes what was sent ("gamma update"), as before.
 * Left out of Samsung's set-bl sequence, so as the boot loader set them:
 *   B5           TSET, MPS and ELVSS (and, at offset 22, ELVSS's temperature part). ELVSS is a supply voltage of the
 *                panel (its OLEDs' cathode), set by Samsung per luminance and per temperature, the temperature from a
 *                thermistor this driver does not read: a voltage register, so not touched. The light is the gamma's
 *                and the AOR's; what the boot loader's ELVSS does at the ends of the range (Samsung's: 0x12 at 2 nits,
 *                0x22 from 14 to about 120, 0x16 at 400; MPS 0xCC up to 39 nits, 0xDC above) is for the phone to show.
 *   F4           VGH and VINT, supply voltages: not touched. (Samsung's VINT is 0x23 at every luminance to 400 nits.)
 *   B4, 55       ACL, the current limit Samsung turns on to save power: left as it is.
 *   B8, B1+6,    IRC (17 values per brightness step, and on) and the "POC compensation" (B1 at offset 6, B8 at 0x53,
 *   B8+0x53      per luminance): Samsung's compensations of the picture, sent with the gamma on Android; the sources do
 *                not say what they compensate. Left as the boot loader set them: if colours at the ends of the range
 *                look wrong on the phone, they are the next to look at.
 *
 * The fallback, with panel_gamma=0 or when the MTP cannot be read: the AOR alone, as before. B1 from the dimmest of
 * Samsung's table for this panel (its level 0: 0x0B76 on S0, 0x0B72 on S1; 0x0B76 when the ID was not read) to 0x000C
 * by the square of the level, gamma as the boot loader left it, so the panel goes from the boot loader's level down
 * (to about 2 % of it).
 *
 * Writing a packet: its payload, four bytes a word, into PAYLOAD (0x5C), then its header into PKTHDR (0x58): the data
 * type, and the length (or the two bytes of a short one). 0x05 a DCS command without a parameter, 0x39 one with (Samsung's
 * tables send every longer one so). Before a header, the header FIFO must have room (FIFOCTRL 0x68, bit 11 full; bit 9
 * the payload's); after it, its being sent shows as INTSRC (0x50) bit 28, written back to clear. The DSIM keeps command
 * packets out of the window the panel is reading the frame in (its TE protection), so the picture is not disturbed;
 * Android sends brightness at any moment, behind a lock.
 *
 * Reading the panel (dsim_drv.c dsim_read_data, dsim_reg.c, common_mipi_lcd.c mipi_read): first "set maximum return
 * packet size" (data type 0x37, the count in the header's first byte), so the panel answers with no more than asked;
 * then the DCS read (0x06, the register in the first byte) with PKTHDR's bit 24 set: the bus is turned round for the
 * answer. The DSIM puts the answer in its RX FIFO, read a word at a time at RXFIFO (0x60); INTSRC bit 18 says it is in
 * (RX data done, written back to clear; bit 20 a turn-round timeout, 21 a low-power receive timeout), and FIFOCTRL bit
 * 12 is set while the FIFO is empty. A word's low byte is a packet's type: 0x02 acknowledge and error report (bits 23-8
 * the errors, 0x3F3F of them real ones), 0x08 end of transmission, 0x21/0x22 (DCS) or 0x11/0x12 (generic) a short
 * answer of 1 or 2 bytes in bits 15-8 and 23-16, 0x1C (DCS) or 0x1A (generic) a long one, its length in bits 23-8 and
 * its bytes in the words after it, four a word, the first lowest. Samsung waits 100 ms for an answer, takes at most 64
 * words, and wants exactly the count asked for; so does this. (A read from an offset would be preceded by B0 <offset>,
 * data type 0x15; every read here starts at 0.) Whatever is in the RX FIFO before a read is read out first.
 * Read at start, each logged (panel_gamma=1 or 2):
 *   04, 3 bytes             the panel's ID, with no key (as Samsung's read_panel_id): which of Samsung's two tables
 *   9F A5 A5, F0 5A 5A, FC 5A 5A    the three keys opened
 *   C8, 34 bytes, twice     the MTP; the two must agree, and 34 times one byte is not an MTP
 *   CA, 34 bytes; B1, 2     the gamma and AOR the boot loader left, and which of the 74 rows that gamma is, if one is:
 *                           on the phone, the check that this calculation is Samsung's
 *   FC A5 A5, F0 A5 A5, 9F 5A 5A    the keys closed
 *
 * Boot-args:
 *   panel=1             the driver: without it nothing here runs
 *   panel_brightness=N  a brightness of 0-255 set once at start
 *   panel_gamma=2       gamma and AOR sent (the default)
 *   panel_gamma=1       the panel read and its gamma worked out and logged; the AOR alone sent
 *   panel_gamma=0       the AOR alone, nothing read: the driver as it was
 *   panel_sleep=1       the screen's off and on from iOS sleep and wake the panel (hakios_s6e3ha8_power(), below;
 *                       off by default: the screen is then dark by its brightness alone, as before)
 *   panel_gamma_log=1   all 74 rows in the log, as Android's kernel prints them at start ("gamma[nits] : ...")
 *                       to set beside Android's log of the same phone
 *
 * Sources: the phone's device tree (dsim@0x16080000; s6e3ha8_star_ddi: mode 2, command mode, data_lane 4, dsc_en;
 * panel_drv@001's panel-lookup), Linux's drivers/video/fbdev/exynos/dpu_9810 (dsim_drv.c, dsim_reg.c, regs-dsim.h,
 * dsim.h, panels/common_mipi_lcd.c), include/video/mipi_display.h, and panel/ (panel.c, panel_bl.c, s6e3ha8/) of the
 * S9's kernel, github.com/ExyHyperBrick/android_kernel_samsung_exynos9810, lineage-24.0 (GPL: read for these facts, not
 * copied).
 *
 * kern.hakios.brightness: 0-255, written to set it (read: what it was set to, -1 never). CTLFLAG_KERN, so that the USB
 * console's "sysctl kern.hakios.brightness" reaches it too (XNU refuses the kernel any sysctl without it, EPERM). What iOS sets reaches the
 * panel through hakios.backlight.cpp, where this driver registers at start: the display service's "brightness", and with
 * backlight=2 AppleARMBacklight's. Each set the DSIM took is logged ("s6e3ha8: brightness N sent (..., AOR 0x....)"),
 * at most once a second, with how many were left out since the last line.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_soc.h>
#include "hakios.s6e3ha8_gamma.h"
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

/* The iOS 27 series has no hakios.backlight.cpp (iOS 16.5's service that takes iOS's "brightness"): the framebuffer calls
 * hakios_panel_brightness() and _nits() below (SweetFramebuffer.cpp; boot-arg sweetspeed&512). */
static int
hakios_panel_register(__unused const char *name, __unused int (*set)(int level))
{
	return 0;
}
extern uint32_t sweet_speed_flags(void);       /* boot-arg sweetspeed (osfmk/arm64/sweet_a64e.c) */

SYSCTL_DECL(_kern_hakios);

#define DSIM_PA         EXYNOS_DSIM0_PA
#define DSIM_CONFIG     0x4C
#define  PER_FRAME_READ (1u << 8)       /* (a read only once a frame; Samsung leaves it off) */
#define DSIM_INTSRC     0x50
#define  SFR_PH_EMPTY   (1u << 28)
#define  LPRX_TIMEOUT   (1u << 21)
#define  BTA_TIMEOUT    (1u << 20)
#define  RX_DATA_DONE   (1u << 18)
#define DSIM_PKTHDR     0x58
#define  PKTHDR_BTA     (1u << 24)      /* turn the bus round after this packet: the panel answers */
#define DSIM_PAYLOAD    0x5C
#define DSIM_RXFIFO     0x60
#define DSIM_FIFOCTRL   0x68
#define  EMPTY_RX       (1u << 12)
#define  FULL_PH        (1u << 11)
#define  EMPTY_PH       (1u << 10)      /* (the header FIFO empty: Samsung takes that for "sent" as well) */
#define  FULL_PL        (1u << 9)
#define RX_WORDS        64              /* Samsung's DSIM_RX_FIFO_MAX_DEPTH */

#define DCS_SHORT       0x05
#define DCS_READ        0x06
#define MAX_RETURN      0x37            /* set maximum return packet size */
#define DCS_LONG        0x39
#define RX_ACK_ERROR    0x02
#define RX_EOT          0x08
#define RX_GEN_SHORT_1  0x11
#define RX_GEN_SHORT_2  0x12
#define RX_GEN_LONG     0x1A
#define RX_DCS_LONG     0x1C
#define RX_DCS_SHORT_1  0x21
#define RX_DCS_SHORT_2  0x22
#define RX_ERRORS       0x3F3F          /* MIPI_DSI_ERR_BIT_MASK */

#define REG_ID          0x04
#define REG_MTP         0xC8
#define REG_GAMMA       0xCA
#define REG_AOR         0xB1

#define AOR_BRIGHTEST   0x000C
#define AOR_DIMMEST     0x0B76

static vm_offset_t dsim;
static lck_mtx_t *panel_lock;
static int panel_level = -1;
static uint32_t panel_failures, read_failures;

/* the gamma, once worked out from the MTP at start (read only after) */
static struct ha8_work gamma_work;
static uint8_t gamma_rows[HA8_ROWS][HA8_CA];
static const struct ha8_panel *gamma_panel;
static bool gamma_ready;

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(dsim + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(dsim + off) = v;
}

/* one packet, its payload (if any) and then its header: true once the DSIM says the header has gone out */
static bool
dsim_packet(uint32_t header, const uint8_t *payload, uint32_t len)
{
	uint64_t deadline = deadline_us(50000);

	while ((rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) {
		return false;
	}
	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t word = 0;

		for (uint32_t j = 0; j < 4 && i + j < len; j++) {
			word |= (uint32_t)payload[i + j] << (8 * j);
		}
		wr(DSIM_PAYLOAD, word);
	}
	wr(DSIM_PKTHDR, header);
	for (deadline = deadline_us(50000); !(rd(DSIM_INTSRC) & SFR_PH_EMPTY) && !(rd(DSIM_FIFOCTRL) & EMPTY_PH) &&
	    mach_absolute_time() < deadline;) {
	}
	bool sent = (rd(DSIM_INTSRC) & SFR_PH_EMPTY) || (rd(DSIM_FIFOCTRL) & EMPTY_PH);

	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	return sent;
}

/* one DCS command: a byte alone short, anything longer long */
static bool
dsim_send(const uint8_t *data, uint32_t len)
{
	if (len == 1) {
		return dsim_packet(DCS_SHORT | (uint32_t)data[0] << 8, NULL, 0);
	}
	return dsim_packet(DCS_LONG | (len & 0xff) << 8 | (len >> 8) << 16, data, len);
}

/* the next word of the RX FIFO, waiting at most `us' for one */
static bool
rx_pop(uint32_t *word, uint64_t us)
{
	uint64_t deadline = deadline_us(us);

	while ((rd(DSIM_FIFOCTRL) & EMPTY_RX) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & EMPTY_RX) {
		return false;
	}
	*word = rd(DSIM_RXFIFO);
	return true;
}

/* a DCS read of `len' bytes (1-40) of register `reg' (dsim_read_data): true when exactly that many came */
static bool
dsim_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	uint32_t word = 0, first = 0, got = 0, words = 0, stale = 0, intsrc;
	const char *why = NULL;
	uint64_t deadline;

	while (!(rd(DSIM_FIFOCTRL) & EMPTY_RX) && stale < RX_WORDS) {
		(void)rd(DSIM_RXFIFO);
		stale++;
	}
	wr(DSIM_INTSRC, RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT);
	if (!dsim_packet(MAX_RETURN | len << 8, NULL, 0) ||
	    !dsim_packet(DCS_READ | (uint32_t)reg << 8 | PKTHDR_BTA, NULL, 0)) {
		why = "the request was not sent";
		goto out;
	}
	for (deadline = deadline_us(100000); !(rd(DSIM_INTSRC) & (RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT)) &&
	    mach_absolute_time() < deadline;) {
	}
	/* (an answer in the FIFO is read even without RX data done: whether INTSRC shows that bit while it is masked is
	 * for the phone to say) */
	while (why == NULL && words < RX_WORDS && rx_pop(&word, words == 0 ? 1000 : 0)) {
		uint32_t size;

		first = words++ == 0 ? word : first;
		switch (word & 0xff) {
		case RX_ACK_ERROR:
			if ((word >> 8) & RX_ERRORS) {
				why = "the panel reported an error";
			}
			break;
		case RX_EOT:
			break;
		case RX_DCS_SHORT_1:
		case RX_GEN_SHORT_1:
		case RX_DCS_SHORT_2:
		case RX_GEN_SHORT_2:
			size = ((word & 0xff) == RX_DCS_SHORT_1 || (word & 0xff) == RX_GEN_SHORT_1) ? 1 : 2;
			for (uint32_t i = 0; i < size && i < len; i++) {
				buf[i] = (uint8_t)(word >> (8 + 8 * i));
			}
			got = size;
			break;
		case RX_DCS_LONG:
		case RX_GEN_LONG:
			size = (word >> 8) & 0xffff;
			for (uint32_t i = 0; i < size && why == NULL; i += 4) {
				uint32_t data;

				if (words++ >= RX_WORDS || !rx_pop(&data, 1000)) {
					why = "a long answer cut short";
					break;
				}
				for (uint32_t j = 0; j < 4 && i + j < size && i + j < len; j++) {
					buf[i + j] = (uint8_t)(data >> (8 * j));
				}
			}
			got = size;
			break;
		default:
			why = "a packet of unknown type";
			break;
		}
	}
	if (why == NULL && got != len) {
		why = words == 0 ? "no answer" : "an answer of another length";
	}
out:
	intsrc = rd(DSIM_INTSRC);
	wr(DSIM_INTSRC, RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT);
	if (why != NULL && read_failures++ < 8) {
		say("s6e3ha8: read %02X (%u bytes): %s (got %u in %u words, first %08x; INTSRC %x, FIFOCTRL %x%s)\n", reg,
		    len, why, got, words, first, intsrc, rd(DSIM_FIFOCTRL), stale ? ", stale words read out first" : "");
	}
	return why == NULL;
}

/* a read, tried three times (Samsung's mipi_read tries five, resetting the DSIM in between: not done here) */
static bool
panel_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		if (dsim_read(reg, buf, len)) {
			return true;
		}
	}
	return false;
}

/* the level-1, -2 and -3 keys opened (and closed again in the other order), as Samsung's resource-init sequence does */
static bool
panel_keys(bool open)
{
	static const uint8_t on[3][3] = { { 0x9F, 0xA5, 0xA5 }, { 0xF0, 0x5A, 0x5A }, { 0xFC, 0x5A, 0x5A } };
	static const uint8_t off[3][3] = { { 0xFC, 0xA5, 0xA5 }, { 0xF0, 0xA5, 0xA5 }, { 0x9F, 0x5A, 0x5A } };
	bool ok = true;

	for (int i = 0; i < 3; i++) {
		ok = dsim_send(open ? on[i] : off[i], 3) && ok;         /* (all three closed, whatever happened) */
		if (open && !ok) {
			break;
		}
	}
	return ok;
}

static void
say_bytes(const char *what, const uint8_t *bytes, uint32_t len)
{
	char line[3 * 40 + 1];
	uint32_t at = 0;

	for (uint32_t i = 0; i < len && i < 40; i++) {
		at += (uint32_t)snprintf(line + at, sizeof(line) - at, " %02X", bytes[i]);
	}
	line[at] = 0;
	say("s6e3ha8: %s:%s\n", what, line);
}

/* read the panel and work its gamma out (panel_gamma=1 or 2) */
static void
panel_gamma_init(uint32_t mode)
{
	uint8_t id[3] = { 0 }, mtp[2][HA8_CA], ca[HA8_CA], aor[2];
	bool have_id, keys, have_mtp = false, have_ca = false, have_aor = false, same = true, repeated = true;
	uint32_t panel_id, clamped, show_all = 0;
	const char *refused = NULL;
	int match = -1;

	lck_mtx_lock(panel_lock);
	have_id = panel_read(REG_ID, id, sizeof(id));
	keys = panel_keys(true);
	if (keys) {
		have_mtp = panel_read(REG_MTP, mtp[0], HA8_CA) && panel_read(REG_MTP, mtp[1], HA8_CA);
		have_ca = panel_read(REG_GAMMA, ca, HA8_CA);
		have_aor = panel_read(REG_AOR, aor, sizeof(aor));
	}
	keys = panel_keys(false) && keys;
	lck_mtx_unlock(panel_lock);

	panel_id = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];
	gamma_panel = ha8_panel_of(panel_id);
	say("s6e3ha8: ID %06x%s: Samsung's tables for the %s panel; keys %s; DSIM CONFIG %x (per-frame read %s)\n",
	    panel_id, have_id ? "" : " (NOT READ)", gamma_panel->name, keys ? "opened and closed" : "NOT ALL SENT",
	    rd(DSIM_CONFIG), (rd(DSIM_CONFIG) & PER_FRAME_READ) ? "on" : "off");
	if (have_mtp) {
		say_bytes("MTP (C8)", mtp[0], HA8_CA);
		for (int i = 0; i < HA8_CA; i++) {
			same = same && mtp[0][i] == mtp[1][i];
			repeated = repeated && mtp[0][i] == mtp[0][0];
		}
		if (!same) {
			say_bytes("MTP read again", mtp[1], HA8_CA);
		}
		refused = !same ? "the two reads differ" : repeated ? "one byte 34 times" : NULL;
	} else {
		refused = keys ? "not read" : "not read (the keys were not sent)";
	}
	if (have_ca) {
		say_bytes("the boot loader's gamma (CA)", ca, HA8_CA);
	}
	if (have_aor) {
		say("s6e3ha8: the boot loader's AOR (B1): %02X %02X\n", aor[0], aor[1]);
	}
	if (refused != NULL) {
		say("s6e3ha8: no MTP (%s): brightness by the AOR alone\n", refused);
		return;
	}
	clamped = ha8_gamma(&gamma_work, gamma_panel, mtp[0], gamma_rows);
	for (int r = 0; r < HA8_ROWS && have_ca && match < 0; r++) {
		bool equal = true;

		for (int i = 0; i < HA8_CA; i++) {
			equal = equal && gamma_rows[r][i] == ca[i];
		}
		match = equal ? r : -1;
	}
	if (match >= 0) {
		say("s6e3ha8: gamma worked out for the 74 luminances (%u values kept within range); the boot loader's is "
		    "the row of %u nits: the calculation agrees with Samsung's\n", clamped,
		    (unsigned)gamma_panel->rows[match].nits);
	} else {
		say("s6e3ha8: gamma worked out for the 74 luminances (%u values kept within range); the boot loader's "
		    "is %s\n", clamped, have_ca ? "none of them" : "not known");
	}
	(void)PE_parse_boot_argn("panel_gamma_log", &show_all, sizeof(show_all));
	for (int r = 0; r < HA8_ROWS; r++) {
		unsigned nits = gamma_panel->rows[r].nits;
		char what[16];

		if (show_all || nits == 2 || nits == 100 || nits == 400) {
			snprintf(what, sizeof(what), "gamma[%3u] ", nits);      /* (Android's "gamma[%3d] : ") */
			say_bytes(what, gamma_rows[r], HA8_CA);
		}
	}
	gamma_ready = mode >= 2;
	say("s6e3ha8: brightness by %s\n", gamma_ready ? "gamma and AOR (Samsung's tables)" :
	    "the AOR alone (panel_gamma=1: worked out, not sent)");
}

/* brightness 0-255: Samsung's gamma row and AOR for the level, or (the fallback) the AOR alone, the light made to
 * follow the level's square (a dim end that is usable) */
static int
panel_set(int level)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, take[] = { 0xF7, 0x03 }, key_off[] = { 0xF0, 0xA5, 0xA5 };
	uint8_t gamma[1 + HA8_CA] = { REG_GAMMA };
	uint32_t aor;
	bool ok;

	if (dsim == 0 || level < 0 || level > 255) {
		return EINVAL;
	}
	if (gamma_ready) {
		int row = ha8_row_of_level(level);

		aor = gamma_panel->aor[level];
		for (int i = 0; i < HA8_CA; i++) {
			gamma[1 + i] = gamma_rows[row][i];
		}
	} else {
		uint32_t lit = (uint32_t)(level * level);

		/* (the dimmest is Samsung's level 0 of this panel's table once its ID is known: 0x0B72 on S1) */
		uint32_t dimmest = gamma_panel != NULL ? gamma_panel->aor[0] : AOR_DIMMEST;

		aor = dimmest - (dimmest - AOR_BRIGHTEST) * lit / (255 * 255);
	}
	uint8_t aor_packet[] = { REG_AOR, (uint8_t)(aor >> 8), (uint8_t)aor };

	lck_mtx_lock(panel_lock);
	ok = dsim_send(key_on, sizeof(key_on));
	ok = ok && (!gamma_ready || dsim_send(gamma, sizeof(gamma)));
	ok = ok && dsim_send(aor_packet, sizeof(aor_packet)) && dsim_send(take, sizeof(take));
	ok = dsim_send(key_off, sizeof(key_off)) && ok;         /* (the key off whatever happened) */
	if (ok) {
		panel_level = level;
	} else {
		panel_failures++;
	}
	lck_mtx_unlock(panel_lock);
	if (!ok && panel_failures <= 3) {
		say("s6e3ha8: brightness %d (%s, AOR 0x%x) not sent: the DSIM's FIFO did not take it "
		    "(FIFOCTRL %x, INTSRC %x)\n", level, gamma_ready ? "gamma row and AOR" : "AOR alone", aor,
		    rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC));
	} else if (ok) {
		/* each set that was sent, at most a line a second (a slider dragged sends many): what went out */
		static uint64_t said_at;
		static uint32_t unsaid;
		uint64_t now = mach_absolute_time(), second;

		nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
		if (said_at == 0 || now - said_at >= second) {
			say("s6e3ha8: brightness %d sent (%s, AOR 0x%04x; the DSIM took every packet; %u sets not logged "
			    "since the last line)\n", level, gamma_ready ? "gamma row and AOR" : "AOR alone", aor, unsaid);
			said_at = now;
			unsaid = 0;
		} else {
			unsaid++;
		}
	}
	return ok ? 0 : EIO;
}

static int
panel_sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = panel_level, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	return panel_set(value);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_KERN,
    NULL, 0,
    panel_sysctl_brightness, "I", "the panel's brightness, 0-255 (-1: never set); writing sets it (Galaxy S9, panel=1)");

/* brightness 0-255 for the framebuffer (SweetFramebuffer.cpp): 0, or an errno (EINVAL while the panel is not started) */
int hakios_panel_brightness(int level);
int
hakios_panel_brightness(int level)
{
	return panel_set(level);
}

/* a brightness in nits (iOS 27's swap brightness): the lowest level whose luminance row reaches it; the level sent, or -1 */
int hakios_panel_brightness_nits(uint32_t nits);
int
hakios_panel_brightness_nits(uint32_t nits)
{
	int level = 255;

	if (gamma_ready && gamma_panel != NULL) {
		for (int l = 0; l <= 255; l++) {
			if (gamma_panel->rows[ha8_row_of_level(l)].nits >= nits) {
				level = l;
				break;
			}
		}
	} else {
		level = nits >= 400 ? 255 : (int)(nits * 255 / 400);
	}
	return panel_set(level) == 0 ? level : -1;
}


/*
 * The panel dark, for a halt that powers the phone off (drivers/power/exynos/hakios.exynos_poweroff.c, halt_off=N): Samsung's
 * display-off sequence (9F A5 A5, 28, 9F 5A 5A: star_a3_s0_display_off_cmdtbl) and then its exit sequence's sleep-in
 * (9F A5 A5, 10, 9F 5A 5A, and its 100 ms: star_a3_s0_exit_cmdtbl, star_a3_s0_wait_sleep_in), with a frame's time between
 * the two, as when Android turns the screen off. Without panel=1 too: the DSIM is mapped here if the driver never started.
 * The lock is only tried (a halt does not wait on a brightness change that never ends); taken or not, the packets go.
 * True when the DSIM took all of them. After it the panel shows nothing and keeps nothing to show: nothing turns it on
 * again but its own power-on sequence, which only a restart (S-Boot) sends.
 */
bool hakios_s6e3ha8_blank(int (*say_as)(const char *, ...));

bool
hakios_s6e3ha8_blank(int (*say_as)(const char *, ...))
{
	static const uint8_t key1_on[] = { 0x9F, 0xA5, 0xA5 }, key1_off[] = { 0x9F, 0x5A, 0x5A };
	static const uint8_t display_off[] = { 0x28 }, sleep_in[] = { 0x10 };
	bool locked = panel_lock != NULL && lck_mtx_try_lock(panel_lock), off, asleep;

	if (dsim == 0) {
		dsim = ml_io_map(DSIM_PA, 0x1000);
	}
	if (dsim == 0) {
		say_as("s6e3ha8: blank: cannot map the DSIM at 0x%lx\n", DSIM_PA);
		if (locked) {
			lck_mtx_unlock(panel_lock);
		}
		return false;
	}
	off = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(display_off, sizeof(display_off));
	off = dsim_send(key1_off, sizeof(key1_off)) && off;
	say_as("s6e3ha8: blank: display off (28) %s (FIFOCTRL %x, INTSRC %x)%s\n", off ? "sent" : "NOT SENT", rd(DSIM_FIFOCTRL),
	    rd(DSIM_INTSRC), panel_lock == NULL ? "" : locked ? "" : "; the panel's lock was held: sent without it");
	delay(20 * 1000);                       /* a frame and more at 60 Hz */
	asleep = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(sleep_in, sizeof(sleep_in));
	asleep = dsim_send(key1_off, sizeof(key1_off)) && asleep;
	delay(100 * 1000);                      /* Samsung's wait after sleep-in, before the supplies go */
	say_as("s6e3ha8: blank: sleep-in (10) %s (FIFOCTRL %x, INTSRC %x)\n", asleep ? "sent" : "NOT SENT", rd(DSIM_FIFOCTRL),
	    rd(DSIM_INTSRC));
	if (locked) {
		lck_mtx_unlock(panel_lock);
	}
	return off && asleep;
}


/*
 * panel_sleep=1 (off by default; with panel=1): the screen's off and on from iOS (backboardd's power request to the
 * display service, MiniIOMobileFramebuffer.cpp, through hakios_display_power() in GenericARMPE.cpp) reach the panel:
 *   off  9F A5 A5, 28 (display off), 9F 5A 5A; a frame; 9F A5 A5, 10 (sleep-in), 9F 5A 5A; Samsung's wait after sleep-in
 *        (star_a3_s0_display_off_cmdtbl, then the sleep-in of star_a3_s0_exit_cmdtbl). The supplies stay on, and with
 *        them what the panel was set to: its DSC and PPS, TE, the gamma.
 *   on   9F A5 A5, 11 (sleep-out), 9F 5A 5A; Samsung's 10 ms after sleep-out (star_a3_s0_wait_sleep_out) and a frame more;
 *        the brightness last set, again; then 9F A5 A5, 29 (display on), 9F 5A 5A (star_a3_s0_display_on_cmdtbl).
 * Android goes further on its screen-off (the supplies off, the panel reset, and its whole init sequence at the next
 * screen-on: DSC, PPS, sleep-out, TE on, ...). Here nothing of that init is sent again; whether the S6E3HA8 keeps all of
 * it through a sleep-in is for the phone to show (if the picture comes back wrong or not at all: panel_sleep=0, and the
 * screen as before -- dark only by its brightness). 0 sent, ENODEV without panel_sleep=1 (or panel=1), EIO if the DSIM
 * did not take a packet.
 */
static uint32_t panel_sleep;
static bool panel_asleep;

int hakios_s6e3ha8_power(bool on);

int
hakios_s6e3ha8_power(bool on)
{
	static const uint8_t key1_on[] = { 0x9F, 0xA5, 0xA5 }, key1_off[] = { 0x9F, 0x5A, 0x5A };
	static const uint8_t display_off[] = { 0x28 }, sleep_in[] = { 0x10 }, sleep_out[] = { 0x11 }, display_on[] = { 0x29 };
	bool first, second;
	int level;

	if (!panel_sleep || dsim == 0 || panel_lock == NULL) {
		return ENODEV;
	}
	lck_mtx_lock(panel_lock);
	if (on == !panel_asleep) {
		lck_mtx_unlock(panel_lock);
		return 0;                       /* (already so) */
	}
	if (!on) {
		first = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(display_off, sizeof(display_off));
		first = dsim_send(key1_off, sizeof(key1_off)) && first;
		delay(20 * 1000);               /* a frame and more at 60 Hz */
		second = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(sleep_in, sizeof(sleep_in));
		second = dsim_send(key1_off, sizeof(key1_off)) && second;
		delay(100 * 1000);              /* Samsung's wait after sleep-in */
		panel_asleep = true;
		level = panel_level;
	} else {
		first = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(sleep_out, sizeof(sleep_out));
		first = dsim_send(key1_off, sizeof(key1_off)) && first;
		delay(30 * 1000);               /* Samsung's 10 ms, and a frame more */
		panel_asleep = false;
		level = panel_level;
		second = true;
	}
	lck_mtx_unlock(panel_lock);
	if (on) {
		if (level >= 0) {
			(void)panel_set(level);         /* (logs itself) */
		}
		lck_mtx_lock(panel_lock);
		delay(20 * 1000);
		second = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(display_on, sizeof(display_on));
		second = dsim_send(key1_off, sizeof(key1_off)) && second;
		lck_mtx_unlock(panel_lock);
	}
	say("s6e3ha8: panel_sleep: %s: %s %s, %s %s (brightness %d; FIFOCTRL %x, INTSRC %x)\n", on ? "screen on" : "screen off",
	    on ? "sleep-out (11)" : "display off (28)", first ? "sent" : "NOT SENT", on ? "display on (29)" : "sleep-in (10)",
	    second ? "sent" : "NOT SENT", level, rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC));
	return first && second ? 0 : EIO;
}

void
hakios_s6e3ha8_init(void)
{
	uint32_t enabled = 0, level = 0, mode = 2;

	(void)PE_parse_boot_argn("panel", &enabled, sizeof(enabled));
	if ((sweet_speed_flags() & 512) != 0) {
		enabled = 1;                    /* (the iOS 27 command line is short of room: sweetspeed&512 is panel=1) */
	}
	if (enabled == 0) {
		return;
	}
	panel_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s6e3ha8", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	dsim = ml_io_map(DSIM_PA, 0x1000);
	if (dsim == 0) {
		say("s6e3ha8: cannot map the DSIM at 0x%lx\n", DSIM_PA);
		return;
	}
	(void)PE_parse_boot_argn("panel_gamma", &mode, sizeof(mode));
	say("s6e3ha8: DSIM0: FIFOCTRL %x, INTSRC %x; panel_gamma=%u; kern.hakios.brightness sets the panel's brightness\n",
	    rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC), mode);
	if (mode >= 1) {
		panel_gamma_init(mode);
	}
	(void)hakios_panel_register("s6e3ha8", panel_set);
	(void)PE_parse_boot_argn("panel_sleep", &panel_sleep, sizeof(panel_sleep));
	if (panel_sleep) {
		say("s6e3ha8: panel_sleep=%u: the screen's off from iOS puts the panel to sleep (28, 10), its on wakes it (11, 29)\n",
		    panel_sleep);
	}
	if (PE_parse_boot_argn("panel_brightness", &level, sizeof(level))) {
		int error = panel_set((int)level);

		if (gamma_ready && level <= 255) {
			say("s6e3ha8: brightness %u: %s (the row of %u nits, AOR 0x%04x)\n", level, error == 0 ? "sent" :
			    "NOT SENT", gamma_panel->rows[ha8_row_of_level((int)level)].nits, gamma_panel->aor[level]);
		} else {
			say("s6e3ha8: brightness %u: %s\n", level, error == 0 ? "sent" : "NOT SENT");
		}
	}
}
