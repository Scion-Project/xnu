#define HAKIOS_SOC_FIXED
/*
 * The Galaxy S9's USB port, the SoC's half: the Exynos 9810's clocks, power isolation and USB PHY, brought up for the
 * device-neutral gadget driver (drivers/usb/hakios.dwc3.c, which owns the boot-args: usb_gadget=N). This file is the
 * glue (sys: hakios.usb_glue.h); the DWC3 controller itself is the gadget driver's.
 *
 * The hardware. The DWC3 (DesignWare USB 3.0 DRD) at 0x10C00000, interrupt SPI 244 (the gadget driver polls). Its
 * USB 2.0 PHY and link control ("USBCON", Samsung's USB 3.1 CAL, version 0x300) at 0x11100000; the USB/DisplayPort
 * combo PHY for SuperSpeed at 0x110A0000/0x110B0000, which is NOT used: the link runs at high speed with its PIPE3
 * side forced idle, as the BSP itself leaves it until the SuperSpeed PHY is set up. Power: the PMU's USB PHY
 * isolation at 0x14060000 + 0x72C bit 0 (the DP/SuperSpeed one, +0x704, left alone). Clocks: CMU_TOP's
 * CLKCMU_FSYS0_USB30DRD (mux, divider, gate at 0x1A240000 +0x1060/+0x1858/+0x2064) into CMU_FSYS0's user mux
 * (0x11000000 +0x1E0) and the link's Q-channel gates (+0x304C/+0x3050/+0x3054/+0x3058), which Linux leaves to
 * the hardware (composite.c:1375: enabling a VCLK_GATE is a no-op). Supplies: the S2MPS18's LDO9-14, always on
 * (the dt's regulator-always-on, mode 1); Android's USB stack puts them into mode 3 (normal) while USB is in use.
 *
 * What UEFI did. Nothing: Silicium's starlte FD at uefi/BASE (e51cd877) has no USB driver in its DXE list
 * (Platforms/Samsung/starltePkg/Include/DXE.inc), and the S5E9810 has only the null PlatformUsbfnLib, so its
 * UsbfnDwc3Dxe would refuse the SoC even if it were there. What S-Boot did is not known (it has a USB download
 * mode and charger detection): step 1 reads it, and step 3 (the gadget driver) stops a running controller and
 * resets it whatever it finds. The PHY sequence of step 2 resets the link and the PHY itself (CLKRST).
 *
 * THE SEQUENCE (step 2), each write read back and said; the BSP's order (dwc3_core_soft_reset: phy_power_on, then
 * phy_init's exynos_usbdrd_utmi_init: phy_exynos_usb_v3p1_enable then _pipe_ovrd):
 *   a  PMU +0x72C bit 0 set: the PHY out of isolation (phy-exynos-usbdrd.c:415-426, 1495, 1777; dt pmu_offset 0x72C,
 *      pmu_mask 0 -> BIT(0))
 *   b  LINK_CTRL: the Q-channel workaround: DIS_QACT_ID0/VBUS_VALID/BVALID/LINKGATE set, FORCE_QACT clear, 500 us,
 *      then FORCE_QACT set (phy-exynos-usb3p1.c:26-45)
 *   c  CLKRST LINK_SW_RST pulsed 10 us (usb3p1.c:230-239)
 *   d  CLKRST PHY_SW_RST and PHY_RST_SEL set (:101-116)
 *   e  HSP_TEST SIDDQ clear: the 2.0 PHY powered (:87-94, version 3.0.0)
 *   f  UTMI FORCE_SUSPEND, FORCE_SLEEP, DP_PULLDOWN, DM_PULLDOWN clear (:252-257)
 *   g  HSP EN_UTMISUSPEND set, COMMONONN set (dt common_block_disable = 1) (:259-265)
 *   h  100 us, then CLKRST PHY_RST_SEL set, PHY_SW_RST and PORT_RST clear (:279-280, 118-134)
 *   i  VBUS from the PHY's own pad is not used (dt is_not_vbus_pad = 1): LINK_CTRL BUS_FILTER_BYPASS = 0xF, UTMI
 *      FORCE_BVALID and FORCE_VBUSVALID set, HSP VBUSVLDEXTSEL and VBUSVLDEXT set (:56-67, 307-325)
 *   j  LINK_PORT HUB_PORT_SEL_OCD_U3/U2 set (dt use_io_for_ovc = 0) (:327-336)
 *   k  (not done: ReWA, the BSP's hs_rewa = 1 (usbdrd.c:1009, usb3p1.c:339-340), is the wake-up assist of a suspended
 *      link; a link that never suspends does not need it)
 *   l  pipe_ovrd: LINK_CTRL PIPE3_FORCE_EN and PIPE3_FORCE_RX_ELEC_IDLE set, PIPE3_FORCE_PHY_STATUS clear;
 *      COMBO_PMA_CTRL PMA_LOW_PWRN set (:196-212). The BSP undoes this in pipe3_init (pma_ready, :136-165) when it
 *      brings the SuperSpeed PHY up; this driver does not, so the link sees the SuperSpeed lanes as idle.
 * Not done: the PHY tuning (HSP_TUNE from the dt's hs_tune_param; the value the boot left is said), the OTP tuning
 * (CONFIG_EXYNOS_OTP), the Type-C orientation GPIO (SuperSpeed only).
 *
 * Registers: phy-exynos-usb3p1-reg.h (CTRL_VER :22, LINK_CTRL :24-35, LINK_PORT :40, :48-49, CLKRST :64-73, PWR :75,
 * COMBO_PMA_CTRL :139, :146, UTMI :153-161, HSP :163, :173-176, HSP_TUNE :185, HSP_TEST :215, :218-219).
 * Clocks: cmucal-sfr.c (CMU_TOP 0x1A240000 :14, FSYS0 0x11000000 :24; +0x1060/+0x2064/+0x1858 :456-458; FSYS0 user
 * mux +0x1E0 :1060, REF_CLK gate +0x206C :1063, the Q-channels :1087-1090; MUX SELECT [1:0], BUSY 16 :3133-3135; gate
 * CG_VAL 21, MANUAL 20 :3136-3137; divider ratio [3:0] :3141; user mux MUX_SEL bit 4, BUSY 7 :5064-5065; Q-channel
 * ENABLE 0, CLOCK_REQ 1 :5151-5164); Android's setting, mux 1 (PLL_SHARED0 / 4) and divider 7: cmucal-vclklut.c:158-163
 * through cmucal-vclk.c:187-190, 1833-1836; parents cmucal-node.c:195-200, 609-611; the dt's clocks 0x2C0 and 0x2C7 are
 * GATE_USB30DRD_USB30DRD_LINK and USB30DRD (include/dt-bindings/clock/exynos9810.h:260, 265, 272; clk-exynos9810.c:191,
 * 416). PMU 0x14060000: the dt's system-controller@14060000 (starlte.dts:4969-4971), the phy node's pmu_offset
 * (starlte.dts:5340-5342). The LDOs: s2mps18.c:127-148 (s2m_ldo_set_mode: L9CTRL + n - 9, mode << 6, mask 0xC0),
 * s2mps18-private.h:116, 272; dwc3/otg.c:196-210 (mode 3 on, 1 off); the dt's LDO9-14 (starlte_eur_open_26.dts:8997-9062).
 * All of Samsung's kernel fork (Scion-Project/android_kernel_samsung_exynos9810, lineage-24.0, e5906667068e), GPL-2.0:
 * read for facts only, no code taken.
 */
