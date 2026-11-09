/*
 * What the drivers use of XNU and of the port's kernel code that no public header declares. One place, so that a move to
 * another XNU has one list to check (docs/modular-kernel.md, "The seam").
 */
#ifndef _SYS_HAKIOS_XNU_H_
#define _SYS_HAKIOS_XNU_H_

#include <mach/mach_types.h>
#include <stdint.h>

/* osfmk/arm64: MMIO and cache maintenance */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

/* threads and time */
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void delay(int usec);
extern void IOSleep(unsigned int milliseconds);

/* pexpert and BSD */
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

/* the port: the loader's log, and handing the phone back to it */
extern void loader_log_line(const char *line);
extern void loader_return(void);

#endif /* _SYS_HAKIOS_XNU_H_ */
