/*
 * SWEET: HID event services for the iOS userland (backboardd's HID event
 * system).
 *
 * backboardd watches for IOService objects with HIDServiceSupport = true,
 * opens them with IOServiceOpen type 'esuc' (IOHIDEventServiceUserClient,
 * compiled here from IOHIDFamily-1915.140.3, the version the iOS 16.7
 * userland ships) and reads serialized IOHIDEvents from the user client's
 * shared queue. This file has the minimal IOHIDEventService base class and
 * two services:
 *
 *   SweetHIDButtons  consumer-control keys (power, volume, home)
 *   SweetHIDTouch    the built-in touch screen (digitizer collections); on a
 *                    macOS boot an absolute pointer instead (see below)
 *
 * Hardware drivers feed them through the C entry points sweet_hid_key()
 * and sweet_hid_touch(). Boot-arg sweethidtest=N (seconds) presses Home
 * every N seconds, to test the event path without input hardware.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>

#include "IOHIDEventService.h"
#include "IOHIDEventServiceUserClient.h"
#include "IOHIDEvent.h"
#include "IOHIDKeys.h"
#include "IOHIDPrivateKeys.h"
#include "IOHIDUsageTables.h"

extern "C" uint64_t mach_absolute_time(void);

/* One open client: the user client and its event callback. */
class IOHIDEventServiceClient : public OSObject {
	OSDeclareDefaultStructors(IOHIDEventServiceClient);
public:
	bool                       initClient() { return OSObject::init(); }
	IOService                 *client;
	void                      *context;
	IOHIDEventService::Action  action;
};
OSDefineMetaClassAndStructors(IOHIDEventServiceClient, OSObject);

#define super IOService
OSDefineMetaClassAndStructors(IOHIDEventService, IOService);

bool
IOHIDEventService::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	_clientLock = IOLockAlloc();
	_clients = OSArray::withCapacity(2);
	_hidWorkLoop = getWorkLoop();
	if (!_clientLock || !_clients || !_hidWorkLoop) {
		return false;
	}
	_hidWorkLoop->retain();
	_hidGate = IOCommandGate::commandGate(this);
	if (!_hidGate || _hidWorkLoop->addEventSource(_hidGate) != kIOReturnSuccess) {
		return false;
	}

	OSString *transport = getTransport();
	OSString *product = getProduct();
	setProperty(kIOHIDTransportKey, transport);
	setProperty(kIOHIDProductKey, product);
	setProperty(kIOHIDManufacturerKey, "Sweet");
	setProperty(kIOHIDVendorIDKey, getVendorID(), 32);
	setProperty(kIOHIDProductIDKey, getProductID(), 32);
	setProperty(kIOHIDLocationIDKey, (unsigned long long)getProductID(), 32);
	setProperty(kIOHIDReportIntervalKey, getReportInterval(), 32);
	setProperty(kIOHIDPrimaryUsagePageKey, getPrimaryUsagePage(), 32);
	setProperty(kIOHIDPrimaryUsageKey, getPrimaryUsage(), 32);
	setProperty(kIOHIDBuiltInKey, kOSBooleanTrue);
	OSSafeReleaseNULL(transport);
	OSSafeReleaseNULL(product);

	OSDictionary *pair = OSDictionary::withCapacity(2);
	OSArray *pairs = OSArray::withCapacity(1);
	OSNumber *page = OSNumber::withNumber(getPrimaryUsagePage(), 32);
	OSNumber *usage = OSNumber::withNumber(getPrimaryUsage(), 32);
	if (pair && pairs && page && usage) {
		pair->setObject(kIOHIDDeviceUsagePageKey, page);
		pair->setObject(kIOHIDDeviceUsageKey, usage);
		pairs->setObject(pair);
		setProperty(kIOHIDDeviceUsagePairsKey, pairs);
	}
	OSSafeReleaseNULL(pair);
	OSSafeReleaseNULL(pairs);
	OSSafeReleaseNULL(page);
	OSSafeReleaseNULL(usage);

	/* What IOHIDFamily's "IOHIDEventServiceUserClient" personality merges in. */
	setProperty("HIDServiceSupport", kOSBooleanTrue);
	setProperty("IOUserClientClass", "IOHIDEventServiceUserClient");
	return true;
}

void
IOHIDEventService::free(void)
{
	if (_hidGate && _hidWorkLoop) {
		_hidWorkLoop->removeEventSource(_hidGate);
	}
	OSSafeReleaseNULL(_hidGate);
	OSSafeReleaseNULL(_hidWorkLoop);
	OSSafeReleaseNULL(_clients);
	if (_clientLock) {
		IOLockFree(_clientLock);
		_clientLock = NULL;
	}
	super::free();
}

bool
IOHIDEventService::open(IOService *client, IOOptionBits options, void *context, Action action)
{
	IOHIDEventServiceClient *data = OSTypeAlloc(IOHIDEventServiceClient);
	bool ok = false;

	if (data && data->initClient()) {
		data->client = client;
		data->context = context;
		data->action = action;
		ok = super::open(client, options, data);
	}
	OSSafeReleaseNULL(data);
	IOLog("SWEET HID: %s open by %s -> %d\n", getName(), client ? client->getName() : "?", ok);
	return ok;
}

void
IOHIDEventService::close(IOService *forClient, IOOptionBits options)
{
	super::close(forClient, options);
}

