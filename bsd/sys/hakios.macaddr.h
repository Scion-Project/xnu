/*
 * Darwin-on-hakios: a radio's own address written as text, as Samsung's Android keeps the Galaxy S9's in
 * /efs/wifi/.mac.info ("00:12:34:AB:CD:EF" and a newline; bcmdhd's dhd_custom_cis.c, dhd_set_macaddr_from_file(),
 * reads it with MAC_CUSTOM_FORMAT, and writes a random one with Samsung's prefix 00:12:34 when the file is missing or
 * all zeroes) and as boot-arg wifi_mac gives it. Nothing but C here, so that tools/brcmfmac-test.py compiles it on the
 * machine the kernel is built on.
 *
 * hakios_macaddr_parse(): six bytes of one or two hexadecimal digits each, separated by ':' (or '-'), spaces and a line's
 * end around them; nothing else after. An address of all zeroes, a group address (bit 0 of the first byte) and anything
 * else is refused: 0, and nothing written. 1 when `address` has it.
 */
#ifndef _SYS_HAKIOS_MACADDR_H_
#define _SYS_HAKIOS_MACADDR_H_

#include <stddef.h>
#include <stdint.h>

static inline int
hakios_macaddr_parse(const char *text, size_t length, uint8_t address[6])
{
	uint8_t bytes[6];
	unsigned count = 0, digits = 0, value = 0, any = 0;
	size_t i = 0;

	while (i < length && (text[i] == ' ' || text[i] == '\t')) {
		i++;
	}
	for (;; i++) {
		char c = i < length ? text[i] : 0;
		int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

		if (digit >= 0) {
			if (digits == 2 || count == 6) {
				return 0;
			}
			value = value << 4 | (unsigned)digit;
			digits++;
			continue;
		}
		if (digits == 0) {
			return 0;               /* (a separator with no digits before it, or nothing at all) */
		}
		bytes[count++] = (uint8_t)value;
		any |= value;
		digits = value = 0;
		if (count < 6) {
			if (c != ':' && c != '-') {
				return 0;
			}
			continue;
		}
		/* the sixth: only blanks and the line's end may follow, up to the end of the text or a NUL */
		for (; i < length && text[i] != 0; i++) {
			if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') {
				return 0;
			}
		}
		break;
	}
	if (any == 0 || (bytes[0] & 1) != 0) {
		return 0;
	}
	for (unsigned b = 0; b < 6; b++) {
		address[b] = bytes[b];
	}
	return 1;
}

#endif
