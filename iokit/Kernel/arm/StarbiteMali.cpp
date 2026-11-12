/*
 * Project Starbite: the Mali-G72 for iOS apps (stage 6 of
 * bsd/dev/starlte_gpu.c). The IOService "StarbiteMali" and its user client
 * speak Linux Panfrost's DRM interface (tools/gpu/panfrost_drm.h, MIT), one
 * IOConnect selector per ioctl, so Mesa's Panfrost driver can run on top
 * through a small libdrm shim (the HackiOS approach with msm):
 *
 *   0 SUBMIT, 1 WAIT_BO, 2 CREATE_BO, 3 MMAP_BO, 4 GET_PARAM,
 *   5 GET_BO_OFFSET, 8 MADVISE   structs of panfrost_drm.h, in and out
 *   0x100 GEM_CLOSE             struct drm_gem_close (handle, pad)
 *   0x101 VERSION               out: u32 major, minor, patch (1.3.0)
 *   0x102 LOG                   in: text for the kernel log (test tools)
 *   0x103 SURFACE               an IOSurface (SweetSurface) by ID as a buffer:
 *                               its pages mapped at a GPU VA, handle back;
 *                               flags 1 = only describe it (size, layout)
 *
 * Buffers are XNU memory in 4 KiB pages, cleaned out of the CPU caches and
 * mapped write-combined into the app (the GPU is not IO-coherent), at GPU
 * VAs from 0x04000000 in the apps' address space (AS1). Jobs run to
 * completion inside SUBMIT, so WAIT_BO has nothing to wait for. Heap
 * buffers (PANFROST_BO_HEAP) are allocated in full: nothing handles GPU
 * page faults yet.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <sys/errno.h>

#include "SweetSurface.h"
#include <pexpert/pexpert.h>

/* boot-arg sweetspeed&64 lets clients that ask (SBM_ASYNC) have their submits return at once */
extern "C" uint32_t sweet_speed_flags(void);
static bool sbm_async_ok;

extern "C" void sweet_memstat_gpu_client(struct proc *p);  /* bsd/kern/kern_memorystatus.c */
extern "C" void *get_bsdtask_info(task_t);
extern "C" void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);

extern "C" {
int starlte_gpu_user_ready(void);
bool starlte_gpu_user_param(uint32_t param, uint64_t *value);
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n);
void starlte_gpu_user_unmap(uint64_t va, unsigned n);
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements);
int starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq);
int starlte_gpu_user_wait_seq(uint64_t seq, bool block);
uint64_t starlte_gpu_user_seq(void);
void starbite_mali_publish(void);
}

/* panfrost_drm.h layouts. */
struct sbm_submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements, jm_ctx_handle, pad;
};
struct sbm_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};
struct sbm_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};
struct sbm_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct sbm_get_param {
	uint32_t param, pad;
	uint64_t value;
};
struct sbm_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};
struct sbm_madvise {
	uint32_t handle, madv, retained;
};
struct sbm_gem_close {
	uint32_t handle, pad;
};
struct sbm_version {
	uint32_t major, minor, patch;
};
struct sbm_surface {
	uint32_t surface, handle;               /* in: IOSurface ID; out: buffer handle */
	uint64_t size;                          /* out: bytes of the surface's memory */
	uint32_t offset, row_bytes;             /* out: where the pixels start, row length */
	uint32_t width, height, pixel_format, planes;
	uint32_t flags, pad;                    /* in: SBM_SURFACE_DESCRIBE */
};
#define SBM_SURFACE_DESCRIBE    1u
struct sbm_import_host {
	uint64_t addr, size;                    /* in: the caller's own memory, 16 KiB aligned */
	uint32_t handle, pad;                   /* out: buffer handle */
	uint64_t va;                            /* out: its GPU address */
};
#define SBM_SUBMIT_V1_SIZE      40u             /* struct drm_panfrost_submit before jm_ctx_handle */

enum {
	SBM_SUBMIT = 0, SBM_WAIT_BO = 1, SBM_CREATE_BO = 2, SBM_MMAP_BO = 3, SBM_GET_PARAM = 4,
	SBM_GET_BO_OFFSET = 5, SBM_MADVISE = 8, SBM_GEM_CLOSE = 0x100, SBM_VERSION = 0x101,
	SBM_LOG = 0x102, SBM_SURFACE = 0x103, SBM_IMPORT_HOST = 0x104, SBM_ASYNC = 0x105, SBM_WAIT_IDLE = 0x106,
};

#define SBM_MAX_BOS     8192u
#define SBM_VA_FIRST    0x04000000ull
#define SBM_VA_END      0x100000000ull
#define SBM_MAX_BO      (256u << 20)

struct sbm_bo {
	IOMemoryDescriptor *mem;
	IOMemoryMap *map;
	uint64_t va;
	uint32_t pages;
	uint32_t surface;                       /* IOSurface ID of an imported buffer, else 0 */
	bool host;                              /* the caller's own pages, wired for the GPU (IMPORT_HOST) */
	uint64_t seq;                           /* last chain that named it (async submits) */
	uint64_t dying;                         /* closed by its owner: it goes once this chain is done (the queue); 0 = live */
};