bool
IOHIDEventService::handleOpen(IOService *client, IOOptionBits options __unused, void *argument)
{
	IOHIDEventServiceClient *data = OSDynamicCast(IOHIDEventServiceClient, (OSObject *)argument);
	bool ok = false;

	if (!data) {
		return false;
	}
	IOLockLock(_clientLock);
	if (!handleIsOpenLocked(client)) {
		/* Copy on write: dispatchEvent iterates a snapshot without the lock. */
		OSArray *next = OSArray::withArray(_clients, _clients->getCount() + 1);
		if (next && next->setObject(data)) {
			_clients->release();
			_clients = next;
			ok = true;
		} else {
			OSSafeReleaseNULL(next);
		}
	}
	IOLockUnlock(_clientLock);
	return ok;
}

void
IOHIDEventService::handleClose(IOService *client, IOOptionBits options __unused)
{
	IOLockLock(_clientLock);
	OSArray *next = OSArray::withCapacity(_clients->getCount());
	if (next) {
		for (unsigned int i = 0; i < _clients->getCount(); i++) {
			IOHIDEventServiceClient *data = (IOHIDEventServiceClient *)_clients->getObject(i);
			if (data->client != client) {
				next->setObject(data);
			}
		}
		_clients->release();
		_clients = next;
	}
	IOLockUnlock(_clientLock);
}

bool
IOHIDEventService::handleIsOpenLocked(const IOService *client) const
{
	for (unsigned int i = 0; i < _clients->getCount(); i++) {
		IOHIDEventServiceClient *data = (IOHIDEventServiceClient *)_clients->getObject(i);
		if (client == NULL || data->client == client) {
			return true;
		}
	}
	return false;
}

bool
IOHIDEventService::handleIsOpen(const IOService *client) const
{
	IOLockLock(_clientLock);
	bool open = handleIsOpenLocked(client);
	IOLockUnlock(_clientLock);
	return open;
}

void
IOHIDEventService::dispatchEvent(IOHIDEvent *event, IOOptionBits options)
{
	OSArray *clients;

	event->setSenderID(getRegistryEntryID());
	IOLockLock(_clientLock);
	clients = _clients;
	clients->retain();
	IOLockUnlock(_clientLock);

	for (unsigned int i = 0; i < clients->getCount(); i++) {
		IOHIDEventServiceClient *data = (IOHIDEventServiceClient *)clients->getObject(i);
		if (data->action) {
			(*data->action)(data->client, this, data->context, event, options);
		}
	}
	clients->release();
}

IOHIDEvent *
IOHIDEventService::copyEvent(IOHIDEventType type __unused, IOHIDEvent *matching __unused, IOOptionBits options __unused)
{
	return NULL;
}

IOHIDEvent *
IOHIDEventService::copyMatchingEvent(OSDictionary *matching __unused)
{
	return NULL;
}

IOReturn
IOHIDEventService::setElementValue(UInt32 usagePage __unused, UInt32 usage __unused, UInt32 value __unused)
{
	return kIOReturnUnsupported;
}

IOReturn
IOHIDEventService::setProperties(OSObject *properties)
{
	/* The event system pushes its settings here; nothing to configure. */
	return OSDynamicCast(OSDictionary, properties) ? kIOReturnSuccess : kIOReturnBadArgument;
}

UInt32
IOHIDEventService::getPrimaryUsagePage()
{
	return 0;
}

UInt32
IOHIDEventService::getPrimaryUsage()
{
	return 0;
}

OSString *
IOHIDEventService::getTransport()
{
	return OSString::withCString("SPU");
}

OSString *
IOHIDEventService::getProduct()
{
	return OSString::withCString(getName());
}

UInt32
IOHIDEventService::getVendorID()
{
	return 0x05ac;
}

UInt32
IOHIDEventService::getProductID()
{
	return 0;
}

UInt32
IOHIDEventService::getReportInterval()
{
	return 10000;	/* us */
}

IOReturn
IOHIDEventService::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	IOUserClient *client;

	if (type != kIOHIDEventServiceUserClientType) {
		IOLog("SWEET HID: %s user client type 0x%x unsupported\n", getName(), (unsigned)type);
		return super::newUserClient(owningTask, securityID, type, properties, handler);
	}
	client = OSTypeAlloc(IOHIDEventServiceUserClient);
	if (!client) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type)) {
		client->release();
		IOLog("SWEET HID: %s user client init refused\n", getName());
		return kIOReturnBadArgument;
	}
	if (!client->attach(this)) {
		client->release();
		return kIOReturnUnsupported;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnUnsupported;
	}
	*handler = client;
	return kIOReturnSuccess;
}

/*
 * Buttons. iOS takes the side/volume/home buttons as consumer-control
 * keyboard events (AppleM68Buttons publishes the same usage pair).
 */
class SweetHIDButtons : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDButtons);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void key(UInt32 usage, bool down);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Consumer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Csmr_ConsumerControl; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 1; }
private:
	static void testFired(OSObject *owner, IOTimerEventSource *sender);
	IOTimerEventSource *fTest;
	UInt32 fTestSecs;
	UInt32 fTestCount;
};
OSDefineMetaClassAndStructors(SweetHIDButtons, IOHIDEventService);

/*
 * Touch screen: digitizer collection events with finger children,
 * coordinates normalized to 0..1 like IOHIDEventDriver produces for
 * absolute digitizers.
 */
#define SWEET_TOUCH_FINGERS 10
/*
 * Contacts that start this close to a panel edge (pixels, ~3 mm) carry
 * kIOHIDDigitizerEventFromEdgeTip for their whole life, as Apple's touch
 * firmware reports them. UIKit derives UITouch's edge type from that bit
 * (_UIEventHIDTouchEdgeTypeForHIDEvent); without it there is no home or
 * app-switcher gesture.
 */
