/* included by start.s: the LOADER_CHECKPOINT macro of start_first_cpu */
/*
 * LOADER_CHECKPOINT n
 *   Bring-up aid: if the loader asked to stop at checkpoint n, return to it
 *   right here (reason 2, code n). Usable while executing at physical
 *   addresses, MMU off or on (V=P). Clobbers x0-x3, x9-x11.
 */
.macro LOADER_CHECKPOINT
	adrp	x9, EXT(loader_stub_slot)@page
	add		x9, x9, EXT(loader_stub_slot)@pageoff
	ldr		x10, [x9, #16]
	cmp		x10, #$0
	b.ne	1f
	mov		x0, #0
	mov		x1, #0
	mov		x2, #2
	mov		x3, #$0
	mrs		x11, SCTLR_EL1
	bic		x11, x11, #1
	msr		SCTLR_EL1, x11
	isb
	ldr		x9, [x9, #8]
	br		x9
1:
.endmacro