/*
 * GPU VAs, shared by all clients (they share AS1): a bitmap of 64 KiB granules from SBM_VA_FIRST to
 * 4 GiB, handed out next-fit with one free granule after each buffer (a fault past its end lands
 * in nothing). It used to be a bump allocator that never reused anything; with CoreAnimation
 * making and dropping textures all the time, the page tables behind it ran out within minutes
 * (the user's sessions: "out of level-3 tables" x1282, backboardd crashing and crawling).
 */
#define SBM_VA_GRANULE          0x10000ull
#define SBM_VA_GRANULES         ((SBM_VA_END - SBM_VA_FIRST) / SBM_VA_GRANULE)
static IOLock *sbm_va_lock;
static uint64_t sbm_va_used[SBM_VA_GRANULES / 64];
static uint64_t sbm_va_cursor;

static uint64_t
sbm_va_granules(uint64_t bytes)
{
	return (bytes + SBM_VA_GRANULE - 1) / SBM_VA_GRANULE + 1;
}

static bool
sbm_va_bit(uint64_t g)
{
	return (sbm_va_used[g / 64] >> (g % 64)) & 1;
}

static void
sbm_va_set(uint64_t first, uint64_t n, bool used)
{
	for (uint64_t g = first; g < first + n; g++) {
		if (used) {
			sbm_va_used[g / 64] |= 1ull << (g % 64);
		} else {
			sbm_va_used[g / 64] &= ~(1ull << (g % 64));
		}
	}
}

/* A GPU VA for bytes, or 0. */
static uint64_t
sbm_va_alloc(uint64_t bytes)
{
	uint64_t n = sbm_va_granules(bytes), va = 0;

	IOLockLock(sbm_va_lock);
	for (unsigned pass = 0; pass < 2 && va == 0; pass++) {
		uint64_t g = pass == 0 ? sbm_va_cursor : 0, run = 0;
		uint64_t end = pass == 0 ? SBM_VA_GRANULES : sbm_va_cursor + n;
		for (; g < end && g < SBM_VA_GRANULES; g++) {
			if (sbm_va_bit(g)) {
				run = 0;
			} else if (++run == n) {
				uint64_t first = g + 1 - n;
				sbm_va_set(first, n, true);
				sbm_va_cursor = g + 1;
				va = SBM_VA_FIRST + first * SBM_VA_GRANULE;
				break;
			}
		}
	}
	IOLockUnlock(sbm_va_lock);
	return va;
}

static void
sbm_va_free(uint64_t va, uint64_t bytes)
{
	if (va < SBM_VA_FIRST || va >= SBM_VA_END) {
		return;
	}
	IOLockLock(sbm_va_lock);
	sbm_va_set((va - SBM_VA_FIRST) / SBM_VA_GRANULE, sbm_va_granules(bytes), false);
	IOLockUnlock(sbm_va_lock);
}

/* 4 KiB pages the GPU holds wired, all clients: buffers of ours, imported surfaces, imported host memory. */
static int64_t sbm_live_pages[3];

static unsigned
sbm_bo_kind(const sbm_bo *bo)
{
	return bo->host ? 2 : bo->surface != 0 ? 1 : 0;
}

static void
sbm_count_pages(const sbm_bo *bo, int64_t sign)
{
	OSAddAtomic64(sign * (int64_t)bo->pages, &sbm_live_pages[sbm_bo_kind(bo)]);
}

/*
 * A cap on the buffers this driver makes (CREATE_BO: wired kernel memory, write-combined, that nothing can page out or
 * reclaim). Every PanVK device wires 57 MiB when it is made (tiler heap, varyings, TLS) and textures follow: backboardd
 * alone holds about 285 MiB while it composites on Metal, and several Metal clients at once (the poster renderers) had
 * nothing to stop them from wiring the VM into a hard hang. With the cap a CREATE_BO beyond it fails (kIOReturnNoMemory:
 * the client sees an allocation failure) instead.
 *   boot-arg sweetspeed&134217728: on, with the defaults: all clients together 35 % of the memory, one client half of that
 *   boot-args sbm_cap_mb=<MiB>, sbm_client_cap_mb=<MiB>: the two caps by hand (either one turns the cap on; 0 = that one off)
 * Imported surfaces and host memory are not counted: they are another owner's pages, wired where they were made.
 */
extern "C" {
void proc_selfname(char *buf, int size);
int proc_selfpid(void);
}
static uint64_t sbm_cap_pages, sbm_client_cap_pages;   /* in 4 KiB pages; 0 = no cap */
static uint64_t sbm_refused, sbm_refused_bytes;

static void
sbm_cap_init(void)
{
	uint32_t mb = 0, client_mb = 0;
	bool by_hand = PE_parse_boot_argn("sbm_cap_mb", &mb, sizeof(mb));
	bool client_by_hand = PE_parse_boot_argn("sbm_client_cap_mb", &client_mb, sizeof(client_mb));
	if ((sweet_speed_flags() & 134217728) != 0) {
		if (!by_hand) {
			mb = (uint32_t)((max_mem >> 20) * 35 / 100);
		}
		if (!client_by_hand) {
			client_mb = mb / 2;
		}
	}
	sbm_cap_pages = (uint64_t)mb << 8;
	sbm_client_cap_pages = (uint64_t)client_mb << 8;
	IOLog("STARLTE GPU: buffer cap: all clients %u MiB, one client %u MiB (0 = none; memory %llu MiB; sweetspeed&134217728, "
	    "sbm_cap_mb, sbm_client_cap_mb)\n", mb, client_mb, max_mem >> 20);
}

