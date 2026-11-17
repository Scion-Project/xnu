/*
 * STARLTE (Galaxy S9): the motion sensors as HID event services for the iOS userland.
 *
 * iOS reads its accelerometer and gyroscope as HID events from built-in services of Apple's vendor page (0xFF00:
 * usage 3 the accelerometer, usage 9 the gyroscope), which on an iPhone are the motion coprocessor's. Here the samples
 * come from the phone's sensor hub (bsd/dev/hakios.ssp.c, boot-arg shub=6: Scion's shared driver, which also hands
 * them out on /dev/hakios-sensors) and these two services turn each one into an event, as SweetHIDTouch does for the
 * touch screen (SweetHID.cpp has the service's base class and the user client backboardd opens).
 *
 * Boot-arg shub_hid=N, a mask; without it there are no such services and nothing here runs:
 *   1   the accelerometer's service (event type 13, in g; iOS's sign: at rest -1 g on the axis that points up)
 *   2   the gyroscope's service (event type 20, in rad/s)
 *   4   send every sample from the start, not only while a client has asked for reports (below)
 *   8   log every property the event system sets on the services (the first 40), to learn what it asks for
 *
 * Units and axes. The hub gives Android's frame, which is iOS's (x to the right of the screen, y to its top, z out of
 * it) with the accelerometer's sign turned: lying face up Android reads +1 g on z, iOS -1 g. An accelerometer count is
 * range / 32768 g (range 8: 4096 counts a g). A gyroscope count is a thousandth of the chip's 35 mdps step at its
 * 1000 dps range, so 28571 counts are a degree a second (the hub's own log lines give both forms; hakios.ssp.c's
 * shub_say). Events carry 16.16 fixed point.
 *
 * Reports. As on an iPhone, a service sends nothing until a client sets "ReportInterval" (microseconds; 0 stops it):
 * the event system passes the shortest interval any client asked for. The hub's rate is its own (shub_ms, 20 ms by
 * default): every sample is sent while reports are on, whatever interval was asked.
 *
 * SPDX-License-Identifier: MIT-0
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
extern "C" {
#include <sys/hakios.sensors.h>
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#include "IOHIDEventTypes.h"
#pragma clang diagnostic pop
#include "IOHIDEvent.h"
#include "IOHIDEventService.h"
#include "IOHIDKeys.h"

#define SENSORS_ACCEL           1u
#define SENSORS_GYRO            2u
#define SENSORS_ALWAYS          4u
#define SENSORS_LOG             8u

#define PAGE_APPLE_VENDOR       0xFF00
#define USAGE_ACCELEROMETER     3
#define USAGE_GYRO              9

extern "C" {
/* the hub's driver calls `tap` with every sample it also queues for /dev/hakios-sensors (its thread; bsd/dev/hakios.ssp.c) */
void hakios_ssp_tap(void (*tap)(const struct hakios_sensor_sample *));
}

static uint32_t
sensors_mask(void)
{
	uint32_t mask = 0;

	PE_parse_boot_argn("shub_hid", &mask, sizeof(mask));
	return mask;
}

class SweetHIDMotion : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDMotion);
public:
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void sample(const struct hakios_sensor_sample *s);
protected:
	bool startAs(IOService *provider, uint32_t bit, const char *what);
	virtual IOHIDEvent *make(const struct hakios_sensor_sample *s) = 0;
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return PAGE_APPLE_VENDOR; }
	virtual UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return 20000; }
	const char *fWhat;
	uint32_t fMask;
	volatile uint32_t fInterval;    /* what the event system last asked for, in microseconds; 0: no reports */
	uint64_t fSent, fSkipped;
	uint32_t fLogged;
};
OSDefineMetaClassAndAbstractStructors(SweetHIDMotion, IOHIDEventService);

class SweetHIDAccelerometer : public SweetHIDMotion {
	OSDeclareDefaultStructors(SweetHIDAccelerometer);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
protected:
	virtual IOHIDEvent *make(const struct hakios_sensor_sample *s) APPLE_KEXT_OVERRIDE;
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return USAGE_ACCELEROMETER; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 3; }
};
OSDefineMetaClassAndStructors(SweetHIDAccelerometer, SweetHIDMotion);

class SweetHIDGyroscope : public SweetHIDMotion {
	OSDeclareDefaultStructors(SweetHIDGyroscope);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
protected:
	virtual IOHIDEvent *make(const struct hakios_sensor_sample *s) APPLE_KEXT_OVERRIDE;
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return USAGE_GYRO; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 4; }
};
OSDefineMetaClassAndStructors(SweetHIDGyroscope, SweetHIDMotion);

static SweetHIDAccelerometer *gAccelerometer;
static SweetHIDGyroscope *gGyroscope;

