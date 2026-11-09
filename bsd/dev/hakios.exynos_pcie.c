#define HAKIOS_SOC_FIXED
/*
 * Starbite iOS 27 (xnu-12377, S-Boot -> shim -> XNU) port of Scion's Wi-Fi stage 1, from Scion-Project/scion-kernel
 * drivers/wifi/brcmfmac/hakios.exynos_pcie.c (APSL; HakiOS/Scion), changed only so: wifi_pcie stops at 5 here (stage 1:
 * the link and the endpoint's identity; there is no radio driver in this tree, so no hand-over), Scion's Wi-Fi report
 * file is not kept, and nothing calls hakios_exynos_pcie_quiesce() (no loader to return to; a reset puts the pins back).
 * On this path S-Boot, not Silicium's UEFI, ran before the kernel: step 1 (reads only) says how S-Boot left the PHY,
 * the gates and the pins. Default off.
 */
/*
 * The Galaxy S9's Wi-Fi, stage 1 (starlte, Exynos 9810): the PCIe root complex of channel 0, a DesignWare core with
 * Samsung's PHY, as far as a link to the Broadcom BCM4361 and that chip's identity read from its configuration space.
 * No firmware is loaded and no driver of the radio runs: that is stage 2 (devices/starlte/WIFI.md). Only with boot-arg
 * wifi_pcie=N on this phone (the SoC's registers at 0x10000000); nothing is touched without it.
 *
 * What is where (Samsung's device tree, pcie0@116A0000, and the pins it and the wlan node name):
 *   ELBI      0x116A0000  Samsung's glue: LTSSM enable, link state, resets of the PCS, PMA and MAC, Q-channel
 *   PHY       0x116D0000  the PMA (the common block, then the transceiver's from 0xC0)
 *   PCS       0x116C0000  the PHY's PCS
 *   SYSREG    0x11411044  SYSREG_FSYS1's PCIe words (+0 and +0xC: the PCS's speed, lane 0)
 *   DBI       0x11700000  the DesignWare core's own configuration space and port logic (the iATU's viewport at 0x900)
 *   config    0x127FE000  8 KB: the iATU's window for type 0 configuration requests (the first 4 KB)
 *   memory    0x11800000  16 MB less 64 KB: the window the endpoint's BARs are placed in (identity, bus = CPU address)
 *   PMU       0x14060000  +0x71C bit 0: the PHY out of isolation (PCIE_PHY_CONTROL of channel 0)
 *   SPI 253   the RC's interrupt (INTx, level high): the radio's driver's (hakios_exynos_pcie_intx(), brcmfmac=5)
 *   WLAN_REG_ON   gpg0-5 of PERIC0's GPIO (0x10430080): the radio's power, high = on (Samsung's dhd_custom_exynos.c)
 *   PERST#        gpf2-2 of FSYS1's GPIO (0x11430020): the endpoint's reset, high = out of reset
 *   CLKREQ#       gpf2-0 (function 2, pulled up)
 *   WLAN_HOST_WAKE gpa0-7 of ALIVE's GPIO (0x14050020), PCIE_WAKE gpa3-2 (0x14050080): read only
 * The clocks: the device tree gives pcie-clk-num = 0 and phy-clk-num = 0, so Android's kernel switches none of them; the
 * gates of CMU_FSYS1 (0x11400000) for the Gen2 controller are read and must be open (hardware-controlled or forced
 * on) before the block is touched, else a read of it could stop the bus. wifi_pcie_ungate=1 forces closed ones open.
 *
 *   wifi_pcie=1   a look, reading only: the PMU's isolation, CMU_FSYS1's gates and Q-channels, the five pins
 *   wifi_pcie=2   ... the pins as Android sets them: PERST# low, CLKREQ# to the controller, WLAN_REG_ON high, 200 ms
 *   wifi_pcie=3   ... the PHY: out of isolation and power-down, the core's soft reset, PERST# high (20 ms), the PHY's
 *                 tables (26 MHz reference, Gen2), the resets of the PCS, PMA and MAC, the CDR's; then the core's own
 *                 identity through DBI and the PLL's lock
 *   wifi_pcie=4   ... the root complex set up (one lane, Gen2) and the link trained: LTSSM enabled, the link state polled
 *                 for 20 ms for L0. Up to wifi_pcie_tries times (default 3) from step 3 again, as Android does (10)
 *   wifi_pcie=5   ... the endpoint through the iATU: its vendor and device (Broadcom 0x14e4, BCM4361 0x441f, or 0x4361 /
 *                 0x4420 / 0x4421), class, capabilities, the size of every BAR, which are then placed in the memory
 *                 window. Memory decoding and bus mastering stay off. This is where stage 1 ends
 *   wifi_pcie=6   ... memory decoding on (the command register's bit 1), and the endpoint handed to the radio's driver
 *                 (sys/hakios.exynos_pcie.h; hakios.brcmfmac.c, brcmfmac=N), which sets bus mastering itself
 * Each step says what it is about to do before it does it. A link that does not come up is put back: PERST# low, the
 * LTSSM off, WLAN_REG_ON low. A link that does stays up until the run ends, for stage 2; on the way back to the loader
 * (hakios_exynos_pcie_quiesce(): loader_return(), and the radio driver's hakios_wifi_stop() after it has taken the driver
 * down) the root complex's INTx enables are cleared and the chip is held in reset and unpowered. The endpoint's
 * configuration space is not touched then: after brcmfmac's removal (which resets the chip through its watchdog) or a
 * panic the link may be down, and a request that never completes could keep the log from the loader. Nothing here writes a PMIC: the radio's supplies are the battery's and
 * always-on rails that Android does not switch either (the wlan node has no regulator).
 * Step 6 opens the radio's way to memory (dma_path_open(), below): the data path's gates, the PCIe SysMMU (0x11420000)
 * clocked (its registers read and set to pass addresses through only with wifi_pcie_mmu_probe=1), and with wifi_pcie_coherent=1 the I/O coherency bits of SYSREG_FSYS1 (+0x700).
 * Not done here: translation by the SysMMU (Android's 1:1 table), MSI, the IA sequencer that retrains the CDR on an exit
 * from L1.2 (and L1 substates themselves).
 *
 * What was checked: compiled; every register and value against the BSP files below (the PR has the table). It has
 * not run on the phone.
 *
 * Sources (Samsung's GPL kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, read for
 * these facts, not copied): drivers/pci/host/pci-exynos.c (exynos_pcie_poweron, _establish_link, _setup_rc,
 * _assert_phy_reset, _rd/_wr_own_conf, _rd_other_conf, _prog_viewport_cfg0/_mem_outbound, NCLK_OFF_CONTROL),
 * pci-exynos.h (the ELBI, DBI, SYSREG and PMU offsets), pci-exynos9810_cal.c (exynos_pcie_phy_config,
 * _phy_all_pwrdn_clear, _phy_check_rx_elecidle: channel 0's tables), pcie-designware.c (dw_pcie_setup_rc),
 * drivers/pinctrl/samsung/pinctrl-exynos.c (the 9810's banks), drivers/soc/samsung/cal-if/exynos9810/cmucal-sfr.c
 * (CMU_FSYS1's PCIe gates and Q-channels), drivers/net/wireless/broadcom/bcmdhd_101_12/dhd_custom_exynos.c
 * (WLAN_REG_ON, 200 ms, then the RC on) and include/bcmdevs_legacy.h (the BCM4361's IDs); the phone's device tree
 * (Silicium's starlte.dts: pcie0@116A0000, wlan, cfg_wlanen, wlan_host_wake, pcie0_clkreq, pcie0_perst, pcie_wake).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_pcie.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

/* ---- where ---------------------------------------------------------------------------------------------------- */