#define SWEET_TOUCH_EDGE_PX 48
class SweetHIDTouch : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDTouch);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void touch(int finger, bool down, int x, int y);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return fMouse ? kHIDPage_GenericDesktop : kHIDPage_Digitizer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return fMouse ? kHIDUsage_GD_Mouse : kHIDUsage_Dig_TouchScreen; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 2; }
private:
	void mouseTouch(int finger, bool down, int x, int y, IOFixed nx, IOFixed ny);
	void trackpadTouch(int finger, bool down, int x, int y);
	/* sweettouchtest=N: N s after boot, then every 60 s, a synthetic one-finger drag (0390): the
	 * trackpad path checked on an unattended boot (the frame dumps show the pointer move) */
	static void touchTestFired(OSObject *owner, IOTimerEventSource *sender);
	/* 0397 direct touch: the pointer is steered to the finger (closed loop on the drawn pointer) */
	static void steerFired(OSObject *owner, IOTimerEventSource *sender);
	void steerTick(void);
	IOTimerEventSource *fSteer;
	IOTimerEventSource *fTouchTest;
	int fTouchTestStep;
	IOLock *fLock;
	int fLogicalW, fLogicalH, fOffX, fOffY, fPanelW, fPanelH;
	bool fDown[SWEET_TOUCH_FINGERS];
	bool fEdge[SWEET_TOUCH_FINGERS];
	IOFixed fX[SWEET_TOUCH_FINGERS];
	IOFixed fY[SWEET_TOUCH_FINGERS];
	/* macOS mode (mouseTouch): the touch screen is an absolute pointer */
	bool fMouse;
	bool fScroll;
	int fPrimary;                   /* the finger that drives the pointer, -1 none */
	UInt32 fButtons;                /* button state of the last pointer event */
	int fScrollStep;                /* panel pixels per scroll line; < 0 flips the direction */
	int fPX, fPY;                   /* the primary finger in panel pixels (scroll deltas) */
	int fAccX, fAccY;
};
OSDefineMetaClassAndStructors(SweetHIDTouch, IOHIDEventService);

static SweetHIDButtons *gSweetButtons;
static SweetHIDTouch *gSweetTouch;

bool
SweetHIDButtons::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	if (PE_parse_boot_argn("sweethidtest", &fTestSecs, sizeof(fTestSecs)) && fTestSecs) {
		fTest = IOTimerEventSource::timerEventSource(this, &SweetHIDButtons::testFired);
		if (fTest && _hidWorkLoop->addEventSource(fTest) == kIOReturnSuccess) {
			fTest->setTimeoutMS(fTestSecs * 1000);
		}
	}
	gSweetButtons = this;
	registerService();
	IOLog("SWEET HID: buttons registered (test every %u s)\n", fTestSecs);
	return true;
}

void
SweetHIDButtons::key(UInt32 usage, bool down)
{
	IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), kHIDPage_Consumer, usage, down);

	if (event) {
		IOLog("SWEET HID: key 0x%x %s (%u clients)\n", (unsigned)usage, down ? "down" : "up",
		    _clients ? _clients->getCount() : 0);
		dispatchEvent(event);
		event->release();
	}
}

void
SweetHIDButtons::testFired(OSObject *owner, IOTimerEventSource *sender)
{
	SweetHIDButtons *self = (SweetHIDButtons *)owner;

	/*
	 * Home (Menu) press, then the release 100 ms later; Setup's "Hello"
	 * screen waits for it. Even counts press, odd counts release.
	 */
	bool press = (self->fTestCount & 1) == 0;
	self->key(kHIDUsage_Csmr_Menu, press);
	if (++self->fTestCount < 60) {
		sender->setTimeoutMS(press ? 100 : self->fTestSecs * 1000);
	}
}

/*
 * macOS has no touch-screen UI: on a macOS boot the touch screen is an absolute pointer, as an
 * absolute USB tablet or a VM's pointing device is. Same test as SweetHIDSystem.cpp's IOHIDSystem
 * (0367): the device tree's root model is a Mac ("Mac...", the Mac ADT); boot-arg
 * sweettouchmouse=1 / =0 forces it on / off. iOS boots keep the digitizer unchanged.
 */
static bool
sweet_touch_is_mouse(void)
{
	uint32_t force = 2;
	PE_parse_boot_argn("sweettouchmouse", &force, sizeof(force));
	if (force != 2) {
		return force != 0;
	}
	IORegistryEntry *root = IORegistryEntry::fromPath("/", gIODTPlane);
	bool mac = false;
	if (root != NULL) {
		OSData *model = OSDynamicCast(OSData, root->getProperty("model"));
		mac = model != NULL && model->getLength() >= 3 && memcmp(model->getBytesNoCopy(), "Mac", 3) == 0;
		root->release();
	}
	return mac;
}