/* May this client (holding `mine` pages of buffers) have `pages` more? Says so when not (the first 10, then every 100th). */
static bool
sbm_cap_allows(uint64_t mine, uint32_t pages)
{
	uint64_t all = (uint64_t)sbm_live_pages[0];
	bool over_all = sbm_cap_pages != 0 && all + pages > sbm_cap_pages;
	bool over_mine = sbm_client_cap_pages != 0 && mine + pages > sbm_client_cap_pages;
	if (!over_all && !over_mine) {
		return true;
	}
	uint64_t n = (uint64_t)OSIncrementAtomic64((SInt64 *)&sbm_refused);
	OSAddAtomic64((SInt64)pages * 4096, (SInt64 *)&sbm_refused_bytes);
	if (n < 10 || (n % 100) == 0) {
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("STARLTE GPU: buffer of %u KiB REFUSED for %s[%d]: %s (all clients %llu of %llu MiB, this one %llu of %llu MiB; "
		    "%llu refused so far)\n", pages * 4, name, proc_selfpid(), over_all ? "the cap for all clients" : "this client's cap",
		    all >> 8, sbm_cap_pages >> 8, mine >> 8, sbm_client_cap_pages >> 8, n + 1);
	}
	return false;
}

class StarbiteMali : public IOService {
	OSDeclareDefaultStructors(StarbiteMali);
};

OSDefineMetaClassAndStructors(StarbiteMali, IOService);

class StarbiteMaliUserClient : public IOUserClient {
	OSDeclareDefaultStructors(StarbiteMaliUserClient);
	task_t fTask = NULL;
	IOLock *fLock = NULL;
	sbm_bo *fBos = NULL;            /* handle h is fBos[h - 1] */
	uint32_t fCount = 0;
	bool fAsync = false;
	uint64_t fBufferPages = 0;              /* buffers of its own (CREATE_BO), 4 KiB pages: for the per-client cap */
	void markBOs(uint64_t list, uint32_t count, uint64_t seq);

	IOReturn createBO(sbm_create_bo *a);
	IOReturn importSurface(sbm_surface *a);
	IOReturn importHost(sbm_import_host *a);
	IOReturn mmapBO(sbm_mmap_bo *a);
	sbm_bo *lookup(uint32_t handle);
	void closeBO(sbm_bo *bo);
	void reap(void);
	uint32_t fDying[256];                   /* handles closed by the client whose buffers still wait for their chains */
	uint32_t fDyingCount = 0;

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn dispatchMethod(uint32_t selector, IOExternalMethodArguments *args);
};

OSDefineMetaClassAndStructors(StarbiteMaliUserClient, IOUserClient);

bool
StarbiteMaliUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
	if (!starlte_gpu_user_ready() || !IOUserClient::initWithTask(owningTask, securityID, type,
	    properties)) {
		return false;
	}
	fTask = owningTask;
	if (void *bsd = get_bsdtask_info(owningTask)) {
		sweet_memstat_gpu_client((struct proc *)bsd);
	}
	fLock = IOLockAlloc();
	fBos = (sbm_bo *)IOMallocZeroData(SBM_MAX_BOS * sizeof(sbm_bo));
	if (fLock == NULL || fBos == NULL) {
		return false;
	}
	/* No entitlement needed; externalMethod calls are serialized by IOKit. */
	setProperty("IOUserClientEntitlements", kOSBooleanFalse);
	setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
	return true;
}

sbm_bo *
StarbiteMaliUserClient::lookup(uint32_t handle)
{
	return (handle >= 1 && handle <= fCount && fBos[handle - 1].mem != NULL && fBos[handle - 1].dying == 0) ?
	       &fBos[handle - 1] : NULL;
}

/*
 * fLock held. The buffers the client has closed whose chains are done by now go; the others stay on the list. With the
 * queue a GEM_CLOSE used to wait for every chain queued by then (up to 90 ms on the thread that closed, seen on a
 * phone); now it only notes the chain, and the buffer is unmapped and freed here, at one of the client's next calls.
 */
void
StarbiteMaliUserClient::reap(void)
{
	uint32_t kept = 0;
	for (uint32_t i = 0; i < fDyingCount; i++) {
		sbm_bo *bo = &fBos[fDying[i] - 1];
		if (bo->mem != NULL && bo->dying != 0 && starlte_gpu_user_wait_seq(bo->dying, false) != 0) {
			fDying[kept++] = fDying[i];             /* its chain is still queued or running */
			continue;
		}
		if (bo->mem != NULL && bo->dying != 0) {
			closeBO(bo);
		}
	}
	fDyingCount = kept;
}

