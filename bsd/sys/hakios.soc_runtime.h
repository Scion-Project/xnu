/* Made by tools/soc-runtime.py from the tables in socs/. Do not edit. */

#ifndef _SYS_HAKIOS_SOC_RUNTIME_H_
#define _SYS_HAKIOS_SOC_RUNTIME_H_

#include <sys/types.h>

struct hakios_soc {
	const char *name;
	uint32_t product_id;
	uint32_t acpm_fvmap_offset;
	uint32_t acpm_initdata;
	uint32_t acpm_sram_first;
	uint32_t acpm_sram_size;
	uint32_t apm_mbox_pa;
	uint32_t apm_sram_pa;
	uint32_t decon_pa;
	uint32_t dpp_pa;
	uint32_t dpu_dma_pa;
	uint32_t ufs_exy_sysreg_coh_bits;
	uint32_t ufs_hci_pa;
	uint32_t ufs_pma_pa;
	uint32_t ufs_pmu_phy_pa;
	uint32_t ufs_sysreg_coh_pa;
	uint32_t ufs_unipro_pa;
	uint32_t ufs_vs_pa;
};

/* The table of the chip the kernel is on, picked at first use; all zero, every block absent, when no table has its chip id */
extern const struct hakios_soc *hakios_soc_table;
const struct hakios_soc *hakios_soc_select(void);
int hakios_soc_is_9810(void);

static inline const struct hakios_soc *
hakios_soc(void)
{
	return __builtin_expect(hakios_soc_table != 0, 1) ? hakios_soc_table : hakios_soc_select();
}

#define HAKIOS_SOC_HAS(name)	(EXYNOS_##name != 0)

#define EXYNOS_ACPM_FVMAP_OFFSET	(hakios_soc()->acpm_fvmap_offset)
#define EXYNOS_ACPM_INITDATA	(hakios_soc()->acpm_initdata)
#define EXYNOS_ACPM_SRAM_FIRST	(hakios_soc()->acpm_sram_first)
#define EXYNOS_ACPM_SRAM_SIZE	(hakios_soc()->acpm_sram_size)
#define EXYNOS_APM_MBOX_PA	(hakios_soc()->apm_mbox_pa)
#define EXYNOS_APM_SRAM_PA	(hakios_soc()->apm_sram_pa)
#define EXYNOS_DECON_PA	(hakios_soc()->decon_pa)
#define EXYNOS_DPP_PA	(hakios_soc()->dpp_pa)
#define EXYNOS_DPU_DMA_PA	(hakios_soc()->dpu_dma_pa)
#define EXYNOS_UFS_EXY_SYSREG_COH_BITS	(hakios_soc()->ufs_exy_sysreg_coh_bits)
#define EXYNOS_UFS_HCI_PA	(hakios_soc()->ufs_hci_pa)
#define EXYNOS_UFS_PMA_PA	(hakios_soc()->ufs_pma_pa)
#define EXYNOS_UFS_PMU_PHY_PA	(hakios_soc()->ufs_pmu_phy_pa)
#define EXYNOS_UFS_SYSREG_COH_PA	(hakios_soc()->ufs_sysreg_coh_pa)
#define EXYNOS_UFS_UNIPRO_PA	(hakios_soc()->ufs_unipro_pa)
#define EXYNOS_UFS_VS_PA	(hakios_soc()->ufs_vs_pa)

#define EXYNOS_M3_PMU_CL1_CPU_CONFIG(n)         (0x2000 + 8 * (n))
#define EXYNOS_M3_PMU_CL1_CPU_STATUS(n)         (0x2004 + 8 * (n))
#define EXYNOS_M3_PMU_CL0_CPU_STATUS(n)         (0x2024 + 8 * (n))
#define EXYNOS_SCION_GOV { \
	.big_limit = { { 2, 2314000 }, { 4, 1794000 } }, \
	.big_floor = 650000, .big_touch = 1469000, .big_frame = 1261000, .big_safe = 1261000, \
	.lit_floor = 1053000, .lit_touch = 1248000, .lit_safe = 1248000, \
	.heavy_pct = 62, .idle_pct = 11, .lit_heavy_pct = 20, \
	.change_ms = 100, \
	.park_under_mc = 70000, .unpark_at_mc = 78000, \
}

#endif /* _SYS_HAKIOS_SOC_RUNTIME_H_ */