#include <stdarg.h>
#include <stdint.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_soc.h>
#include "hakios.usb_glue.h"
#include <sys/hakios.xnu.h>


#define DWC3_PA                 ((uint32_t)EXYNOS_USB_DWC3_PA)
#define DWC3_SIZE               0x10000u
#define USBCON_PA               ((uint32_t)EXYNOS_USB_CON_PA)
#define USBCON_SIZE             0x1000u         /* the dt's 0x200, mapped as a page */
#define PMU_PA                  ((uint32_t)EXYNOS_PMU_PA)
#define PMU_USB_PHY             EXYNOS_USB_PMU_USB_PHY         /* bit 0: out of isolation */
#define PMU_USB_DP_PHY          EXYNOS_USB_PMU_USB_DP_PHY
#define PMU_ISOL_BIT            (1u << 0)
#define CMU_TOP_PA              ((uint32_t)EXYNOS_CMU_TOP_PA)
#define TOP_MUX_USB30DRD        EXYNOS_USB_TOP_MUX_USB30DRD
#define TOP_DIV_USB30DRD        EXYNOS_USB_TOP_DIV_USB30DRD
#define TOP_GATE_USB30DRD       EXYNOS_USB_TOP_GATE_USB30DRD
#define CMU_FSYS0_PA            ((uint32_t)EXYNOS_CMU_FSYS0_PA)
#define FSYS0_USER_MUX          EXYNOS_USB_FSYS0_USER_MUX         /* MUX_SEL bit 4: 0 OSCCLK, 1 CMU_TOP's CLKCMU_FSYS0_USB30DRD */
#define FSYS0_REFCLK_GATE       EXYNOS_USB_FSYS0_REFCLK_GATE
#define FSYS0_QCH_CTRL          EXYNOS_USB_FSYS0_QCH_CTRL
#define FSYS0_QCH_LINK          EXYNOS_USB_FSYS0_QCH_LINK
#define FSYS0_QCH_DPPHY         EXYNOS_USB_FSYS0_QCH_DPPHY
#define FSYS0_QCH_PCS           EXYNOS_USB_FSYS0_QCH_PCS
#define GATE_MANUAL             (1u << 20)
#define GATE_CG_VAL             (1u << 21)
#define USER_MUX_SEL            (1u << 4)
#define ANDROID_TOP_MUX         EXYNOS_USB_ANDROID_TOP_MUX              /* DIV_PLL_SHARED0_DIV4 */
#define ANDROID_TOP_DIV         EXYNOS_USB_ANDROID_TOP_DIV