bool
SweetHIDTouch::start(IOService *provider)
{
	fLock = IOLockAlloc();
	/* before IOHIDEventService::start, which publishes the usage page and usage */
	fMouse = sweet_touch_is_mouse();
	fPrimary = -1;
	fScrollStep = 24;
	if (fMouse) {
		int step = 0;
		if (PE_parse_boot_argn("sweettouchscroll", &step, sizeof(step)) && step != 0) {
			fScrollStep = step;
		}
	}
	if (!fLock || !IOHIDEventService::start(provider)) {
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* starlte_touch.c reports 1440x2960 panel pixels, as AppleCLCD. */
	fPanelW = 1440;
	fPanelH = 2960;
	fLogicalW = native ? 1440 : 1125;
	fLogicalH = native ? 2960 : 2436;
	/* sweetfb_scale + sweetfb_lw/lh: the canvas is drawn full height and
	 * centred between black bars (SweetFramebuffer.cpp scaledCopy);
	 * normalize over that strip, in panel pixels. */
	int scale = 0;
	uint32_t lw = 0, lh = 0;
	if (PE_parse_boot_argn("sweetfb_scale", &scale, sizeof(scale)) && scale &&
	    PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw)) &&
	    PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh)) && lw && lh) {
		fLogicalH = 2960;
		fLogicalW = (int)((uint64_t)lw * 2960 / lh);
		if (fLogicalW > 1440) {
			fLogicalW = 1440;
			fLogicalH = (int)((uint64_t)lh * 1440 / lw);
		}
	}
#elif defined(ARM64_BOARD_CONFIG_HEROLTE)
	/* sec_ts reports 1440x2560 panel pixels; same geometry as AppleCLCD. */
	fPanelW = 1440;
	fPanelH = 2560;
	fLogicalW = native ? 1440 : 1125;
	fLogicalH = native ? 2560 : 2436;
	/* sweetfb_scale: the display is scaled onto the whole panel, so panel
	 * coordinates normalize over the panel (SweetFramebuffer.cpp). */
	int scale = 0;
	if (PE_parse_boot_argn("sweetfb_scale", &scale, sizeof(scale)) && scale) {
		fLogicalW = 1440;
		fLogicalH = 2560;
		/* A canvas taller than 16:9 (sweetfb_lw/lh, the iPhone XR's
		 * 828x1792) is drawn full height and centered between black bars
		 * (herolte_gpu.c): normalize over that strip. */
		uint32_t lw = 0, lh = 0;
		PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw));
		PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh));
		if (lw && lh && (uint64_t)lw * 2560 < (uint64_t)lh * 1440) {
			fLogicalW = (int)((uint64_t)lw * 2560 / lh);
		}
	}
#elif defined(ARM64_BOARD_CONFIG_A5XELTE)
	fPanelW = fLogicalW = 1080;
	fPanelH = fLogicalH = 1920;
#else
	fPanelW = 1080;
	fPanelH = 2400;
	fLogicalW = native ? 1080 : 1125;
	fLogicalH = native ? 2400 : 2436;
#endif
	fOffX = (fLogicalW - fPanelW) / 2;
	fOffY = (fLogicalH - fPanelH) / 2;
	/* The digitizer covers the main display (backboardd maps it 1:1). */
	setProperty("DisplayIntegrated", kOSBooleanTrue);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	setProperty("SurfaceWidth", (unsigned long long)(64 << 16), 32);	/* mm, 16.16 */
	setProperty("SurfaceHeight", (unsigned long long)(132 << 16), 32);
#elif defined(ARM64_BOARD_CONFIG_HEROLTE)
	setProperty("SurfaceWidth", (unsigned long long)(64 << 16), 32);	/* mm, 16.16 */
	setProperty("SurfaceHeight", (unsigned long long)(113 << 16), 32);
#else
	setProperty("SurfaceWidth", (unsigned long long)(68 << 16), 32);	/* mm, 16.16 */
#endif
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#elif defined(ARM64_BOARD_CONFIG_A5XELTE)
	setProperty("SurfaceHeight", (unsigned long long)(121 << 16), 32);
#else
	setProperty("SurfaceHeight", (unsigned long long)(151 << 16), 32);
#endif
	if (fMouse) {
		/* A mouse's usage pairs (pointer + mouse), as a real Mac's HID pointer services publish;
		 * the event system then treats it as a pointing device, not a touch screen. */
		OSArray *pairs = OSArray::withCapacity(2);
		static const UInt32 usages[] = { kHIDUsage_GD_Mouse, kHIDUsage_GD_Pointer };
		for (unsigned i = 0; pairs && i < 2; i++) {
			OSDictionary *pair = OSDictionary::withCapacity(2);
			OSNumber *page = OSNumber::withNumber(kHIDPage_GenericDesktop, 32);
			OSNumber *usage = OSNumber::withNumber(usages[i], 32);
			if (pair && page && usage) {
				pair->setObject(kIOHIDDeviceUsagePageKey, page);
				pair->setObject(kIOHIDDeviceUsageKey, usage);
				pairs->setObject(pair);
			}
			OSSafeReleaseNULL(pair);
			OSSafeReleaseNULL(page);
			OSSafeReleaseNULL(usage);
		}
		if (pairs) {
			setProperty(kIOHIDDeviceUsagePairsKey, pairs);
			pairs->release();
		}
		removeProperty("DisplayIntegrated");
		/* What a Mac's USB mouse service publishes for the pointer filter: WindowServer read our
		 * relative events (500 queued, 0 dropped, boot 154332) but the pointer never moved; the
		 * acceleration step scales by the device resolution, and there was none. */
		setProperty("HIDPointerResolution", (unsigned long long)(400 << 16), 32);
		setProperty("HIDScrollResolution", (unsigned long long)(400 << 16), 32);
		setProperty("HIDPointerAccelerationType", "HIDMouseAcceleration");
		setProperty("HIDScrollAccelerationType", "HIDMouseScrollAcceleration");
		setProperty("HIDPointerButtonCount", 2ULL, 32);
		setProperty("HIDPointerReportRate", 120ULL, 32);
	}
	gSweetTouch = this;
	registerService();
	if (fMouse) {
		fSteer = IOTimerEventSource::timerEventSource(this, &SweetHIDTouch::steerFired);
		if (fSteer && _hidWorkLoop->addEventSource(fSteer) != kIOReturnSuccess) {
			fSteer->release();
			fSteer = NULL;
		}
	}
	int testSecs = 0;
	if (fMouse && PE_parse_boot_argn("sweettouchtest", &testSecs, sizeof(testSecs)) && testSecs > 0) {
		fTouchTest = IOTimerEventSource::timerEventSource(this, &SweetHIDTouch::touchTestFired);
		if (fTouchTest && _hidWorkLoop->addEventSource(fTouchTest) == kIOReturnSuccess) {
			fTouchTest->setTimeoutMS((uint32_t)testSecs * 1000);
			IOLog("SWEET HID: touch test drag at %d s, then every 60 s\n", testSecs);
		}
	}
	if (fMouse) {
		IOLog("SWEET HID: touch registered as an absolute pointer (macOS): %dx%d panel, %dx%d shown strip at %d,%d; "
		    "scroll %d px/line\n", fPanelW, fPanelH, fLogicalW, fLogicalH, -fOffX, -fOffY, fScrollStep);
	} else {
		IOLog("SWEET HID: touch registered\n");
	}
	return true;
}

