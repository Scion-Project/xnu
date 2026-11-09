/*
 * The Samsung Exynos 9810 (S5E9810), whatever phone it is in: where its blocks are and the SoC-specific offsets the shared
 * Exynos drivers use. Nothing but numbers. A driver includes <sys/hakios.exynos_soc.h>, which picks this file by the build's
 * CONFIG_SOC, and keeps its own name for each number, so a new SoC is a new socs/<soc>/include/hakios.<soc>_soc.h and no
 * edit of the driver unless the hardware itself differs (docs/porting/repositories.md, "A new Exynos").
 *
 * Sources: the phones' device trees and Samsung's GPL kernel; each driver names the place beside its macro. How the memory
 * outside XNU's RAM is used is in sys/hakios.exynos9810_mem.h.
 */
#ifndef _SYS_HAKIOS_EXYNOS9810_SOC_H_
#define _SYS_HAKIOS_EXYNOS9810_SOC_H_

/* The SoC, and the power processor */
#define EXYNOS_APM_SRAM_PA                      0x02039000UL
#define EXYNOS_SOC_BASE_PA                      0x10000000UL
#define EXYNOS_CHIPID_PA                        0x10000000UL
#define EXYNOS_PRODUCT_ID                       0xE9810000u
#define EXYNOS_PMU_PA                           0x14060000UL
#define EXYNOS_APM_MBOX_PA                      0x14100000UL

/* Clock controllers (CMU) */
#define EXYNOS_CMU_PERIS_PA                     0x10020000UL
#define EXYNOS_CMU_PERIC0_PA                    0x10400000UL
#define EXYNOS_CMU_PERIC1_PA                    0x10800000UL
#define EXYNOS_CMU_FSYS0_PA                     0x11000000UL
#define EXYNOS_CMU_FSYS1_PA                     0x11400000UL
#define EXYNOS_CMU_CHUB_PA                      0x13A00000UL
#define EXYNOS_CMU_CMGP_PA                      0x14200000UL
#define EXYNOS_CMU_ISPPRE_PA                    0x16200000UL
#define EXYNOS_CMU_G3D_PA                       0x17400000UL
#define EXYNOS_CMU_MFC_PA                       0x17800000UL
#define EXYNOS_CMU_AUD_PA                       0x17C00000UL
#define EXYNOS_CMU_BUSC_PA                      0x1A200000UL
#define EXYNOS_CMU_TOP_PA                       0x1A240000UL
#define EXYNOS_CMU_TOP_PAGE2_PA                 0x1A242000UL

/* GPIO */
#define EXYNOS_GPIO_PERIC0_PA                   0x10430000UL
#define EXYNOS_GPIO_PERIC1_PA                   0x10830000UL
#define EXYNOS_GPIO_FSYS1_PA                    0x11430000UL
#define EXYNOS_GPIO_CHUB_PA                     0x13A80000UL
#define EXYNOS_GPIO_ALIVE_PA                    0x14050000UL
#define EXYNOS_GPIO_AUD_PA                      0x17C60000UL

/* System registers */
#define EXYNOS_SYSREG_PERIC0_PA                 0x10410000UL
#define EXYNOS_SYSREG_FSYS1_PA                  0x11410000UL
#define EXYNOS_SYSREG_FSYS1_PAGE2_PA            0x11411000UL
#define EXYNOS_SYSREG_ISPPRE_PA                 0x16210000UL

/* USB */
#define EXYNOS_USB_DWC3_PA                      0x10C00000UL
#define EXYNOS_USB_CON_PA                       0x11100000UL

/* Thermal */
#define EXYNOS_TMU_TOP_PA                       0x10070000UL
#define EXYNOS_TMU_SUB_PA                       0x10074000UL