#define ELBI_PA         EXYNOS_PCIE_ELBI_PA
#define PHY_PA          EXYNOS_PCIE_PHY_PA
#define PCS_PA          EXYNOS_PCIE_PCS_PA
#define SYSREG_PA       EXYNOS_SYSREG_FSYS1_PAGE2_PA    /* the page; channel 0's words are +0x44 and +0x50 */
#define DBI_PA          EXYNOS_PCIE_DBI_PA
#define CFG_PA          EXYNOS_PCIE_CFG_PA    /* the first half of "config": type 0 requests (cfg0) */
#define CFG_SIZE        0x1000UL
#define MEM_PA          EXYNOS_PCIE_MEM_PA    /* "ranges": 32-bit memory, bus address = CPU address */
#define MEM_SIZE        0x00FF0000UL
#define PMU_PA          EXYNOS_PMU_PA
#define CMU_FSYS1_PA    EXYNOS_CMU_FSYS1_PA
#define CMU_TOP_PA      EXYNOS_CMU_TOP_PAGE2_PA    /* the page of CMU_TOP (0x1A240000) with GATE_CLKCMU_FSYS1_PCIE at +0x74 */
#define GPIO_PERIC0_PA  EXYNOS_GPIO_PERIC0_PA
#define GPIO_FSYS1_PA   EXYNOS_GPIO_FSYS1_PA
#define GPIO_ALIVE_PA   EXYNOS_GPIO_ALIVE_PA

/* the PMU */
#define PMU_PCIE_PHY_CONTROL    EXYNOS_PCIE_PMU_PCIE_PHY_CONTROL   /* channel 0; bit 0: 1 = the PHY out of isolation */
#define PMU_WAKEUP_MASK         EXYNOS_PCIE_PMU_WAKEUP_MASK   /* bit 5: channel 0's wake-up masked (read only here) */

/* ELBI (pci-exynos.h) */
#define ELBI_IRQ_PULSE          0x000   /* the latch: INTA bit 0, INTB 2, INTC 4, INTD 6 (asserts); one clears */
#define ELBI_IRQ_LEVEL          0x004
#define ELBI_IRQ_SPECIAL        0x008
#define ELBI_IRQ_EN_PULSE       0x00C
#define ELBI_IRQ_EN_LEVEL       0x010
#define ELBI_IRQ_EN_SPECIAL     0x014
#define ELBI_IRQ_TOE_EN_PULSE2  0x034
#define ELBI_IRQ_TOE_EN_LEVEL   0x038
#define ELBI_INTX_ASSERTS       0x55u   /* IRQ_INTA_ASSERT | _INTB_ | _INTC_ | _INTD_ (pci-exynos.h) */
#define ELBI_SW_WAKE            0x018   /* bit 1 clear: the bus number enabled */
#define ELBI_APP_LTSSM_ENABLE   0x02C
#define ELBI_RDLH_LINKUP        0x074   /* [4:0] the LTSSM's state: 0x0D-0x14 is a link (0x11 L0) */
#define ELBI_PM_DSTATE          0x088
#define ELBI_HISTORY(i)         (0x138 + 4 * (i))
#define ELBI_LINKDOWN_RST_SEL   0x1B8
#define ELBI_SOFT_CORE_RESET    0x1D0
#define ELBI_HISTORY_CHECK      0x274
#define ELBI_PCS_G_RST          0x288
#define ELBI_PHY_RST            0x28C   /* the PCS and the PMA (CMN) */
#define ELBI_MAC_RST            0x290
#define ELBI_STATE_POWER_S      0x2BC
#define ELBI_STATE_POWER_M      0x2C0
#define ELBI_QCH_SEL            0x2C8   /* [11:0] the clock gating of the PMU, APB and AXI sides: all cleared */
#define ELBI_L12ERR_CTRL        0x2F0   /* bit 2: NCLK off, the 9810's guard against speculative access while unlinked */
#define ELBI_APP_REQ_EXIT_L1_MODE 0x0F4 /* bit 0: hardware mode; bit 4: L1 request NAK when the master is busy */

/* DBI: the core's configuration space and port logic */
#define DBI_ID                  0x000
#define DBI_COMMAND             0x004
#define DBI_CLASS               0x008
#define DBI_BAR0                0x010
#define DBI_BAR1                0x014
#define DBI_BUS                 0x018
#define DBI_MEM_WINDOW          0x020
#define DBI_INTERRUPT           0x03C
#define DBI_PM_CTRL             0x044   /* the PM capability (0x40) + 4 */
#define DBI_LNKCAP              0x07C   /* the PCIe capability (0x70) + 0x0C */
#define DBI_LNKCTL              0x080   /* + 0x10; the status in [31:16] */
#define DBI_LNKCTL2             0x0A0   /* + 0x30 */
#define DBI_PORT_LINK_CONTROL   0x710
#define DBI_LINK_WIDTH_SPEED    0x80C
#define DBI_MISC_CONTROL        0x8BC   /* bit 0: read-only registers writable */
#define DBI_AUX_CLK_FREQ        0xB40
#define DBI_L1_SUBSTATES        0xB44
#define DBI_ATU_VIEWPORT        0x900
#define DBI_ATU_CR1             0x904
#define DBI_ATU_CR2             0x908
#define DBI_ATU_LOWER_BASE      0x90C
#define DBI_ATU_UPPER_BASE      0x910
#define DBI_ATU_LIMIT           0x914
#define DBI_ATU_LOWER_TARGET    0x918
#define DBI_ATU_UPPER_TARGET    0x91C
#define ATU_ENABLE              (1u << 31)
#define ATU_TYPE_MEM            0
#define ATU_TYPE_CFG0           4

