/*
 * PORT NOTE (Scion's iOS 27 tree, the fork's scion/xnu-12377, xnu-12377 STARLTE): Scion-Project/scion-kernel xnu/files/bsd/sys/
 * hakios.exynos_abox.h (APSL; HakiOS/Scion); only the headphones' hook is left out. On this path (S-Boot -> Starbite
 * shim -> XNU) abox_rmem is kept out of XNU's RAM because the ADT gives the kernel no RAM there (its base RAM is above
 * 4 GB, the extra_ram=1 range ends at 0xD8000000), not by a keep_out of Scion's loader.
 */
/*
 * ABOX, the Exynos 9810's audio block, as the Galaxy S9's speaker path (drivers/audio/abox/hakios.exynos_abox.c, boot-arg
 * abox=N): memory -> RDMA0 -> SPUS (ABOX's stream bus) -> SIFS0 -> UAIF1 (I2S, ABOX the master) -> the two MAX98512
 * amplifiers. ABOX's own CPU stays off ("bypass"): the AP programs what Samsung's ABOX firmware (calliope) would.
 *
 * This header is two things: the amplifiers' driver's way in (the hook below), and ABOX's registers as far as they are
 * known, each marked FACT (read in the S9 kernel's source, file named) or INFERRED (worked out; why is said) -- the
 * record for a firmware mode of the same driver and for an open ABOX firmware. The GPL source was read for these facts;
 * nothing of it is copied.
 *
 * THE FORMAT ON UAIF1 (what the amplifiers' driver must match). From Samsung's machine driver (exynos9810_madera.c,
 * the "UAIF1" link: SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBS_CFS) and the defaults of ABOX's
 * driver, which UAIF1 gets when nothing in user space says otherwise (abox.c: every SIF at 48000 Hz, S16, 2 channels;
 * abox_if.c: the UAIF's width, channels and rate follow them):
 *   - I2S (Philips): two slots a frame, LRCLK low for the left slot (slot 0), high for the right (slot 1); each sample's
 *     MSB one BCLK after the LRCLK edge
 *   - NB_NF: data changes on BCLK's falling edge and is sampled on its rising edge; LRCLK not inverted. In ABOX's terms
 *     UAIF1_CTRL1 BCLK_POLARITY = 1, WS_POLAR = 0, WS_MODE = 0 (abox_if.c, abox_uaif_set_fmt)
 *   - CBS_CFS: ABOX drives BCLK and LRCLK; the amplifiers are slaves (max98512.c: PCM_MASTER_MODE slave)
 *   - 48 kHz, 2 slots of 16 bits, S16: BCLK = 32 x fs = 1.536 MHz, LRCLK = 48 kHz (max98512.c's BCLK/LRCLK ratio for
 *     2 x 16: BSEL 32)
 *   - slot 0 (left) is for the amplifier at 0x39, slot 1 (right) for the one at 0x3A (the device tree's max98512@39,
 *     maxim,mono_stereo = 1; max98512.c: the left chip takes PCM channel 0, the right channel 1)
 * Android's user space can set other widths through the "UAIF1 width/channel/rate" controls; nothing in the kernel's
 * source does, and that is not followed here. The hook passes the numbers, so an amplifier driver can check them.
 */
#ifndef _SYS_HAKIOS_EXYNOS_ABOX_H_
#define _SYS_HAKIOS_EXYNOS_ABOX_H_

#include <stdint.h>
#include <sys/hakios.exynos_soc.h>
#define HAKIOS_ABOX_RATE                48000u
#define HAKIOS_ABOX_SLOT_BITS           16u
#define HAKIOS_ABOX_CHANNELS            2u

#ifdef KERNEL
/*
 * The speaker amplifiers' driver (another file) registers itself here. start() is called once UAIF1's BCLK and LRCLK
 * run (RDMA0 feeding it, silence at first) and stop() before they stop; both from the driver's own thread, which may
 * sleep. With none registered, the stream runs and the log says that nothing can be heard.
 */
