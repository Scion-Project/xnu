/*
 * The arithmetic and the byte formats of the Galaxy S9's Bluetooth bring-up (drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c), in
 * plain C with no kernel headers, so that tools/bcm_hci_test.c checks every one of them on a computer
 * (tools/bcm-hci-test.sh). Nothing in here touches hardware.
 *
 *   H4           the UART transport of HCI: a packet is one type byte and the packet itself; 1 command (opcode le16,
 *                parameter length u8, parameters), 2 ACL data (handle and flags le16, length le16), 3 SCO (handle le16,
 *                length u8), 4 event (code u8, length u8). Linux's include/net/bluetooth/hci.h:279-282 names the types.
 *   events       Command Complete (0x0E: Num_HCI_Command_Packets u8, the opcode le16, the return parameters, the first of
 *                which is a status for every command used here) and Command Status (0x0F: status, Num_HCI_Command_
 *                Packets, opcode), hci.h:1593-1605; Read Local Version's return (status, HCI version u8, HCI revision le16,
 *                LMP version u8, manufacturer le16, LMP subversion le16), hci.h:1114-1122.
 *   .hcd         Broadcom's patch file: nothing but HCI commands one after another, each as opcode le16, length u8 and
 *                that many bytes, which are sent to the chip in order after Download Minidriver (0xFC2E); the records are
 *                Write_RAM (0xFC4C: a le32 address, then the bytes) and a last Launch_RAM (0xFC4E: the address
 *                0xFFFFFFFF). That is how Linux's btbcm.c sends it (btbcm_patchram(), drivers/bluetooth/btbcm.c:106-167 of
 *                the S9's kernel). The S9's two files (bcm4361B2_murata.hcd, _semco.hcd of the phone's /vendor/firmware)
 *                were checked to be exactly that, 440 Write_RAM and one Launch_RAM each, but they are not in this
 *                repository and the test does not use them.
 *   the UART     the divisor of an Exynos UART for a baud rate from its clock: UBRDIV = div / 16 - 1 and UFRACVAL =
 *                div % 16, where div = clock / baud, rounded up when that lands nearer (unless the sixteenths are 15),
 *                as Samsung's serial driver does (drivers/tty/serial/samsung.c:1088-1110 and 1186-1212).
 *
 * The GPL sources above were read for these facts (field orders, numbers, the rounding rule), not copied.
 */
#ifndef _HAKIOS_BCM_HCI_H_
#define _HAKIOS_BCM_HCI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H4_COMMAND              0x01
#define H4_ACL                  0x02
#define H4_SCO                  0x03
#define H4_EVENT                0x04

#define HCI_EV_COMMAND_COMPLETE 0x0E
#define HCI_EV_COMMAND_STATUS   0x0F
#define HCI_EV_VENDOR           0xFF

#define HCI_RESET               0x0C03
#define HCI_READ_LOCAL_NAME     0x0C14
#define HCI_READ_LOCAL_VERSION  0x1001
#define BCM_DOWNLOAD_MINIDRIVER 0xFC2E
#define BCM_WRITE_RAM           0xFC4C
#define BCM_LAUNCH_RAM          0xFC4E
#define BCM_MANUFACTURER        0x000F  /* Broadcom's company identifier (hci_bcm.c:816 gives 15) */

/* ---- H4, out ------------------------------------------------------------------------------------------------- */

/* A command as H4 bytes into out: 4 + plen of them, or 0 when out is too small. */
static inline size_t
h4_command(uint8_t *out, size_t cap, uint16_t opcode, const uint8_t *param, uint8_t plen)
{
	if (cap < 4u + plen) {
		return 0;
	}
	out[0] = H4_COMMAND;
	out[1] = (uint8_t)(opcode & 0xff);
	out[2] = (uint8_t)(opcode >> 8);
	out[3] = plen;
	for (unsigned i = 0; i < plen; i++) {
		out[4 + i] = param[i];
	}
	return 4u + plen;
}

/* ---- H4, in -------------------------------------------------------------------------------------------------- */