/* CMU: a gate is open while hardware runs it (MANUAL, bit 20, clear) or it is forced on (MANUAL and CG_VAL, bit 21) */
#define GATE_MANUAL             (1u << 20)
#define GATE_CG_VAL             (1u << 21)
static const struct { uint16_t off; const char *name; } gates[] = {
	{ 0x2000, "PHY_REFCLK_IN" }, { 0x2038, "DBI_ACLK" }, { 0x203C, "PHY_APB" }, { 0x2040, "MSTR_ACLK" },
	{ 0x2044, "SUB_CTRL_APB" }, { 0x2048, "PCS_APB" }, { 0x204C, "SLV_ACLK" }, { 0x20B0, "XIU_DBI" },
	{ 0x20B4, "XIU_SLV" },
};
static const struct { uint16_t off; const char *name; } qchs[] = {
	{ 0x302C, "APB" }, { 0x3030, "DBI" }, { 0x3034, "MSTR" }, { 0x3038, "PCS" }, { 0x303C, "PHY" }, { 0x3000, "SOCPLL" },
};

/* GPIO banks (bank_type_6/7: CON 4 bits a pin at +0, DAT 1 bit at +4, PUD 4 bits at +8) */
#define BANK_GPG0               0x080   /* PERIC0 */
#define BANK_GPF2               0x020   /* FSYS1 */
#define BANK_GPA0               0x020   /* ALIVE */
#define BANK_GPA3               0x080   /* ALIVE */
#define PIN_WLAN_EN             5       /* gpg0 */
#define PIN_PERST               2       /* gpf2 */
#define PIN_CLKREQ              0       /* gpf2 */
#define PIN_HOST_WAKE           7       /* gpa0 */
#define PIN_PCIE_WAKE           2       /* gpa3 */

#define BCM_VENDOR              0x14e4
#define WIFI_TURNON_MS          200     /* dhd_custom_exynos.c, WIFI_TURNON_DELAY */
#define LINK_POLLS              2000    /* of 10 us: pci-exynos.h MAX_TIMEOUT */
#define MAX_LINK_SPEED          2       /* the device tree's max-link-speed: Gen2 */

static vm_offset_t elbi, phy, pcs, sysreg, dbi, cfg, pmu, cmu, cmu_top, peric0, fsys1, alive;
static bool linked;
static bool reg_on;            /* WLAN_REG_ON driven high by this run (step 2), until put back */

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline void
set_bits(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t v)
{
	wr(base, off, (rd(base, off) & ~mask) | (v & mask));
}

/* ---- pins ----------------------------------------------------------------------------------------------------- */

static uint32_t
pin_con(vm_offset_t g, uint32_t bank, unsigned pin)
{
	return (rd(g, bank) >> (4 * pin)) & 0xf;
}

static uint32_t
pin_dat(vm_offset_t g, uint32_t bank, unsigned pin)
{
	return (rd(g, bank + 4) >> pin) & 1;
}

static uint32_t
pin_pud(vm_offset_t g, uint32_t bank, unsigned pin)
{
	return (rd(g, bank + 8) >> (4 * pin)) & 0xf;
}

/* an output, driven: the level first, then the function, as gpio_direction_output() does */
static void
pin_out(vm_offset_t g, uint32_t bank, unsigned pin, unsigned level)
{
	set_bits(g, bank + 4, 1u << pin, (uint32_t)level << pin);
	set_bits(g, bank, 0xfu << (4 * pin), 1u << (4 * pin));
}

static void
say_pins(const char *when)
{
	say("exynos_pcie: %s: WLAN_REG_ON gpg0-5 CON %x DAT %u; PERST# gpf2-2 CON %x DAT %u; CLKREQ# gpf2-0 CON %x PUD %x; "
	    "HOST_WAKE gpa0-7 CON %x DAT %u; PCIE_WAKE gpa3-2 CON %x DAT %u\n", when,
	    pin_con(peric0, BANK_GPG0, PIN_WLAN_EN), pin_dat(peric0, BANK_GPG0, PIN_WLAN_EN),
	    pin_con(fsys1, BANK_GPF2, PIN_PERST), pin_dat(fsys1, BANK_GPF2, PIN_PERST),
	    pin_con(fsys1, BANK_GPF2, PIN_CLKREQ), pin_pud(fsys1, BANK_GPF2, PIN_CLKREQ),
	    pin_con(alive, BANK_GPA0, PIN_HOST_WAKE), pin_dat(alive, BANK_GPA0, PIN_HOST_WAKE),
	    pin_con(alive, BANK_GPA3, PIN_PCIE_WAKE), pin_dat(alive, BANK_GPA3, PIN_PCIE_WAKE));
}

/* ---- the PHY (pci-exynos9810_cal.c, channel 0) ------------------------------------------------------------------ */

/* the common block (26 MHz reference, Gen2), written from the PHY's offset 0 a word a byte */
static const uint8_t phy_cmn[48] = {
	0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0C, 0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xE3, 0xFC,
	0xD8, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00, 0x60, 0x11, 0x00, 0xA0, 0x05, 0x04, 0x18, 0x88,
	0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x30, 0x4F, 0xDC, 0x40, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80,
};

