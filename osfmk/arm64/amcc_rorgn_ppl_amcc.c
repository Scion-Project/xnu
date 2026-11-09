/*
 * sweet: (11417 splits the PPL part into _amcc / _ctrr3) files.arm64 builds this for nos_arm_asm, but the source only ships
 * in Apple's closed per-SoC library. It implements the AMCC read-only region
 * (KTRR/CTRR kernel text lockdown), which needs Apple memory controller
 * hardware; nothing references it on SWEET (no KERNEL_INTEGRITY_KTRR/CTRR).
 */
#include <stdint.h>

static const uint8_t sweet_no_amcc_rorgn __attribute__((unused)) = 0;