struct hakios_abox_amp {
	const char *name;
	int (*start)(uint32_t rate, uint32_t slot_bits, uint32_t channels);   /* after UAIF1's clocks run; 0 or an errno */
	void (*stop)(void);                                                    /* before they stop */
	void (*status)(const char *when);   /* may be NULL: what the amplifiers say of themselves, into the log */
};
void hakios_abox_set_amp(const struct hakios_abox_amp *amp);              /* NULL: none */

void hakios_exynos_abox_init(void);    /* boot-arg abox=N on the Galaxy S9: the first N steps of ABOX's speaker path */
void hakios_exynos_abox_quiesce(void); /* loader_return(): RDMA0 and UAIF1 stopped, if they run */
void hakios_abox_console(const char *arg);      /* the USB console's "abox" (hakios.dwc3.c): status, tone, r, w, set */
/* (port) hakios_abox_headphones(): not in this port (no CS47L93 codec driver in this tree) */
#endif

/*
 * ---- Where things are (FACT: the S9's device tree, exynos9810-starlte_eur_open_26.dts) ----------------------------------
 */
#define ABOX_SFR_PA             ((uint32_t)EXYNOS_ABOX_SFR_PA)     /* abox@17c50000 "sfr", 64 KB */
#define ABOX_SYSREG_PA          ((uint32_t)EXYNOS_ABOX_SYSREG_PA)     /* ... "sysreg": SYSREG_AUD */
#define ABOX_SRAM_PA            ((uint32_t)EXYNOS_ABOX_SRAM_PA)     /* ... "sram", 0x31000 bytes: the CPU's memory (unused in bypass) */
#define ABOX_GICD_PA            ((uint32_t)EXYNOS_ABOX_GICD_PA)     /* abox_gic: ABOX's own GIC-400 (unused in bypass) */
#define ABOX_CMU_PA             ((uint32_t)EXYNOS_CMU_AUD_PA)     /* CMU_AUD (cal-if/exynos9810/cmucal-sfr.c: SFR_BLOCK(AUD), 0x8000) */
#define ABOX_GPIO_PA            ((uint32_t)EXYNOS_GPIO_AUD_PA)     /* pinctrl@17C60000: gpb0, gpb1, gpb2 (GPIO_AUD) */
#define ABOX_SYSMMU_PA          ((uint32_t)EXYNOS_ABOX_SYSMMU_PA)     /* sysmmu@17E00000, "Abox", ABOX's iommus */
#define ABOX_PMU_PA             ((uint32_t)EXYNOS_PMU_PA)     /* the PMU (system-controller@14060000) */
/* abox_rmem (the device tree's reserved-memory for ABOX, 0xFB200000 + 0x2800000): below 4 GB, kept out of UEFI and XNU
 * by the loader (starlte's keep_out); the ring goes at its start when no RAM below 4 GB is the kernel's */
#define ABOX_RMEM_PA            0xFB200000u
#define ABOX_RMEM_SIZE          0x02800000u
#define ABOX_DTZPC_ARG          EXYNOS_ABOX_ABOX_DTZPC_ARG     /* pd-aud's need_smc: the DTZPC restore's argument */

/*
 * ---- PMU (FACT) -----------------------------------------------------------------------------------------------------------
 * cal-if/exynos9810/flexpmu_cal_local_exynos9810.h (aud_on, aud_off) and abox.c (the CONFIG_SOC_EXYNOS9810 block)
 */
