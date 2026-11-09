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
 * Scion: the kernel's read-only region (KTRR, CTRR, and the paravirtualized CTRR of VMAPPLE) as amcc_rorgn.h declares
 * it, for a build from the open source, which has the header but not Apple's amcc_rorgn_*.c. This keeps no region:
 * nothing is locked, so the kernel's text and constant data stay writable by EL1 as on a machine without the hardware.
 * A machine that has a lock to set (or a hypervisor that takes the paravirtualized one) gets its own code here.
 */

#include <mach/vm_types.h>
#include <pexpert/arm64/board_config.h>
#include <arm64/amcc_rorgn.h>

#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)

/* the region's physical bounds; zero: none (zalloc.c and platform_tests.c read rorgn_begin and rorgn_end) */
vm_offset_t rorgn_begin = 0;
vm_offset_t rorgn_end = 0;
/* the region's virtual bounds as the CTRR registers would hold them */
vm_offset_t ctrr_begin = 0;
vm_offset_t ctrr_end = 0;

void
rorgn_stash_range(void)
{
}

void
rorgn_lockdown(void)
{
}

/* whether [addr, addr + size) lies in the region; with none set up, what the caller says to assume (defval) */
bool
rorgn_contains(vm_offset_t addr, vm_size_t size, bool defval)
{
#pragma unused(addr, size)
	return defval;
}

void
rorgn_validate_core(void)
{
}

#endif /* defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR) */
