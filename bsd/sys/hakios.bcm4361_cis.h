/*
 * Darwin-on-hakios: which module the Galaxy S9's BCM4361 is (Murata or Samsung Electro-Mechanics, and its revision), from
 * the chip's own OTP, and so which of the vendor's files are this phone's: the Wi-Fi NVRAM and the Bluetooth patch.
 *
 * Facts, from Samsung's bcmdhd for the S9 (drivers/net/wireless/broadcom/bcmdhd_101_12 of its GPL kernel, read, not
 * copied): dhd_custom_cis.c, dhd_parse_board_information_bcm() reads the CIS tuples the module maker wrote into OTP, a
 * word at a time from the backplane at 0x18011110..0x18011167, after writing OTP Control 1 (0x18010324) with 0x00fa0000 and
 * the PMU's minimum resource mask (0x18012618) with 0x0e4fffff; the first tuple's id is 0x80, and each tuple is an id, a
 * length (of the tag and data), a tag and data: tag 0x81 is the module's "VID" (two bytes here: the module's revision and
 * its maker, 0x33 SEMCO, 0x22 Murata; bcmdhd matches only a VID of exactly two), tag 0x1b its board type. vid_info[] of the BCM4361 names the VIDs, and
 * bcm4361_naming_table[] turns the name's last three parts into the NVRAM's suffix; a name with "_mur_" (a Murata front
 * end) puts "_murata" before it (dhd_pcie.c, concate_revision_bcm4361()). The table below has the S9's B2 entries and
 * the B0 ones; what the vendor's /vendor/firmware has for B2 is nvram.txt_r02a_b2, nvram.txt_r02b_b2 (SEMCO) and
 * nvram.txt_murata_r041_b2 (Murata, whose VID that BSP's table does not list), and the Bluetooth patches
 * bcm4361B2_murata.hcd and bcm4361B2_semco.hcd.
 *
 * Nothing but C here, so that tools/brcmfmac-test.py compiles it on the machine the kernel is built on.
 */
#ifndef _SYS_HAKIOS_BCM4361_CIS_H_
#define _SYS_HAKIOS_BCM4361_CIS_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef KERNEL
#include <libkern/libkern.h>     /* snprintf */
#else
#include <stdio.h>
#endif

#define BCM4361_CIS_START       0x18011110u     /* the first word of the CIS tuples in the backplane's view of OTP */
#define BCM4361_CIS_END         0x18011167u     /* the last byte */
#define BCM4361_CIS_WORDS       ((BCM4361_CIS_END - BCM4361_CIS_START + 1) / 4)
#define BCM4361_OTP_CONTROL1    0x18010324u     /* written 0x00fa0000 before the read (bcmdhd) */
#define BCM4361_OTP_CONTROL1_ON 0x00fa0000u
#define BCM4361_PMU_MIN_RES     0x18012618u     /* written 0x0e4fffff before the read: "keep on ARMHTAVAIL" (bcmdhd) */
#define BCM4361_PMU_MIN_RES_ON  0x0e4fffffu

struct bcm4361_module {
	uint8_t vid[8];
	unsigned vid_length;            /* 0: none found */
	int boardtype;                  /* -1: none found */
	char maker[8];                  /* "murata", "semco" or "" */
	char nvram[40];                 /* the vendor's NVRAM file for it, or "" */
	int known;                      /* 1 when the VID is one of the table's, 0 when the name is only the maker's */
};