/* the transceiver, from word 0x30 (offset 0xC0) */
static const uint8_t phy_trsv[80] = {
	0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x6C, 0x66, 0x09, 0x61, 0x42, 0x44, 0xC6, 0x50, 0x0A, 0x33, 0x58, 0xE7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
	0x00, 0x00, 0x20, 0x3F, 0x00, 0x03, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* exynos_phy_check_rx_elecidle(): PCS +0xEC bit 3, 1 = ignore the receiver's electrical idle while DBI is used */
static void
rx_elecidle_ignore(bool ignore)
{
	set_bits(pcs, 0xEC, 1u << 3, ignore ? 1u << 3 : 0);
}

/* exynos_phy_all_pwrdn_clear() */
static void
phy_power_down_clear(void)
{
	set_bits(pcs, 0x100, 1u << 6, 0);
	set_bits(pcs, 0x104, 1u << 7, 0);
	wr(phy, 0x20 * 4, 0xC0);
	wr(phy, 0x57 * 4, 0x7E);
}

static void
pulse(uint32_t off, unsigned last_us)
{
	wr(elbi, off, 1);
	delay(10);
	wr(elbi, off, 0);
	delay(10);
	wr(elbi, off, 1);
	delay(last_us);
}

/* exynos_pcie_phy_config(), channel 0 (no OTP tuning: the BSP's defconfig has no CONFIG_EXYNOS_OTP) */
static void
phy_config(void)
{
	uint32_t v;

	/* SYSREG_FSYS1: the PCS's high speed, and lane 0 */
	set_bits(sysreg, 0x44, 1u << 1, 1u << 1);
	v = rd(sysreg, 0x50);
	v = (v & ~(0xfu << 4) & ~(0xfu << 2)) | (0x3u << 2);
	v &= ~(1u << 1);
	wr(sysreg, 0x50, v);
	set_bits(sysreg, 0x50, 1u << 12, 1u << 12);

	pulse(ELBI_PCS_G_RST, 10);
	for (unsigned i = 0; i < sizeof(phy_cmn); i++) {
		wr(phy, 4 * i, phy_cmn[i]);
	}
	for (unsigned i = 0; i < sizeof(phy_trsv); i++) {
		wr(phy, 4 * (0x30 + i), phy_trsv[i]);
	}
	wr(pcs, 0xF8, 0x70);                    /* tx latency */
	wr(pcs, 0x100, 0x87);                   /* the PCS's reference clock out */
	wr(pcs, 0x104, 0x50);
	v = rd(pcs, 0x0C);                      /* PRGM_TIMEOUT_L1SS_VAL */
	v = (v & ~(1u << 1)) | (1u << 4);
	wr(pcs, 0x0C, v);
	pulse(ELBI_MAC_RST, 10);
	pulse(ELBI_PHY_RST, 100);
	/* the CDR's reset: [7:6] = 3, 20 us, = 2, = 0 */
	v = rd(pcs, 0xD0) | (0x3u << 6);
	wr(pcs, 0xD0, v);
	delay(20);
	v = (rd(pcs, 0xD0) & ~(0x3u << 6)) | (0x2u << 6);
	wr(pcs, 0xD0, v);
	v &= ~(0x3u << 6);
	wr(pcs, 0xD0, v);
}

/* ---- DBI: the core's own registers (exynos_pcie_rd/_wr_own_conf: guarded while there is no link) --------------- */

static void
dbi_open(void)
{
	if (!linked) {
		wr(elbi, ELBI_L12ERR_CTRL, 0);
		rx_elecidle_ignore(true);
	}
}

static void
dbi_close(void)
{
	if (!linked) {
		rx_elecidle_ignore(false);
		wr(elbi, ELBI_L12ERR_CTRL, 1u << 2);
	}
}

static uint32_t
dbi_rd(uint32_t off)
{
	uint32_t v;

	dbi_open();
	v = rd(dbi, off);
	dbi_close();
	return v;
}

static void
dbi_wr(uint32_t off, uint32_t v)
{
	dbi_open();
	wr(dbi, off, v);
	dbi_close();
}

static void
dbi_set(uint32_t off, uint32_t mask, uint32_t v)
{
	dbi_wr(off, (dbi_rd(off) & ~mask) | (v & mask));
}

/* one outbound region of the iATU, through its viewport (exynos_pcie_prog_viewport_*) */
static void
atu_outbound(uint32_t index, uint32_t type, uint64_t cpu, uint64_t size, uint64_t target)
{
	dbi_wr(DBI_ATU_VIEWPORT, index);        /* bit 31 clear: outbound */
	dbi_wr(DBI_ATU_CR1, type);
	dbi_wr(DBI_ATU_LOWER_BASE, (uint32_t)cpu);
	dbi_wr(DBI_ATU_UPPER_BASE, (uint32_t)(cpu >> 32));
	dbi_wr(DBI_ATU_LIMIT, (uint32_t)(cpu + size - 1));
	dbi_wr(DBI_ATU_LOWER_TARGET, (uint32_t)target);
	dbi_wr(DBI_ATU_UPPER_TARGET, (uint32_t)(target >> 32));
	dbi_wr(DBI_ATU_CR2, ATU_ENABLE);
}

/* ---- the steps ------------------------------------------------------------------------------------------------ */

static bool
map_all(void)
{
	elbi = ml_io_map(ELBI_PA, 0x1000);
	phy = ml_io_map(PHY_PA, 0x1000);
	pcs = ml_io_map(PCS_PA, 0x1000);
	sysreg = ml_io_map(SYSREG_PA, 0x1000);
	dbi = ml_io_map(DBI_PA, 0x1000);
	cfg = ml_io_map(CFG_PA, CFG_SIZE);
	pmu = ml_io_map(PMU_PA, 0x1000);
	cmu = ml_io_map(CMU_FSYS1_PA, 0x4000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x1000);
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	fsys1 = ml_io_map(GPIO_FSYS1_PA, 0x1000);
	alive = ml_io_map(GPIO_ALIVE_PA, 0x1000);
	return elbi && phy && pcs && sysreg && dbi && cfg && pmu && cmu && cmu_top && peric0 && fsys1 && alive;
}

/* step 1: the gates; true if every one of the Gen2 controller's is open */
static bool
look_gates(bool ungate)
{
	bool open = true;

	say("exynos_pcie: 1: CMU_TOP GATE_CLKCMU_FSYS1_PCIE %08x; CMU_FSYS1 MUX_CLKCMU_FSYS1_PCIE_USER %08x\n",
	    rd(cmu_top, 0x074), rd(cmu, 0x180));
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t v = rd(cmu, gates[i].off);
		bool closed = (v & GATE_MANUAL) && !(v & GATE_CG_VAL);

		say("exynos_pcie: 1: gate %s (+%#x) %08x%s\n", gates[i].name, gates[i].off, v, closed ? ": CLOSED" : "");
		if (closed && ungate) {
			say("exynos_pcie: forcing gate %s open (wifi_pcie_ungate=1)\n", gates[i].name);
			wr(cmu, gates[i].off, v | GATE_CG_VAL);
			closed = !(rd(cmu, gates[i].off) & GATE_CG_VAL);
		}
		open = open && !closed;
	}
	for (unsigned i = 0; i < sizeof(qchs) / sizeof(qchs[0]); i++) {
		say("exynos_pcie: 1: Q-channel %s (+%#x) %08x\n", qchs[i].name, qchs[i].off, rd(cmu, qchs[i].off));
	}
	return open;
}

/* steps 3 and the first half of 4's tries: the PHY and the core up to the moment the LTSSM is let go */
static void
phy_up(void)
{
	say("exynos_pcie: 3: the core's soft reset, PERST# high (20 ms), L1 exit in hardware, no clock gating, the PHY's tables\n");
	rx_elecidle_ignore(true);
	wr(elbi, ELBI_SOFT_CORE_RESET, 0);
	delay(20);
	wr(elbi, ELBI_SOFT_CORE_RESET, 1);
	pin_out(fsys1, BANK_GPF2, PIN_PERST, 1);
	delay_for_interval(20, NSEC_PER_MSEC);
	set_bits(elbi, ELBI_APP_REQ_EXIT_L1_MODE, 0x11, 0x11);
	wr(elbi, ELBI_LINKDOWN_RST_SEL, 1u << 1);       /* the link-down reset by hand */
	set_bits(elbi, ELBI_QCH_SEL, 0xfff, 0);
	phy_config();
	set_bits(elbi, ELBI_SW_WAKE, 1u << 1, 0);       /* the bus number enabled */
	say("exynos_pcie: 3: PHY PLL %x (bits 1:0 set: locked), the core's own ID %08x, class %08x\n",
	    rd(phy, 0xBC) & 3, dbi_rd(DBI_ID), dbi_rd(DBI_CLASS));
}