/*
 * The receiving side, a byte at a time. An event is kept whole (it is at most 2 + 255 bytes); ACL and SCO data, which
 * nothing here asks for, are counted and dropped. A byte that cannot start a packet is dropped and counted (a UART
 * that has lost sync), and the next one is tried as a type.
 */
struct h4_rx {
	uint8_t  type;                  /* 0: waiting for a type byte */
	uint16_t have;                  /* bytes of the packet (after the type) seen */
	uint16_t need;                  /* bytes the packet has in all, once its header says; 0 before */
	uint8_t  event[2 + 255];        /* an event: code, length, parameters */
	uint32_t dropped_bytes;         /* not a packet type */
	uint32_t dropped_packets;       /* ACL and SCO */
};

enum { H4_RX_MORE = 0, H4_RX_EVENT = 1, H4_RX_SKIPPED = 2, H4_RX_JUNK = 3 };

static inline void
h4_rx_reset(struct h4_rx *rx)
{
	rx->type = 0;
	rx->have = 0;
	rx->need = 0;
}

/* One byte in. H4_RX_EVENT: rx->event holds a whole event, 2 + rx->event[1] bytes, until the next call. */
static inline int
h4_rx_byte(struct h4_rx *rx, uint8_t b)
{
	if (rx->type == 0) {
		if (b == H4_EVENT || b == H4_ACL || b == H4_SCO) {
			rx->type = b;
			rx->have = 0;
			rx->need = 0;
			return H4_RX_MORE;
		}
		rx->dropped_bytes++;
		return H4_RX_JUNK;
	}
	if (rx->type == H4_EVENT) {
		rx->event[rx->have++] = b;
		if (rx->have == 2) {
			rx->need = (uint16_t)(2 + rx->event[1]);
		}
		if (rx->have >= 2 && rx->have == rx->need) {
			rx->type = 0;
			return H4_RX_EVENT;
		}
		return H4_RX_MORE;
	}
	/* ACL: 4 bytes of header, the length le16 in the last two; SCO: 3, the length u8 in the last */
	if (rx->need == 0) {
		uint16_t hdr = rx->type == H4_ACL ? 4 : 3;

		if (rx->have < 4) {
			rx->event[rx->have] = b;        /* the header only, kept in the event buffer while it is read */
		}
		rx->have++;
		if (rx->have == hdr) {
			uint16_t len = rx->type == H4_ACL ? (uint16_t)(rx->event[2] | rx->event[3] << 8) : rx->event[2];

			rx->need = (uint16_t)(hdr + len);
		}
	} else {
		rx->have++;
	}
	if (rx->need != 0 && rx->have == rx->need) {
		rx->type = 0;
		rx->dropped_packets++;
		return H4_RX_SKIPPED;
	}
	return H4_RX_MORE;
}

/* ---- events -------------------------------------------------------------------------------------------------- */

/*
 * Command Complete: event (code, length, parameters). true with the opcode, the status (the first return parameter;
 * 0xFF when there is none) and the return parameters after the status.
 */
static inline bool
hci_command_complete(const uint8_t *event, uint16_t *opcode, uint8_t *status, const uint8_t **ret, uint8_t *retlen)
{
	uint8_t len = event[1];

	if (event[0] != HCI_EV_COMMAND_COMPLETE || len < 3) {
		return false;
	}
	*opcode = (uint16_t)(event[3] | event[4] << 8);
	*status = len >= 4 ? event[5] : 0xFF;
	*ret = &event[6];
	*retlen = len >= 4 ? (uint8_t)(len - 4) : 0;
	return true;
}

/* Command Status: a status of 0 says the command was taken and will end in another event. */
static inline bool
hci_command_status(const uint8_t *event, uint16_t *opcode, uint8_t *status)
{
	if (event[0] != HCI_EV_COMMAND_STATUS || event[1] < 4) {
		return false;
	}
	*status = event[2];
	*opcode = (uint16_t)(event[4] | event[5] << 8);
	return true;
}

struct hci_local_version {
	uint8_t  hci_ver;
	uint16_t hci_rev;
	uint8_t  lmp_ver;
	uint16_t manufacturer;
	uint16_t lmp_subver;
};

