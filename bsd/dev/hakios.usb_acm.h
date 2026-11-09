/*
 * A USB CDC-ACM function's descriptors and its control requests, as plain C: what the USB gadget driver
 * (drivers/usb/hakios.dwc3.c) answers a host with, and what tools/usb_acm_test.c checks on the build machine
 * (tools/usb-acm-test.sh) by playing the requests Linux, macOS and Windows make when a device is plugged in.
 *
 * Nothing here touches hardware or the kernel: a setup packet goes in, an answer comes out (data to send, data
 * to expect, a status stage, or a stall) with what the controller has to do about it (an address, a
 * configuration, an endpoint's halt, the host opening the port). The controller driver does the transfers.
 *
 * The device: USB 2.0 (high speed, full speed), one configuration, class 02 (communications) at the device
 * level, so that no interface association is needed: interface 0 is the ACM control interface with an interrupt
 * IN endpoint (0x82) that is never sent on, interface 1 the data interface with bulk OUT 0x01 and bulk IN 0x81.
 * Linux binds cdc_acm to it (/dev/ttyACM0), macOS AppleUSBCDCACM (/dev/cu.usbmodem*), Windows 10 and later
 * usbser.sys. Protocol 0 (no AT commands) keeps modem managers from talking to it as a modem.
 *
 * The identity is pid.codes' test VID/PID (0x1209/0x0001): for development only, never to be shipped.
 *
 * Sources: the USB 2.0 specification, chapter 9 (requests, descriptors); the USB CDC 1.2 and PSTN 1.2
 * specifications (the functional descriptors, SET/GET_LINE_CODING, SET_CONTROL_LINE_STATE, SEND_BREAK).
 * No code of any other implementation was used.
 */
#ifndef _HAKIOS_USB_ACM_H_
#define _HAKIOS_USB_ACM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

#define USBACM_VID              0x1209  /* pid.codes */
#define USBACM_PID              0x0001  /* pid.codes' test PID: development only */
#define USBACM_EP0_MPS          64
#define USBACM_EP_OUT           0x01
#define USBACM_EP_IN            0x81
#define USBACM_EP_NOTIFY        0x82
#define USBACM_HS_BULK          512
#define USBACM_FS_BULK          64
#define USBACM_CONFIG_LEN       67
#define USBACM_STRINGS          5

/* what the controller is to do with a setup packet */
enum usbacm_action {
	USBACM_STALL = 0,       /* stall ep0, then wait for the next setup */
	USBACM_STATUS,          /* no data stage: the status stage at once (IN, zero length) */
	USBACM_IN,              /* send r.data (r.len bytes), then the status stage (OUT) */
	USBACM_OUT,             /* receive r.len bytes, give them to usbacm_out_done(), then the status stage (IN) */
};

/* side effects, for the controller; more than one can be set */
#define USBACM_EFF_ADDRESS      0x01u   /* r.value: the new address; set it before the status stage */
#define USBACM_EFF_CONFIG       0x02u   /* r.value: the configuration (1: endpoints up, 0: down) */
#define USBACM_EFF_HALT         0x04u   /* r.ep: halt that endpoint */
#define USBACM_EFF_CLEAR_HALT   0x08u   /* r.ep: clear that endpoint's halt (and reset its toggle) */
#define USBACM_EFF_LINES        0x10u   /* SET_CONTROL_LINE_STATE: s.lines has DTR (bit 0) and RTS (bit 1) */
#define USBACM_EFF_LINE_CODING  0x20u   /* SET_LINE_CODING's data arrived: s.line_coding */
#define USBACM_EFF_BREAK        0x40u

struct usbacm_reply {
	int             action;
	const uint8_t   *data;
	uint16_t        len;
	uint32_t        effects;
	uint8_t         value;
	uint8_t         ep;
};

struct usbacm {
	bool            high_speed;     /* the controller says which, at connect-done */
	uint8_t         address;
	uint8_t         config;
	uint16_t        lines;          /* DTR bit 0, RTS bit 1 */
	uint8_t         line_coding[7]; /* dwDTERate (LE), bCharFormat, bParityType, bDataBits */
	uint8_t         halted;         /* bit 0: 0x01, bit 1: 0x81, bit 2: 0x82 */
	uint8_t         out_request;    /* the request whose OUT data stage is under way, 0 if none */
	const char      *strings[USBACM_STRINGS];      /* 1-4 used; ASCII */
	uint8_t         buf[256];       /* answers are built here */
};