void
StarbiteMaliUserClient::closeBO(sbm_bo *bo)
{
	sbm_count_pages(bo, -1);
	if (bo->surface == 0 && !bo->host && fBufferPages >= bo->pages) {
		fBufferPages -= bo->pages;
	}
	starlte_gpu_user_unmap(bo->va, bo->pages);
	sbm_va_free(bo->va, (uint64_t)bo->pages * 0x1000);
	OSSafeReleaseNULL(bo->map);
	/* an imported surface's pages were wired for the GPU (importSurface) */
	if ((bo->surface != 0 || bo->host) && bo->mem != NULL) {
		bo->mem->complete();
	} else if (bo->mem != NULL) {
		/*
		 * A buffer of ours was write-combined: its pages keep that attribute when freed
		 * (kernel-object pages are not reset by vm_page_free), and came back as Normal-NC
		 * heap in some task, where atomics take an external abort. Default them first.
		 */
		IOByteCount length = bo->mem->getLength();
		for (IOByteCount offset = 0; offset < length; offset += PAGE_SIZE) {
			IOByteCount len = 0;
			addr64_t pa = bo->mem->getPhysicalSegment(offset, &len, kIOMemoryMapperNone);
			if (pa != 0) {
				pmap_set_cache_attributes((ppnum_t)atop_64(pa), 0);
			}
		}
	}
	OSSafeReleaseNULL(bo->mem);
	bo->va = 0;
	bo->pages = 0;
	bo->surface = 0;
	bo->host = false;
	bo->dying = 0;
	bo->seq = 0;
}

/* Async submits: the buffers a chain names are not closed or waited for as idle before it is done. */
void
StarbiteMaliUserClient::markBOs(uint64_t list, uint32_t count, uint64_t seq)
{
	uint32_t h[128];
	for (uint32_t i = 0; i < count;) {
		uint32_t n = count - i < 128u ? count - i : 128u;
		if (copyin(list + (uint64_t)i * 4u, h, n * sizeof(uint32_t)) != 0) {
			return;
		}
		for (uint32_t j = 0; j < n; j++) {
			if (sbm_bo *bo = lookup(h[j])) {
				bo->seq = seq;
			}
		}
		i += n;
	}
}

IOReturn
StarbiteMaliUserClient::createBO(sbm_create_bo *a)
{
	if (a->size == 0 || a->size > SBM_MAX_BO) {
		return kIOReturnBadArgument;
	}
	uint32_t handle = 0;
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem == NULL) {
			handle = i + 1;
			break;
		}
	}
	if (handle == 0) {
		if (fCount >= SBM_MAX_BOS) {
			return kIOReturnNoResources;
		}
		handle = ++fCount;
	}
	/* whole 16 KiB pages: Mesa maps the buffer into a task with 16 KiB pages */
	uint64_t bytes = ((uint64_t)a->size + 0x3fff) & ~0x3fffull;
	uint32_t pages = (uint32_t)(bytes / 0x1000);
	if (!sbm_cap_allows(fBufferPages, pages)) {
		return kIOReturnNoMemory;
	}
	uint64_t va = sbm_va_alloc(bytes);
	if (va == 0) {
		return kIOReturnNoSpace;
	}

	IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
	    (vm_size_t)round_page(bytes), page_size);
	uint64_t *pa = (uint64_t *)IOMallocData((vm_size_t)pages * sizeof(uint64_t));
	if (mem == NULL || pa == NULL) {
		OSSafeReleaseNULL(mem);
		if (pa) {
			IOFreeData(pa, (vm_size_t)pages * sizeof(uint64_t));
		}
		sbm_va_free(va, bytes);
		return kIOReturnNoMemory;
	}
	/* Out of the CPU caches by physical address, then zeroed write-combined:
	 * no dirty line can later be evicted over what the GPU wrote. */
	mem->performOperation(kIOMemoryIncoherentIOFlush, 0, mem->getLength());
	bzero(mem->getBytesNoCopy(), (size_t)bytes);
	bool ok = true;
	for (uint32_t i = 0; i < pages && ok; i++) {
		IOByteCount len = 0;
		pa[i] = mem->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);
		ok = pa[i] != 0;
	}
	ok = ok && starlte_gpu_user_map(va, pa, pages);
	IOFreeData(pa, (vm_size_t)pages * sizeof(uint64_t));
	if (!ok) {
		mem->release();
		sbm_va_free(va, bytes);
		return kIOReturnVMError;
	}
	fBos[handle - 1] = (sbm_bo){ .mem = mem, .map = NULL, .va = va, .pages = pages };
	sbm_count_pages(&fBos[handle - 1], 1);
	fBufferPages += pages;
	a->handle = handle;
	a->offset = va;
	return kIOReturnSuccess;
}

/*
 * An IOSurface as a buffer, so the GPU can draw into it or read it: the
 * surface's pages (buffer or physical) at a GPU VA of their own. One handle
 * per surface and client, as Panfrost keeps one buffer per handle.
 */