#define PMU_AUD_CONFIGURATION   EXYNOS_ABOX_PMU_AUD_CONFIGURATION          /* [3:0] = 0xF on, 0 off */
#define PMU_AUD_STATUS          EXYNOS_ABOX_PMU_AUD_STATUS          /* [3:0] == 0xF when on, 0 when off */
#define PMU_GPIO_MODE_ABOX      EXYNOS_ABOX_PMU_GPIO_MODE_ABOX          /* GPIO_MODE_ABOX_SYS_PWR_REG: bit 0, set by abox_pad_retention() both ways */
#define PMU_PAD_RETENTION_ABOX  EXYNOS_ABOX_PMU_PAD_RETENTION_ABOX          /* PAD_RETENTION_ABOX_OPTION: bit 28 set releases the pads' retention */
#define PMU_PAD_RELEASE         (1u << 28)
#define PMU_ABOX_CPU_CONFIG     EXYNOS_ABOX_PMU_ABOX_CPU_CONFIG          /* ABOX_CPU_CONFIGURATION: bit 0 the CPU's local power */
#define PMU_ABOX_CPU_STATUS     EXYNOS_ABOX_PMU_ABOX_CPU_STATUS          /* bit 0: the CPU's power is on */
#define PMU_ABOX_CPU_OPTION     EXYNOS_ABOX_PMU_ABOX_CPU_OPTION          /* bit 28: the CPU released (ENABLE_CPU) */
#define PMU_ABOX_CPU_ENABLE     EXYNOS_ABOX_PMU_ABOX_CPU_ENABLE
#define PMU_ABOX_CPU_STANDBY    EXYNOS_ABOX_PMU_ABOX_CPU_STANDBY          /* bit 29 WFE, bit 28 WFI */
/* After AUD_STATUS reads on, the CAL makes SMC 0x82000410 (SMC_CMD_PREAPRE_PD_ONOFF, linux/smc.h) with (1, 0x17C10204):
 * the DTZPC restore (exynos-el3_mon.c, exynos_pd_tz_restore), before any of the block's registers is touched. */
#define ABOX_SMC_PD_ONOFF       0x82000410u

/*
 * ---- CMU_AUD (FACT: cal-if/exynos9810/cmucal-sfr.c, -node.c, -qch.c, -vclklut.c; ra.c for the sequences) ------------------
 * PLL_AUD is a PLL_1031X fed by OSCCLK_AUD, 26 MHz: Fout = 26 MHz x (M + K/65536) / (P x 2^S). For 48 kHz the BSP's
 * table has M 45, P 1, S 0, K 24319: 1179.648040 MHz (abox.h AUD_PLL_RATE_HZ_FOR_48000).
 */
