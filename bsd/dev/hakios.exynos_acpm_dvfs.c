/*
 * The Exynos 9810's APM asked for DVFS rates (and its "fvmap" read), for the Galaxy S9's CPU governor
 * (bsd/dev/hakios.exynos_cpufreq.c). From HakiOS/Scion: drivers/soc/exynos/hakios.exynos_acpm.c of Scion-Project/scion-kernel
 * (hakios_acpm_dvfs_get/set/table and their notes), ported to the Starbite iOS 27 (xnu-12377) STARLTE tree.
 *
 * One client on the APM's mailbox. This tree already has one: bsd/dev/starlte_touch.c's (its lock, its sequence numbers,
 * the touch supplies, vdd_g3d for starlte_gpu.c, the fixed CPU rates and the GPU's DVFS). Scion's own client is NOT
 * brought in: two of them on one mailbox would take each other's answers and number their requests alike. So every request
 * here goes through starlte_touch.c (starlte_acpm_dvfs_send(): its lock, its sequence numbers, the DVFS descriptor it
 * already uses, channel index 0, as Samsung's kernel), and the fvmap is read through starlte_acpm_sram_read() (read
 * only, 32-bit, above the SRAM's faulting first pages). hakios_acpm_init() and the PMIC calls of sys/hakios.exynos_acpm.h
 * stay the stubs of starlte_usbcon.c (their callers' behaviour unchanged); only DVFS is answered here.
 *
 * DVFS (channel 0): word 0 = the domain's index, word 1 = kHz, word 2 = 0 set, 1 get; a get's answer in word 1 (the S9's
 * APM answers 0 for both CPU domains: see the governor). Sources (Scion's notes): the phone's device tree and Samsung's
 * drivers/soc/samsung/acpm and cal-if (acpm_dvfs.c, fvmap.c, fvmap.h, fw_header/): GPL, read for these facts only.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos9810_soc.h>

/* bsd/dev/starlte_touch.c: the tree's one ACPM client */
extern bool starlte_acpm_dvfs_ready(void);
extern bool starlte_acpm_dvfs_send(uint32_t cmd[4]);
extern bool starlte_acpm_sram_read(uint32_t off, uint32_t *value);

#define SRAM_SIZE       EXYNOS_ACPM_SRAM_SIZE
#define SRAM_FIRST      EXYNOS_ACPM_SRAM_FIRST          /* below: pages that fault; never read */
#define INITDATA        EXYNOS_ACPM_INITDATA

int
hakios_acpm_request(uint32_t channel, uint32_t cmd[4])
{
	if (channel != HAKIOS_ACPM_CH_DVFS) {
		return ENODEV;          /* the PMIC's requests go through starlte_pmic_read/write */
	}
	if (!starlte_acpm_dvfs_ready()) {
		return ENODEV;
	}
	return starlte_acpm_dvfs_send(cmd) ? 0 : ETIMEDOUT;
}

int
hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz)
{
	uint32_t cmd[4] = { index, 0, 1, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);

	if (error == 0) {
		*khz = cmd[1];
	}
	return error;
}

int
hakios_acpm_dvfs_set(uint32_t index, uint32_t khz)
{
	uint32_t cmd[4] = { index, khz, 0, 0 };

	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);
}

/* an offset into the SRAM that may be touched: above the faulting pages, inside, 32-bit aligned */
static inline bool
sram_ok(uint32_t off, uint32_t bytes)
{
	return off >= SRAM_FIRST && off <= SRAM_SIZE && bytes <= SRAM_SIZE - off && (off & 3) == 0;
}

static uint32_t
srd(uint32_t off)
{
	uint32_t v = 0;

	(void)starlte_acpm_sram_read(off, &v);
	return v;
}

static bool
printable(uint32_t off, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)(srd((off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			return i > 0;
		}
		if (c < 0x20 || c > 0x7e) {
			return false;
		}
	}
	return true;
}