IOReturn
StarbiteMaliUserClient::importSurface(sbm_surface *a)
{
	SweetSurface *s = sweet_surface_copy(a->surface);
	if (s == NULL || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNotFound;
	}
	a->size = round_page(s->memory->getLength());
	a->offset = s->offset;
	a->row_bytes = s->bytesPerRow;
	a->width = s->width;
	a->height = s->height;
	a->pixel_format = s->pixelFormat;
	a->planes = s->planeCount;
	a->handle = 0;
	if (a->flags & SBM_SURFACE_DESCRIBE) {
		s->release();
		return kIOReturnSuccess;
	}
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem != NULL && fBos[i].surface == a->surface && fBos[i].mem == s->memory) {
			a->handle = i + 1;
			s->release();
			return kIOReturnSuccess;
		}
	}
	uint32_t handle = 0;
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem == NULL) {
			handle = i + 1;
			break;
		}
	}
	if (handle == 0) {
		if (fCount >= SBM_MAX_BOS) {
			s->release();
			return kIOReturnNoResources;
		}
		handle = ++fCount;
	}
	uint32_t pages = (uint32_t)(a->size / 0x1000);
	/*
	 * The GPU keeps the surface's pages mapped until the buffer is closed: they stay wired
	 * that long (a wrapped or client-memory surface is unwired when the surface goes, and
	 * its pages would be reused while the GPU still writes into them).
	 */
	if (s->memory->prepare() != kIOReturnSuccess) {
		s->release();
		return kIOReturnVMError;
	}
	uint64_t va = sbm_va_alloc(a->size);
	if (va == 0) {
		s->memory->complete();
		s->release();
		return kIOReturnNoSpace;
	}
	uint64_t *pa = (uint64_t *)IOMallocData((vm_size_t)pages * sizeof(uint64_t));
	bool ok = pa != NULL;
	for (uint32_t i = 0; i < pages && ok; i++) {
		IOByteCount len = 0;
		pa[i] = s->memory->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);
		ok = pa[i] != 0;
	}
	ok = ok && starlte_gpu_user_map(va, pa, pages);
	if (pa != NULL) {
		IOFreeData(pa, (vm_size_t)pages * sizeof(uint64_t));
	}
	if (!ok) {
		s->memory->complete();
		s->release();
		sbm_va_free(va, a->size);
		return kIOReturnVMError;
	}
	s->memory->retain();
	fBos[handle - 1] = (sbm_bo){ .mem = s->memory, .map = NULL, .va = va, .pages = pages,
		                     .surface = a->surface };
	sbm_count_pages(&fBos[handle - 1], 1);
	a->handle = handle;
	s->release();
	return kIOReturnSuccess;
}

/*
 * The caller's own memory as a buffer (VK_EXT_external_memory_host): wired until the buffer is
 * closed, its pages at a GPU VA of their own. Whole 16 KiB pages, as the task maps them.
 */
IOReturn
StarbiteMaliUserClient::importHost(sbm_import_host *a)
{
	if (a->size == 0 || a->size > SBM_MAX_BO || (a->addr & 0x3fff) != 0 || (a->size & 0x3fff) != 0) {
		return kIOReturnBadArgument;
	}
	uint32_t handle = 0;
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem == NULL) {
			handle = i + 1;
			break;
		}
	}
	if (handle == 0) {
		if (fCount >= SBM_MAX_BOS) {
			return kIOReturnNoResources;
		}
		handle = ++fCount;
	}
	IOMemoryDescriptor *mem = IOMemoryDescriptor::withAddressRange(a->addr, a->size, kIODirectionInOut, fTask);
	if (mem == NULL) {
		return kIOReturnNoMemory;
	}
	if (mem->prepare() != kIOReturnSuccess) {
		/* read-only memory (a mapped file, a constant buffer): wired for the GPU to read */
		mem->release();
		mem = IOMemoryDescriptor::withAddressRange(a->addr, a->size, kIODirectionOut, fTask);
		if (mem == NULL) {
			return kIOReturnNoMemory;
		}
		if (mem->prepare() != kIOReturnSuccess) {
			mem->release();
			return kIOReturnVMError;
		}
	}
	uint32_t pages = (uint32_t)(a->size / 0x1000);
	uint64_t va = sbm_va_alloc(a->size);
	uint64_t *pa = va ? (uint64_t *)IOMallocData((vm_size_t)pages * sizeof(uint64_t)) : NULL;
	bool ok = pa != NULL;
	for (uint32_t i = 0; i < pages && ok; i++) {
		IOByteCount len = 0;
		pa[i] = mem->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);
		ok = pa[i] != 0;
	}
	ok = ok && starlte_gpu_user_map(va, pa, pages);
	if (pa != NULL) {
		IOFreeData(pa, (vm_size_t)pages * sizeof(uint64_t));
	}
	if (!ok) {
		if (va) {
			sbm_va_free(va, a->size);
		}
		mem->complete();
		mem->release();
		return va ? kIOReturnVMError : kIOReturnNoSpace;
	}
	fBos[handle - 1] = (sbm_bo){ .mem = mem, .map = NULL, .va = va, .pages = pages, .surface = 0, .host = true };
	sbm_count_pages(&fBos[handle - 1], 1);
	a->handle = handle;
	a->va = va;
	return kIOReturnSuccess;
}