#define CMU_PLL_LOCKTIME        EXYNOS_ABOX_CMU_PLL_LOCKTIME          /* [19:0]: P x 500 for a PLL with K (pll_spec.c: PLL_1031X flock_time 500) */
#define CMU_PLL_CON0            EXYNOS_ABOX_CMU_PLL_CON0          /* S [2:0], P [13:8], M [25:16], MUX_SEL 4, MUX_BUSY 7, STABLE 29, ENABLE 31 */
#define CMU_PLL_CON3            EXYNOS_ABOX_CMU_PLL_CON3          /* K [15:0], signed */
#define PLL_MUX_SEL             (1u << 4)       /* 1: the PLL's output, 0: OSCCLK (ra.h) */
#define PLL_MUX_BUSY            (1u << 7)
#define PLL_STABLE              (1u << 29)
#define PLL_ENABLE              (1u << 31)
#define PLL_AUD_M               45u
#define PLL_AUD_P               1u
#define PLL_AUD_S               0u
#define PLL_AUD_K               24319u
#define PLL_AUD_LOCK            500u
/* dividers: DIVRATIO (the ratio less one) in the low bits, BUSY bit 16, ENABLE_AUTOMATIC_CLKGATING bit 28 */
#define CMU_DIV_AUDIF           EXYNOS_ABOX_CMU_DIV_AUDIF          /* [8:0] DIV_CLK_AUD_AUDIF from PLL_AUD: abox's "audif" */
#define CMU_DIV_BUS             EXYNOS_ABOX_CMU_DIV_BUS          /* [2:0] DIV_CLK_AUD_BUS from PLL_AUD: ABOX's ACLK, abox's "bus" */
#define CMU_DIV_BUSP            EXYNOS_ABOX_CMU_DIV_BUSP          /* [1:0] DIV_CLK_AUD_BUSP from DIV_CLK_AUD_BUS: the block's APB */
#define CMU_DIV_PLL             EXYNOS_ABOX_CMU_DIV_PLL          /* [3:0] DIV_CLK_AUD_PLL: toward the CPU's clock (not touched) */
#define CMU_DIV_UAIF1           EXYNOS_ABOX_CMU_DIV_UAIF1          /* [8:0] DIV_CLK_AUD_UAIF1 from DIV_CLK_AUD_AUDIF: UAIF1's BCLK */
#define CMU_MUX_CPU             EXYNOS_ABOX_CMU_MUX_CPU          /* MUX_CLK_AUD_CPU (not touched) */
#define CMU_MUX_UAIF1           EXYNOS_ABOX_CMU_MUX_UAIF1          /* [0]: 0 DIV_CLK_AUD_UAIF1, 1 CLKIO_AUD_UAIF1 (the pin); BUSY 16 */
#define CMU_DIV_BUSY            EXYNOS_ABOX_CMU_DIV_BUSY
/* rates: the CAL's common table for BLK_AUD (cmucal-vclklut.c blk_aud_lut_params) has DIV_CLK_AUD_BUS 1, BUSP 2; abox.c's
 * abox_register_bclk_usage() with uaif_max_div 512 (device tree) asks AUDIF for 384000 x 96 x 2 = 73.728 MHz (ratio 16), and
 * abox_if.c BCLK = rate x channels x width = 1.536 MHz (ratio 48 under AUDIF) */
#define AUD_DIV_BUS             1u              /* 589.824 MHz */
#define AUD_DIV_BUSP            2u              /* 196.608 MHz */
#define AUD_DIV_AUDIF           15u             /* 73.728 MHz */
#define AUD_DIV_UAIF1           47u             /* 1.536 MHz */
/* gates of UAIF1 (CG_VAL 21, MANUAL 20, ENABLE_AUTOMATIC_CLKGATING 28): read, not written */
#define CMU_GATE_BCLK_UAIF1     0x2020
#define CMU_GATE_RSTN_UAIF1     0x2094
/* Q-channels: ENABLE bit 0 (1: the hardware gates the clock by itself), CLOCK_REQ bit 1 (with ENABLE 0: 1 = clock on),
 * IGNORE_FORCE_PM_EN bit 2, EXPIRE_VAL [25:16]. ra.c ra_req_enable_qch(): a clk_enable sets CLOCK_REQ only when ENABLE is 0 */
#define CMU_QCH_DUMMY           0x3000          /* ABOX_QCH_DUMMY: abox's "cpu" clock (GATE_ABOX_DUMMY) */
#define CMU_QCH_ACLK            0x3024          /* ABOX_QCH_ACLK (no Linux driver enables it) */
#define CMU_QCH_BCLK1           0x302C          /* ABOX_QCH_BCLK1: UAIF1's "bclk_gate" (GATE_ABOX_BCLK1) */
#define CMU_QCH_GPIO            0x304C          /* GPIO_AUD_QCH */
#define CMU_QCH_SYSMMU          0x3060          /* SYSMMU_AUD_QCH: the SysMMU's "aclk" (GATE_SMMU_ABOX) */
#define QCH_ENABLE              (1u << 0)
#define QCH_CLOCK_REQ           (1u << 1)
#define CMU_CONTROLLER_OPTION   0x0800          /* [29:28] kept by the CAL across power-down (aud_save) */
#define CMU_HCHGEN_CLKMUX       0x0850

/*
 * ---- SYSREG_AUD (FACT: abox.h, abox.c abox_enable(); flexpmu aud_save) ----------------------------------------------------
 */
