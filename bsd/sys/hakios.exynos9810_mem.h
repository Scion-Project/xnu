/*
 * The Galaxy S9's low DRAM bank as this kernel's drivers use it on the S-Boot -> Starbite shim -> XNU path (Starbite
 * iOS 27 port; the names are those of Scion's sys/hakios.exynos9810_mem.h, the values this path's).
 *
 * The display's carve-out, 0xD8000000-0xDFFFFFFF: the DECON's DMA takes 32-bit addresses, so the scan-out buffers and
 * fb_zc's surface pool are below 4 GB, outside XNU's RAM. It must not be in the extra RAM the ADT gives the kernel
 * (/chosen hakios-extra-ram; tools/adt-extra-ram.py ends that range at 0xD8000000):
 * bsd/dev/hakios.exynos_dpu.c checks, and gives no memory out if any page of it is the VM's.
 * Nothing the display scans out is at or above 0xE0000000 (Scion: a slot that ran over it showed black rows).
 */
#ifndef _SYS_HAKIOS_EXYNOS9810_MEM_H_
#define _SYS_HAKIOS_EXYNOS9810_MEM_H_

#define EXYNOS9810_MEM_DISPLAY_PA       0xD8000000ULL   /* the carve-out */
#define EXYNOS9810_MEM_DISPLAY_END      0xE0000000ULL
#define EXYNOS9810_MEM_DISPLAY_LIMIT    0xE0000000ULL   /* nothing scanned out at or above */

#define EXYNOS9810_MEM_DPU_SCAN_PA      0xD8000000ULL   /* fb_flip / fb_dpp buffers: 48 MB */
#define EXYNOS9810_MEM_DPU_SCAN_END     0xDB000000ULL
#define EXYNOS9810_MEM_DPU_POOL2_PA     0xDB000000ULL   /* fb_zc's pool: four 12 MB slots, 0xDB000000-0xDDFFFFFF */
#define EXYNOS9810_MEM_DPU_POOL2_END    0xDBC00000ULL
#define EXYNOS9810_MEM_DPU_POOL0_PA     0xDBC00000ULL
#define EXYNOS9810_MEM_DPU_POOL0_END    0xDC800000ULL
#define EXYNOS9810_MEM_DPU_POOL1_PA     0xDC800000ULL
#define EXYNOS9810_MEM_DPU_POOL1_END    0xDE000000ULL

_Static_assert(EXYNOS9810_MEM_DPU_POOL1_END <= EXYNOS9810_MEM_DISPLAY_LIMIT, "the pool below 0xE0000000");

#endif /* _SYS_HAKIOS_EXYNOS9810_MEM_H_ */