static const uint8_t usbacm_device_desc[18] = {
	18, 1, 0x00, 0x02,              /* bLength, DEVICE, bcdUSB 2.00 (no BOS asked for) */
	0x02, 0x00, 0x00,               /* class: communications, at device level */
	USBACM_EP0_MPS,
	USBACM_VID & 0xff, USBACM_VID >> 8, USBACM_PID & 0xff, USBACM_PID >> 8,
	0x00, 0x01,                     /* bcdDevice 1.00 */
	1, 2, 3, 1                      /* iManufacturer, iProduct, iSerialNumber, bNumConfigurations */
};

static const uint8_t usbacm_qualifier_desc[10] = {
	10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, USBACM_EP0_MPS, 1, 0
};

static const uint8_t usbacm_lang_desc[4] = { 4, 3, 0x09, 0x04 };   /* English (United States) */

static inline void
usbacm_init(struct usbacm *s)
{
	memset(s, 0, sizeof(*s));
	s->line_coding[0] = 0x00;       /* 115200 8N1: a value to answer GET_LINE_CODING with, nothing depends on it */
	s->line_coding[1] = 0xc2;
	s->line_coding[2] = 0x01;
	s->line_coding[6] = 8;
	s->strings[1] = "HakiOS";
	s->strings[2] = "HakiOS kernel log and console";
	s->strings[3] = "0";
	s->strings[4] = "log";
}

/* the configuration descriptor at a speed (type 2, or 7 for "other speed"); USBACM_CONFIG_LEN bytes into out */
static inline size_t
usbacm_config_desc(uint8_t *out, bool high_speed, uint8_t type)
{
	uint16_t bulk = high_speed ? USBACM_HS_BULK : USBACM_FS_BULK;
	const uint8_t d[USBACM_CONFIG_LEN] = {
		9, type, USBACM_CONFIG_LEN, 0, 2, 1, 0, 0xc0, 50,     /* 2 interfaces, value 1, self-powered, 100 mA */
		9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 4,                     /* interface 0: CDC, ACM, no protocol */
		5, 0x24, 0x00, 0x10, 0x01,                              /* header, CDC 1.10 */
		5, 0x24, 0x01, 0x00, 1,                                 /* call management: none, data interface 1 */
		4, 0x24, 0x02, 0x02,                                    /* ACM: line coding and serial state */
		5, 0x24, 0x06, 0, 1,                                    /* union: control 0, data 1 */
		7, 5, USBACM_EP_NOTIFY, 3, 16, 0, (uint8_t)(high_speed ? 9 : 32),  /* interrupt IN, 16 bytes, 32 ms */
		9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,                     /* interface 1: CDC data */
		7, 5, USBACM_EP_OUT, 2, (uint8_t)(bulk & 0xff), (uint8_t)(bulk >> 8), 0,
		7, 5, USBACM_EP_IN, 2, (uint8_t)(bulk & 0xff), (uint8_t)(bulk >> 8), 0,
	};

	memcpy(out, d, sizeof(d));
	return sizeof(d);
}