/* UFS */
#define EXYNOS_UFS_SYSREG_COH_PA                0x11010700UL
#define EXYNOS_UFS_UNIPRO_PA                    0x11110000UL
#define EXYNOS_UFS_HCI_PA                       0x11120000UL
#define EXYNOS_UFS_VS_PA                        0x11121100UL
#define EXYNOS_UFS_PMA_PA                       0x11124000UL
#define EXYNOS_UFS_PMU_PHY_PA                   0x14060724UL

/* Display */
#define EXYNOS_DPP_PA                           0x16020000UL
#define EXYNOS_DECON_PA                         0x16030000UL
#define EXYNOS_DPU_DMA_PA                       0x16070000UL
#define EXYNOS_DSIM0_PA                         0x16080000UL

/* GPU */
#define EXYNOS_MALI_PA                          0x17500000UL

/* Video */
#define EXYNOS_MFC_SYSMMU0_PA                   0x17880000UL
#define EXYNOS_MFC_SYSMMU1_PA                   0x178A0000UL
#define EXYNOS_MFC_PA                           0x178D0000UL

/* PCIe (Wi-Fi) */
#define EXYNOS_PCIE_SYSMMU_PA                   0x11420000UL
#define EXYNOS_PCIE_ELBI_PA                     0x116A0000UL
#define EXYNOS_PCIE_PCS_PA                      0x116C0000UL
#define EXYNOS_PCIE_PHY_PA                      0x116D0000UL
#define EXYNOS_PCIE_DBI_PA                      0x11700000UL
#define EXYNOS_PCIE_MEM_PA                      0x11800000UL
#define EXYNOS_PCIE_CFG_PA                      0x127FE000UL

/* Camera: CSIS and its DMA */
#define EXYNOS_CSIS1_PHY_PA                     0x16180000UL
#define EXYNOS_CSIS3_PHY_PA                     0x16190000UL
#define EXYNOS_CSIS1_PA                         0x16240000UL
#define EXYNOS_CSIS3_PA                         0x16260000UL
#define EXYNOS_CSIS1_WDMA_PA                    0x162C0000UL
#define EXYNOS_CSIS3_WDMA_PA                    0x162E0000UL
#define EXYNOS_CSIS_DMA_PA                      0x162F0000UL
#define EXYNOS_CSIS_SYSMMU_PA                   0x16310000UL

/* Audio: ABOX */
#define EXYNOS_ABOX_SYSREG_PA                   0x17C10000UL
#define EXYNOS_ABOX_SFR_PA                      0x17C50000UL
#define EXYNOS_ABOX_GICD_PA                     0x17CF1000UL
#define EXYNOS_ABOX_SRAM_PA                     0x17D00000UL
#define EXYNOS_ABOX_SYSMMU_PA                   0x17E00000UL

/* USI blocks: each one's I2C or SPI and its mode word; which device is on which is the driver's */
#define EXYNOS_PERIC0_USI01_MODE_PA             0x1041100CUL
#define EXYNOS_PERIC0_USI03_MODE_PA             0x1041101CUL
#define EXYNOS_PERIC0_USI03_HSI2C_PA            0x104B0000UL
#define EXYNOS_PERIC0_USI00_SPI_PA              0x10450000UL
#define EXYNOS_PERIC0_USI01_SPI_PA              0x10470000UL
#define EXYNOS_PERIC1_USI07_MODE_PA             0x10811024UL
#define EXYNOS_PERIC1_USI08_MODE_PA             0x1081102CUL
#define EXYNOS_PERIC1_USI10_MODE_PA             0x1081103CUL
#define EXYNOS_PERIC1_USI07_HSI2C0_PA           0x108C0000UL
#define EXYNOS_PERIC1_USI07_HSI2C1_PA           0x108D0000UL
#define EXYNOS_PERIC1_USI08_HSI2C1_PA           0x108F0000UL
#define EXYNOS_PERIC1_USI10_HSI2C0_PA           0x10920000UL
#define EXYNOS_PERIC1_USI10_HSI2C1_PA           0x10930000UL
#define EXYNOS_CHUB_USI00_MODE_PA               0x13A13000UL
#define EXYNOS_CHUB_USI00_SPI_PA                0x13AC0000UL
#define EXYNOS_CMGP_USI03_MODE_PA               0x14212030UL
#define EXYNOS_CMGP_USI03_HSI2C_PA              0x14360000UL