#define SYSREG_DRCG_EN          0x0104          /* BUS_COMPONENT_DRCG_EN (kept by the CAL across power-down) */
#define SYSREG_SNPS_EMA         0x0300
#define SYSREG_L2_CACHE_CON     0x0328          /* only with the "share vts sram" quirk, which the S9 has not */
#define SYSREG_MISC_CON         0x032C          /* abox_enable() writes 1 before anything else; what bit 0 does is not said */

/*
 * ---- GPIO_AUD, bank gpb1 (FACT: pinctrl-exynos.c exynos9810_pin_banks1, bank_type_6; the device tree's aud-i2s1-bus) -----
 * gpb1-0..3 are UAIF1's pins: function 2 and pud 1 when in use ("default"), function 0 and pud 1 when idle ("sleep").
 * Every field 4 bits a pin but DAT (1) and CONPDN (2).
 */
#define GPIO_GPB1               0x0020
#define GPIO_CON                0x00
#define GPIO_DAT                0x04
#define GPIO_PUD                0x08
#define GPIO_DRV                0x0C
#define GPIO_CONPDN             0x10
#define GPIO_PUDPDN             0x14
#define UAIF1_PINS_MASK         0xFFFFu         /* pins 0..3 */
#define UAIF1_PINS_FUNC         0x2222u
#define UAIF1_PINS_IDLE         0x0000u
#define UAIF1_PINS_PUD          0x1111u

/*
 * ---- ABOX's SysMMU (FACT: drivers/iommu/exynos-iommu.h, -reg.h) -----------------------------------------------------------
 * Linux's values: CTRL_ENABLE 0x5, CTRL_BLOCK 0x7, CTRL_DISABLE 0x0, CTRL_BLOCK_DISABLE 0x3 (what its runtime suspend
 * leaves). So bit 2 is set exactly in the translating states and bit 1 in the blocked ones (INFERRED from those four
 * values); MMU_STATUS bit 0 says blocked (sysmmu_block() waits for it).
 */
#define MMU_CTRL                0x000
#define MMU_CFG                 0x004
#define MMU_STATUS              0x008
#define MMU_VERSION             0x034
#define MMU_PT_BASE             0x00C           /* REG_PT_BASE_PPN: the page table's page number (exynos-iommu.h) */
#define MMU_CTRL_TRANSLATE      (1u << 2)
#define MMU_CTRL_BLOCK          (1u << 1)
#define MMU_CTRL_DISABLE        0x0u

/*
 * ---- ABOX's own registers (FACT: abox.h, the 9810's side of it; abox.c's regmap defaults abox_reg_defaults_9810) ---------
 */
#define ABOX_IP_INDEX           0x0000          /* reads 0x41424F58, "ABOX" */
#define ABOX_IP_INDEX_VALUE     0x41424F58u
#define ABOX_VERSION            0x0004
#define ABOX_SYSPOWER_CTRL      0x0010          /* bit 0; never written by Linux */
#define ABOX_SYSPOWER_STATUS    0x0014
#define ABOX_SYSTEM_CONFIG0     0x0020          /* resets to 0x4444 */
#define ABOX_REMAP_MASK         0x0024          /* 0xFFF00000 */
#define ABOX_REMAP_ADDR         0x0028          /* 0x17D00000: the CPU's address 0 is the SRAM */
#define ABOX_DYN_CLOCK_OFF      0x0030          /* [30:0], resets to 0x7FFFFFFF */
#define ABOX_QCHANNEL_DISABLE   0x0038          /* bit = enum qchannel: 0 CA7, 1 ACLK, 2..5 BCLK_UAIF0..3, 6 BCLK_DSIF ... */
#define QCHANNEL_ACLK           (1u << 1)       /* 1: ABOX's bus clock never gated (the T570 work keeps it so while it plays) */
#define QCHANNEL_BCLK_UAIF1     (1u << 3)       /* 1: never gated. Samsung sets it for UAIF0 (the codec) while it plays */
#define ABOX_ROUTE_CTRL0        0x0040          /* UAIFn SPK at [4n+3:4n]: 1 SIFS0, 2 SIFS1, 3 SIFS2, 12 SIFMS; DSIF [23:20] */
#define ROUTE_UAIF1_SHIFT       4
#define ROUTE_SIFS0             1u
#define ROUTE_SIFS1             2u
#define ABOX_ROUTE_CTRL1        0x0044          /* NSRCn [4n+3:4n], SPUSM [19:16] */
#define ABOX_ROUTE_CTRL2        0x0048          /* RSRCn [4n+3:4n]: 1..3 SIFS0..2, 8..11 NSRC0..3 */
#define ABOX_SPUS_CTRL0         0x0200          /* chain n at [4n+3:4n]: IN bit 3 (0 RDMAn, 1 SIFSM), OUT [2:1] (0 SIFS1,
                                                 * 1 SIFS0, 2 SIFS2), ASRC bit 0 (off/on) */