/*
 * macOS mode. Like Apple's IOHIDEventService::dispatchAbsolutePointerEvent (26.5 KDK, IOHIDFamily):
 * an absolutePointerEvent with x/y normalized to 0..1 (16.16) over the shown canvas, which the
 * event system maps onto the display (WindowServer's 828x1792 points), and the previous button
 * state, so the event carries button children only when a button changes.
 *   - the first finger moves the pointer and holds button 1 (touch = click, drag = drag);
 *   - a second finger while one is down releases the button and turns finger movement into
 *     scroll-wheel lines (two-finger scroll) until every finger is up.
 */
/*
 * Trackpad mode (sweettouchrel=1, the default on macOS; =0 is the absolute pointer below): the
 * absolute events reached WindowServer's event system but never moved the pointer (boots 141658,
 * 151456), so the first finger drives it the way every Mac mouse does, with relative motion: a drag
 * moves the pointer by the finger's movement (panel pixels -> the 828x1792 canvas' points), a short
 * tap (< 300 ms, < 40 px of travel) clicks button 1. Other fingers are ignored.
 */
static bool
sweet_touch_relative(void)
{
	static int rel = -1;
	if (rel < 0) {
		rel = 1;
		PE_parse_boot_argn("sweettouchrel", &rel, sizeof(rel));
	}
	return rel != 0;
}

void
SweetHIDTouch::touchTestFired(OSObject *owner, IOTimerEventSource *sender)
{
	SweetHIDTouch *self = (SweetHIDTouch *)owner;
	int i = self->fTouchTestStep++;
	/* down at 300,600, 40 moves of +15,+20 panel pixels 16 ms apart, up; next drag 60 s later */
	if (i <= 40) {
		self->touch(0, true, 300 + 15 * i, 600 + 20 * i);
		sender->setTimeoutMS(16);
	} else if (i == 41) {
		self->touch(0, false, 300 + 15 * 40, 600 + 20 * 40);
		sender->setTimeoutMS(1000);
	} else if (i == 42) {
		self->touch(0, true, 700, 1800);        /* then a tap (0397: a click where the finger is) */
		sender->setTimeoutMS(80);
	} else {
		self->touch(0, false, 700, 1800);
		self->fTouchTestStep = 0;
		sender->setTimeoutMS(60 * 1000);
	}
}

/*
 * Direct touch (0397, replaces 0394's trackpad; macOS only moves the pointer for relative events):
 *   one finger: the pointer goes where the finger is and follows it; a short tap (< 300 ms, < 40 px)
 *     clicks there; touch, hold still 350 ms, then move = drag (button 1 held until the finger lifts)
 *   two fingers: drag = scroll (natural), a short tap = secondary click (0394)
 * The pointer is steered by relative events: every 16 ms the driver compares the pointer layer's drawn
 * origin (SweetFramebuffer.cpp, sweet_fb_cur_*) plus the hotspot with the finger and sends the
 * difference, once per drawn frame. The hotspot is measured once: the pointer pushed into the top-left
 * corner (position 0,0) is drawn at -hotspot.
 */
extern volatile int32_t sweet_fb_cur_x, sweet_fb_cur_y;
extern volatile uint32_t sweet_fb_cur_seq;
static struct {
	int primary, lastX, lastY, travel, accX, accY;
	uint32_t downMask;
	bool two, dragging, active;
	uint64_t downAt;
	int32_t tgtX, tgtY;             /* the finger, logical points */
	int cal;                        /* 0 hotspot unknown, 1 pushed to the corner, 2 known */
	int calTicks;
	int32_t hotX, hotY;
	uint32_t sentSeq;
	int stale;                      /* ticks since the last correction without a redrawn pointer */
	int32_t estX, estY;             /* the pointer by dead reckoning when no redraw tells us */
	bool estValid;
	UInt32 buttons;
	UInt32 clickButton;             /* a click to make once the pointer is there */
	int clickTicks;
	int canvasW, canvasH;
} st = { -1 };

static void
sweet_canvas(int *w, int *h)
{
	if (st.canvasW == 0) {
		uint32_t lw = 828, lh = 1792;
		PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw));
		PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh));
		st.canvasW = (int)lw;
		st.canvasH = (int)lh;
	}
	*w = st.canvasW;
	*h = st.canvasH;
}

