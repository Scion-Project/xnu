/* included by commpage.c (arm64): the ID registers lowered to what every core has, for the Exynos M3s next to the Cortex-A55s */
#include <pexpert/pexpert.h>

/*
 * HakiOS (Galaxy S9, patch 1510): with the Exynos M3s started next to the Cortex-A55s, user space must be told only what every core
 * has: the M3 is ARMv8.0 (seen on the phone, 2 November: launchd killed with SIGILL on an M3 when it was told the A55's
 * ARMv8.2 set). Boot-arg isa_floor=1 (also set by m3_prep=3): every 4-bit field of the ID registers below is lowered to
 * the M3's -- ISAR0 AES+PMULL, SHA1, SHA2 (not 512), CRC32; nothing of ISAR1, MMFR2 or PFR1; PFR0's FP and AdvSIMD
 * without half precision. (The S9's Linux /proc/cpuinfo: fp asimd evtstrm aes pmull sha1 sha2 crc32.)
 */
static int hakios_isa_floor = -1;
static uint64_t
hakios_id_floor(uint64_t value, uint64_t floor)
{
	if (hakios_isa_floor < 0) {
		uint32_t on = 0, m3 = 0;

		PE_parse_boot_argn("isa_floor", &on, sizeof(on));
		PE_parse_boot_argn("m3_prep", &m3, sizeof(m3));
		hakios_isa_floor = (on != 0 || m3 >= 3) ? 1 : 0;
		if (hakios_isa_floor) {
			printf("commpage: isa_floor: user space told the ARMv8.0 set the Exynos M3 has\n");
		}
	}
	if (!hakios_isa_floor) {
		return value;
	}
	uint64_t out = 0;
	for (int shift = 0; shift < 64; shift += 4) {
		uint64_t a = (value >> shift) & 0xf, b = (floor >> shift) & 0xf;
		out |= (a < b ? a : b) << shift;
	}
	return out;
}