IOReturn
StarbiteMaliUserClient::mmapBO(sbm_mmap_bo *a)
{
	sbm_bo *bo = lookup(a->handle);
	if (bo == NULL) {
		return kIOReturnBadArgument;
	}
	if (bo->map == NULL) {
		/*
		 * The cache mode is the physical pages', for every mapping of them: an imported
		 * surface may be a wrapped piece of the client's own heap, which must stay
		 * cacheable (atomics on Normal-NC memory are external aborts here). Only buffers
		 * of ours, which nothing else maps, are write-combined. Copy-back (= the default
		 * attribute, VM_WIMG_COPYBACK == VM_WIMG_DEFAULT) and not kIOMapDefaultCache: that
		 * shares the task's own VM object a second time, and backboardd faulting on its
		 * wired pages through it slept in vm_page_sleep for good (GPU boot 9).
		 */
		IOOptionBits cache = (bo->surface != 0 || bo->host) ? kIOMapCopybackCache : kIOMapWriteCombineCache;
		bo->map = bo->mem->createMappingInTask(fTask, 0, kIOMapAnywhere | cache);
		if (bo->map == NULL) {
			return kIOReturnVMError;
		}
	}
	/* With this shim the "mmap offset" is the mapped address itself. */
	a->offset = bo->map->getAddress();
	return kIOReturnSuccess;
}

IOReturn
StarbiteMaliUserClient::dispatchMethod(uint32_t selector, IOExternalMethodArguments *args)
{
	union {
		sbm_submit submit;
		sbm_wait_bo wait;
		sbm_create_bo create;
		sbm_mmap_bo mmap;
		sbm_get_param param;
		sbm_get_bo_offset offset;
		sbm_madvise madvise;
		sbm_gem_close close;
		sbm_version version;
		sbm_surface surface;
		sbm_import_host host;
		char text[256];
		int64_t timeout;
		uint32_t flag;
	} u;
	size_t want;
	switch (selector) {
	case SBM_SUBMIT: want = sizeof(u.submit); break;
	case SBM_WAIT_BO: want = sizeof(u.wait); break;
	case SBM_CREATE_BO: want = sizeof(u.create); break;
	case SBM_MMAP_BO: want = sizeof(u.mmap); break;
	case SBM_GET_PARAM: want = sizeof(u.param); break;
	case SBM_GET_BO_OFFSET: want = sizeof(u.offset); break;
	case SBM_MADVISE: want = sizeof(u.madvise); break;
	case SBM_GEM_CLOSE: want = sizeof(u.close); break;
	case SBM_VERSION: want = 0; break;
	case SBM_LOG: want = args->structureInputSize; break;
	case SBM_SURFACE: want = sizeof(u.surface); break;
	case SBM_IMPORT_HOST: want = sizeof(u.host); break;
	case SBM_ASYNC: want = sizeof(u.flag); break;
	case SBM_WAIT_IDLE: want = sizeof(u.timeout); break;
	default:
		return kIOReturnUnsupported;
	}
	bzero(&u, sizeof(u));
	if (selector == SBM_LOG) {
		if (want == 0 || want >= sizeof(u.text)) {
			return kIOReturnBadArgument;
		}
	} else if (selector == SBM_SUBMIT && args->structureInputSize == SBM_SUBMIT_V1_SIZE) {
		want = SBM_SUBMIT_V1_SIZE;              /* Mesa 25.0's uapi: no jm_ctx_handle */
	} else if (args->structureInputSize != want) {
		return kIOReturnBadArgument;
	}
	if (want) {
		bcopy(args->structureInput, &u, want);
	}

	IOReturn ret = kIOReturnSuccess;
	/*
	 * The waits sleep (or spin) for as long as the GPU works: not under fLock, or this client's other threads
	 * (the render thread's next submit while a completion thread waits on a fence) would wait with them.
	 */
	if (selector == SBM_WAIT_IDLE) {
		return starlte_gpu_user_wait_seq(0, u.timeout != 0) == 0 ? kIOReturnSuccess : kIOReturnTimeout;
	}
	if (selector == SBM_WAIT_BO || selector == SBM_GEM_CLOSE) {
		IOLockLock(fLock);
		sbm_bo *bo = lookup(selector == SBM_WAIT_BO ? u.wait.handle : u.close.handle);
		uint64_t seq = bo ? bo->seq : 0;
		IOLockUnlock(fLock);
		if (bo == NULL) {
			return kIOReturnBadArgument;
		}
		if (selector == SBM_GEM_CLOSE && starlte_gpu_user_seq() != 0) {
			seq = 0;                        /* (the queue: nothing is waited for here, see GEM_CLOSE below) */
		}
		bool block = selector == SBM_GEM_CLOSE || u.wait.timeout_ns != 0;
		if (seq != 0 && starlte_gpu_user_wait_seq(seq, block) != 0 && selector == SBM_WAIT_BO) {
			return kIOReturnTimeout;
		}
		if (selector == SBM_WAIT_BO) {
			if (args->structureOutput != NULL) {
				if (args->structureOutputSize < want) {
					return kIOReturnBadArgument;
				}
				bcopy(&u, args->structureOutput, want);
				args->structureOutputSize = (uint32_t)want;
			}
			return kIOReturnSuccess;
		}
	}
	IOLockLock(fLock);
	if (fDyingCount != 0) {
		reap();
	}
	switch (selector) {
	case SBM_SUBMIT: {
		int err;
		if (fAsync) {
			uint64_t seq = 0;
			err = starlte_gpu_user_submit2(u.submit.jc, u.submit.requirements, true, &seq);
			if (seq != 0) {
				markBOs(u.submit.bo_handles, u.submit.bo_handle_count, seq);
			}
		} else {
			err = starlte_gpu_user_submit(u.submit.jc, u.submit.requirements);
		}
		ret = err == 0 ? kIOReturnSuccess : err == ETIMEDOUT ? kIOReturnTimeout : kIOReturnIOError;
		break;
	}
	case SBM_ASYNC:
		fAsync = sbm_async_ok && u.flag != 0;
		u.flag = fAsync;
		break;
	case SBM_CREATE_BO:
		ret = createBO(&u.create);
		break;
	case SBM_MMAP_BO:
		ret = mmapBO(&u.mmap);
		break;
	case SBM_GET_PARAM:
		ret = starlte_gpu_user_param(u.param.param, &u.param.value) ? kIOReturnSuccess :
		    kIOReturnBadArgument;
		break;
	case SBM_GET_BO_OFFSET: {
		sbm_bo *bo = lookup(u.offset.handle);
		u.offset.offset = bo ? bo->va : 0;
		ret = bo ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	}
	case SBM_MADVISE:
		u.madvise.retained = 1;                 /* never purged */
		ret = lookup(u.madvise.handle) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case SBM_GEM_CLOSE: {
		sbm_bo *bo = lookup(u.close.handle);
		if (bo) {
			/*
			 * With the queue, a buffer goes only once every chain queued by now is done, not just the last one
			 * that listed it: a chain reaches buffers its submit does not list (PanVK lists a batch's pools and
			 * targets; a descriptor may point at any live buffer), and chains wait in a queue behind others. The
			 * close does not wait for that: the buffer is marked and goes at a later call (reap()).
			 */
			uint64_t last = starlte_gpu_user_seq();
			uint64_t need = last > bo->seq ? last : bo->seq;
			if (last != 0 && need != 0 && fDyingCount < sizeof(fDying) / sizeof(fDying[0]) &&
			    starlte_gpu_user_wait_seq(need, false) != 0) {
				bo->dying = need;
				fDying[fDyingCount++] = u.close.handle;
			} else {
				/* (no queue, or its chains are done, or the list is full: as before, wait and close) */
				if (need != 0) {
					(void)starlte_gpu_user_wait_seq(need, true);
				}
				closeBO(bo);
			}
		}
		ret = bo ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	}
	case SBM_VERSION:
		u.version = (sbm_version){ 1, 3, 0 };
		want = sizeof(u.version);
		break;
	case SBM_SURFACE:
		ret = importSurface(&u.surface);
		break;
	case SBM_IMPORT_HOST:
		ret = importHost(&u.host);
		break;
	case SBM_LOG:
		u.text[want] = 0;
		IOLog("STARLTE GPU APP: %s\n", u.text);
		want = 0;
		break;
	}
	IOLockUnlock(fLock);
	if (ret == kIOReturnSuccess && want && args->structureOutput != NULL) {
		if (args->structureOutputSize < want) {
			return kIOReturnBadArgument;
		}
		bcopy(&u, args->structureOutput, want);
		args->structureOutputSize = (uint32_t)want;
	}
	return ret;
}