/* Other peripherals */
#define EXYNOS_PWM_PA                           0x10510000UL
#define EXYNOS_UART_BT_PA                       0x10840000UL
#define EXYNOS_ADC_PA                           0x14230000UL

/* USB's glue (hakios.exynos_usb.c): PMU isolation, the clocks' offsets, Android's dividers */
#define EXYNOS_USB_PMU_USB_PHY                  0x072Cu
#define EXYNOS_USB_PMU_USB_DP_PHY               0x0704u
#define EXYNOS_USB_TOP_MUX_USB30DRD             0x1060u
#define EXYNOS_USB_TOP_DIV_USB30DRD             0x1858u
#define EXYNOS_USB_TOP_GATE_USB30DRD            0x2064u
#define EXYNOS_USB_FSYS0_USER_MUX               0x01E0u
#define EXYNOS_USB_FSYS0_REFCLK_GATE            0x206Cu
#define EXYNOS_USB_FSYS0_QCH_CTRL               0x304Cu
#define EXYNOS_USB_FSYS0_QCH_LINK               0x3050u
#define EXYNOS_USB_FSYS0_QCH_DPPHY              0x3054u
#define EXYNOS_USB_FSYS0_QCH_PCS                0x3058u
#define EXYNOS_USB_ANDROID_TOP_MUX              1u
#define EXYNOS_USB_ANDROID_TOP_DIV              7u

/* poweroff */
#define EXYNOS_POWEROFF_PMU_PS_HOLD_CONTROL     0x330Cu

/* the keys' GPIO banks */
#define EXYNOS_KEYS_BANK_GPA0                   0x020
#define EXYNOS_KEYS_BANK_GPA2                   0x060

/* ACPM's SRAM */
#define EXYNOS_ACPM_SRAM_SIZE                   0x30000
#define EXYNOS_ACPM_SRAM_FIRST                  0x3000
#define EXYNOS_ACPM_INITDATA                    0x7F00
#define EXYNOS_ACPM_FVMAP_OFFSET                0x6700

/* the big cluster's PMU fields (hakios.exynos_m3.c) */
#define EXYNOS_M3_PMU_RESET_DIS_WDT             0x0414
#define EXYNOS_M3_PMU_RESET_DIS_SOFT            0x041C
#define EXYNOS_M3_PMU_RESET_SEQ_CONFIG          0x0500
#define EXYNOS_M3_PMU_CL1_CPU_CONFIG(n)         (0x2000 + 8 * (n))
#define EXYNOS_M3_PMU_CL1_CPU_STATUS(n)         (0x2004 + 8 * (n))
#define EXYNOS_M3_PMU_CL0_CPU_STATUS(n)         (0x2024 + 8 * (n))
#define EXYNOS_M3_PMU_CL1_NONCPU_CONFIG         0x2040
#define EXYNOS_M3_PMU_CL1_NONCPU_STATUS         0x2044
#define EXYNOS_M3_PMU_CL0_NONCPU_STATUS         0x204C
#define EXYNOS_M3_PMU_CL1_L2_STATUS             0x2380
#define EXYNOS_M3_ACPM_DVFS_CPUCL0              2u
#define EXYNOS_M3_ACPM_DVFS_CPUCL1              3u

/* the TMU's gate */
#define EXYNOS_TMU_CMU_PERIS_TMU_GATE           0x2014u
#define EXYNOS_TMU_CMU_PERIS_TMU_QCH            0x3004u

/* the secure OS's notification interrupt */
#define EXYNOS_TEE_SSIQ_GIC_NUMBER              (233u + 32)