/* Read Local Version's return parameters after the status (8 bytes). */
static inline bool
hci_local_version(const uint8_t *ret, uint8_t retlen, struct hci_local_version *v)
{
	if (retlen < 8) {
		return false;
	}
	v->hci_ver = ret[0];
	v->hci_rev = (uint16_t)(ret[1] | ret[2] << 8);
	v->lmp_ver = ret[3];
	v->manufacturer = (uint16_t)(ret[4] | ret[5] << 8);
	v->lmp_subver = (uint16_t)(ret[6] | ret[7] << 8);
	return true;
}

/* Read Local Name's return parameters after the status: up to 248 bytes, ended by a 0 when shorter; printable copy. */
static inline void
hci_local_name(const uint8_t *ret, uint8_t retlen, char *out, size_t cap)
{
	size_t n = 0;

	for (unsigned i = 0; i < retlen && i < 248 && ret[i] != 0 && n + 1 < cap; i++) {
		out[n++] = (ret[i] >= 0x20 && ret[i] < 0x7f) ? (char)ret[i] : '?';
	}
	if (cap > 0) {
		out[n] = 0;
	}
}

/* ---- the .hcd file ------------------------------------------------------------------------------------------- */

struct bcm_hcd_record {
	uint16_t opcode;
	uint8_t  plen;
	const uint8_t *param;
};

/* The next record at *off: 1 and the record, 0 at the end of the file, -1 when the file ends inside a record. */
static inline int
bcm_hcd_next(const uint8_t *data, size_t size, size_t *off, struct bcm_hcd_record *rec)
{
	size_t at = *off;

	if (at >= size) {
		return 0;
	}
	if (size - at < 3) {
		return -1;
	}
	rec->opcode = (uint16_t)(data[at] | data[at + 1] << 8);
	rec->plen = data[at + 2];
	if (size - at - 3 < rec->plen) {
		return -1;
	}
	rec->param = &data[at + 3];
	*off = at + 3 + rec->plen;
	return 1;
}

struct bcm_hcd_info {
	uint32_t records;               /* all of them */
	uint32_t writes;                /* Write_RAM */
	uint32_t write_bytes;           /* the bytes they write */
	uint32_t others;                /* any other vendor command (none in the S9's files) */
	uint32_t launch_address;        /* Launch_RAM's */
	char     name[64];              /* the patch's own description, "BCM4361B2 ..." where the file has one */
};

enum {
	BCM_HCD_OK = 0,
	BCM_HCD_EMPTY = -1,             /* no records */
	BCM_HCD_TRUNCATED = -2,         /* the file ends inside a record */
	BCM_HCD_NOT_VENDOR = -3,        /* a record that is not a vendor command (OGF 0x3F): not a patch file */
	BCM_HCD_SHORT_WRITE = -4,       /* Write_RAM with fewer than 4 bytes (no address) */
	BCM_HCD_NO_LAUNCH = -5,         /* the last record is not Launch_RAM */
	BCM_HCD_LAUNCH_EARLY = -6,      /* a Launch_RAM before the last record */
	BCM_HCD_BAD_LAUNCH = -7,        /* Launch_RAM without its 4-byte address */
};

static inline const char *
bcm_hcd_error(int e)
{
	switch (e) {
	case BCM_HCD_OK: return "good";
	case BCM_HCD_EMPTY: return "no records";
	case BCM_HCD_TRUNCATED: return "the file ends inside a record";
	case BCM_HCD_NOT_VENDOR: return "a record that is not a vendor command";
	case BCM_HCD_SHORT_WRITE: return "a Write_RAM without its address";
	case BCM_HCD_NO_LAUNCH: return "the last record is not Launch_RAM";
	case BCM_HCD_LAUNCH_EARLY: return "a Launch_RAM before the end";
	case BCM_HCD_BAD_LAUNCH: return "a Launch_RAM without its address";
	default: return "?";
	}
}