#define SPUS_CHAIN_MASK         0xFu
#define SPUS_IN_SIFSM           (1u << 3)
#define SPUS_OUT_SIFS1          (0u << 1)
#define SPUS_OUT_SIFS0          (1u << 1)
#define SPUS_ASRC_ON            (1u << 0)
#define ABOX_SPUS_CTRL1         0x0204          /* MIXP_FORMAT [4:0] (SIFS0, the mixer), SIFS_OUT1_SEL [18:16] (SPUS OUTn),
                                                 * SIFS_OUT2_SEL [21:19], SIFM_IN_SEL [24:22] */
#define SPUS_MIXP_FORMAT_MASK   0x1Fu
#define SPUS_SIFS1_SEL_SHIFT    16
#define SPUS_SIFS1_SEL_MASK     (7u << 16)
#define ABOX_SPUS_CTRL2         0x0208          /* MIXP_FLUSH bit 0: written 1 before the mixer comes up and after it goes */
#define ABOX_SPUS_CTRL3         0x020C          /* SIFS1_FLUSH 0, SIFS2_FLUSH 1, SIFM_FLUSH 2 */
#define ABOX_SPUS_SIFS_CNT0     0x0280          /* SIFS0/1 counts: only for SIFS looped to NSRC (capture) */
/*
 * SPUS's sample banks: the piece of ABOX's own buffer memory each RDMA (and the ASRCs, the mixer) fills ahead of SPUS.
 * FACT that they are there on the 9810 and reset to 0: abox.c's abox_reg_defaults_9810 lists 0x0220-0x023C, 0x0240 and
 * 0x0260 with 0, and neither abox_readable_reg() nor abox_writeable_reg() takes them, so the 9810's AP never reads or
 * writes them (calliope does, or nobody). FACT of the fields, from Samsung's next ABOX driver (the Galaxy S10's kernel,
 * github.com/ivanmeler/android_kernel_samsung_beyondlte, sound/soc/samsung/abox/abox_soc.h, whose "ABOX version below
 * 2.0.0" branch is the 9810's: its Kconfig gives SOC_EXYNOS9810 ABOX version 0x10100): SPUS_SBANK_RDMAn at 0x0220 + 4n,
 * SBANK_ASRC at 0x0240, SBANK_MIXP at 0x0260; SIZE [29:20] in 16-byte units, STR [13:4]. There the AP sets an RDMA's SIZE
 * itself before each stream (abox_cmpnt_v20.c abox_cmpnt_adjust_sbank(): 128 bytes for periods of 10 ms or less, else
 * 512), keeping STR. INFERRED: a SIZE of 0 gives the RDMA nowhere to put what it reads, so in bypass the driver sets
 * RDMA0's (abox_sbank=, 512 bytes, as Samsung does for our 10.7 ms periods) when it reads 0.
 */
