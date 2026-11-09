/*
 * Scion: stand-in for arm64/ppl/uat.h, which Apple's open-source XNU includes but does not ship. The UAT (the unified
 * address translator) is the page table format of Apple's own GPU; a machine without that GPU needs nothing from it,
 * so bootstrap is a no-op. One copy for every board of this tree (VMAPPLE and the generic ARMv8 boards).
 */
#ifndef _ARM64_PPL_UAT_H_
#define _ARM64_PPL_UAT_H_

static inline void
uat_bootstrap(void)
{
}

#endif /* _ARM64_PPL_UAT_H_ */