void
SweetHIDTouch::steerFired(OSObject *owner, IOTimerEventSource *sender)
{
	(void)sender;
	((SweetHIDTouch *)owner)->steerTick();
}

void
SweetHIDTouch::steerTick(void)
{
	IOHIDEvent *events[3] = { NULL, NULL, NULL };
	uint64_t now = mach_absolute_time();
	bool again;

	IOLockLock(fLock);
	uint32_t seq = __atomic_load_n(&sweet_fb_cur_seq, __ATOMIC_ACQUIRE);
	if (st.cal == 0) {
		/* into the top-left corner: the pointer is then at 0,0 */
		events[0] = IOHIDEvent::relativePointerEvent(now, -4000, -4000, 0, st.buttons, st.buttons);
		st.cal = 1;
		st.calTicks = 0;
		st.sentSeq = seq;
		st.estX = st.estY = 0;
		st.estValid = true;
	} else if (st.cal == 1) {
		if (seq != st.sentSeq && ++st.calTicks >= 3) {
			st.hotX = -sweet_fb_cur_x;
			st.hotY = -sweet_fb_cur_y;
			st.cal = 2;
			IOLog("SWEET HID: pointer hotspot %d,%d\n", st.hotX, st.hotY);
		} else if (st.calTicks++ > 30) {
			st.hotX = st.hotY = 0;  /* no pointer drawn in half a second: dead reckoning only */
			st.cal = 2;
			IOLog("SWEET HID: no pointer layer seen, steering by dead reckoning\n");
		}
	} else if (seq != st.sentSeq || st.sentSeq == 0 || ++st.stale >= 3) {
		/* one correction per drawn frame; without a redraw for 3 ticks (a pointer WindowServer draws
		 * itself, or not at all), by dead reckoning from what was sent */
		int cw, ch;
		sweet_canvas(&cw, &ch);
		int32_t px, py;
		if (seq != st.sentSeq || !st.estValid) {
			px = sweet_fb_cur_x + st.hotX;
			py = sweet_fb_cur_y + st.hotY;
		} else {
			px = st.estX;
			py = st.estY;
		}
		st.stale = 0;
		int32_t ex = st.tgtX - px, ey = st.tgtY - py;
		bool there = (ex < 0 ? -ex : ex) <= 2 && (ey < 0 ? -ey : ey) <= 2;
		if (!there && !st.two) {
			events[0] = IOHIDEvent::relativePointerEvent(now, ex, ey, 0, st.buttons, st.buttons);
			st.sentSeq = seq;
			st.estX = st.tgtX < 0 ? 0 : (st.tgtX >= cw ? cw - 1 : st.tgtX);
			st.estY = st.tgtY < 0 ? 0 : (st.tgtY >= ch ? ch - 1 : st.tgtY);
			st.estValid = true;
		}
		if (st.clickButton && (there || ++st.clickTicks > 20)) {
			events[1] = IOHIDEvent::relativePointerEvent(now, 0, 0, 0, st.clickButton, st.buttons);
			events[2] = IOHIDEvent::relativePointerEvent(now, 0, 0, 0, st.buttons, st.clickButton);
			IOLog("SWEET HID: click %u at %d,%d\n", (unsigned)st.clickButton, st.tgtX, st.tgtY);
			st.clickButton = 0;
		}
	}
	/* touch, hold still, then move: drag */
	if (st.primary >= 0 && !st.two && !st.dragging && st.travel < 20) {
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(now - st.downAt, &ns);
		if (ns > 350000000ULL) {
			st.dragging = true;
			st.buttons = 1;
			if (!events[1]) {
				events[1] = IOHIDEvent::relativePointerEvent(now, 0, 0, 0, 1, 0);
			}
			IOLog("SWEET HID: drag start at %d,%d\n", st.tgtX, st.tgtY);
		}
	}
	again = st.primary >= 0 || st.clickButton != 0 || st.cal == 1;
	st.active = again;
	IOLockUnlock(fLock);
	for (int i = 0; i < 3; i++) {
		if (events[i]) {
			dispatchEvent(events[i]);
			events[i]->release();
		}
	}
	if (again && fSteer) {
		fSteer->setTimeoutMS(16);
	}
}

