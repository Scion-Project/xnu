/*
 * Scion: stand-in for arm64/tunables/tunables.s, which Apple's open-source XNU includes but does not ship. It holds
 * the per-core register settings of Apple's own CPUs (APPLY_TUNABLES, start.s: implementation-defined HID/ACC
 * registers); other CPUs (a VM's, Cortex-A76/A55 and the like) have none of them, so APPLY_TUNABLES does nothing.
 * One copy for every board of this tree (VMAPPLE and the generic ARMv8 boards).
 */
.macro APPLY_TUNABLES midr, tmp1, tmp2
.endmacro
