/* included by start.s after LowExceptionVectorBase: the early-exception handler that reports to the loader, the loader's stub slot, and the vectors of a core PSCI has just started */
/*
 * Early-boot exceptions used to spin here. When the XNU loader has patched
 * the address of its return stub into the slot below, report the exception
 * to it instead (see osfmk/arm64/loader_return.c):
 *   x0/x1 = PA/length of the captured kernel log, x2 = 1 (early exception),
 *   x3 = ESR, x4 = ELR, x5 = FAR, x6 = LR, x7 = SP
 * This vector is only installed while VBAR holds a physical address, so
 * PC-relative addressing yields physical addresses, MMU on (V=P) or off.
 */
	.align	3
Lloader_early_exception:
	mov		x6, lr
	mov		x7, sp
	mrs		x3, ESR_EL1
	mrs		x4, ELR_EL1
	mrs		x5, FAR_EL1
	adrp	x0, EXT(loader_log)@page
	add		x0, x0, EXT(loader_log)@pageoff
	adrp	x1, EXT(loader_log_len)@page
	add		x1, x1, EXT(loader_log_len)@pageoff
	ldr		w1, [x1]
	// Write the log back to RAM: cached copies are lost once the MMU is off
	mov		x9, x0
	add		x10, x0, x1
1:
	dc		cvac, x9
	add		x9, x9, #64
	cmp		x9, x10
	b.lo	1b
	dsb		sy
	mov		x2, #1
	mrs		x9, SCTLR_EL1
	bic		x9, x9, #1
	msr		SCTLR_EL1, x9
	isb
	adr		x9, Lloader_stub_slot
	ldr		x9, [x9, #8]
	cbz		x9, .
	br		x9
	.align	3
	.globl EXT(loader_stub_slot)
LEXT(loader_stub_slot)
Lloader_stub_slot:
	.ascii	"XNULDRST"				// magic the loader searches for
	.quad	0						// PA of the loader's return stub
	.quad	0						// checkpoint number to stop at (0 = none)
	.quad	0						// (physical - virtual) offset of the static kernel image

#if defined(GENERIC_PSCI_SMP)
/*
 * The same for a core other than the boot processor, from its first
 * instruction (reset_vector) until common_start gives it the kernel's
 * vectors. The boot loader is not this core's to return to; it leaves a note
 * of what happened where the processor that is waiting for it looks
 * (psci_smp_start_cpu()) and switches itself off again.
 *
 * VBAR holds a physical address here too, so the note is addressed
 * physically; the store goes straight to memory when the MMU is still off,
 * and is sent there when it is not.
 */
	.align	11
	.globl EXT(LowSecondaryExceptionVectorBase)
LEXT(LowSecondaryExceptionVectorBase)
	/* EL1 SP 0 */
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	/* EL1 SP1 */
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	/* EL0 64 */
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	/* EL0 32 */
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception
	.align	7
	b		Lsecondary_early_exception

	.align	3
Lsecondary_early_exception:
	// struct psci_smp_start_fault (psci_smp.c, patch 1509: two cache lines): { valid, MPIDR, ESR, ELR,
	// FAR, LR, SP, SPSR, TPIDR_EL1, TTBR0, TTBR1, TCR, SCTLR, VBAR, x21 (start.s: the cpu data's PA), source }
	adrp	x9, EXT(psci_smp_start_fault)@page
	add		x9, x9, EXT(psci_smp_start_fault)@pageoff
	mrs		x10, MPIDR_EL1
	str		x10, [x9, #8]
	mrs		x10, ESR_EL1
	str		x10, [x9, #16]
	mrs		x10, ELR_EL1
	str		x10, [x9, #24]
	mrs		x10, FAR_EL1
	str		x10, [x9, #32]
	str		lr, [x9, #40]
	mov		x10, sp
	str		x10, [x9, #48]
	mrs		x10, SPSR_EL1
	str		x10, [x9, #56]
	mrs		x10, TPIDR_EL1
	str		x10, [x9, #64]
	mrs		x10, TTBR0_EL1
	str		x10, [x9, #72]
	mrs		x10, TTBR1_EL1
	str		x10, [x9, #80]
	mrs		x10, TCR_EL1
	str		x10, [x9, #88]
	mrs		x10, SCTLR_EL1
	str		x10, [x9, #96]
	mrs		x10, VBAR_EL1
	str		x10, [x9, #104]
	str		x21, [x9, #112]
	mov		x10, #1								// source 1: the early vectors
	str		x10, [x9, #120]
	add		x10, x9, #64
	dc		civac, x10							// the second line first: valid is in the first
	dsb		sy
	mov		x10, #1
	str		x10, [x9]
	dc		civac, x9
	dsb		sy
	// m3_diag=1: park here (an M3 does not come back from CPU_OFF, and Carbon leaves VBAR_EL2 unset on a
	// secondary core, so neither HVC nor CPU_OFF is safe on the core being diagnosed)
	adrp	x10, EXT(psci_smp_diag)@page
	add		x10, x10, EXT(psci_smp_diag)@pageoff
	ldr		w10, [x10]
	cbz		w10, Lsecondary_early_off
	msr		DAIFSet, #(DAIFSC_ALL)
Lsecondary_early_park:
	wfe
	b		Lsecondary_early_park
Lsecondary_early_off:
	// PSCI CPU_OFF, by the firmware's calling convention (psci_smp_hvc)
	adrp	x9, EXT(psci_smp_hvc)@page
	add		x9, x9, EXT(psci_smp_hvc)@pageoff
	ldr		w9, [x9]
	mov		x0, #0x0002
	movk	x0, #0x8400, lsl #16
	cbnz	w9, 1f
	smc		#0
	b		2f
1:
	hvc		#0
2:
	wfi											// it does not return; if it does, stay here
	b		2b
#endif /* defined(GENERIC_PSCI_SMP) */