void
SweetHIDTouch::trackpadTouch(int finger, bool down, int x, int y)
{
	uint64_t now = mach_absolute_time();
	IOHIDEvent *events[2] = { NULL, NULL };
	const char *what = NULL;
	bool kick = false;
	int cw, ch;
	sweet_canvas(&cw, &ch);
	/* panel pixels -> logical points: the canvas is the panel's full height, centred (fLogicalW wide) */
	int strip = fLogicalW > 0 ? fLogicalW : fPanelW;
	int bar = (fPanelW - strip) / 2;
	int32_t lx = (int32_t)((int64_t)(x - bar) * cw / strip), ly = (int32_t)((int64_t)y * ch / fPanelH);
	lx = lx < 0 ? 0 : (lx >= cw ? cw - 1 : lx);
	ly = ly < 0 ? 0 : (ly >= ch ? ch - 1 : ly);

	IOLockLock(fLock);
	uint32_t bit = 1u << finger;
	bool was = (st.downMask & bit) != 0;
	if (down) {
		st.downMask |= bit;
	} else {
		st.downMask &= ~bit;
	}
	if (down && !was && st.primary < 0) {
		st.primary = finger;
		st.lastX = x;
		st.lastY = y;
		st.travel = 0;
		st.two = false;
		st.dragging = false;
		st.downAt = now;
		st.tgtX = lx;
		st.tgtY = ly;
		st.clickButton = 0;
		kick = true;
		what = "down";
	} else if (down && !was && finger != st.primary && st.primary >= 0 && !st.two) {
		st.two = true;
		st.accX = st.accY = 0;
		what = "two fingers";
	} else if (down && finger == st.primary) {
		int dx = x - st.lastX, dy = y - st.lastY;
		st.lastX = x;
		st.lastY = y;
		st.travel += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
		if (st.two) {
			st.accX += dx;
			st.accY += dy;
			int sx = st.accX / 24, sy = st.accY / 24;
			if (sx || sy) {
				st.accX -= sx * 24;
				st.accY -= sy * 24;
				events[0] = IOHIDEvent::scrollEvent(now, -sx, -sy, 0);
			}
		} else {
			st.tgtX = lx;
			st.tgtY = ly;
			kick = !st.active;
		}
	} else if (!down && was && st.downMask == 0 && st.primary >= 0) {
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(now - st.downAt, &ns);
		bool tap = ns < 300000000ULL && st.travel < 40;
		if (st.dragging) {
			events[0] = IOHIDEvent::relativePointerEvent(now, 0, 0, 0, 0, st.buttons);
			st.buttons = 0;
			st.dragging = false;
			what = "drag end";
		} else if (tap && st.two) {
			events[0] = IOHIDEvent::relativePointerEvent(now, 0, 0, 0, 2, 0);
			events[1] = IOHIDEvent::relativePointerEvent(now, 0, 0, 0, 0, 2);
			what = "two-finger tap = secondary click";
		} else if (tap) {
			st.clickButton = 1;     /* once the pointer is under the finger */
			st.clickTicks = 0;
			kick = !st.active;
			what = "tap = click";
		} else {
			what = st.two ? "scroll end" : "up";
		}
		st.primary = -1;
		st.two = false;
	} else if (!down && was && finger == st.primary) {
		for (int i = 0; i < SWEET_TOUCH_FINGERS; i++) {
			if (st.downMask & (1u << i)) {
				st.primary = i;
				break;
			}
		}
	}
	if (kick) {
		st.active = true;
	}
	IOLockUnlock(fLock);
	if (what) {
		IOLog("SWEET HID: touch %s (finger %d) at %d,%d -> %d,%d travel %d\n", what, finger, x, y, lx, ly, st.travel);
	}
	for (int i = 0; i < 2; i++) {
		if (events[i]) {
			dispatchEvent(events[i]);
			events[i]->release();
		}
	}
	if (kick && fSteer) {
		fSteer->setTimeoutMS(1);
	}
}

void
SweetHIDTouch::mouseTouch(int finger, bool down, int x, int y, IOFixed nx, IOFixed ny)
{
	uint64_t now = mach_absolute_time();
	IOHIDEvent *event = NULL, *scroll = NULL;
	const char *what = NULL;

	if (sweet_touch_relative()) {
		trackpadTouch(finger, down, x, y);
		return;
	}

	IOLockLock(fLock);
	bool was = fDown[finger];
	if (was == down && (!down || (nx == fX[finger] && ny == fY[finger]))) {
		IOLockUnlock(fLock);
		return;
	}
	fDown[finger] = down;
	if (down) {
		fX[finger] = nx;
		fY[finger] = ny;
	}
	int count = 0;
	for (int i = 0; i < SWEET_TOUCH_FINGERS; i++) {
		count += fDown[i] ? 1 : 0;
	}
	UInt32 old = fButtons;
	if (!was && down) {
		if (fPrimary < 0 && !fScroll) {
			fPrimary = finger;
			fPX = x;
			fPY = y;
			fButtons = 1;
			what = "down";
		} else if (fPrimary >= 0 && !fScroll) {
			fScroll = true;
			fAccX = fAccY = 0;
			fButtons = 0;
			what = "scroll start";
		}
	} else if (was && !down) {
		if (finger == fPrimary) {
			fPrimary = -1;
			if (fButtons) {
				fButtons = 0;
				what = "up";
			}
		}
		if (count == 0) {
			if (fScroll) {
				what = "scroll end";
			}
			fScroll = false;
			fPrimary = -1;
		}
	} else if (fScroll) {
		/* the primary finger's movement scrolls; with macOS's default natural scrolling the
		 * content follows the fingers */
		if (finger == fPrimary) {
			fAccX += x - fPX;
			fAccY += y - fPY;
			fPX = x;
			fPY = y;
			int step = fScrollStep < 0 ? -fScrollStep : fScrollStep;
			int lx = fAccX / step, ly = fAccY / step;
			if (lx || ly) {
				fAccX -= lx * step;
				fAccY -= ly * step;
				if (fScrollStep > 0) {
					lx = -lx;
					ly = -ly;
				}
				scroll = IOHIDEvent::scrollEvent(now, lx, ly, 0);
			}
		}
		IOLockUnlock(fLock);
		if (scroll) {
			dispatchEvent(scroll);
			scroll->release();
		}
		return;
	} else if (finger == fPrimary) {
		fPX = x;
		fPY = y;
	}
	/* the pointer stays where the primary finger is (or was) */
	bool moves = finger == fPrimary || (what && strcmp(what, "up") == 0);
	if (moves || old != fButtons) {
		IOFixed px = moves ? nx : fX[fPrimary >= 0 ? fPrimary : finger];
		IOFixed py = moves ? ny : fY[fPrimary >= 0 ? fPrimary : finger];
		event = IOHIDEvent::absolutePointerEvent(now, px, py, 0, fButtons, old);
	}
	IOLockUnlock(fLock);

	if (what) {
		IOLog("SWEET HID: pointer %s (finger %d) at %d,%d -> %d.%03d,%d.%03d buttons %u\n", what, finger, x, y,
		    (int)(nx >> 16), (int)(((nx & 0xffff) * 1000) >> 16), (int)(ny >> 16), (int)(((ny & 0xffff) * 1000) >> 16),
		    (unsigned)fButtons);
	}
	if (event) {
		dispatchEvent(event);
		event->release();
	}
}

