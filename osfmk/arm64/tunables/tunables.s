/*
 * Scion: stand-in for arm64/tunables/tunables.s, which Apple's open-source XNU includes but does not ship. It holds
 * the per-core register settings of Apple's own CPUs (APPLY_TUNABLES, start.s); other CPUs take none.
 */
.macro APPLY_TUNABLES midr, tmp1, tmp2
.endmacro