#define ABOX_SPUS_SBANK_RDMA(n) (0x0220 + 4 * (n))
#define ABOX_SPUS_SBANK_ASRC0   0x0240
#define ABOX_SPUS_SBANK_MIXP    0x0260
#define SBANK_SIZE_SHIFT        20
#define SBANK_SIZE_MASK         (0x3FFu << SBANK_SIZE_SHIFT)
#define SBANK_STR_MASK          (0x3FFu << 4)
#define SBANK_BYTES(v)          ((((v) & SBANK_SIZE_MASK) >> SBANK_SIZE_SHIFT) * 16u)
#define ABOX_UAIF_CTRL0(n)      (0x0500 + 0x10 * (n))  /* resets to 0x01000010 */
#define ABOX_UAIF_CTRL1(n)      (0x0504 + 0x10 * (n))  /* resets to 0 */
#define ABOX_UAIF_STATUS(n)     (0x050C + 0x10 * (n))  /* ERROR_OF_SPK bit 0, ERROR_OF_MIC bit 1 */
/* UAIF CTRL0: START_FIFO_DIFF_MIC [31:28], START_FIFO_DIFF_SPK [27:24], DATA_MODE 4, IRQ_MODE 3, MODE 2 (1: master,
 * CBS_CFS), MIC_ENABLE 1, SPK_ENABLE 0. Playback starts with SPK_ENABLE = 1 (the trigger) and stops with it 0 (shutdown) */
#define UAIF_MODE_MASTER        (1u << 2)
#define UAIF_SPK_ENABLE         (1u << 0)
/* UAIF CTRL1: FORMAT [28:24], BCLK_POLARITY 23, WS_MODE 22 (0 I2S, 1 DSP_A), WS_POLAR 21, SLOT_MAX [20:18] (channels - 1),
 * SBIT_MAX [17:12] (width - 1), VALID_STR [11:6], VALID_END [5:0] (0 and 0 for two channels, width - 1 otherwise) */
#define UAIF_FORMAT_SHIFT       24
#define UAIF_BCLK_POLARITY      (1u << 23)
#define UAIF_WS_DSP_A           (1u << 22)
#define UAIF_WS_POLAR           (1u << 21)
#define UAIF_SLOT_MAX_SHIFT     18
#define UAIF_SBIT_MAX_SHIFT     12
/* the format code ABOX uses for a stream (abox.h abox_get_format()): channels - 1 in [2:0], width in [4:3] (1 16-bit,
 * 2 24-bit, 3 32-bit). 16-bit stereo: 0x9. UAIF CTRL1 FORMAT, SPUS MIXP_FORMAT and SPUM RECP_SRC_FORMAT take it */
#define ABOX_FORMAT(width, channels)    ((uint32_t)((channels) - 1) | ((width) == 16 ? 1u << 3 : (width) == 24 ? 2u << 3 : 3u << 3))

