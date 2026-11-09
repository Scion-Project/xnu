/*
 * Copyright (c) 2026 Scion contributors. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * Scion: __chkstk_darwin with a BTI landing pad. The kernel calls it through
 * an authenticated pointer (blraa), and the copy in Xcode's libcc_kext.a has no
 * "bti c", so a kernel with BTI enforced (VMAPPLE) faults on the first call.
 * Same contract as the compiler's: x9 = bytes about to be allocated below sp;
 * touches each 4 KB page of that range from the top down; clobbers x9, x10.
 * Only with SCION_NATIVE_PAC (a kernel for a VM without Apple's hypervisor);
 * otherwise this file is empty and the compiler's copy is linked as before.
 */
#if SCION_NATIVE_PAC
	.text
	.align 2
	.globl ___chkstk_darwin
___chkstk_darwin:
	bti		c
	cmp		x9, #0x1, lsl #12
	mov		x10, sp
	b.lo	2f
1:
	sub		x10, x10, #0x1, lsl #12
	ldrb	wzr, [x10]
	sub		x9, x9, #0x1, lsl #12
	cmp		x9, #0x1, lsl #12
	b.hi	1b
2:
	sub		x10, x10, x9
	ldrb	wzr, [x10]
	ret
#endif /* SCION_NATIVE_PAC */