static void
copy_string(uint32_t off, char *out, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 1 < len; i++) {
		char c = (char)(srd((off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			break;
		}
		out[i] = c;
	}
	out[i] = 0;
}

/*
 * A DVFS domain's levels as the APM holds them, read only: its rate and voltage per level, the fastest first. Samsung's
 * kernel reads the same "fvmap" once at start and only reads it: the voltages in it are the ones the APM itself applies
 * with each rate. A DVFS request names a rate, never a voltage.
 *   initdata +0x00   where the APM's plugins are (an offset into the SRAM), +0x04 how many
 *   a plugin         0x30 bytes: +0x04 its base (bit 0 a flag), +0x20 attached (a byte), +0x24 its size, +0x28 stays
 *                    attached (a byte), +0x2C its name
 *   the DVFS plugin  the one whose name has "dvfs" in it; its map at base + the acpm node's fvmap_offset (0x6700) when the
 *                    boot left it attached, at base + size otherwise
 *   the map          a 24-byte header per domain, in the order of the DVFS indexes: +0x01 how many levels, +0x14 (16 bits)
 *                    where they are from the map's start; a level is two words, the rate (kHz) and the voltage (uV)
 */
#define ID_PLUGINS      0x00
#define ID_NPLUGINS     0x04
#define PL_SIZE         0x30
#define PL_BASE         0x04
#define PL_ATTACHED     0x20
#define PL_FWSIZE       0x24
#define PL_STAYS        0x28
#define PL_NAME         0x2C
#define FVMAP_OFFSET    EXYNOS_ACPM_FVMAP_OFFSET
#define FV_HEADER       24
#define FV_NUM_LV       0x01
#define FV_O_RATEVOLT   0x14

static uint32_t
srd8(uint32_t off)
{
	return (srd(off & ~3u) >> (8 * (off & 3))) & 0xff;
}

static uint32_t
srd16(uint32_t off)
{
	return (srd(off & ~3u) >> (8 * (off & 2))) & 0xffff;
}

static bool
names_dvfs(const char *name)
{
	for (const char *p = name; p[0] && p[1] && p[2] && p[3]; p++) {
		if ((p[0] | 0x20) == 'd' && (p[1] | 0x20) == 'v' && (p[2] | 0x20) == 'f' && (p[3] | 0x20) == 's') {
			return true;
		}
	}
	return false;
}

uint32_t
hakios_acpm_dvfs_table(uint32_t index, uint32_t *khz, uint32_t *uv, uint32_t most, int (*say)(const char *, ...))
{
	uint32_t plugins, count, map = 0, header, levels, at;
	char name[24];

	if (!starlte_acpm_dvfs_ready()) {
		return 0;
	}
	plugins = srd(INITDATA + ID_PLUGINS);
	count = srd(INITDATA + ID_NPLUGINS);
	if (count == 0 || count > 32 || !sram_ok(plugins, count * PL_SIZE)) {
		say("acpm: fvmap: the plugins' table (+%#x, %u of them) is not where it may be read\n", plugins, count);
		return 0;
	}
	for (uint32_t i = 0; i < count && map == 0; i++) {
		uint32_t p = plugins + i * PL_SIZE, at_name = srd(p + PL_NAME), base = srd(p + PL_BASE) & ~1u;
		uint32_t attached = srd8(p + PL_ATTACHED), stays = srd8(p + PL_STAYS), size = srd(p + PL_FWSIZE);

		if (at_name == 0 || !sram_ok(at_name & ~3u, sizeof(name) + 4) || !printable(at_name, sizeof(name) - 1)) {
			continue;
		}
		copy_string(at_name, name, sizeof(name));
		if (!names_dvfs(name)) {
			continue;
		}
		map = base + (attached ? FVMAP_OFFSET : size);
		say("acpm: fvmap: plugin %u \"%s\": base +%#x, attached %u, stays %u, size %#x: the map at +%#x\n", i, name,
		    base, attached, stays, size, map);
	}
	if (map == 0) {
		say("acpm: fvmap: no plugin named for DVFS among %u\n", count);
		return 0;
	}
	header = map + index * FV_HEADER;
	if (!sram_ok(header & ~3u, FV_HEADER + 4)) {
		say("acpm: fvmap: domain %u's header (+%#x) is not where it may be read\n", index, header);
		return 0;
	}
	levels = srd8(header + FV_NUM_LV);
	at = map + srd16(header + FV_O_RATEVOLT);
	if (levels == 0 || levels > most || !sram_ok(at, levels * 8)) {
		say("acpm: fvmap: domain %u: %u levels at +%#x: not a table (at most %u, inside the SRAM)\n", index, levels, at,
		    most);
		return 0;
	}
	for (uint32_t l = 0; l < levels; l++) {
		khz[l] = srd(at + 8 * l);
		uv[l] = srd(at + 8 * l + 4);
		if (khz[l] < 100000 || khz[l] > 4000000 || uv[l] < 300000 || uv[l] > 1500000 || (l > 0 && khz[l] >= khz[l - 1])) {
			say("acpm: fvmap: domain %u: level %u reads %u kHz %u uV: not a table (the fastest first, kHz and uV)\n",
			    index, l, khz[l], uv[l]);
			return 0;
		}
	}
	return levels;
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