/*
 * RDMAn at 0x1000 + 0x100 n (RDMA0: 0x17C51000, the device tree's abox_rdma@17c51000).
 *   FACT (abox.h, abox_rdma.c, abox.c):
 *     CTRL0 0x00      ENABLE bit 0; the AP waits up to 40 ms after a stop for it to read 0 (abox_rdma_disable_barrier)
 *     CTRL1 0x04      no field is named
 *     BUF_STR 0x08, BUF_END 0x0C, BUF_OFFSET 0x10, STR_POINT 0x14   named, no field
 *     VOL_FACTOR 0x18 [23:0] a volume (DMA_VOL_FACTOR_MAX_STEPS 0xFFFFFF); RDMA3's resets to 0
 *     VOL_CHANGE 0x1C, SBACK_LIMIT 0x20   named, no field
 *     STATUS 0x30     PROGRESS bit 31; RBUF_OFFSET [28:16] in 16-byte units; RBUF_CNT [12:0] in bytes: the position the
 *                     AP reports is offset + count, the offset taken as the start of the period being read (the S10's
 *                     header gives [27:16] and [11:0] for ABOX below 2.0.1: the same for any ring under 64 KB and
 *                     periods under 4 KB, as here)
 *     the RDMA raises "buffer empty" to ABOX's GIC (IPC id RDMA0_BUF_EMPTY 0xB), the period's end as the AP takes it
 *     the firmware is told, for a stream: PCM_SET_BUFFER (the buffer's address as ABOX sees it, the period's bytes, the
 *     number of periods), PCM_PLTDAI_HW_PARAMS (rate, bit depth, channels), PCM_PLTDAI_TRIGGER (1/0), and answers
 *     with the position as an address (PCM_PLTDAI_POINTER)
 *   FACT from the S10's ABOX driver (abox_soc.h, as above; the field has no version condition there): CTRL0 BURST_LEN
 *   [22:19], which the AP sets before each stream (abox_rdma.c hw_params: 8, or 1 with a sample bank under 128 bytes).
 *   The 9810's AP does not write it; the driver sets 8 when it reads 0 (abox_burst=).
 *   INFERRED (from the names and those messages; see the driver's head): BUF_STR the first byte, BUF_END one past the
 *   last, BUF_OFFSET the period's bytes, STR_POINT where to start (= BUF_STR); the format (rate, width, channels) in bits
 *   of CTRL0 or CTRL1 that nothing names. A run of the firmware (a firmware mode) would settle all of it: after its
 *   PCM_PLTDAI_TRIGGER, RDMA0's registers read back are what calliope wrote.
 */
#define ABOX_RDMA(n)            (0x1000 + 0x100 * (n))
#define RDMA_CTRL0              0x00
#define RDMA_CTRL1              0x04
#define RDMA_BUF_STR            0x08
#define RDMA_BUF_END            0x0C
#define RDMA_BUF_OFFSET         0x10
#define RDMA_STR_POINT          0x14
#define RDMA_VOL_FACTOR         0x18
#define RDMA_VOL_CHANGE         0x1C
#define RDMA_SBACK_LIMIT        0x20
#define RDMA_STATUS             0x30
#define RDMA_ENABLE             (1u << 0)
/*
 * CTRL0's other fields, from the Galaxy Tab Active3's (SM-T570, the same Exynos 9810) firmware-less A-Box work, which
 * read them out of calliope's own RDMA accessors (github.com/exynos-linux/samsung-gtactive3wifi docs/re/abox.md, patch
 * 0106; read for facts only): BUF_TYPE 4, BUF_UPDATE 5 (calliope sets it with every new buffer), FORMAT [12:8] (the
 * abox_get_format() code, 0x9 for 16-bit stereo), EXPAND 13 (16-bit mono to stereo), FUNC [18:16] (0 run, 1 pause).
 * SBANK_LIMIT (0x20) [13:7]: the sample bank's size in 128-byte units, as calliope sets it. Not run there on hardware
 * either: what the S9 does with them is in AUDIO.md.
 */
#define RDMA_BUF_TYPE           (1u << 4)
#define RDMA_BUF_UPDATE         (1u << 5)
#define RDMA_FORMAT_SHIFT       8
#define RDMA_FORMAT_MASK        (0x1Fu << RDMA_FORMAT_SHIFT)
#define RDMA_EXPAND             (1u << 13)
#define RDMA_FUNC_MASK          (7u << 16)
#define RDMA_SBANK_LIMIT_SHIFT  7
#define RDMA_BURST_SHIFT        19
#define RDMA_BURST_MASK         (0xFu << RDMA_BURST_SHIFT)
#define RDMA_VOL_MASK           0xFFFFFFu
#define RDMA_PROGRESS           (1u << 31)
#define RDMA_RBUF_OFFSET(s)     ((((s) >> 16) & 0x1FFFu) << 4)
#define RDMA_RBUF_CNT(s)        ((s) & 0x1FFFu)

#endif /* _SYS_HAKIOS_EXYNOS_ABOX_H_ */
