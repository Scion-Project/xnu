/*
 * IOHIDSystem for macOS userland (Starbite): the legacy event driver macOS's WindowServer opens
 * before anything else. SkyLight's WSInitialize (macOS 26.5) does
 *   IOServiceGetMatchingServices(IOServiceMatching("IOHIDSystem")) -> IOServiceOpen(type 0)
 *   -> IOHIDCreateSharedMemory(connect, 4) -> IOHIDSetEventsEnable(connect, 1)
 *   -> IOConnectMapMemory(connect, kIOHIDGlobalMemory, ...) -> mach_make_memory_entry,
 * and calls exit(1) after "Cannot open the HID and display/cursor system." when any step fails
 * (no IOHIDSystem here: WindowServer exited about 1 s after start, respawned every ~10 s).
 * It also reads "HIDParameters" from IOService:/IOResources/IOHIDSystem.
 *
 * Methods as the 26.5 KDK's IOHIDUserClient::getTargetAndMethodForIndex table (legacy
 * IOExternalMethod, scalar in/out counts): 0 createShmem(1), 1 setEventsEnable(1),
 * 2 setCursorEnable(1), 3 postEvent(struct 72), 4 setMouseLocation(struct), 5 getButtonEventNum(1 -> 1),
 * 6 setBounds(struct 8), 7 registerVirtualDisplay(0 -> 1), 8 unregisterVirtualDisplay(1),
 * 9 setVirtualDisplayBounds(5), 10 getUserHidActivityState(0 -> 1), 11 setContinuousCursorEnable(1),
 * 12 setOnScreenBounds(struct 12). Everything is accepted; the shared memory is EvOffsets + EvGlobals
 * set up as IOHIDSystem::initShmem does (version 4, structSize, an empty circular event queue). No
 * events are posted yet: input reaches macOS through IOHIDEventService (SweetHID) and the HID event
 * system, as on Apple-silicon Macs.
 *
 * Published only when the device tree's root model is a Mac ("Mac...", the Mac ADT), so iOS boots are
 * unchanged; boot-arg sweethidsys=1 / =0 forces it on / off.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include "IOHIDEventServiceUserClient.h"	/* the type headers IOHIDShared.h needs, as there */
#include "IOHIDShared.h"

class IOHIDSystem : public IOService {
	OSDeclareDefaultStructors(IOHIDSystem);

public:
	IOBufferMemoryDescriptor *fShmem;
	IOLock                   *fLock;

	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn createShmem(uint32_t version);
};

OSDefineMetaClassAndStructors(IOHIDSystem, IOService);

class IOHIDUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOHIDUserClient);

public:
	IOHIDSystem *fSystem;
	uint32_t     fType;

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOService *getService(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOHIDUserClient, IOUserClient);

static bool
sweet_hidsys_is_mac(void)
{
	IORegistryEntry *root = IORegistryEntry::fromPath("/", gIODTPlane);
	bool mac = false;
	if (root != NULL) {
		OSData *model = OSDynamicCast(OSData, root->getProperty("model"));
		mac = model != NULL && model->getLength() >= 3 && memcmp(model->getBytesNoCopy(), "Mac", 3) == 0;
		root->release();
	}
	return mac;
}

IOService *
IOHIDSystem::probe(IOService *provider, SInt32 *score)
{
	/* boot-arg sweethidsys=1 / =0 forces it on / off */
	uint32_t force = 2;
	PE_parse_boot_argn("sweethidsys", &force, sizeof(force));
	bool on = force == 2 ? sweet_hidsys_is_mac() : force != 0;
	return on ? IOService::probe(provider, score) : NULL;
}