/* USBCON (phy-exynos-usb3p1-reg.h) */
#define CTRL_VER                0x00u
#define LINK_CTRL               0x04u
#define  LC_FORCE_RX_ELEC_IDLE  (1u << 18)
#define  LC_FORCE_PHY_STATUS    (1u << 17)
#define  LC_PIPE3_FORCE_EN      (1u << 16)
#define  LC_DIS_QACT_LINKGATE   (1u << 12)
#define  LC_DIS_QACT_ID0        (1u << 11)
#define  LC_DIS_QACT_VBUS_VALID (1u << 10)
#define  LC_DIS_QACT_BVALID     (1u << 9)
#define  LC_FORCE_QACT          (1u << 8)
#define  LC_BUS_FILTER_BYPASS   (0xFu << 4)
#define LINK_PORT               0x08u
#define  LP_SEL_OCD_U3          (1u << 3)
#define  LP_SEL_OCD_U2          (1u << 2)
#define CLKRST                  0x20u
#define  CR_LINK_PCLK_SEL       (1u << 7)
#define  CR_PHY_SW_RST          (1u << 3)
#define  CR_PHY_RST_SEL         (1u << 2)
#define  CR_PORT_RST            (1u << 1)
#define  CR_LINK_SW_RST         (1u << 0)
#define PWR                     0x24u
#define COMBO_PMA_CTRL          0x48u
#define  PMA_LOW_PWRN           (1u << 4)
#define UTMI                    0x50u
#define  UTMI_FORCE_VBUSVALID   (1u << 5)
#define  UTMI_FORCE_BVALID      (1u << 4)
#define  UTMI_DP_PULLDOWN       (1u << 3)
#define  UTMI_DM_PULLDOWN       (1u << 2)
#define  UTMI_FORCE_SUSPEND     (1u << 1)
#define  UTMI_FORCE_SLEEP       (1u << 0)
#define HSP                     0x54u
#define  HSP_VBUSVLDEXTSEL      (1u << 13)
#define  HSP_VBUSVLDEXT         (1u << 12)
#define  HSP_EN_UTMISUSPEND     (1u << 9)
#define  HSP_COMMONONN          (1u << 8)
#define HSP_TUNE                0x58u
#define HSP_TEST                0x5Cu
#define  HSP_TEST_SIDDQ         (1u << 24)
#define  HSP_TEST_LINESTATE(v)  (((v) >> 20) & 3u)

/* the S2MPS18's LDO9-14 (s2mps18-private.h:116, 272) */
#define S2MPS18_L9CTRL          0x52u
#define LDO_MODE_SHIFT          6
#define LDO_MODE_MASK           0xC0u