/* What bcmdhd's vid_info[] (B0 and B2 of the BCM4361) and naming table make of each VID: the NVRAM's suffix */
static const struct {
	uint8_t rev, maker;
	const char *suffix;     /* after "nvram.txt" */
} bcm4361_vids[] = {
	{ 0x51, 0x33, "_r01d_b0" }, { 0x61, 0x33, "_r01f_b0" }, { 0x62, 0x33, "_r02g_b0" }, { 0x71, 0x33, "_r01h_b0" },
	{ 0x81, 0x33, "_r01i_b0" }, { 0x82, 0x33, "_r02j_b0" },
	{ 0x91, 0x33, "_r02a_b2" }, { 0xa1, 0x33, "_r02b_b2" },
	{ 0x13, 0x22, "_murata_r013_b0" }, { 0x14, 0x22, "_murata_r014_b0" }, { 0x15, 0x22, "_murata_r015_b0" },
	{ 0x20, 0x22, "_murata_r020_b0" }, { 0x21, 0x22, "_murata_r021_b0" }, { 0x22, 0x22, "_murata_r022_b0" },
	{ 0x23, 0x22, "_murata_r023_b0" }, { 0x24, 0x22, "_murata_r024_b0" }, { 0x30, 0x22, "_murata_r030_b0" },
	{ 0x31, 0x22, "_murata_r030_b0" },      /* (bcmdhd: "exceptional case: r31 -> r30") */
	{ 0x32, 0x22, "_murata_r032_b0" }, { 0x33, 0x22, "_murata_r033_b0" }, { 0x34, 0x22, "_murata_r034_b0" },
	{ 0x50, 0x22, "_murata_r020_b2" }, { 0x51, 0x22, "_murata_r021_b2" }, { 0x52, 0x22, "_murata_r022_b2" },
	{ 0x61, 0x22, "_murata_r031_b2" },
};

/*
 * The tuples in `cis` (length bytes, the words read from BCM4361_CIS_START in the chip's order: little-endian) read into
 * *m. 1 when the first tuple is the start tag and a VID was found; 0 otherwise (*m then says what was).
 */
static inline int
bcm4361_cis_parse(const uint8_t *cis, size_t length, struct bcm4361_module *m)
{
	size_t at = 0;

	memset(m, 0, sizeof(*m));
	m->boardtype = -1;
	if (length < 3 || cis[0] != 0x80) {
		return 0;
	}
	while (at + 2 <= length) {
		unsigned len = cis[at + 1];

		if (at + 2 + len > length) {
			break;
		}
		if (len >= 1 && cis[at + 2] == 0x81 && m->vid_length == 0) {
			m->vid_length = len - 1 > sizeof(m->vid) ? sizeof(m->vid) : len - 1;
			memcpy(m->vid, cis + at + 3, m->vid_length);
		} else if (len >= 2 && cis[at + 2] == 0x1b && m->boardtype < 0) {
			m->boardtype = cis[at + 3];
		}
		if (m->vid_length != 0 && m->boardtype >= 0) {
			break;
		}
		at += 2 + len;
	}
	return m->vid_length != 0;
}

/*
 * The module's files for chip revision `rev_tag` ("b0" or "b2"): its maker from the VID's second byte, its NVRAM from
 * the table; a VID of a known maker that the table does not have gets that maker's one B2 NVRAM of the vendor's (SEMCO:
 * nvram.txt_r02b_b2, the later of the two; Murata: nvram.txt_murata_r041_b2), with known = 0.
 */
static inline void
bcm4361_module_files(struct bcm4361_module *m, const char *rev_tag)
{
	m->maker[0] = 0;
	m->nvram[0] = 0;
	m->known = 0;
	if (m->vid_length < 2) {
		return;
	}
	if (m->vid[1] == 0x33) {
		snprintf(m->maker, sizeof(m->maker), "semco");
	} else if (m->vid[1] == 0x22) {
		snprintf(m->maker, sizeof(m->maker), "murata");
	} else {
		return;
	}
	for (size_t i = 0; i < sizeof(bcm4361_vids) / sizeof(bcm4361_vids[0]); i++) {
		size_t sl = strlen(bcm4361_vids[i].suffix);

		if (m->vid_length == 2 && bcm4361_vids[i].rev == m->vid[0] && bcm4361_vids[i].maker == m->vid[1] && sl > 3 &&
		    strcmp(bcm4361_vids[i].suffix + sl - 2, rev_tag) == 0) {
			snprintf(m->nvram, sizeof(m->nvram), "nvram.txt%s", bcm4361_vids[i].suffix);
			m->known = 1;
			return;
		}
	}
	if (strcmp(rev_tag, "b2") == 0) {
		snprintf(m->nvram, sizeof(m->nvram), "%s", m->vid[1] == 0x33 ? "nvram.txt_r02b_b2" : "nvram.txt_murata_r041_b2");
	}
}

#endif /* _SYS_HAKIOS_BCM4361_CIS_H_ */