/* dw_pcie_setup_rc() then exynos_pcie_setup_rc(), for one lane at Gen2 */
static void
setup_rc(void)
{
	dbi_set(DBI_PORT_LINK_CONTROL, 0x3fu << 16, 1u << 16);
	dbi_set(DBI_LINK_WIDTH_SPEED, 0x1fu << 8, 1u << 8);
	dbi_wr(DBI_BAR0, 0x4);
	dbi_wr(DBI_BAR1, 0);
	dbi_set(DBI_INTERRUPT, 0xff00, 0x0100);         /* INTA */
	dbi_set(DBI_BUS, 0x00ffffff, 0x00ff0100);       /* primary 0, secondary 1, subordinate 0xff */
	dbi_set(DBI_COMMAND, 0xffff, 0x0107);           /* I/O, memory, bus master, SERR */
	dbi_wr(DBI_BAR0, 0);
	dbi_set(DBI_CLASS, 0xffff0000u, 0x0604u << 16); /* a PCI-to-PCI bridge */
	dbi_set(DBI_LINK_WIDTH_SPEED, 1u << 17, 1u << 17);      /* speed change */

	dbi_wr(DBI_MISC_CONTROL, 1);                    /* read-only registers writable */
	dbi_set(DBI_LNKCAP, 0x38000 | 0x3f0 | 0xf, (0x7u << 15) | (1u << 4) | 2);  /* L1 exit 64 us, x1, 5 GT/s */
	dbi_wr(DBI_AUX_CLK_FREQ, 0x1A);                 /* 26 MHz */
	dbi_wr(DBI_L1_SUBSTATES, 0xEA);
	dbi_wr(DBI_PM_CTRL, 0);
	dbi_set(DBI_LNKCTL, 1u << 5, 1u << 5);          /* retrain */
	dbi_set(DBI_LNKCTL2, 0xf, MAX_LINK_SPEED);
}

static bool
train(void)
{
	uint32_t v = 0;

	say("exynos_pcie: 4: the root complex set up (x1, Gen2), then the LTSSM enabled\n");
	setup_rc();
	rx_elecidle_ignore(false);
	say("exynos_pcie: 4: D state %x, link state %x before\n", rd(elbi, ELBI_PM_DSTATE) & 7, rd(elbi, ELBI_RDLH_LINKUP));
	wr(elbi, ELBI_APP_LTSSM_ENABLE, 1);
	for (unsigned i = 0; i < LINK_POLLS; i++) {
		v = rd(elbi, ELBI_RDLH_LINKUP) & 0x1f;
		if (v >= 0x0d && v <= 0x14) {
			linked = true;
			wr(elbi, ELBI_L12ERR_CTRL, 0);
			v = rd(dbi, DBI_LNKCTL) >> 16;
			say("exynos_pcie: 4: LINK UP after %u us (state %x): Gen%u x%u (at most Gen%u)\n", 10 * i,
			    rd(elbi, ELBI_RDLH_LINKUP) & 0x1f, v & 0xf, (v >> 4) & 0x3f, MAX_LINK_SPEED);
			wr(elbi, ELBI_IRQ_PULSE, rd(elbi, ELBI_IRQ_PULSE));    /* what is pending, cleared */
			return true;
		}
		delay(10);
	}
	say("exynos_pcie: 4: no link in %u us: state %x; the LTSSM's last states, oldest first:\n", 10 * LINK_POLLS,
	    rd(elbi, ELBI_RDLH_LINKUP));
	for (int i = 31; i >= 0; i -= 8) {
		say("exynos_pcie:    ");
		for (int j = i; j > i - 8; j--) {
			uint32_t h = rd(elbi, ELBI_HISTORY(j));

			say(" %02x/%x/%x", (h >> 16) & 0x3f, h & 7, (h >> 8) & 7);   /* LTSSM / L1 substate / D state */
		}
		say("\n");
	}
	pin_out(fsys1, BANK_GPF2, PIN_PERST, 0);
	wr(elbi, ELBI_APP_LTSSM_ENABLE, 0);
	return false;
}

static uint32_t
cfg_rd(uint32_t off)
{
	return rd(cfg, off);
}

static void
cfg_wr(uint32_t off, uint32_t v)
{
	wr(cfg, off, v);
}

/* where step 5 placed the endpoint's BARs (address 0: not placed), and whether step 6 has handed it over */
static uint64_t bar_address[6], bar_size[6];
static volatile bool handed_over;

/* step 5: bus 1, device 0, function 0; true if it is an endpoint with a BAR placed */
static bool
enumerate(void)
{
	bool placed = false;

	uint32_t id, class, hdr, sub, next = MEM_PA;

	say("exynos_pcie: 5: the iATU: region 1 to type 0 configuration of 1:00.0 at %#lx, region 0 to memory at %#lx\n",
	    CFG_PA, MEM_PA);
	atu_outbound(1, ATU_TYPE_CFG0, CFG_PA, CFG_SIZE, 1u << 24);
	atu_outbound(0, ATU_TYPE_MEM, MEM_PA, MEM_SIZE, MEM_PA);
	/* the bridge's memory window: 0x11800000 to 0x126FFFFF (1 MB granules inside "ranges") */
	dbi_wr(DBI_MEM_WINDOW, (0x126Fu << 16) | 0x1180u);

	id = cfg_rd(0x00);
	if ((id & 0xffff) == 0xffff || id == 0) {
		say("exynos_pcie: 5: nothing answers at 1:00.0 (%08x)\n", id);
		return false;
	}
	class = cfg_rd(0x08);
	hdr = cfg_rd(0x0C);
	sub = cfg_rd(0x2C);
	say("exynos_pcie: 5: 1:00.0 is %04x:%04x%s, class %06x rev %02x, header %02x, subsystem %04x:%04x\n", id & 0xffff,
	    id >> 16, (id & 0xffff) == BCM_VENDOR ? " (Broadcom)" : " (NOT Broadcom)", class >> 8, class & 0xff,
	    (hdr >> 16) & 0x7f, sub & 0xffff, sub >> 16);
	if (cfg_rd(0x04) & (0x10u << 16)) {
		uint32_t at = cfg_rd(0x34) & 0xfc;

		say("exynos_pcie: 5: capabilities:");
		for (unsigned n = 0; at >= 0x40 && n < 24; n++) {
			uint32_t cap = cfg_rd(at);

			say(" %02x@%02x", cap & 0xff, at);
			if ((cap & 0xff) == 0x10) {
				say("(link %x)", cfg_rd(at + 0x10) >> 16);
			}
			at = (cap >> 8) & 0xfc;
		}
		say("\n");
	}
	if (((hdr >> 16) & 0x7f) != 0) {
		say("exynos_pcie: 5: not an endpoint's header: BARs left alone\n");
		return false;
	}
	/* the BARs: sized with decoding off (it is, after reset), then placed in the window, aligned to their size */
	for (unsigned b = 0; b < 6; b++) {
		uint32_t off = 0x10 + 4 * b, was = cfg_rd(off), lo, hi = 0xffffffffu;
		bool is64 = (was & 0x7) == 0x4;
		uint64_t mask, size;

		cfg_wr(off, 0xffffffffu);
		lo = cfg_rd(off);
		cfg_wr(off, was);
		if (is64 && b < 5) {
			uint32_t was_hi = cfg_rd(off + 4);

			cfg_wr(off + 4, 0xffffffffu);
			hi = cfg_rd(off + 4);
			cfg_wr(off + 4, was_hi);
		}
		if (lo == 0) {
			continue;
		}
		if (was & 1) {
			say("exynos_pcie: 5: BAR%u: I/O (%08x), left alone\n", b, lo);
			continue;
		}
		mask = ((uint64_t)hi << 32) | (lo & ~0xfu);
		size = ~mask + 1;
		if (!is64) {
			size &= 0xffffffffu;
		}
		next = (uint32_t)((next + size - 1) & ~(size - 1));
		if (size == 0 || next + size > MEM_PA + MEM_SIZE) {
			say("exynos_pcie: 5: BAR%u: %s, %#llx bytes: does not fit the window, left unplaced\n", b,
			    is64 ? "64-bit" : "32-bit", size);
		} else {
			cfg_wr(off, next | (was & 0xf));
			if (is64) {
				cfg_wr(off + 4, 0);
			}
			say("exynos_pcie: 5: BAR%u: %s%s memory, %#llx bytes (%llu KB), placed at %#x (read back %08x)\n", b,
			    is64 ? "64-bit" : "32-bit", (was & 8) ? " prefetchable" : "", size, size >> 10, next, cfg_rd(off));
			bar_address[b] = next;
			bar_size[b] = size;
			placed = true;
			next += (uint32_t)size;
		}
		if (is64) {
			b++;
		}
	}
	say("exynos_pcie: 5: stage 1 done: the link is up and the endpoint known. Decoding and bus mastering stay off; "
	    "no firmware (stage 2)\n");
	return placed;
}