/* the hub's thread: one sample, to the service of its kind */
static void
sensors_tap(const struct hakios_sensor_sample *s)
{
	if (s->type == HAKIOS_SENSOR_ACCEL && gAccelerometer != NULL) {
		gAccelerometer->sample(s);
	} else if (s->type == HAKIOS_SENSOR_GYRO && gGyroscope != NULL) {
		gGyroscope->sample(s);
	}
}

bool
SweetHIDMotion::startAs(IOService *provider, uint32_t bit, const char *what)
{
	fMask = sensors_mask();
	fWhat = what;
	if (!(fMask & bit) || !IOHIDEventService::start(provider)) {
		return false;
	}
	if (fMask & SENSORS_ALWAYS) {
		fInterval = 20000;
	}
	return true;
}

/*
 * What the event system sets on the service. "ReportInterval" (and "BatchInterval", which is not used here) come when
 * a client starts or stops the sensor; everything else is taken and ignored.
 */
IOReturn
SweetHIDMotion::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	OSNumber *interval;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	if ((fMask & SENSORS_LOG) && fLogged < 40) {
		OSCollectionIterator *keys = OSCollectionIterator::withCollection(dict);
		OSObject *key;

		while (keys != NULL && (key = keys->getNextObject()) != NULL && fLogged < 40) {
			OSString *name = OSDynamicCast(OSString, key);
			OSNumber *number = name ? OSDynamicCast(OSNumber, dict->getObject(name)) : NULL;

			fLogged++;
			IOLog("SWEET HID: the %s is told \"%s\"%s%llu\n", fWhat, name ? name->getCStringNoCopy() : "?",
			    number ? " = " : " (not a number) ", number ? number->unsigned64BitValue() : 0ULL);
		}
		OSSafeReleaseNULL(keys);
	}
	interval = OSDynamicCast(OSNumber, dict->getObject(kIOHIDReportIntervalKey));
	if (interval != NULL) {
		uint32_t us = interval->unsigned32BitValue();

		if (fMask & SENSORS_ALWAYS) {
			us = 20000;
		}
		if ((us != 0) != (fInterval != 0)) {
			IOLog("SWEET HID: the %s's reports %s (interval asked %u us; %llu events sent so far)\n", fWhat,
			    us ? "on" : "off", interval->unsigned32BitValue(), fSent);
		}
		fInterval = us;
		setProperty(kIOHIDReportIntervalKey, interval);
	}
	return kIOReturnSuccess;
}

void
SweetHIDMotion::sample(const struct hakios_sensor_sample *s)
{
	IOHIDEvent *event;

	if (fInterval == 0) {
		fSkipped++;
		return;
	}
	event = make(s);
	if (event != NULL) {
		if (fSent++ == 0) {
			IOLog("SWEET HID: the %s's first event (%d %d %d from the hub; %u clients)\n", fWhat, s->v[0], s->v[1], s->v[2],
			    _clients ? _clients->getCount() : 0);
		}
		dispatchEvent(event);
		event->release();
	}
}

bool
SweetHIDAccelerometer::start(IOService *provider)
{
	if (!startAs(provider, SENSORS_ACCEL, "accelerometer")) {
		return false;
	}
	gAccelerometer = this;
	hakios_ssp_tap(sensors_tap);
	registerService();
	IOLog("SWEET HID: accelerometer registered (page 0xff00 usage 3; shub_hid=%#x)\n", fMask);
	return true;
}

IOHIDEvent *
SweetHIDAccelerometer::make(const struct hakios_sensor_sample *s)
{
	/* counts * range / 32768 g, the sign turned, in 16.16: counts * range * 2 */
	int32_t k = -2 * (s->scale > 0 ? s->scale : 8);

	return IOHIDEvent::accelerometerEvent(s->time, (IOFixed)(s->v[0] * k), (IOFixed)(s->v[1] * k), (IOFixed)(s->v[2] * k));
}

bool
SweetHIDGyroscope::start(IOService *provider)
{
	if (!startAs(provider, SENSORS_GYRO, "gyroscope")) {
		return false;
	}
	gGyroscope = this;
	hakios_ssp_tap(sensors_tap);
	registerService();
	IOLog("SWEET HID: gyroscope registered (page 0xff00 usage 9; shub_hid=%#x)\n", fMask);
	return true;
}

IOHIDEvent *
SweetHIDGyroscope::make(const struct hakios_sensor_sample *s)
{
	/* a count is 35e-6 degrees a second = 6.1087e-7 rad/s; in 16.16 that is counts * 0.040034 */
	IOFixed v[3];

	for (int i = 0; i < 3; i++) {
		v[i] = (IOFixed)((int64_t)s->v[i] * 40034 / 1000000);
	}
	return IOHIDEvent::gyroEvent(s->time, v[0], v[1], v[2]);
}