static vm_offset_t usbcon, pmu, top, fsys0;
static bool mapped;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
map_all(usb_glue_say_t say)
{
	if (mapped) {
		return true;
	}
	pmu = ml_io_map(PMU_PA, 0x1000);
	top = ml_io_map(CMU_TOP_PA, 0x4000);
	fsys0 = ml_io_map(CMU_FSYS0_PA, 0x4000);
	usbcon = ml_io_map(USBCON_PA, USBCON_SIZE);
	if (pmu == 0 || top == 0 || fsys0 == 0 || usbcon == 0) {
		say("usb: exynos9810: could not map the PMU, CMU_TOP, CMU_FSYS0 or USBCON\n");
		return false;
	}
	mapped = true;
	return true;
}

static const char *const linestates[4] = { "SE0", "J", "K", "SE1" };

static void
say_usbcon(usb_glue_say_t say, const char *when)
{
	uint32_t test = rd(usbcon, HSP_TEST);

	say("usb: exynos9810: USBCON %s: VER %08x LINK_CTRL %08x LINK_PORT %08x CLKRST %08x PWR %08x PMA %08x\n", when,
	    rd(usbcon, CTRL_VER), rd(usbcon, LINK_CTRL), rd(usbcon, LINK_PORT), rd(usbcon, CLKRST), rd(usbcon, PWR),
	    rd(usbcon, COMBO_PMA_CTRL));
	say("usb: exynos9810: USBCON %s: UTMI %08x HSP %08x HSP_TUNE %08x HSP_TEST %08x (SIDDQ %u, line %s)\n", when,
	    rd(usbcon, UTMI), rd(usbcon, HSP), rd(usbcon, HSP_TUNE), test, (test & HSP_TEST_SIDDQ) ? 1 : 0,
	    linestates[HSP_TEST_LINESTATE(test)]);
}

static void
say_ldos(usb_glue_say_t say)
{
	char line[160];
	int n = 0;

	if (!hakios_acpm_init(say)) {
		say("usb: exynos9810: ACPM not usable: the S2MPS18's LDO9-14 not read\n");
		return;
	}
	for (uint32_t i = 0; i < 6; i++) {
		uint8_t v = 0;
		int e = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, S2MPS18_L9CTRL + i, &v);

		n += snprintf(line + n, sizeof(line) - (size_t)n, " L%u %s%02x(mode %u)", 9 + i, e ? "err " : "", v,
		    (unsigned)(v >> LDO_MODE_SHIFT));
	}
	say("usb: exynos9810: S2MPS18 LDO9-14 CTRL:%s (mode 1 on with the AP, 3 Android's USB mode)\n", line);
}

/* the clocks as the boot left them; true when the DWC3 and USBCON have a clock to be read with */
static bool
say_clocks(usb_glue_say_t say)
{
	uint32_t gate = rd(top, TOP_GATE_USB30DRD), mux = rd(top, TOP_MUX_USB30DRD), div = rd(top, TOP_DIV_USB30DRD);
	uint32_t user = rd(fsys0, FSYS0_USER_MUX);
	bool top_shut = (gate & GATE_MANUAL) && !(gate & GATE_CG_VAL);
	bool from_top = (user & USER_MUX_SEL) != 0;

	say("usb: exynos9810: CMU_TOP CLKCMU_FSYS0_USB30DRD: mux %08x (select %u; Android 1), div %08x (ratio %u; Android 7), "
	    "gate %08x (MANUAL %u CG_VAL %u)%s\n", mux, mux & 3, div, div & 0xf, gate, (gate & GATE_MANUAL) ? 1 : 0,
	    (gate & GATE_CG_VAL) ? 1 : 0, top_shut ? ": SHUT" : "");
	say("usb: exynos9810: CMU_FSYS0: user mux %08x (%s), REF_CLK gate %08x, QCH CTRL %08x LINK %08x DPPHY %08x PCS %08x\n",
	    user, from_top ? "from CMU_TOP" : "OSCCLK", rd(fsys0, FSYS0_REFCLK_GATE), rd(fsys0, FSYS0_QCH_CTRL),
	    rd(fsys0, FSYS0_QCH_LINK), rd(fsys0, FSYS0_QCH_DPPHY), rd(fsys0, FSYS0_QCH_PCS));
	return !(from_top && top_shut);
}