/* ---- for the radio's driver (sys/hakios.exynos_pcie.h) ---------------------------------------------------------- */

bool
hakios_exynos_pcie_ready(void)
{
	return handed_over;
}

uint32_t
hakios_exynos_pcie_config_read(uint32_t offset)
{
	return handed_over && offset < CFG_SIZE ? cfg_rd(offset & ~3u) : 0xffffffffu;
}

void
hakios_exynos_pcie_config_write(uint32_t offset, uint32_t value)
{
	if (handed_over && offset < CFG_SIZE) {
		cfg_wr(offset & ~3u, value);
	}
}

bool
hakios_exynos_pcie_bar(unsigned bar, uint64_t *address, uint64_t *size)
{
	if (!handed_over || bar >= 6 || bar_address[bar] == 0) {
		return false;
	}
	*address = bar_address[bar];
	*size = bar_size[bar];
	return true;
}

/*
 * INTx (sys/hakios.exynos_pcie.h): what Android's exynos_pcie_enable_interrupts() writes, INTA to INTD enabled in
 * IRQ_EN_PULSE and the TOE, LEVEL and SPECIAL sources off; the latch cleared first, so that nothing old fires.
 */
void
hakios_exynos_pcie_intx(bool on)
{
	if (!handed_over) {
		return;
	}
	wr(elbi, ELBI_IRQ_EN_PULSE, 0);
	wr(elbi, ELBI_IRQ_PULSE, rd(elbi, ELBI_IRQ_PULSE));
	if (on) {
		wr(elbi, ELBI_IRQ_TOE_EN_PULSE2, 0);
		wr(elbi, ELBI_IRQ_TOE_EN_LEVEL, 0);
		wr(elbi, ELBI_IRQ_EN_LEVEL, 0);
		wr(elbi, ELBI_IRQ_EN_SPECIAL, 0);
		wr(elbi, ELBI_IRQ_EN_PULSE, ELBI_INTX_ASSERTS);
	}
}

/*
 * What stands between the radio and memory (sys/hakios.exynos_pcie.h). The radio's reads and writes of our memory leave
 * the controller's master port (MSTR_ACLK) and cross FSYS1's data path to the bus: XIU_D_FSYS1, the PCIe SysMMU
 * (0x11420000), PPMU_FSYS1, BTM_FSYS1 and LHS_ACEL_D_FSYS1. Their gates in CMU_FSYS1 are said at step 6 and, closed,
 * forced open (MANUAL and CG_VAL): Android's exynos-pcie-iommu.c switches the SysMMU's clock on (clk_enable of "aclk")
 * whatever it then does with it. Left off (wifi-a4: a read of the SysMMU's CTRL stopped the bus, "Kernel data abort",
 * ESR 0x96000610, a synchronous external abort), nothing that crosses it would get through either.
 *
 * The SysMMU's own registers are not touched by default: wifi-a5 (its gate forced on, 00300000) read them at step 6
 * (CTRL 4, set to 0) but a later read from the interrupt poller (hakios_exynos_pcie_say_dma()) stopped the bus again,
 * a synchronous external abort. Its gate is still forced on, harmless, as the radio's DMA may cross the block in
 * bypass. wifi_pcie_mmu_probe=1 reads them (step 6 and say_dma): CTRL 0 is the bypass (Android's disable), 5
 * translating, 7 and 3 blocking; anything but 0 is then set to 0 (wifi_pcie_mmu_bypass=0 leaves it). The fault
 * registers are the ones of Samsung's exynos-pcie-iommu.h (AR 0x70/0x78, AW 0x80/0x88), nothing between.
 *
 * SYSREG_FSYS1 +0x700 (PCIE_SYSREG_SHARABILITY_CTRL, bits 9:8 for channel 0): 3 makes the radio's reads and writes
 * shareable, so that they look in the processors' caches (Android's use-cache-coherency = "true",
 * enable_cache_cohernecy()). wifi_pcie_coherent=1 sets it; the radio's memory is then the kernel's own cached memory
 * (hakios_wifi_radio_coherent(), hakios.lx.c), not a second, uncached view of it. Without it (the default) the bits are
 * left as they are and the radio's memory is uncached, which a radio whose writes do not look in the caches needs.
 *
 * Also said: the root port's and the endpoint's command/status (bit 29 of status: a master abort received, bit 28 a
 * target abort received, bit 27 one signalled, bit 30 SERR, bit 31 a parity error) and the PCIe device status of both
 * (bits 0-3: correctable, non-fatal, fatal, unsupported request).
 */
#define SYSMMU_PA       EXYNOS_PCIE_SYSMMU_PA
#define SYSREG_FSYS1_PA EXYNOS_SYSREG_FSYS1_PA
#define SHARABILITY     0x700
#define SHARABLE_CH0    (3u << 8)
#define GATE_SYSMMU     EXYNOS_PCIE_GATE_SYSMMU
static const struct { uint16_t gate, qch; const char *name; } dma_path[] = {
	{ 0x2040, 0x3034, "PCIE_GEN2 MSTR_ACLK" }, { 0x20ac, 0, "XIU_D_FSYS1" }, { GATE_SYSMMU, 0x306c, "SYSMMU_FSYS1" },
	{ 0x2078, 0x3060, "PPMU_FSYS1" }, { 0x2018, 0x3014, "BTM_FSYS1" }, { 0x202c, 0x3024, "LHS_ACEL_D_FSYS1" },
	{ 0x209c, 0x3070, "SYSREG_FSYS1" },
};
static vm_offset_t mmu, fsys1_sysreg;
static bool mmu_clocked, mmu_probe, coherent;

