/*
 * sweet: FP helpers for sweet_a64e.c (FCADD and FMLAL emulation). The kernel C code is
 * built without FP; this runs one scalar FADD or FSUB with the user's FPCR.
 *   uint64_t sweet_a64e_fp_addsub(uint64_t a, uint64_t b, uint32_t op_size,
 *       uint32_t fpcr, uint32_t *fpsr)
 *   op_size: bits 3:0 size (1 = H, 2 = S, 3 = D), bit 4: 0 add, 1 sub.
 *   *fpsr |= the exception flags raised. Uses v0/v1 only (caller-saved); IRQ/FIQ masked.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <arm64/asm.h>
	.arch	armv8.2-a+fp16
	.text
	.align	2
	.globl	EXT(sweet_a64e_fp_addsub)
LEXT(sweet_a64e_fp_addsub)
	mrs	x14, DAIF
	msr	DAIFSet, #3                     /* IRQ | FIQ, as in sweet_a64e_fp_fmadd_s */
	mrs	x9, FPCR
	mrs	x10, FPSR
	mov	w3, w3
	msr	FPCR, x3
	msr	FPSR, xzr
	fmov	d0, x0
	fmov	d1, x1
	and	w11, w2, #0xf
	tbnz	w2, #4, Lsub
	cmp	w11, #1
	b.eq	Ladd_h
	cmp	w11, #2
	b.eq	Ladd_s
	fadd	d0, d0, d1
	b	Ldone
Ladd_s:	fadd	s0, s0, s1
	b	Ldone
Ladd_h:	fadd	h0, h0, h1
	b	Ldone
Lsub:	cmp	w11, #1
	b.eq	Lsub_h
	cmp	w11, #2
	b.eq	Lsub_s
	fsub	d0, d0, d1
	b	Ldone
Lsub_s:	fsub	s0, s0, s1
	b	Ldone
Lsub_h:	fsub	h0, h0, h1
Ldone:	fmov	x0, d0
	mrs	x12, FPSR
	ldr	w13, [x4]
	orr	w13, w13, w12
	str	w13, [x4]
	msr	FPCR, x9
	msr	FPSR, x10
	msr	DAIF, x14
	ret

/*
 *   uint32_t sweet_a64e_fp_fmadd_s(uint32_t a, uint32_t n, uint32_t m, uint32_t fpcr, uint32_t *fpsr)
 *   a + n * m in single precision with one rounding (FMADD) under fpcr, for FMLAL/FMLSL
 *   (sweet_a64e.c converts the half-precision operands exactly and handles NaNs and FZ16).
 *   *fpsr |= the exception flags raised. ARMv8.0 FP only, so it also runs on the Mongoose M3.
 *   IRQ/FIQ stay masked while the user's FPCR and v0-v2 are live: kernel NEON state is not kept
 *   across a context switch.
 */
	.globl	EXT(sweet_a64e_fp_fmadd_s)
LEXT(sweet_a64e_fp_fmadd_s)
	mrs	x11, DAIF
	msr	DAIFSet, #3                     /* IRQ | FIQ */
	mrs	x9, FPCR
	mrs	x10, FPSR
	mov	w3, w3                          /* the upper half of a 32-bit argument is undefined */
	msr	FPCR, x3
	msr	FPSR, xzr
	fmov	s0, w0
	fmov	s1, w1
	fmov	s2, w2
	fmadd	s0, s1, s2, s0
	fmov	w0, s0
	mrs	x12, FPSR
	ldr	w13, [x4]
	orr	w13, w13, w12
	str	w13, [x4]
	msr	FPCR, x9
	msr	FPSR, x10
	msr	DAIF, x11
	ret
#endif