bool
IOHIDSystem::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	if (fLock == NULL) {
		return false;
	}
	/*
	 * IOKit's IOHIDCopyCFTypeParameter (NXClickTime, IOHIDGetParameter ...) asks IOConnectGetService
	 * for this service, looks the key up in "HIDParameters", then in the service's own properties, and
	 * only then asks the HID event system (com.apple.iohideventsystem). WindowServer serves that one
	 * itself and calls NXClickTime on its main thread before it serves: with no answer here it waits on
	 * itself for ever (26.5 boot: main thread in io_hideventsystem_open under NXClickTime). The values
	 * are a real Mac's defaults (macOS 27.2, ioreg IOHIDSystem).
	 */
	OSDictionary *params = OSDictionary::withCapacity(64);
	if (params != NULL) {
		static const struct { const char *key; uint64_t value; } numbers[] = {
			{ "HIDClickTime", 500000000ULL }, { "HIDKeyRepeat", 83333333ULL }, { "HIDInitialKeyRepeat", 500000000ULL },
			{ "HIDPointerAcceleration", 45056 }, { "HIDScrollAcceleration", 20480 }, { "HIDTrackpadAcceleration", 45056 },
			{ "HIDMouseAcceleration", 45056 }, { "HIDPointerButtonMode", 2 }, { "HIDF12EjectDelay", 250 },
			{ "HIDSlowKeysDelay", 0 }, { "HIDStickyKeysDisabled", 0 }, { "HIDStickyKeysOn", 0 },
			{ "HIDStickyKeysShiftToggles", 0 }, { "HIDMouseKeysOptionToggles", 0 }, { "HIDFKeyMode", 0 },
			{ "HIDScrollZoomModifierMask", 0 }, { "HIDUseLinearScalingMouseAcceleration", 0 },
			{ "MouseButtonDivision", 55 }, { "Clicking", 0 }, { "Dragging", 0 }, { "DragLock", 0 },
			{ "TrackpadRightClick", 1 }, { "TrackpadCornerSecondaryClick", 0 }, { "TrackpadHorizScroll", 1 },
			{ "TrackpadPinch", 1 }, { "TrackpadRotate", 1 }, { "TrackpadTwoFingerDoubleTapGesture", 1 },
			{ "TrackpadThreeFingerTapGesture", 0 }, { "TrackpadThreeFingerHorizSwipeGesture", 2 },
			{ "TrackpadThreeFingerVertSwipeGesture", 2 }, { "TrackpadFourFingerHorizSwipeGesture", 2 },
			{ "TrackpadFourFingerVertSwipeGesture", 2 }, { "TrackpadFourFingerPinchGesture", 2 },
			{ "TrackpadFiveFingerPinchGesture", 2 }, { "TrackpadTwoFingerFromRightEdgeSwipeGesture", 3 },
			{ "MouseTwoFingerDoubleTapGesture", 3 }, { "MouseTwoFingerHorizSwipeGesture", 2 },
			{ "MouseOneFingerDoubleTapGesture", 0 }, { "USBMouseStopsTrackpad", 0 }, { "FirstClickThreshold", 0 },
			{ "SecondClickThreshold", 0 }, { "EjectDelay", 0 }, { "version", 1 },
		};
		for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
			OSNumber *n = OSNumber::withNumber(numbers[i].value, 64);
			if (n != NULL) {
				params->setObject(numbers[i].key, n);
				n->release();
			}
		}
		static const char *yes[] = { "TrackpadScroll", "TrackpadMomentumScroll", "TrackpadHandResting", "MouseMomentumScroll",
			                     "MouseVerticalScroll", "MouseHorizontalScroll", "UserPreferences", "HIDDefaultParameters" };
		for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
			params->setObject(yes[i], kOSBooleanTrue);
		}
		params->setObject("TrackpadThreeFingerDrag", kOSBooleanFalse);
		params->setObject("ForceSuppressed", kOSBooleanFalse);
		OSString *mode = OSString::withCString("OneButton");
		if (mode != NULL) {
			params->setObject("MouseButtonMode", mode);
			mode->release();
		}
		OSArray *pairs = OSArray::withCapacity(0);
		if (pairs != NULL) {
			params->setObject("HIDKeyboardModifierMappingPairs", pairs);
			pairs->release();
		}
		OSNumber *zero = OSNumber::withNumber(0ULL, 32);
		OSArray *space = OSArray::withCapacity(2);
		if (zero != NULL && space != NULL) {
			space->setObject(zero);
			space->setObject(zero);
			params->setObject("HIDClickSpace", space);
		}
		OSSafeReleaseNULL(space);
		OSSafeReleaseNULL(zero);
		setProperty("HIDParameters", params);
		params->release();
	}
	/* the service's own copies of the scroll-count tuning a real IOHIDSystem shows */
	static const struct { const char *key; uint64_t value; } own[] = {
		{ "HIDScrollCountMax", 2000 }, { "HIDScrollCountMinDeltaToStart", 30 }, { "HIDScrollCountMinDeltaToSustain", 20 },
		{ "HIDScrollCountMaxTimeDeltaBetween", 600 }, { "HIDScrollCountMaxTimeDeltaToSustain", 250 },
		{ "HIDScrollCountAccelerationFactor", 163840 }, { "HIDPowerOnDelayNS", 500000000ULL }, { "HIDIdleTime", 0 },
	};
	for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
		setProperty(own[i].key, own[i].value, 64);
	}
	setProperty(kIOUserClientClassKey, "IOHIDUserClient");
	registerService();
	IOLog("SWEET HIDSYS: IOHIDSystem published for macOS (EvGlobals %u bytes)\n", (unsigned)sizeof(EvGlobals));
	return true;
}