static void
say_kept(const char *format, ...) __printflike(1, 2);

static void
say_kept(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	/* (Scion keeps these lines in its Wi-Fi report too; not here) */
	say("%s", line);
}

/* step 6, before the hand-over: the data path's gates, the SysMMU passing addresses through, the shareability */
static void
dma_path_open(void)
{
	uint32_t bypass = 1, want_coherent = 0, probe = 0, v;

	PE_parse_boot_argn("wifi_pcie_mmu_bypass", &bypass, sizeof(bypass));
	PE_parse_boot_argn("wifi_pcie_coherent", &want_coherent, sizeof(want_coherent));
	PE_parse_boot_argn("wifi_pcie_mmu_probe", &probe, sizeof(probe));
	mmu_probe = probe != 0;
	if (fsys1_sysreg == 0) {
		fsys1_sysreg = ml_io_map(SYSREG_FSYS1_PA, 0x1000);
	}
	if (mmu_probe && mmu == 0) {
		mmu = ml_io_map(SYSMMU_PA, 0x1000);
	}
	for (unsigned i = 0; i < sizeof(dma_path) / sizeof(dma_path[0]); i++) {
		bool closed, force;

		v = rd(cmu, dma_path[i].gate);
		closed = (v & GATE_MANUAL) && !(v & GATE_CG_VAL);
		/* the SysMMU's clock is switched on as Android does, closed or hardware-run (its registers: mmu_probe only) */
		force = closed || dma_path[i].gate == GATE_SYSMMU;
		say("exynos_pcie: 6: the data path: gate %s (+%#x) %08x%s, Q-channel %08x\n", dma_path[i].name,
		    dma_path[i].gate, v, closed ? ": CLOSED" : "", dma_path[i].qch ? rd(cmu, dma_path[i].qch) : 0);
		if (force) {
			wr(cmu, dma_path[i].gate, v | GATE_MANUAL | GATE_CG_VAL);
			v = rd(cmu, dma_path[i].gate);
			say("exynos_pcie: 6: gate %s forced on: %08x\n", dma_path[i].name, v);
		}
		if (dma_path[i].gate == GATE_SYSMMU) {
			mmu_clocked = (v & GATE_MANUAL) && (v & GATE_CG_VAL);
		}
	}
	if (!mmu_probe) {
		say("exynos_pcie: 6: the PCIe SysMMU's registers are left alone (wifi_pcie_mmu_probe=1 reads them)\n");
	} else if (mmu != 0 && mmu_clocked) {
		v = rd(mmu, 0x000);
		say("exynos_pcie: 6: the PCIe SysMMU: CTRL %08x CFG %08x STATUS %08x VERSION %08x (CTRL 0: addresses passed through)\n",
		    v, rd(mmu, 0x004), rd(mmu, 0x008), rd(mmu, 0x034));
		if (v != 0 && bypass != 0) {
			wr(mmu, 0x000, 0);
			say("exynos_pcie: 6: the SysMMU set to pass addresses through: CTRL now %08x\n", rd(mmu, 0x000));
		}
	} else {
		say("exynos_pcie: 6: the PCIe SysMMU's clock is not on: its registers are not read\n");
	}
	if (fsys1_sysreg != 0) {
		v = rd(fsys1_sysreg, SHARABILITY);
		say("exynos_pcie: 6: SYSREG_FSYS1 +0x700 (shareability) %08x: channel 0's %u (3: the radio's reads and writes "
		    "look in the caches)\n", v, (v >> 8) & 3);
		if (want_coherent != 0) {
			wr(fsys1_sysreg, SHARABILITY, v | SHARABLE_CH0);
			v = rd(fsys1_sysreg, SHARABILITY);
			coherent = (v & SHARABLE_CH0) == SHARABLE_CH0;
			say("exynos_pcie: 6: wifi_pcie_coherent=1: shareability now %08x: %s\n", v,
			    coherent ? "coherent, the radio's memory is the kernel's cached memory" : "it did not take, uncached memory");
		}
	}
}

/* wifi_pcie_coherent=1 took (above): the radio's memory need not be uncached (hakios.lx.c) */
bool
hakios_wifi_radio_coherent(void)
{
	return coherent;
}

void
hakios_exynos_pcie_say_dma(const char *when)
{
	uint32_t ep_cap = 0, at;

	if (!handed_over) {
		return;
	}
	if (cmu != 0) {
		say_kept("exynos_pcie: %s: SysMMU gate %08x Q-channel %08x\n", when, rd(cmu, GATE_SYSMMU), rd(cmu, 0x306c));
	}
	if (mmu_probe && mmu != 0 && mmu_clocked) {
		say_kept("exynos_pcie: %s: SysMMU CTRL %08x CFG %08x STATUS %08x VERSION %08x INT_STATUS %08x; "
		    "fault AR %08x (info %08x), AW %08x (info %08x)\n", when,
		    rd(mmu, 0x000), rd(mmu, 0x004), rd(mmu, 0x008), rd(mmu, 0x034), rd(mmu, 0x060),
		    rd(mmu, 0x070), rd(mmu, 0x078), rd(mmu, 0x080), rd(mmu, 0x088));
	}
	if (fsys1_sysreg != 0) {
		say_kept("exynos_pcie: %s: SYSREG_FSYS1 +0x700 (shareability) %08x\n", when, rd(fsys1_sysreg, SHARABILITY));
	}
	at = cfg_rd(0x34) & 0xfc;
	for (unsigned n = 0; at >= 0x40 && n < 24; n++) {
		uint32_t cap = cfg_rd(at);

		if ((cap & 0xff) == 0x10) {
			ep_cap = at;
			break;
		}
		at = (cap >> 8) & 0xfc;
	}
	say_kept("exynos_pcie: %s: root port command/status %08x (device status %04x), endpoint %08x (device status %04x, "
	    "control %04x), ELBI IRQ latch %08x\n", when, dbi_rd(DBI_COMMAND), dbi_rd(0x78) >> 16, cfg_rd(0x04),
	    ep_cap ? cfg_rd(ep_cap + 8) >> 16 : 0xffff, ep_cap ? cfg_rd(ep_cap + 8) & 0xffff : 0xffff, rd(elbi, ELBI_IRQ_PULSE));
}

/* exynos_pcie_irq_handler()'s first two lines: what is latched, written back to clear it */
uint32_t
hakios_exynos_pcie_intx_ack(void)
{
	uint32_t latched;

	if (!handed_over) {
		return 0;
	}
	latched = rd(elbi, ELBI_IRQ_PULSE);
	wr(elbi, ELBI_IRQ_PULSE, latched);
	return latched;
}

