/*
 * Built-in IOSurface for SWEET.
 *
 * Apple's IOSurface kext is closed. IOSurface.framework (iOS 16.7) finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc), +0x10 timestamps, then id, sizes and geometry (layout in
 * sweet_surface_fill_result). Every surface is wired kernel memory mapped
 * into each task that creates or looks it up; mappings are counted per
 * client and dropped on release or close.
 *
 * Values, notifications, accelerators and shared events are accepted and
 * ignored (logged once per selector). Wrapping client memory (sel 6/7)
 * wires the caller's pages (IOMemoryDescriptor::withAddressRange).
 */
#include "SweetSurface.h"

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>
#include <sys/proc.h>

enum {
	kSweetSurfaceCreate             = 0,
	kSweetSurfaceLookup             = 4,
	kSweetSurfaceLimits             = 13,
	kSweetSurfaceLookupFromPort     = 34,
	kSweetSurfaceCreatePort         = 35,
	kSweetSurfaceWrapClientImage    = 6,    /* iPadOS 18: struct in {addr, w, h, fmt, bpe, bpr, size} */
	kSweetSurfaceWrapClientMemory   = 7,    /* iPadOS 18: scalars [addr, size] */
};

enum {
	kSweetSurfaceTrapIncrementUse   = 0,
	kSweetSurfaceTrapDecrementUse   = 1,
	kSweetSurfaceTrapLock           = 2,
	kSweetSurfaceTrapUnlock         = 3,
	kSweetSurfaceTrapRelease        = 4,
	kSweetSurfaceTrapReleaseFailed  = 5,
};

#define SWEET_SURFACE_RESULT_SIZE       0xa68   /* iOS 16 lock result */
#define SWEET_SURFACE_RESULT_SIZE_18    0xc68   /* iPadOS 18: 64-bit geometry, 0x98-byte planes */
#define SWEET_SURFACE_MAX_ID            4096
#define SWEET_SURFACE_MAX_DIM           16384
#define SWEET_SURFACE_MAX_ALLOC         (256u << 20)
#define SWEET_SURFACE_MAX_PROPS         (64u << 10)
#define SWEET_SURFACE_ROW_ALIGN         64
#define SWEET_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define SWEET_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define kIOSurfaceLockReadOnly          0x1

static IOLock       *sweet_surface_lock;
static SweetSurface *sweet_surface_table[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_users[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_next_id = 1;
static uint32_t      sweet_surface_creates_logged;
static uint64_t      sweet_surface_wired;       /* bytes of buffer surfaces */
#define SWEET_SURFACE_WIRED_MAX         (1536ull << 20)

OSDefineMetaClassAndStructors(SweetSurface, OSObject);

void
SweetSurface::free()
{
	if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
		OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&sweet_surface_wired);
	}
	OSSafeReleaseNULL(kernelMap);
	if (prepared && memory != NULL) {
		memory->complete();
	}
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uintptr_t
SweetSurface::kernelAddress()
{
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		return (uintptr_t)buffer->getBytesNoCopy();
	}
	IOLockLock(sweet_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		/*
		 * The surface's own cache mode: the attribute is the physical pages', for every
		 * mapping of them, and client memory or a wrapped image is part of a task's heap,
		 * which must stay cacheable (atomics on Normal-NC take external aborts).
		 * Copy-back rather than kIOMapDefaultCache (see StarbiteMali's mmapBO).
		 */
		IOOptionBits cache = mapCache & kIOMapCacheMask;
		kernelMap = memory->map(cache != kIOMapDefaultCache ? cache : kIOMapCopybackCache);
	}
	IOLockUnlock(sweet_surface_lock);
	return kernelMap != NULL ? (uintptr_t)kernelMap->getVirtualAddress() : 0;
}