IOReturn
IOHIDSystem::createShmem(uint32_t version)
{
	IOLockLock(fLock);
	if (fShmem == NULL) {
		size_t size = round_page(sizeof(EvOffsets) + sizeof(EvGlobals));
		fShmem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, size, page_size);
		if (fShmem != NULL) {
			uint8_t *base = (uint8_t *)fShmem->getBytesNoCopy();
			bzero(base, size);
			EvOffsets *eop = (EvOffsets *)base;
			eop->evGlobalsOffset = sizeof(EvOffsets);
			eop->evShmemOffset = (int)(sizeof(EvOffsets) + sizeof(EvGlobals));
			EvGlobals *evg = (EvGlobals *)(base + sizeof(EvOffsets));
			evg->version = kIOHIDCurrentShmemVersion;
			evg->structSize = sizeof(EvGlobals);
			evg->LLEHead = 1;
			evg->LLETail = 1;
			evg->LLELast = 0;
			for (int i = 0; i < LLEQSIZE; i++) {
				evg->lleq[i].next = (i + 1) % LLEQSIZE;
			}
			evg->waitThreshold = 1000;
		}
	}
	IOReturn ret = fShmem != NULL ? kIOReturnSuccess : kIOReturnNoMemory;
	IOLockUnlock(fLock);
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET HIDSYS: %s createShmem version %u -> 0x%x\n", name, version, ret);
	return ret;
}

bool
IOHIDUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	fType = type;
	return IOUserClient::initWithTask(owningTask, securityID, type);
}

bool
IOHIDUserClient::start(IOService *provider)
{
	fSystem = OSDynamicCast(IOHIDSystem, provider);
	return fSystem != NULL && IOUserClient::start(provider);
}

IOReturn
IOHIDUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* IOConnectGetService(connect): what IOHIDCopyCFTypeParameter reads HIDParameters from (the base returns NULL) */
IOService *
IOHIDUserClient::getService(void)
{
	return fSystem;
}

IOReturn
IOHIDUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	(void)port;
	(void)type;
	(void)refCon;
	return kIOReturnSuccess;
}

IOReturn
IOHIDUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	if (type != kIOHIDGlobalMemory || fSystem->fShmem == NULL) {
		return kIOReturnBadArgument;
	}
	fSystem->fShmem->retain();
	*memory = fSystem->fShmem;
	*options = 0;
	return kIOReturnSuccess;
}

IOReturn
IOHIDUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
    OSObject *target, void *reference)
{
	(void)dispatch;
	(void)target;
	(void)reference;
	static uint32_t logged;
	IOReturn ret = kIOReturnSuccess;

	switch (selector) {
	case 0:         /* createShmem(version) */
		ret = fSystem->createShmem(args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0);
		break;
	case 7:         /* registerVirtualDisplay -> display token */
		if (args->scalarOutputCount > 0) {
			args->scalarOutput[0] = 1;
		}
		break;
	default:        /* 5 getButtonEventNum, 10 getUserHidActivityState: 0; the rest: accepted */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		break;
	}
	if (selector < 32 && !(logged & (1u << selector))) {
		char name[32];
		logged |= 1u << selector;
		proc_selfname(name, sizeof(name));
		IOLog("SWEET HIDSYS: %s type %u sel %u in %u/%u -> 0x%x\n", name, fType, selector, args->scalarInputCount,
		    args->structureInputSize, ret);
	}
	return ret;
}
#endif