static bool
exynos_look(usb_glue_say_t say, __unused uint32_t flags)
{
	if (!map_all(say)) {
		return false;
	}
	uint32_t iso = rd(pmu, PMU_USB_PHY), iso_dp = rd(pmu, PMU_USB_DP_PHY);

	say("usb: exynos9810: PMU USB PHY %08x (%s), DP/SS PHY %08x (%s)\n", iso, (iso & PMU_ISOL_BIT) ? "out of isolation" : "isolated",
	    iso_dp, (iso_dp & PMU_ISOL_BIT) ? "out of isolation" : "isolated");
	bool readable = say_clocks(say);
	say_ldos(say);
	if (!readable) {
		say("usb: exynos9810: the controller's clock is shut at CMU_TOP: USBCON and the DWC3 are not read (usb_gadget_clk=1 opens it)\n");
		return false;
	}
	say_usbcon(say, "as the boot left it");
	return true;
}

/* one register: set and clear bits, read back, said */
static uint32_t
change(usb_glue_say_t say, const char *step, const char *name, uint32_t off, uint32_t set, uint32_t clear)
{
	uint32_t before = rd(usbcon, off), want = (before & ~clear) | set;

	wr(usbcon, off, want);
	uint32_t after = rd(usbcon, off);
	say("usb: exynos9810: 2%s: %s %08x -> %08x%s\n", step, name, before, after, after == want ? "" : " (NOT as written)");
	return after;
}

static int
exynos_phy_on(usb_glue_say_t say, uint32_t flags)
{
	if (!map_all(say)) {
		return ENXIO;
	}

	/* the clocks: open only what is shut, and only as Android sets it */
	uint32_t gate = rd(top, TOP_GATE_USB30DRD), user = rd(fsys0, FSYS0_USER_MUX);
	if ((gate & GATE_MANUAL) && !(gate & GATE_CG_VAL)) {
		if (!(flags & USB_GLUE_CLK)) {
			say("usb: exynos9810: 2: CMU_TOP's USB30DRD gate is shut and usb_gadget_clk is not 1: stopping here\n");
			return ENXIO;
		}
		uint32_t mux = rd(top, TOP_MUX_USB30DRD), div = rd(top, TOP_DIV_USB30DRD);
		if ((mux & 3) != ANDROID_TOP_MUX) {
			wr(top, TOP_MUX_USB30DRD, (mux & ~3u) | ANDROID_TOP_MUX);
		}
		if ((div & 0xf) != ANDROID_TOP_DIV) {
			wr(top, TOP_DIV_USB30DRD, (div & ~0xfu) | ANDROID_TOP_DIV);
		}
		wr(top, TOP_GATE_USB30DRD, gate | GATE_CG_VAL);
		say("usb: exynos9810: 2: usb_gadget_clk=1: CMU_TOP mux %08x div %08x gate %08x\n", rd(top, TOP_MUX_USB30DRD),
		    rd(top, TOP_DIV_USB30DRD), rd(top, TOP_GATE_USB30DRD));
		if (!(rd(top, TOP_GATE_USB30DRD) & GATE_CG_VAL)) {
			return EIO;
		}
	}
	if (!(user & USER_MUX_SEL) && (flags & USB_GLUE_CLK)) {
		wr(fsys0, FSYS0_USER_MUX, user | USER_MUX_SEL);
		delay(10);
		say("usb: exynos9810: 2: usb_gadget_clk=1: CMU_FSYS0 user mux %08x -> %08x (from CMU_TOP)\n", user, rd(fsys0, FSYS0_USER_MUX));
	}

	/* the supplies: mode 3 (Android's while USB is up), only when asked, each read back */
	if ((flags & USB_GLUE_LDO) && hakios_acpm_init(say)) {
		for (uint32_t i = 0; i < 6; i++) {
			uint8_t v = 0;

			if (hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, S2MPS18_L9CTRL + i, 3u << LDO_MODE_SHIFT, LDO_MODE_MASK) != 0 ||
			    hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, S2MPS18_L9CTRL + i, &v) != 0) {
				say("usb: exynos9810: 2: usb_gadget_ldo=1: LDO%u: ACPM failed\n", 9 + i);
				continue;
			}
			say("usb: exynos9810: 2: usb_gadget_ldo=1: LDO%u CTRL now %02x (mode %u)\n", 9 + i, v, (unsigned)(v >> LDO_MODE_SHIFT));
		}
	}

	say_usbcon(say, "before");

	/* a */
	uint32_t iso = rd(pmu, PMU_USB_PHY);
	wr(pmu, PMU_USB_PHY, iso | PMU_ISOL_BIT);
	say("usb: exynos9810: 2a: PMU +0x72C %08x -> %08x (the PHY out of isolation)\n", iso, rd(pmu, PMU_USB_PHY));

	/* b: the Q-channel workaround (phy-exynos-usb3p1.c exynos_cal_usbphy_q_ch(, 1)) */
	uint32_t v = rd(usbcon, LINK_CTRL);
	v |= LC_DIS_QACT_ID0 | LC_DIS_QACT_VBUS_VALID | LC_DIS_QACT_BVALID | LC_DIS_QACT_LINKGATE;
	v &= ~LC_FORCE_QACT;
	delay(500);
	wr(usbcon, LINK_CTRL, v);
	delay(500);
	v = rd(usbcon, LINK_CTRL) | LC_FORCE_QACT;
	delay(500);
	wr(usbcon, LINK_CTRL, v);
	say("usb: exynos9810: 2b: LINK_CTRL %08x (Q-channel: QACT forced, its other sources off)\n", rd(usbcon, LINK_CTRL));

	/* c: the link reset */
	v = rd(usbcon, CLKRST);
	wr(usbcon, CLKRST, v | CR_LINK_SW_RST);
	delay(10);
	wr(usbcon, CLKRST, v & ~CR_LINK_SW_RST);
	say("usb: exynos9810: 2c: CLKRST %08x (LINK_SW_RST pulsed)\n", rd(usbcon, CLKRST));

	change(say, "d", "CLKRST", CLKRST, CR_PHY_SW_RST | CR_PHY_RST_SEL, 0);                 /* d */
	change(say, "e", "HSP_TEST", HSP_TEST, 0, HSP_TEST_SIDDQ);                             /* e */
	change(say, "f", "UTMI", UTMI, 0, UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
	change(say, "g", "HSP", HSP, HSP_EN_UTMISUSPEND | HSP_COMMONONN, 0);                  /* g */
	delay(100);                                                                             /* h */
	change(say, "h", "CLKRST", CLKRST, CR_PHY_RST_SEL, CR_PHY_SW_RST | CR_PORT_RST);
	change(say, "i", "LINK_CTRL", LINK_CTRL, LC_BUS_FILTER_BYPASS, 0);                     /* i */
	change(say, "i", "UTMI", UTMI, UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID, 0);
	change(say, "i", "HSP", HSP, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT, 0);
	change(say, "j", "LINK_PORT", LINK_PORT, LP_SEL_OCD_U3 | LP_SEL_OCD_U2, 0);            /* j */
	change(say, "l", "LINK_CTRL", LINK_CTRL, LC_PIPE3_FORCE_EN | LC_FORCE_RX_ELEC_IDLE, LC_FORCE_PHY_STATUS);   /* l */
	change(say, "l", "COMBO_PMA_CTRL", COMBO_PMA_CTRL, PMA_LOW_PWRN, 0);
	say_usbcon(say, "after");
	return 0;
}

