/*
 * Copyright (c) 2026 the Scion project.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
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
 * Scion: the functions of the arm64 machine layer that Apple's kernel takes from the binary support library of its KDK
 * (lib<machine>.os.<config>.a) and that a build from the open source has nowhere else, for a machine with no monitor (PPL)
 * and no fuses to read.
 */

#include <mach/boolean.h>
#include <arm/machine_routines.h>
#include <arm/pmap.h>

/* the page-table monitor (PPL) is entered here on Apple's machines; there is none */
void
ml_enable_monitor(void)
{
}

/* a production part, not an engineering sample: nothing of the kernel's that tests this is turned on by the answer */
boolean_t
ml_device_is_prod_fused(void)
{
	return TRUE;
}

#if SCHED_HYGIENE_DEBUG && (DEBUG || DEVELOPMENT)
/* pmap.h: the long pmap loops ask this to stop for an urgent AST; the plain test, as a kernel without SCHED_HYGIENE_DEBUG has */
bool
pmap_pending_preemption(void)
{
	return _pmap_pending_preemption_real();
}
#endif /* SCHED_HYGIENE_DEBUG && (DEBUG || DEVELOPMENT) */
