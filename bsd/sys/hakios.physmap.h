/*
 * Physical memory that XNU does not manage, mapped into the kernel: hakios_physmap_wc(pa, bytes) (iokit/Kernel/
 * hakios.physmap.cpp, built into every kernel). For the DMA buffers the Galaxy S9's drivers keep below 4 GB in the
 * lower bank (sys/hakios.exynos9810_mem.h): an IOMemoryDescriptor of the physical range, mapped write-combined (normal
 * memory, not cached, unaligned reads allowed: what a device writes is what the processor reads, with no cache
 * maintenance), never ml_io_map(), whose io_submap is only 8 MB (a few MB of buffer there leaves the next driver's
 * registers no room: a panic). Mapped once and kept for the run: the descriptor and its map are never released.
 * The virtual address, or 0 if it cannot be mapped. The camera's frames and UFS's own mode use it; the display's
 * scan-out buffers (MiniIOMobileFramebuffer.cpp) still map theirs themselves, the same way.
 * hakios_physmap_cached(pa, bytes): the same, cached: a buffer read in bulk, whose owner does the cache maintenance
 * around each DMA. A range is mapped one way only.
 */
#ifndef _SYS_HAKIOS_PHYSMAP_H_
#define _SYS_HAKIOS_PHYSMAP_H_

#include <stdint.h>
#include <mach/vm_types.h>

#ifdef __cplusplus
extern "C"
#endif
vm_offset_t hakios_physmap_wc(uint64_t pa, uint64_t bytes);
#ifdef __cplusplus
extern "C"
#endif
vm_offset_t hakios_physmap_cached(uint64_t pa, uint64_t bytes);

#endif /* _SYS_HAKIOS_PHYSMAP_H_ */