static void
exynos_phy_state(char *out, unsigned size)
{
	if (!mapped) {
		snprintf(out, size, "(not mapped)");
		return;
	}
	uint32_t test = rd(usbcon, HSP_TEST), utmi = rd(usbcon, UTMI);

	snprintf(out, size, "line %s, UTMI %08x (VBUSVALID forced %u), SIDDQ %u", linestates[HSP_TEST_LINESTATE(test)], utmi,
	    (utmi & UTMI_FORCE_VBUSVALID) ? 1 : 0, (test & HSP_TEST_SIDDQ) ? 1 : 0);
}

/* the 2.0 PHY powered down (SIDDQ) and isolated again, as phy_power_en(, 0) and phy_power_off do (usb3p1.c:87-94, usbdrd.c:1502-1511) */
static void
exynos_phy_off(void)
{
	if (!mapped) {
		return;
	}
	wr(usbcon, HSP_TEST, rd(usbcon, HSP_TEST) | HSP_TEST_SIDDQ);
	wr(pmu, PMU_USB_PHY, rd(pmu, PMU_USB_PHY) & ~PMU_ISOL_BIT);
}

static const struct hakios_usb_glue exynos9810_glue = {
	.name = "exynos9810",
	.dwc3_pa = DWC3_PA,
	.dwc3_size = DWC3_SIZE,
	.look = exynos_look,
	.phy_on = exynos_phy_on,
	.phy_state = exynos_phy_state,
	.phy_off = exynos_phy_off,
};

const struct hakios_usb_glue *
hakios_usb_glue_exynos9810(void)
{
	return &exynos9810_glue;
}