/* the video decoder's power, clocks, interrupt */
#define EXYNOS_MFC_PMU_CONFIG                   0x4068
#define EXYNOS_MFC_PMU_STATUS                   0x406C
#define EXYNOS_MFC_TOP_MUX                      0x109C
#define EXYNOS_MFC_TOP_DIV                      0x1898
#define EXYNOS_MFC_TOP_GATE                     0x20A4
#define EXYNOS_MFC_MFC_QCH_FIRST                0x300C
#define EXYNOS_MFC_MFC_QCH_LAST                 0x3058
#define EXYNOS_MFC_DTZPC_MFC                    0x17810204u
#define EXYNOS_MFC_MFC_INTID                    (356 + 32)

/* the front camera's CSIS: power, clocks */
#define EXYNOS_CAM_CIS1_MUX                     0x101C
#define EXYNOS_CAM_CIS1_DIV                     0x1818
#define EXYNOS_CAM_CIS1_GATE                    0x201C
#define EXYNOS_CAM_CIS1_QCH                     0x3008
#define EXYNOS_CAM_PMU_DPHY_ISO_CSIS1           0x0730
#define EXYNOS_CAM_PMU_DPU_STATUS               0x4024
#define EXYNOS_CAM_PMU_ISPPRE_CONFIG            0x4048
#define EXYNOS_CAM_PMU_ISPPRE_STATUS            0x404C
#define EXYNOS_CAM_PMU_ISPPRE_MEMORY            0x4730
#define EXYNOS_CAM_DTZPC_ISPPRE                 0x16210004u
#define EXYNOS_CAM_QCH_CSIS1                    0x3024
#define EXYNOS_CAM_QCH_SYSMMU                   0x305C
#define EXYNOS_CAM_TOP_ISPPRE_MUX               0x1094
#define EXYNOS_CAM_TOP_ISPPRE_DIV               0x1890
#define EXYNOS_CAM_TOP_ISPPRE_GATE              0x209C
#define EXYNOS_CAM_QCH_FIRST                    0x3010
#define EXYNOS_CAM_QCH_LAST                     0x3070
#define EXYNOS_CAM_QCH_DBG                      0x4000
#define EXYNOS_CAM_SYSREG_DRCG_EN               0x0104

/* the iris camera's CSIS: power, clocks */
#define EXYNOS_IRIS_CIS2_MUX                    0x1020
#define EXYNOS_IRIS_CIS2_DIV                    0x181C
#define EXYNOS_IRIS_CIS2_GATE                   0x2020
#define EXYNOS_IRIS_CIS2_QCH                    0x300C
#define EXYNOS_IRIS_PMU_DPHY_ISO                0x0730
#define EXYNOS_IRIS_PMU_DPU_STATUS              0x4024
#define EXYNOS_IRIS_PMU_ISPPRE_CONFIG           0x4048
#define EXYNOS_IRIS_PMU_ISPPRE_STATUS           0x404C
#define EXYNOS_IRIS_PMU_ISPPRE_MEMORY           0x4730
#define EXYNOS_IRIS_DTZPC_ISPPRE                0x16210004u
#define EXYNOS_IRIS_QCH_CSIS3                   0x302C
#define EXYNOS_IRIS_TOP_ISPPRE_MUX              0x1094
#define EXYNOS_IRIS_TOP_ISPPRE_DIV              0x1890
#define EXYNOS_IRIS_TOP_ISPPRE_GATE             0x209C