/* The description string: the first run of at least 12 printable bytes beginning "BCM", in Write_RAM data. */
static inline void
bcm_hcd_find_name(const uint8_t *p, uint8_t n, char *name, size_t cap)
{
	if (name[0] != 0 || cap == 0) {
		return;
	}
	for (unsigned i = 0; i + 3 <= n; i++) {
		unsigned len = 0;

		if (p[i] != 'B' || p[i + 1] != 'C' || p[i + 2] != 'M') {
			continue;
		}
		while (i + len < n && p[i + len] >= 0x20 && p[i + len] < 0x7f) {
			len++;
		}
		if (len >= 12) {
			size_t k;

			for (k = 0; k < len && k + 1 < cap; k++) {
				name[k] = (char)p[i + k];
			}
			name[k] = 0;
			return;
		}
	}
}

/*
 * The whole file read through before a byte of it goes to the chip: BCM_HCD_OK and what is in it, or why it is not a
 * patch to send. A patch is any number of vendor commands (Write_RAM, in the S9's files nothing else) and Launch_RAM last.
 */
static inline int
bcm_hcd_check(const uint8_t *data, size_t size, struct bcm_hcd_info *info)
{
	struct bcm_hcd_record rec;
	size_t off = 0;
	int r;
	bool launched = false;

	*info = (struct bcm_hcd_info){ 0 };
	while ((r = bcm_hcd_next(data, size, &off, &rec)) == 1) {
		if (launched) {
			return BCM_HCD_LAUNCH_EARLY;
		}
		info->records++;
		if ((rec.opcode >> 10) != 0x3F) {
			return BCM_HCD_NOT_VENDOR;
		}
		if (rec.opcode == BCM_WRITE_RAM) {
			if (rec.plen < 4) {
				return BCM_HCD_SHORT_WRITE;
			}
			info->writes++;
			info->write_bytes += rec.plen - 4u;
			bcm_hcd_find_name(rec.param + 4, (uint8_t)(rec.plen - 4), info->name, sizeof(info->name));
		} else if (rec.opcode == BCM_LAUNCH_RAM) {
			if (rec.plen < 4) {
				return BCM_HCD_BAD_LAUNCH;
			}
			info->launch_address = (uint32_t)rec.param[0] | (uint32_t)rec.param[1] << 8 |
			    (uint32_t)rec.param[2] << 16 | (uint32_t)rec.param[3] << 24;
			launched = true;
		} else {
			info->others++;
		}
	}
	if (r < 0) {
		return BCM_HCD_TRUNCATED;
	}
	if (info->records == 0) {
		return BCM_HCD_EMPTY;
	}
	return launched ? BCM_HCD_OK : BCM_HCD_NO_LAUNCH;
}

/* ---- the UART's divisor --------------------------------------------------------------------------------------- */

struct exynos_uart_div {
	uint32_t ubrdiv;                /* UBRDIV (0x28) */
	uint32_t fracval;               /* UFRACVAL (0x2C): sixteenths */
	uint32_t actual;                /* the baud rate that gives */
};

/* false when the clock is too slow for the rate (fewer than 16 clocks a bit) or a value is 0. */
static inline bool
exynos_uart_divisor(uint32_t clock_hz, uint32_t baud, struct exynos_uart_div *d)
{
	uint32_t div;

	if (clock_hz == 0 || baud == 0 || clock_hz / baud < 16) {
		return false;
	}
	div = clock_hz / baud;
	if ((div & 15) != 15) {
		uint32_t down = clock_hz / div, up = clock_hz / (div + 1);
		uint32_t e_down = baud > down ? baud - down : down - baud;
		uint32_t e_up = baud > up ? baud - up : up - baud;

		if (e_down > e_up) {
			div++;
		}
	}
	d->ubrdiv = div / 16 - 1;
	d->fracval = div & 15;
	d->actual = clock_hz / div;
	return true;
}

/* ---- the GPIO expander (PCAL6524): one bit of a port register --------------------------------------------------- */

static inline uint8_t
pcal_with_bit(uint8_t value, unsigned bit, bool on)
{
	return on ? (uint8_t)(value | (1u << bit)) : (uint8_t)(value & ~(1u << bit));
}

#endif /* _HAKIOS_BCM_HCI_H_ */