void
SweetHIDTouch::touch(int finger, bool down, int x, int y)
{
	uint64_t now = mach_absolute_time();
	IOHIDEvent *collection;
	bool any = false;

	if (finger < 0 || finger >= SWEET_TOUCH_FINGERS) {
		return;
	}
	x = x < 0 ? 0 : (x >= fPanelW ? fPanelW - 1 : x);
	y = y < 0 ? 0 : (y >= fPanelH ? fPanelH - 1 : y);

	IOLockLock(fLock);
	bool was = fDown[finger];
	/* Panel pixels -> the presented 1125x2436 display (SweetFramebuffer.cpp). */
	IOFixed nx = (IOFixed)(((int64_t)(x + fOffX) << 16) / fLogicalW);
	IOFixed ny = (IOFixed)(((int64_t)(y + fOffY) << 16) / fLogicalH);
	/* Touches on the black bars beside a narrower display land on its edge. */
	nx = nx < 0 ? 0 : (nx > 0xffff ? 0xffff : nx);
	ny = ny < 0 ? 0 : (ny > 0xffff ? 0xffff : ny);
	if (fMouse) {
		IOLockUnlock(fLock);
		mouseTouch(finger, down, x, y, nx, ny);
		return;
	}
	bool moved = down && (nx != fX[finger] || ny != fY[finger]);
	if (was == down && !moved) {
		IOLockUnlock(fLock);
		return;
	}
	if (!was && down) {
		/* Edges of what is shown: the panel's, or the bars' inner sides. */
		int left = fOffX < 0 ? -fOffX : 0, right = fOffX < 0 ? fPanelW + fOffX : fPanelW;
		fEdge[finger] = x < left + SWEET_TOUCH_EDGE_PX || x > right - 1 - SWEET_TOUCH_EDGE_PX ||
		    y < SWEET_TOUCH_EDGE_PX || y > fPanelH - 1 - SWEET_TOUCH_EDGE_PX;
	}

	collection = IOHIDEvent::digitizerEvent(now, 0, kIOHIDDigitizerTransducerTypeHand, false, 0, 0, 0);
	if (!collection) {
		IOLockUnlock(fLock);
		return;
	}
	collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);
	/* iOS 26's touch streams drop "detached" touches (filterDetachedTouches): mark the
	 * collection and every contact as coming from the display's own digitizer. */
	collection->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);

	UInt32 cmask = 0;
	for (int i = 0; i < SWEET_TOUCH_FINGERS; i++) {
		bool d = (i == finger) ? down : fDown[i];
		if (i != finger && !d) {
			continue;
		}
		IOFixed fx = (i == finger && down) ? nx : fX[i];
		IOFixed fy = (i == finger && down) ? ny : fY[i];
		IOHIDEvent *child = IOHIDEvent::digitizerEvent(now, (UInt32)i + 2, kIOHIDDigitizerTransducerTypeFinger,
		    d, d ? 1 : 0, fx, fy);
		if (!child) {
			continue;
		}
		UInt32 mask = fEdge[i] ? kIOHIDDigitizerEventFromEdgeTip : 0;
		if (i == finger) {
			if (was != down) {
				mask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;
			}
			if (moved) {
				mask |= kIOHIDDigitizerEventPosition;
			}
			if (!was && down) {
				mask |= kIOHIDDigitizerEventIdentity;
			}
		}
		child->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);
		child->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i);
		child->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, i + 2);
		child->setIntegerValue(kIOHIDEventFieldDigitizerTouch, d);
		child->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, mask);
		collection->appendChild(child);
		child->release();
		cmask |= mask;
		any |= d;
	}
	fDown[finger] = down;
	if (down) {
		fX[finger] = nx;
		fY[finger] = ny;
	}
	collection->setFixedValue(kIOHIDEventFieldDigitizerX, nx);
	collection->setFixedValue(kIOHIDEventFieldDigitizerY, ny);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerRange, any);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch, any);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, cmask);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask, any ? 1 : 0);
	IOLockUnlock(fLock);

	if (was != down) {
		IOLog("SWEET HID: touch %d %s at %d,%d%s\n", finger, down ? "down" : "up", x, y,
		    fEdge[finger] ? " edge" : "");
	}
	dispatchEvent(collection);
	collection->release();
}

/* key: 0 power (side button), 1 volume up, 2 volume down, 3 home */
extern "C" void
sweet_hid_key(int key, int down)
{
	static const UInt32 usages[] = {
		kHIDUsage_Csmr_Power, kHIDUsage_Csmr_VolumeIncrement,
		kHIDUsage_Csmr_VolumeDecrement, kHIDUsage_Csmr_Menu,
	};

	if (gSweetButtons && key >= 0 && key < (int)(sizeof(usages) / sizeof(usages[0]))) {
		gSweetButtons->key(usages[key], down != 0);
	}
}

extern "C" void
sweet_hid_touch(int finger, int down, int x, int y)
{
	if (gSweetTouch) {
		gSweetTouch->touch(finger, down != 0, x, y);
	}
}