/* ABOX: power and clocks */
#define EXYNOS_ABOX_CMU_DIV_UAIF0               0x1824
#define EXYNOS_ABOX_CMU_MUX_UAIF0               0x1004
#define EXYNOS_ABOX_CMU_QCH_BCLK0               0x3028
#define EXYNOS_ABOX_GPIO_GPB0                   0x0000
#define EXYNOS_ABOX_ABOX_DTZPC_ARG              0x17C10204u
#define EXYNOS_ABOX_PMU_AUD_CONFIGURATION       0x4000
#define EXYNOS_ABOX_PMU_AUD_STATUS              0x4004
#define EXYNOS_ABOX_PMU_GPIO_MODE_ABOX          0x1424
#define EXYNOS_ABOX_PMU_PAD_RETENTION_ABOX      0x4170
#define EXYNOS_ABOX_PMU_ABOX_CPU_CONFIG         0x415C
#define EXYNOS_ABOX_PMU_ABOX_CPU_STATUS         0x4160
#define EXYNOS_ABOX_PMU_ABOX_CPU_OPTION         0x4164
#define EXYNOS_ABOX_PMU_ABOX_CPU_ENABLE         (1u << 28)
#define EXYNOS_ABOX_PMU_ABOX_CPU_STANDBY        0x3804
#define EXYNOS_ABOX_CMU_PLL_LOCKTIME            0x0000
#define EXYNOS_ABOX_CMU_PLL_CON0                0x0120
#define EXYNOS_ABOX_CMU_PLL_CON3                0x018C
#define EXYNOS_ABOX_CMU_DIV_AUDIF               0x1800
#define EXYNOS_ABOX_CMU_DIV_BUS                 0x1804
#define EXYNOS_ABOX_CMU_DIV_BUSP                0x1808
#define EXYNOS_ABOX_CMU_DIV_PLL                 0x1820
#define EXYNOS_ABOX_CMU_DIV_UAIF1               0x1828
#define EXYNOS_ABOX_CMU_MUX_CPU                 0x1000
#define EXYNOS_ABOX_CMU_MUX_UAIF1               0x1008
#define EXYNOS_ABOX_CMU_DIV_BUSY                (1u << 16)

/* PCIe's PMU fields */
#define EXYNOS_PCIE_PMU_PCIE_PHY_CONTROL        0x71C
#define EXYNOS_PCIE_PMU_WAKEUP_MASK             0x610
#define EXYNOS_PCIE_GATE_SYSMMU                 0x2098

/* UFS's coherency bits */
#define EXYNOS_UFS_EXY_SYSREG_COH_BITS          0x300u

/* Mali's DTZPC */
#define EXYNOS_MALI_DTZPC_G3D                   0x17410204u

/*
 * The scion governor's limits (struct scion_soc, sys/hakios.scion_gov.h). Samsung's device tree for the S9
 * (exynos9810-starlte_eur_open_26.dts): hotplug_governor's dual_freq and quad_freq (single_freq is dual_freq, triple_freq
 * quad_freq), big_heavy_thr 640, big_idle_thr 113 and lit_heavy_thr 200 of 1024, dual_change_ms; cpufreq's min-freq. Measured:
 * the floors, 1261000 and 1469000 from the replay of usb-1104.log; the A55s' 1248000 because the interface does not run on
 * them during a touch (the busiest 2% at the median second); the safe rates, the frame floor, what the phone spent 71% of a
 * day of use below. The temperatures (phone 2, 5 November): 2314000 on two M3s went from 46 C to 92 C in seconds, and 1794000
 * on two sits at 78 to 83 C once it has run a while, so a burst ends at 78 C and only under 70 C can a new one start.
 */
#define EXYNOS_SCION_GOV { \
	.big_limit = { { 2, 2314000 }, { 4, 1794000 } }, \
	.big_floor = 650000, .big_touch = 1469000, .big_frame = 1261000, .big_safe = 1261000, \
	.lit_floor = 1053000, .lit_touch = 1248000, .lit_safe = 1248000, \
	.heavy_pct = 62, .idle_pct = 11, .lit_heavy_pct = 20, \
	.change_ms = 100, \
	.park_under_mc = 70000, .unpark_at_mc = 78000, \
}

#endif /* _SYS_HAKIOS_EXYNOS9810_SOC_H_ */
