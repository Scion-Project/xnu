/*
 * Scion: stand-in for arm64/ppl/sart.h, which Apple's open-source XNU includes but does not ship. SART is the DMA
 * address filter of Apple's own SoCs (the IOMMU of NVMe/ANS); a machine without one (a VM, a phone that is not
 * Apple's, e.g. Qualcomm or Samsung hardware) has nothing to set up, so bootstrap is a no-op.
 * One copy for every board of this tree (VMAPPLE and the generic ARMv8 boards).
 */
#ifndef _ARM64_PPL_SART_H_
#define _ARM64_PPL_SART_H_

static inline void
sart_bootstrap(void)
{
}

#endif /* _ARM64_PPL_SART_H_ */