/* step 6: the endpoint's memory decoding on, and the endpoint to the radio's driver */
static void
hand_over(void)
{
	say("exynos_pcie: 6: 1:00.0's memory decoding on (command bit 1; bus mastering is the radio driver's to set)\n");
	cfg_wr(0x04, (cfg_rd(0x04) & 0xffffu) | 0x2u);
	dma_path_open();
	say("exynos_pcie: 6: command/status now %08x; the endpoint handed to the radio's driver (brcmfmac=N)\n", cfg_rd(0x04));
	handed_over = true;
	hakios_brcmfmac_start();
}

static void
wifi_pcie_thread(void *arg, __unused wait_result_t wr_)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, tries = 3, ungate = 0;
	bool open;

	PE_parse_boot_argn("wifi_pcie_tries", &tries, sizeof(tries));
	PE_parse_boot_argn("wifi_pcie_ungate", &ungate, sizeof(ungate));
	tries = tries == 0 ? 1 : tries > 10 ? 10 : tries;
	say("exynos_pcie: wifi_pcie=%u: PCIe channel 0 to the BCM4361, step by step (5: stage 1; 6: to the driver)\n", steps);
	if (!map_all()) {
		say("exynos_pcie: the registers cannot be mapped\n");
		goto out;
	}
	say("exynos_pcie: 1: PMU PCIE_PHY_CONTROL %08x (bit 0: out of isolation), WAKEUP_MASK %08x\n",
	    rd(pmu, PMU_PCIE_PHY_CONTROL), rd(pmu, PMU_WAKEUP_MASK));
	open = look_gates(steps >= 3 && ungate != 0);
	say_pins("1");
	if (steps < 2) {
		goto out;
	}

	say("exynos_pcie: 2: PERST# low, CLKREQ# to the controller (function 2, pull-up), WLAN_REG_ON high, %u ms\n",
	    WIFI_TURNON_MS);
	pin_out(fsys1, BANK_GPF2, PIN_PERST, 0);
	set_bits(fsys1, BANK_GPF2 + 8, 0xfu << (4 * PIN_CLKREQ), 0x3u << (4 * PIN_CLKREQ));
	set_bits(fsys1, BANK_GPF2, 0xfu << (4 * PIN_CLKREQ), 0x2u << (4 * PIN_CLKREQ));
	pin_out(peric0, BANK_GPG0, PIN_WLAN_EN, 1);
	reg_on = true;
	delay_for_interval(WIFI_TURNON_MS, NSEC_PER_MSEC);
	say_pins("2");
	if (pin_dat(peric0, BANK_GPG0, PIN_WLAN_EN) != 1) {
		say("exynos_pcie: 2: WLAN_REG_ON does not read back high: stopping\n");
		goto out;
	}
	if (steps < 3) {
		goto out;
	}

	if (!open) {
		say("exynos_pcie: 3: a gate of the controller is closed (above): the block is not touched. "
		    "wifi_pcie_ungate=1 opens it\n");
		goto off;
	}
	say("exynos_pcie: 3: the PHY out of isolation (PMU +0x71C bit 0), out of power-down, the LTSSM's history on\n");
	set_bits(pmu, PMU_PCIE_PHY_CONTROL, 1, 1);
	if ((rd(pmu, PMU_PCIE_PHY_CONTROL) & 1) == 0) {
		say("exynos_pcie: 3: PCIE_PHY_CONTROL does not read back 1 (%08x): stopping\n", rd(pmu, PMU_PCIE_PHY_CONTROL));
		goto off;
	}
	phy_power_down_clear();
	wr(elbi, ELBI_HISTORY_CHECK, 0);
	wr(elbi, ELBI_HISTORY_CHECK, 3);
	wr(elbi, ELBI_STATE_POWER_S, 0x200000);
	wr(elbi, ELBI_STATE_POWER_M, 0xffffffffu);
	for (uint32_t t = 1;; t++) {
		phy_up();
		if (steps < 4) {
			say("exynos_pcie: 3: done; the PHY is left as it is, PERST# high, until the run ends\n");
			goto out;
		}
		if (train()) {
			break;
		}
		if (t >= tries) {
			say("exynos_pcie: 4: no link in %u tries\n", tries);
			goto off;
		}
		say("exynos_pcie: 4: try %u of %u\n", t + 1, tries);
	}
	if (steps < 5) {
		say("exynos_pcie: 4: done; the link stays up until the run ends\n");
		goto out;
	}
	if (!enumerate() || steps < 6) {
		goto out;
	}
	hand_over();
	goto out;
off:
	say("exynos_pcie: put back: PERST# low, the LTSSM off, WLAN_REG_ON low\n");
	pin_out(fsys1, BANK_GPF2, PIN_PERST, 0);
	if (elbi != 0 && open) {
		wr(elbi, ELBI_APP_LTSSM_ENABLE, 0);
	}
	pin_out(peric0, BANK_GPG0, PIN_WLAN_EN, 0);
	reg_on = false;
out:
	thread_terminate(current_thread());
}

/*
 * On the way back to the loader (loader_return(), a panic's way out too: one processor left, no lock taken): the chip held
 * in reset and its power off, PERST# low then WLAN_REG_ON low, as the stopping path above does -- if this run powered it.
 * From step 2 on the steps leave it powered "until the run ends", and with its firmware up (brcmfmac=3 and on) it is a bus
 * master with rings in the kernel's memory, which the loader reuses for the log while the BCM4361 would still write there.
 * The radio's driver calls this too, from hakios_wifi_stop(), once it has taken brcmfmac down in order; the second call
 * does nothing.
 */
void
hakios_exynos_pcie_quiesce(void)
{
	if (!reg_on || peric0 == 0 || fsys1 == 0) {
		return;
	}
	if (handed_over) {
		/* INTx off (brcmfmac=5): nothing latched, nothing to raise SPI 253 */
		wr(elbi, ELBI_IRQ_EN_PULSE, 0);
		wr(elbi, ELBI_IRQ_PULSE, rd(elbi, ELBI_IRQ_PULSE));
	}
	pin_out(fsys1, BANK_GPF2, PIN_PERST, 0);
	pin_out(peric0, BANK_GPG0, PIN_WLAN_EN, 0);
	reg_on = false;
	say("exynos_pcie: at the run's end: PERST# low, WLAN_REG_ON low (reads %u)\n", pin_dat(peric0, BANK_GPG0, PIN_WLAN_EN));
}

void
hakios_exynos_pcie_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi_pcie", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (steps > 5) {
		say("exynos_pcie: wifi_pcie=%u: this kernel has stage 1 only (no radio driver): stopping at 5\n", steps);
		steps = 5;
	}
	if (pe_arm_get_soc_base_phys() != EXYNOS_SOC_BASE_PA) {
		return;                 /* not the Exynos 9810 */
	}
	if (kernel_thread_start(wifi_pcie_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* (Starbite) no radio driver in this tree: wifi_pcie stops at 5, so this is never called */
void
hakios_brcmfmac_start(void)
{
}