/* a string descriptor of ASCII text (UTF-16LE), at most cap bytes; its length */
static inline size_t
usbacm_string_desc(uint8_t *out, size_t cap, const char *ascii)
{
	size_t n = 2;

	for (; *ascii != 0 && n + 2 <= cap && n + 2 <= 254; ascii++, n += 2) {
		out[n] = (uint8_t)*ascii;
		out[n + 1] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static inline uint8_t
usbacm_ep_bit(uint8_t ep)
{
	return ep == USBACM_EP_OUT ? 1 : ep == USBACM_EP_IN ? 2 : ep == USBACM_EP_NOTIFY ? 4 : 0;
}

static inline void
usbacm_in(struct usbacm_reply *r, const uint8_t *data, size_t len, uint16_t wlength)
{
	r->data = data;
	r->len = (uint16_t)(len < wlength ? len : wlength);
	/* nothing asked for (wLength 0): no data stage */
	r->action = r->len == 0 && wlength == 0 ? USBACM_STATUS : USBACM_IN;
}

/*
 * One setup packet. The answer in *r; returns r->action. s->buf holds what an IN answer points at, until the next
 * call.
 */
static inline int
usbacm_setup(struct usbacm *s, const uint8_t p[8], struct usbacm_reply *r)
{
	uint8_t type = p[0], req = p[1];
	uint16_t value = (uint16_t)(p[2] | p[3] << 8), index = (uint16_t)(p[4] | p[5] << 8), length = (uint16_t)(p[6] | p[7] << 8);
	bool in = (type & 0x80) != 0;
	uint8_t recipient = type & 0x1f;

	memset(r, 0, sizeof(*r));
	r->action = USBACM_STALL;
	s->out_request = 0;

	if ((type & 0x60) == 0x00) {            /* standard */
		switch (req) {
		case 0x00:                      /* GET_STATUS */
			if (!in || length < 2 || value != 0) {
				break;
			}
			s->buf[0] = 0;
			s->buf[1] = 0;
			if (recipient == 0) {
				s->buf[0] = 1;  /* self-powered */
			} else if (recipient == 1) {
				if (s->config == 0 || index > 1) {
					break;
				}
			} else if (recipient == 2) {
				uint8_t bit = usbacm_ep_bit((uint8_t)index);

				if ((index & 0x7f) != 0 && (bit == 0 || s->config == 0)) {
					break;
				}
				s->buf[0] = (s->halted & bit) ? 1 : 0;
			} else {
				break;
			}
			usbacm_in(r, s->buf, 2, length);
			break;
		case 0x01:                      /* CLEAR_FEATURE */
		case 0x03:                      /* SET_FEATURE */
			if (in || length != 0) {
				break;
			}
			if (recipient == 0 && value == 1) {     /* DEVICE_REMOTE_WAKEUP: not offered, accepted as no-op */
				r->action = USBACM_STATUS;
			} else if (recipient == 2 && value == 0) {  /* ENDPOINT_HALT */
				uint8_t bit = usbacm_ep_bit((uint8_t)index);

				if ((index & 0x7f) == 0) {
					r->action = USBACM_STATUS;      /* ep0's halt clears itself */
					break;
				}
				if (bit == 0 || s->config == 0) {
					break;
				}
				if (req == 0x01) {
					s->halted &= (uint8_t)~bit;
					r->effects |= USBACM_EFF_CLEAR_HALT;
				} else {
					s->halted |= bit;
					r->effects |= USBACM_EFF_HALT;
				}
				r->ep = (uint8_t)index;
				r->action = USBACM_STATUS;
			}
			break;
		case 0x05:                      /* SET_ADDRESS */
			if (in || recipient != 0 || length != 0 || index != 0 || value > 127 || s->config != 0) {
				break;
			}
			s->address = (uint8_t)value;
			r->effects |= USBACM_EFF_ADDRESS;
			r->value = (uint8_t)value;
			r->action = USBACM_STATUS;
			break;
		case 0x06: {                    /* GET_DESCRIPTOR */
			uint8_t dtype = (uint8_t)(value >> 8), dindex = (uint8_t)value;
			size_t n;

			if (!in || recipient != 0) {
				break;
			}
			switch (dtype) {
			case 1:
				usbacm_in(r, usbacm_device_desc, sizeof(usbacm_device_desc), length);
				break;
			case 2:
			case 7:
				if (dindex != 0) {
					break;
				}
				/* 7, other speed: the configuration at the speed this is not running at */
				n = usbacm_config_desc(s->buf, dtype == 2 ? s->high_speed : !s->high_speed, dtype);
				usbacm_in(r, s->buf, n, length);
				break;
			case 3:
				if (dindex == 0) {
					usbacm_in(r, usbacm_lang_desc, sizeof(usbacm_lang_desc), length);
				} else if (dindex < USBACM_STRINGS && s->strings[dindex] != NULL) {
					n = usbacm_string_desc(s->buf, sizeof(s->buf), s->strings[dindex]);
					usbacm_in(r, s->buf, n, length);
				}
				break;
			case 6:
				/* a high-speed capable device answers it at either speed */
				usbacm_in(r, usbacm_qualifier_desc, sizeof(usbacm_qualifier_desc), length);
				break;
			default:                        /* BOS (15) and the rest: not had */
				break;
			}
			break;
		}
		case 0x08:                      /* GET_CONFIGURATION */
			if (!in || recipient != 0 || length < 1) {
				break;
			}
			s->buf[0] = s->config;
			usbacm_in(r, s->buf, 1, length);
			break;
		case 0x09:                      /* SET_CONFIGURATION */
			if (in || recipient != 0 || length != 0 || value > 1 || s->address == 0) {
				break;
			}
			s->config = (uint8_t)value;
			s->halted = 0;
			r->effects |= USBACM_EFF_CONFIG;
			r->value = (uint8_t)value;
			r->action = USBACM_STATUS;
			break;
		case 0x0a:                      /* GET_INTERFACE */
			if (!in || recipient != 1 || s->config == 0 || index > 1 || length < 1) {
				break;
			}
			s->buf[0] = 0;
			usbacm_in(r, s->buf, 1, length);
			break;
		case 0x0b:                      /* SET_INTERFACE: alternate 0 only */
			if (in || recipient != 1 || s->config == 0 || index > 1 || value != 0 || length != 0) {
				break;
			}
			r->action = USBACM_STATUS;
			break;
		default:
			break;
		}
		return r->action;
	}

	if ((type & 0x60) == 0x20 && recipient == 1 && index == 0) {    /* class, to the ACM interface */
		switch (req) {
		case 0x20:                      /* SET_LINE_CODING */
			if (in || length != 7) {
				break;
			}
			s->out_request = req;
			r->len = 7;
			r->action = USBACM_OUT;
			break;
		case 0x21:                      /* GET_LINE_CODING */
			if (!in || length < 7) {
				break;
			}
			memcpy(s->buf, s->line_coding, 7);
			usbacm_in(r, s->buf, 7, length);
			break;
		case 0x22:                      /* SET_CONTROL_LINE_STATE */
			if (in || length != 0) {
				break;
			}
			s->lines = value & 3;
			r->effects |= USBACM_EFF_LINES;
			r->action = USBACM_STATUS;
			break;
		case 0x23:                      /* SEND_BREAK */
			if (in || length != 0) {
				break;
			}
			r->effects |= USBACM_EFF_BREAK;
			r->action = USBACM_STATUS;
			break;
		default:
			break;
		}
	}
	return r->action;
}

/* The OUT data stage of the request usbacm_setup() answered USBACM_OUT to: len bytes. USBACM_STATUS or USBACM_STALL. */
static inline int
usbacm_out_done(struct usbacm *s, const uint8_t *data, size_t len, struct usbacm_reply *r)
{
	memset(r, 0, sizeof(*r));
	r->action = USBACM_STALL;
	if (s->out_request == 0x20 && len == 7) {
		memcpy(s->line_coding, data, 7);
		r->effects |= USBACM_EFF_LINE_CODING;
		r->action = USBACM_STATUS;
	}
	s->out_request = 0;
	return r->action;
}

/* A bus reset: the default state again (address 0, not configured, nothing halted). */
static inline void
usbacm_reset(struct usbacm *s)
{
	s->address = 0;
	s->config = 0;
	s->halted = 0;
	s->lines = 0;
	s->out_request = 0;
}

/*
 * The line console's input: bytes from the host gathered into a line. Returns true when c ended a line (CR or LF;
 * an empty line is not one), which is then in l->line, NUL-terminated. Backspace and DEL take a character back;
 * other control characters and anything past the buffer are dropped.
 */
struct usbacm_line {
	char            line[128];
	uint32_t        len;
	bool            overflow;
};

static inline bool
usbacm_line_feed(struct usbacm_line *l, uint8_t c)
{
	if (c == '\r' || c == '\n') {
		if (l->len == 0) {
			l->overflow = false;
			return false;
		}
		l->line[l->len] = 0;
		l->len = 0;
		l->overflow = false;
		return true;
	}
	if (c == 8 || c == 127) {
		if (l->len > 0) {
			l->len--;
		}
		return false;
	}
	if (c < 32 || c > 126) {
		return false;
	}
	if (l->len + 1 >= sizeof(l->line)) {
		l->overflow = true;
		return false;
	}
	l->line[l->len++] = (char)c;
	return false;
}

#endif /* _HAKIOS_USB_ACM_H_ */