/*
 * Where an app's time in this driver goes (the render thread of backboardd spends a quarter of
 * its scrolling time in calls here): count and time of each call, all clients together, a line
 * every 10 s while calls come: "STARLTE GPU CALLS: submit N x avg us (max), create ..., close ...".
 */
static uint64_t sbm_calls[6], sbm_call_ns[6], sbm_call_max[6], sbm_calls_since;
/*
 * boot-arg sbm_prof=1 (off by default): who waits. With the ten-second line, "STARLTE GPU WAITS: ...": the waits for one
 * buffer and the waits for the whole queue, and the four threads that waited longest, by thread id and name.
 */
static uint32_t sbm_prof;
static int sbm_prof_checked;
static struct { uint64_t tid, n, ns; char name[24]; } sbm_waiter[8];
static uint64_t sbm_wait_kind_n[2], sbm_wait_kind_ns[2];
extern "C" uint64_t thread_tid(thread_t thread);
extern "C" void bsd_getthreadname(void *uth, char *buffer);
extern "C" void *current_uthread(void);

static unsigned
sbm_call_kind(uint32_t selector)
{
	switch (selector) {
	case SBM_SUBMIT: return 0;
	case SBM_CREATE_BO: return 1;
	case SBM_GEM_CLOSE: return 2;
	case SBM_SURFACE: case SBM_IMPORT_HOST: return 3;
	case SBM_WAIT_BO: case SBM_WAIT_IDLE: return 5;
	default: return 4;
	}
}