static uint32_t *
sweet_surface_seed(SweetSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_SEED);
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
sweet_surface_insert_locked(SweetSurface *s)
{
	for (uint32_t n = 0; n < SWEET_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = sweet_surface_next_id;
		sweet_surface_next_id = id + 1 < SWEET_SURFACE_MAX_ID ? id + 1 : 1;
		if (sweet_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			sweet_surface_table[id] = s;
			sweet_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
sweet_surface_get(SweetSurface *s)
{
	bool ok = true;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s) {
		sweet_surface_users[s->id]++;
	} else if (sweet_surface_table[s->id] == NULL) {
		s->retain();
		sweet_surface_table[s->id] = s;
		sweet_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
sweet_surface_put(SweetSurface *s)
{
	bool drop = false;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s && --sweet_surface_users[s->id] == 0) {
		sweet_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(sweet_surface_lock);
	if (drop) {
		s->release();
	}
}

SweetSurface *
sweet_surface_copy(uint32_t id)
{
	SweetSurface *s = NULL;
	if (id == 0 || id >= SWEET_SURFACE_MAX_ID || sweet_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(sweet_surface_lock);
	s = sweet_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(sweet_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
sweet_surface_init_lock(void)
{
	if (sweet_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static SweetSurface *
sweet_surface_alloc(void)
{
	sweet_surface_init_lock();
	SweetSurface *s = OSTypeAlloc(SweetSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	return s;
}

static bool
sweet_surface_publish(SweetSurface *s)
{
	bool ok;
	IOLockLock(sweet_surface_lock);
	ok = sweet_surface_insert_locked(s);
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

SweetSurface *
sweet_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory == NULL || !sweet_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

/*
 * Tasks map memory in 16 KiB pages on this kernel (4 KiB pages): a surface mapped into one
 * must fill its last user page, or what is past its end in that page has no memory behind it
 * and touching it is a bus error.
 */
#define SWEET_SURFACE_PAGE      16384ULL
#define sweet_surface_round(x)  (((uint64_t)(x) + SWEET_SURFACE_PAGE - 1) & ~(SWEET_SURFACE_PAGE - 1))

SweetSurface *
sweet_surface_create_buffer(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = (width * 4 + SWEET_SURFACE_ROW_ALIGN - 1) & ~(uint32_t)(SWEET_SURFACE_ROW_ALIGN - 1);
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)sweet_surface_round((uint64_t)s->bytesPerRow * height);
	IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
	if (mem == NULL) {
		s->release();
		return NULL;
	}
	OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
	bzero(mem->getBytesNoCopy(), s->allocSize);
	s->memory = mem;
	s->mapCache = kIOMapDefaultCache;
	if (!sweet_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
sweet_surface_port_in_task(SweetSurface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
sweet_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static uint32_t
sweet_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
sweet_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)sweet_div_up(width, ew) * bpe;
	return (uint32_t)((row + SWEET_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(SWEET_SURFACE_ROW_ALIGN - 1));
}

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
sweet_surface_layout(SweetSurface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = sweet_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = sweet_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)sweet_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = sweet_dict_u32(props, "IOSurfacePixelFormat", 0);
	s->cacheMode = sweet_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = sweet_dict_u32(props, "IOSurfaceOffset", 0);
	if (s->width > SWEET_SURFACE_MAX_DIM || s->height > SWEET_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = sweet_dict_u32(props, "IOSurfaceBytesPerRow",
	    sweet_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)sweet_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * sweet_div_up(s->height, s->elementHeight);

	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		if (planes->getCount() > SWEET_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct sweet_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = sweet_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = sweet_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)sweet_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > SWEET_SURFACE_MAX_DIM || p->height > SWEET_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = sweet_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    sweet_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = sweet_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = sweet_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * sweet_div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
	}

	uint64_t alloc = sweet_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	alloc = sweet_surface_round(alloc);
	if (alloc == 0 || alloc > SWEET_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class SweetSurfaceRef : public OSObject {
	OSDeclareDefaultStructors(SweetSurfaceRef);

public:
	SweetSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetSurfaceRef, OSObject);

void
SweetSurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		sweet_surface_put(surface);
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	sweet_surface_init_lock();
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("SWEET SURF: %s registered\n", getName());
	return true;
}

/*
 * macOS IOSurface.framework (26.x; read in macOS 27.2's ____ioSurfaceConnectInternal_block_invoke)
 * finds the root with IOServiceNameMatching("IOSurfaceRoot"); iOS 16-26 use "IOCoreSurfaceRoot". The
 * same root under the macOS name: a second personality of the same code (one surface table, one lock,
 * the same IOSurfaceRootUserClient). iOS never asks for this name.
 */
class IOSurfaceRoot : public IOCoreSurfaceRoot {
	OSDeclareDefaultStructors(IOSurfaceRoot);
};

OSDefineMetaClassAndStructors(IOSurfaceRoot, IOCoreSurfaceRoot);

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;

	SweetSurfaceRef *findRef(uint32_t id, unsigned int *index);
	IOReturn addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(SweetSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn wrap(uint32_t selector, IOExternalMethodArguments *args);
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethodInner(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference);
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t sweet_surf_logged;      /* selectors / traps already logged (< 64) */

static void
sweet_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (sweet_surf_logged & (1ULL << bit))) {
		return;
	}
	sweet_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET SURF: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
SweetSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 *
 * A lookup of a surface this client already has maps nothing and counts
 * nothing (*existing): IOSurface.framework then gets base address -1 in the
 * lock result and reuses its cached IOSurfaceClient. Counting it instead
 * made the framework build a second client for the same id; releasing that
 * one (traps 4 + 5) unmapped the memory under the first (backboardd
 * snapshot crash in CGDataProviderCreateWithData).
 */
IOReturn
IOSurfaceRootUserClient::addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
		IOLockUnlock(fLock);
		sweet_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(SweetSurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		sweet_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	SweetSurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
	fRefs->setObject(ref);
	IOLockUnlock(fLock);
	ref->release();         /* fRefs holds it */
	*refOut = ref;
	return kIOReturnSuccess;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

/*
 * iPadOS 18's lock result (copied to IOSurfaceClient + 0x70; offsets from the
 * IOSurfaceClientGet* accessors): the geometry is 64-bit now, the header is
 * 0x88 bytes and each of the 20 plane records 0x98 bytes.
 */
static void
sweet_surface_fill_result_18(SweetSurface *s, SweetSurfaceRef *ref, uint8_t *out)
{
	bzero(out, SWEET_SURFACE_RESULT_SIZE_18);
	put64(out, 0x00, ref->data->getAddress());
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + SWEET_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put64(out, 0x20, s->allocSize);
	put64(out, 0x28, s->width);
	put64(out, 0x30, s->height);
	put64(out, 0x38, s->bytesPerRow);
	put64(out, 0x40, s->offset);
	put32(out, 0x48, s->pixelFormat);
	put64(out, 0x50, s->allocSize);
	put32(out, 0x58, s->planeCount);
	memcpy(out + 0x60, &s->bytesPerElement, 2);
	out[0x62] = s->elementWidth;
	out[0x63] = s->elementHeight;
	put32(out, 0x64, s->cacheMode);
	put32(out, 0x70, s->parentID);
	put32(out, 0x74, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x88 + i * 0x98;
		const struct sweet_surface_plane *pl = &s->planes[i];
		put64(p, 0x00, pl->width);
		put64(p, 0x08, pl->height);
		put64(p, 0x18, pl->offset);
		put64(p, 0x20, pl->bytesPerRow);
		put64(p, 0x28, pl->size);
		memcpy(p + 0x30, &pl->bytesPerElement, 2);
		p[0x32] = pl->elementWidth;
		p[0x33] = pl->elementHeight;
	}
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x68). */
static void
sweet_surface_fill_result(SweetSurface *s, SweetSurfaceRef *ref, uint8_t *out)
{
	bzero(out, SWEET_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress());
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + SWEET_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x68 + i * 0x80;
		const struct sweet_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		put32(p, 0x08, pl->bytesPerRow);
		put32(p, 0x0c, pl->offset);
		put32(p, 0x14, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

/* How many bytes the caller can take back (picks the iOS 16 or 18 layout). */
static uint32_t
sweet_struct_out_capacity(IOExternalMethodArguments *args)
{
	if (args->structureOutputDescriptor != NULL) {
		return (uint32_t)args->structureOutputDescriptor->getLength();
	}
	return args->structureOutput != NULL ? args->structureOutputSize : 0;
}

static IOReturn
sweet_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(SweetSurface *s, IOExternalMethodArguments *args)
{
	SweetSurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	bool v18 = sweet_struct_out_capacity(args) >= SWEET_SURFACE_RESULT_SIZE_18;
	uint32_t size = v18 ? SWEET_SURFACE_RESULT_SIZE_18 : SWEET_SURFACE_RESULT_SIZE;
	uint8_t *out = (uint8_t *)IOMallocZeroData(size);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		if (v18) {
			sweet_surface_fill_result_18(s, ref, out);
		} else {
			sweet_surface_fill_result(s, ref, out);
		}
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* "already yours": use the cached client */
		}
		ret = sweet_struct_out(args, out, size);
		IOFreeData(out, size);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
		trapRelease((void *)(uintptr_t)s->id, NULL, NULL, NULL, NULL, NULL);
	}
	return ret;
}

/*
 * Back s with `size` bytes of task's own memory at `address` (any alignment): the pages are wired
 * for the surface's lifetime, and s->offset grows by where the address falls in its page. Writable
 * if the pages are (an image's pixels in malloc'd memory); else read-only (an image mapped from
 * a file: boot51's "cannot wire").
 */
static IOReturn
sweet_surface_wrap_memory(SweetSurface *s, task_t task, uint64_t address, uint64_t size)
{
	/* (the task owns the whole 16 KiB page the address is in) */
	uint64_t lead = address & (SWEET_SURFACE_PAGE - 1);
	uint64_t length = sweet_surface_round(lead + size);
	bool readOnly = false;

	if (size == 0 || length > SWEET_SURFACE_MAX_ALLOC) {
		return kIOReturnBadArgument;
	}
	IOReturn rw = kIOReturnSuccess, ro = kIOReturnSuccess;
	IOMemoryDescriptor *mem = IOMemoryDescriptor::withAddressRange(address - lead, length, kIODirectionInOut, task);
	if (mem != NULL && (rw = mem->prepare()) != kIOReturnSuccess) {
		mem->release();
		mem = IOMemoryDescriptor::withAddressRange(address - lead, length, kIODirectionOut, task);
		readOnly = true;
		if (mem != NULL && (ro = mem->prepare()) != kIOReturnSuccess) {
			OSSafeReleaseNULL(mem);
		}
	}
	if (mem == NULL) {
		/*
		 * iOS 27 on STARLTE (iOS-target kernel): prepare() refuses some of backboardd's image memory
		 * (CA wrap_buffer, Settings' 80x80 icons). The caller is the task itself (IOSurface create /
		 * wrap from backboardd), so take a snapshot instead: a kernel buffer filled by copyin. Images
		 * do not change after they are wrapped; the surface simply no longer aliases the task's pages.
		 */
		static uint32_t failed, copied;
		IOBufferMemoryDescriptor *buf = NULL;
		bool ok = false;
		if (task == current_task() && (uint64_t)OSAddAtomic64((SInt64)length, (volatile SInt64 *)&sweet_surface_wired) +
		    length <= SWEET_SURFACE_WIRED_MAX) {
			buf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
			    length, page_size);
			if (buf != NULL) {
				bzero(buf->getBytesNoCopy(), length);
				ok = copyin((user_addr_t)address, (char *)buf->getBytesNoCopy() + lead, (vm_size_t)size) == 0;
			}
			if (!ok) {
				OSSafeReleaseNULL(buf);
				OSAddAtomic64(-(SInt64)length, (volatile SInt64 *)&sweet_surface_wired);
			}
		}
		if (ok) {
			if (++copied <= 5 || (copied % 500) == 0) {
				IOLog("SWEET SURF: %#llx+%#llx not wireable (prepare %#x/%#x): copied instead (%u so far)\n",
				    address, size, rw, ro, copied);
			}
			s->memory = buf;
			s->prepared = false;
			s->offset += (uint32_t)lead;
			s->allocSize = (uint32_t)length;
			s->mapCache = kIOMapDefaultCache;
			return kIOReturnSuccess;
		}
		if (++failed <= 10 || (failed % 500) == 0) {
			IOLog("SWEET SURF: cannot wire %#llx+%#llx (prepare %#x/%#x; %u failures)\n", address, size, rw, ro, failed);
		}
		return kIOReturnVMError;
	}
	s->memory = mem;
	s->prepared = true;
	s->offset += (uint32_t)lead;
	s->allocSize = (uint32_t)length;
	s->mapCache = kIOMapDefaultCache | (readOnly ? kIOMapReadOnly : 0);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const char *buf = (const char *)args->structureInput;
	uint32_t len = args->structureInputSize;
	char *copy = NULL;
	IOReturn ret;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > SWEET_SURFACE_MAX_PROPS) {
			return kIOReturnBadArgument;
		}
		copy = (char *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		args->structureInputDescriptor->readBytes(0, copy, len);
		buf = copy;
	}
	OSObject *obj = (buf != NULL && len > 0) ? OSUnserializeXML(buf, len) : NULL;
	if (copy != NULL) {
		IOFreeData(copy, len);
	}
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("SWEET SURF: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = sweet_surface_layout(s, props);
	OSNumber *client = OSDynamicCast(OSNumber, props->getObject("IOSurfaceAddress"));
	if (ret == kIOReturnSuccess && client != NULL) {
		/*
		 * kIOSurfaceClientAddress ("IOSurfaceAddress"): a surface around the caller's memory
		 * (CA::Render::Image::retain_iosurface makes one of an image's pixels: icons; boot49-55
		 * had them all zeros from a fresh buffer).
		 */
		uint64_t need = sweet_dict_u32(props, "IOSurfaceAllocSize", 0);
		uint64_t end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * sweet_div_up(s->height, s->elementHeight);
		if (s->planeCount > 0 || need < end) {
			need = s->planeCount > 0 ? s->allocSize : end;
		}
		ret = sweet_surface_wrap_memory(s, fTask, client->unsigned64BitValue(), need);
		if (ret == kIOReturnSuccess && !sweet_surface_publish(s)) {
			ret = kIOReturnNoResources;
		}
		static unsigned told;
		if (told < 40) {
			/* (whether the memory holds a picture when the surface is made) */
			uintptr_t k = ret == kIOReturnSuccess ? s->kernelAddress() : 0;
			uint64_t set = 0, look = need < 65536 ? need : 65536;
			for (uint64_t i = 0; k != 0 && i < look; i++) {
				set += ((const uint8_t *)k)[s->offset + i] != 0;
			}
			char name[32];
			proc_selfname(name, sizeof(name));
			told++;
			IOLog("SWEET SURF: %s client memory id %u %ux%u fmt 0x%x at %#llx+%#llx (offset %u)%s: %llu of %llu bytes set -> 0x%x\n",
			    name, s->id, s->width, s->height, s->pixelFormat, client->unsigned64BitValue(), need, s->offset,
			    s->mapCache & kIOMapReadOnly ? " ro" : "", set, look, ret);
		}
	} else if (ret == kIOReturnSuccess) {
		IOBufferMemoryDescriptor *mem = NULL;
		if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired) +
		    s->allocSize <= SWEET_SURFACE_WIRED_MAX) {
			mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
		}
		if (mem == NULL) {
			OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
			ret = kIOReturnNoMemory;
		} else {
			bzero(mem->getBytesNoCopy(), s->allocSize);
			s->memory = mem;
			s->mapCache = kIOMapDefaultCache;
			if (!sweet_surface_publish(s)) {
				ret = kIOReturnNoResources;
			}
		}
	}
	if (ret != kIOReturnSuccess || sweet_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		sweet_surface_creates_logged++;
		IOLog("SWEET SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u%s -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(),
		    client != NULL ? (s->mapCache & kIOMapReadOnly ? " client memory (ro)" : " client memory") : "", ret);
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

/*
 * sel 6: struct in { u64 address; u32 width, height, pixelFormat, bytesPerElement,
 * bytesPerRow, size } (0x20 bytes); sel 7: scalars [address, size], a bare range.
 * The pages are wired for the surface's lifetime (prepare/complete).
 */
IOReturn
IOSurfaceRootUserClient::wrap(uint32_t selector, IOExternalMethodArguments *args)
{
	uint64_t address, size;
	uint32_t width = 0, height = 0, pixelFormat = 0, bytesPerElement = 1, bytesPerRow = 0;

	if (selector == kSweetSurfaceWrapClientImage) {
		const uint8_t *in = (const uint8_t *)args->structureInput;
		if (in == NULL || args->structureInputSize < 0x20) {
			return kIOReturnBadArgument;
		}
		memcpy(&address, in, 8);
		memcpy(&width, in + 0x8, 4);
		memcpy(&height, in + 0xc, 4);
		memcpy(&pixelFormat, in + 0x10, 4);
		memcpy(&bytesPerElement, in + 0x14, 4);
		memcpy(&bytesPerRow, in + 0x18, 4);
		uint32_t size32;
		memcpy(&size32, in + 0x1c, 4);
		size = size32;
	} else {
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		address = args->scalarInput[0];
		size = args->scalarInput[1];
	}
	/* (the address is rarely page aligned, and the size often ends with the last row's pixels) */
	uint64_t rows = (height > 0 ? (uint64_t)bytesPerRow * (height - 1) : 0) + (uint64_t)width * bytesPerElement;
	if (size == 0 || size > SWEET_SURFACE_MAX_ALLOC || width > SWEET_SURFACE_MAX_DIM ||
	    height > SWEET_SURFACE_MAX_DIM || bytesPerElement > 64 || rows > size) {
		IOLog("SWEET SURF: wrap: bad range %#llx+%#llx %ux%u bpr %u\n", address, size, width, height, bytesPerRow);
		return kIOReturnBadArgument;
	}
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return kIOReturnNoMemory;
	}
	s->width = width;
	s->height = height;
	s->pixelFormat = pixelFormat;
	s->bytesPerElement = (uint16_t)bytesPerElement;
	s->elementWidth = s->elementHeight = 1;
	s->bytesPerRow = bytesPerRow;
	IOReturn wired = sweet_surface_wrap_memory(s, fTask, address, size);
	if (wired != kIOReturnSuccess) {
		s->release();
		return wired;
	}
	if (!sweet_surface_publish(s)) {
		s->release();
		return kIOReturnNoResources;
	}
	if (sweet_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		sweet_surface_creates_logged++;
		IOLog("SWEET SURF: %s wrap id %u %ux%u fmt 0x%x bpe %u bpr %u at %#llx size %u%s\n", name, s->id,
		    width, height, pixelFormat, bytesPerElement, bytesPerRow, address, s->allocSize,
		    s->mapCache & kIOMapReadOnly ? " (ro)" : "");
	}
	IOReturn ret = returnSurface(s, args);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	/* sweetsurfdbg=1: every call (first 60) with its sizes and result, for new userlands */
	static int dbg = -1, left = 60;
	if (dbg < 0) {
		dbg = 0;
		PE_parse_boot_argn("sweetsurfdbg", &dbg, sizeof(dbg));
	}
	if (dbg && left > 0) {
		left--;
		IOReturn r = externalMethodInner(selector, args, dispatch, target, reference);
		IOLog("SWEET SURF dbg: sel %u sin %u [%#llx %#llx] in %u%s out %u%s osc %u -> %#x\n", selector,
		    args->scalarInputCount, args->scalarInputCount > 0 ? args->scalarInput[0] : 0,
		    args->scalarInputCount > 1 ? args->scalarInput[1] : 0, args->structureInputSize,
		    args->structureInputDescriptor ? "(desc)" : "", args->structureOutputSize,
		    args->structureOutputDescriptor ? "(desc)" : "", args->scalarOutputCount, r);
		return r;
	}
	return externalMethodInner(selector, args, dispatch, target, reference);
}

IOReturn
IOSurfaceRootUserClient::externalMethodInner(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kSweetSurfaceLimits: {
		/* iOS 16: 0x18 bytes, ABI version u16 at +0x16. iPadOS 18: 0x28
		 * bytes (middle fields widened to u64), version at +0x22. The
		 * framework closes the connection unless the version is 1. */
		uint8_t limits[0x28] = { 0 };
		uint32_t len = sweet_struct_out_capacity(args) >= 0x28 ? 0x28 : 0x18;
		put32(limits, 0x0, SWEET_SURFACE_ROW_ALIGN - 1);
		put32(limits, 0x4, SWEET_SURFACE_ROW_ALIGN - 1);
		limits[len == 0x28 ? 0x22 : 0x16] = 1;
		/*
		 * macOS 26.5 IOSurface.framework (IOSurfaceClientGetPropertyMaximum) reads
		 * the 0x28 form's maxima: +0x08 bytes per row, +0x10 width, +0x18 height
		 * (u64; with zeros every maximum is 0). +0x0 / +0x4 are the offset and
		 * row alignment masks (IOSurfaceClientAlignProperty). Filled for macOS
		 * callers only (PLATFORM_MACOS = 1): iOS 26 keeps the answer it boots with.
		 */
		if (len == 0x28 && proc_platform(current_proc()) == 1) {
			put64(limits, 0x08, (uint64_t)SWEET_SURFACE_MAX_DIM * 64);
			put64(limits, 0x10, SWEET_SURFACE_MAX_DIM);
			put64(limits, 0x18, SWEET_SURFACE_MAX_DIM);
		}
		sweet_surf_log_once("sel", selector);
		return sweet_struct_out(args, limits, len);
	}
	case kSweetSurfaceCreate:
		return create(args);
	case kSweetSurfaceLookup: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = sweet_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		SweetSurface *s = OSDynamicCast(SweetSurface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && sweet_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceCreatePort: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = sweet_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	case kSweetSurfaceWrapClientImage:
	case kSweetSurfaceWrapClientMemory:
		/*
		 * A surface over the caller's own memory (CA::Render::Image::retain_iosurface wraps
		 * page-aligned image data so the GPU can read it in place: icons, glyphs). The
		 * framework builds a client from whatever lock result comes back, so this must
		 * either succeed properly or fail: refused, CoreAnimation's fallback copy came out
		 * all zeros on this kernel (boot49), so it is done properly.
		 */
		sweet_surf_log_once("sel", selector);
		return wrap(selector, args);
	default:
		/* Values, notifications, ownership, timestamps...: accept and ignore. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
sweet_surface_seed_out(SweetSurface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)sweet_surface_seed(s)) + 1 :
	    *sweet_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static SweetSurface *
sweet_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	SweetSurface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, __unused void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = sweet_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = sweet_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index);
	if (ref != NULL && --ref->count == 0) {
		fRefs->removeObject(index);     /* unmaps and drops the user count */
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap sweet_surface_traps[] = {
	[kSweetSurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kSweetSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kSweetSurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kSweetSurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	[kSweetSurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
	[kSweetSurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(sweet_surface_traps) / sizeof(sweet_surface_traps[0])) {
		sweet_surf_log_once("trap", index);
		return NULL;
	}
	sweet_surf_log_once("trap", index);
	*targetP = this;
	return &sweet_surface_traps[index];
}
#endif /* SWEET */
