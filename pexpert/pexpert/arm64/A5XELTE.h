/* Samsung Galaxy A5 (2016), SM-A510F: Exynos 7580, Cortex-A53. */
#ifndef _PEXPERT_ARM64_A5XELTE_H
#define _PEXPERT_ARM64_A5XELTE_H

#define NO_MONITOR                    1
#define NO_ECORE                      1

#define A5XELTE                       1
/* Reuse donor's SoC-neutral early services until their guards are renamed. */
#define SWEET                         1

/* Firmware scanout buffer and the earliest XNU marker (physical). */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* Galaxy S9 SM-G960F (Exynos 9810): reuses the HEROLTE port. S-Boot leaves
 * DECON_F (0x16030000) scanning out this buffer (uniLoader board-starlte.c,
 * s3cfb.bootloaderfb in the stock cmdline); the shim/starlte probe drew on it. */
#define HEROLTE                       1
#define STARLTE                       1
/* Boot CPU: Cortex-A55 "ananke", MPIDR 0x81000000 (Aff1|Aff0 = 0). Only CPU0
 * is described until the other cores' MPIDRs are measured. */
#define SWEET_SMP_MAX_CPUS            8
#define A5XELTE_FB_PA                 0xcc000000
#define A5XELTE_FB_WIDTH              1440
#define A5XELTE_FB_HEIGHT             2960
#define A5XELTE_FB_STRIDE             5760
#define A5XELTE_FB_BLOCKS             9     /* 2 MiB VA=PA bootstrap blocks (16.3 MiB) */
#define A5XELTE_FB_TEXT_SCALE         3
#define A5XELTE_FB_DOT_PITCH          458   /* iPhone X userland density */
/* The boot console runs between the Project Starbite header and the
 * card slot the loader draws (tools/brand/overlay.mjs HEADER_H,
 * BAND_TOP), so both stay on screen through the kernel boot. */
#define A5XELTE_CONSOLE_TOP           640
#define A5XELTE_CONSOLE_HEIGHT        1880
/* Log page below the XNU window 0x92000000; the vendor logs end at 0x9179c000. */
#define A5XELTE_MARKER_PA             0x91fff100
#define A5XELTE_BOOT_MPIDR            0x0
#elif defined(ARM64_BOARD_CONFIG_HEROLTE)
/* Galaxy S7 SM-G930F: S-Boot leaves DECON_F scanning out this buffer. */
#define HEROLTE                       1
/* Logical 0..3: Cortex-A53 "Apollo" MPIDR 0x100..0x103 (boot cluster);
 * 4..7: Mongoose MPIDR 0x0..0x3. All started with PSCI CPU_ON. */
#define SWEET_SMP_MAX_CPUS            8
#define A5XELTE_FB_PA                 0xe2a00000
#define A5XELTE_FB_WIDTH              1440
#define A5XELTE_FB_HEIGHT             2560
#define A5XELTE_FB_STRIDE             5760
#define A5XELTE_FB_BLOCKS             8     /* 2 MiB VA=PA bootstrap blocks */
#define A5XELTE_FB_TEXT_SCALE         3
#define A5XELTE_FB_DOT_PITCH          458   /* iPhone X userland density */
#define A5XELTE_MARKER_PA             0x8ffff100
/* S-Boot starts on the Cortex-A53 "Apollo" cluster, Aff1 = 1. */
#define A5XELTE_BOOT_MPIDR            0x100
#else
#define SWEET_SMP_MAX_CPUS            1
#define A5XELTE_FB_PA                 0x67000000
#define A5XELTE_FB_WIDTH              1080
#define A5XELTE_FB_HEIGHT             1920
#define A5XELTE_FB_STRIDE             4320
#define A5XELTE_FB_BLOCKS             5
#define A5XELTE_FB_TEXT_SCALE         2
#define A5XELTE_FB_DOT_PITCH          424   /* 1080 pixels across 5.2 inches */
#define A5XELTE_MARKER_PA             0x469ff100
#define A5XELTE_BOOT_MPIDR            0x0
#endif

#ifndef A5XELTE_CONSOLE_HEIGHT
#define A5XELTE_CONSOLE_HEIGHT        A5XELTE_FB_HEIGHT
#endif
#ifndef A5XELTE_CONSOLE_TOP
#define A5XELTE_CONSOLE_TOP           0
#endif
/* Physical start of the console rows. */
#define A5XELTE_CONSOLE_PA            (A5XELTE_FB_PA + A5XELTE_CONSOLE_TOP * A5XELTE_FB_STRIDE)

#define ARM_ARCH_TIMER
#define __ARM_ARCH__                  8
#define __ARM_VMSA__                  8
#define __ARM_VFP__                   4
#define __ARM_COHERENT_CACHE__        1
#define __ARM_DEBUG__                 7
#define __ARM_ENABLE_SWAP__           1
#define __ARM_V8_CRYPTO_EXTENSIONS__  1
#define __ARM_PAN_AVAILABLE__         0

/* Exynos7580 has a GICv2 distributor and CPU interface, not GICv3. */
#define HAS_GIC_V3                    0
#define A5XELTE_VTIMER_INTID         27

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif

#endif /* _PEXPERT_ARM64_A5XELTE_H */