IOReturn
StarbiteMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	uint64_t t0 = mach_absolute_time();
	IOReturn ret = dispatchMethod(selector, args);
	uint64_t t1 = mach_absolute_time(), ns = 0;
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	unsigned k = sbm_call_kind(selector);
	if (!sbm_prof_checked) {
		sbm_prof_checked = 1;
		PE_parse_boot_argn("sbm_prof", &sbm_prof, sizeof(sbm_prof));
	}
	IOLockLock(sbm_va_lock);
	if (sbm_prof && k == 5) {
		uint64_t tid = thread_tid(current_thread());
		unsigned w = selector == SBM_WAIT_IDLE ? 1 : 0, slot = 8;
		sbm_wait_kind_n[w]++;
		sbm_wait_kind_ns[w] += ns;
		for (unsigned i = 0; i < 8; i++) {
			if (sbm_waiter[i].tid == tid || sbm_waiter[i].n == 0) {
				slot = i;
				break;
			}
		}
		if (slot < 8) {
			if (sbm_waiter[slot].n == 0) {
				sbm_waiter[slot].tid = tid;
				sbm_waiter[slot].name[0] = 0;
				char tn[64] = "";
				bsd_getthreadname(current_uthread(), tn);
				strlcpy(sbm_waiter[slot].name, tn, sizeof(sbm_waiter[slot].name));
			}
			sbm_waiter[slot].n++;
			sbm_waiter[slot].ns += ns;
		}
	}
	sbm_calls[k]++;
	sbm_call_ns[k] += ns;
	if (ns > sbm_call_max[k]) {
		sbm_call_max[k] = ns;
	}
	if (sbm_calls_since == 0) {
		sbm_calls_since = t1;
	}
	uint64_t window = 0;
	absolutetime_to_nanoseconds(t1 - sbm_calls_since, &window);
	if (window >= 10ull * NSEC_PER_SEC) {
		static const char *names[6] = { "submit", "create", "close", "surface", "other", "wait" };
		char line[720];
		int n = snprintf(line, sizeof(line), "STARLTE GPU CALLS (%llu s):", window / NSEC_PER_SEC);
		for (unsigned i = 0; i < 6; i++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %llu x %llu us (max %llu), %llu ms;", names[i],
			    sbm_calls[i], sbm_calls[i] ? sbm_call_ns[i] / sbm_calls[i] / 1000 : 0, sbm_call_max[i] / 1000,
			    sbm_call_ns[i] / 1000000);
			sbm_calls[i] = sbm_call_ns[i] = sbm_call_max[i] = 0;
		}
		snprintf(line + n, sizeof(line) - (size_t)n, " wired: buffers %lld MiB, surfaces %lld MiB, host %lld MiB",
		    sbm_live_pages[0] / 256, sbm_live_pages[1] / 256, sbm_live_pages[2] / 256);
		sbm_calls_since = t1;
		char wline[400];
		wline[0] = 0;
		if (sbm_prof) {
			int wn = snprintf(wline, sizeof(wline), "STARLTE GPU WAITS: one buffer %llu x %llu us, whole queue %llu x %llu us; by thread:",
			    sbm_wait_kind_n[0], sbm_wait_kind_n[0] ? sbm_wait_kind_ns[0] / sbm_wait_kind_n[0] / 1000 : 0,
			    sbm_wait_kind_n[1], sbm_wait_kind_n[1] ? sbm_wait_kind_ns[1] / sbm_wait_kind_n[1] / 1000 : 0);
			for (unsigned i = 0; i < 8 && sbm_waiter[i].n != 0 && wn < (int)sizeof(wline) - 60; i++) {
				wn += snprintf(wline + wn, sizeof(wline) - (size_t)wn, " %llu(%s) %llu x %llu us", sbm_waiter[i].tid,
				    sbm_waiter[i].name, sbm_waiter[i].n, sbm_waiter[i].ns / sbm_waiter[i].n / 1000);
			}
			bzero(sbm_waiter, sizeof(sbm_waiter));
			sbm_wait_kind_n[0] = sbm_wait_kind_n[1] = sbm_wait_kind_ns[0] = sbm_wait_kind_ns[1] = 0;
		}
		IOLockUnlock(sbm_va_lock);
		IOLog("%s\n", line);
		if (wline[0]) {
			IOLog("%s\n", wline);
		}
		return ret;
	}
	IOLockUnlock(sbm_va_lock);
	return ret;
}

IOReturn
StarbiteMaliUserClient::clientClose(void)
{
	IOLockLock(fLock);
	if (fAsync) {
		(void)starlte_gpu_user_wait_seq(0, true);
	}
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem != NULL) {
			closeBO(&fBos[i]);
		}
	}
	fDyingCount = 0;
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

void
StarbiteMaliUserClient::free(void)
{
	if (fBos != NULL) {
		IOFreeData(fBos, SBM_MAX_BOS * sizeof(sbm_bo));
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
	}
	IOUserClient::free();
}

/* Called once by starlte_gpu.c when the GPU is ready for apps. */
void
starbite_mali_publish(void)
{
	sbm_async_ok = (sweet_speed_flags() & 64) != 0;
	sbm_cap_init();
	IOLog("STARLTE GPU: StarbiteMali async submits %s (sweetspeed&64)\n", sbm_async_ok ? "allowed" : "off");
	sbm_va_lock = IOLockAlloc();
	StarbiteMali *service = OSTypeAlloc(StarbiteMali);
	IOService *provider = IOService::getResourceService();
	if (sbm_va_lock == NULL || service == NULL || !service->init() || provider == NULL ||
	    !service->attach(provider)) {
		IOLog("STARLTE GPU: StarbiteMali not published\n");
		OSSafeReleaseNULL(service);
		return;
	}
	service->setProperty(kIOUserClientClassKey, "StarbiteMaliUserClient");
	service->setProperty("GPU", "Mali-G72 MP18 (Bifrost), Panfrost DRM interface 1.3");
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return;
	}
	service->registerService();
	IOLog("STARLTE GPU: StarbiteMali published for apps\n");
	service->release();
}
