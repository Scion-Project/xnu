/*
 * Built-in AppleKeyStore for SWEET: a device without a passcode whose
 * system keybag is always loaded and unlocked.
 *
 * Apple's AppleKeyStore kext (closed, SEP-backed) owns the keybags behind
 * data protection. libaks (AppleKeyStore.framework, statically in keybagd)
 * opens IOServiceMatching("AppleKeyStore") with type 0, calls selector 0,
 * then one selector per aks_* function (iOS 16.7 numbering below). keybagd
 * --init asks for the system bag (14) and only loads systembag.kb when the
 * answer is kIOReturnNotFound; answering with a handle is the path a
 * userspace reboot takes, so no bag file is needed.
 *
 * The first call of each selector is logged (selector, caller, argument
 * shapes, result) so the next selectors the system needs show up on the
 * console, plus the first failures; unknown ones fail with
 * kIOReturnUnsupported. (Logging every call cost CPU: see wrap/unwrap.)
 *
 * Galaxy S9: wrap/unwrap (10/11). securityd wraps the keychain's metadata
 * keys with aks_wrap_key; answered "unsupported" it retried forever, ~300
 * calls a second, ~80 % of a core plus launchd's share. Without a SEP there
 * is no hardware secret to wrap with, so a wrapped key is the key behind an
 * 8-byte tag: the keychain works, but it is NOT protected at rest.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSString.h>
#include <sys/proc.h>

/* libaks selectors (AppleKeyStore.framework, iOS 16.7). */
enum {
	kAKSOpen             = 0,
	kAKSCreateBag        = 2,
	kAKSSaveBag          = 3,
	kAKSUnloadBag        = 4,
	kAKSSetSystem        = 5,
	kAKSLoadBag          = 6,
	kAKSGetLockState     = 7,
	kAKSUnlockDevice     = 9,
	kAKSWrapKey          = 10,
	kAKSUnwrapKey        = 11,
	kAKSUnlockBag        = 12,
	kAKSLockBag          = 13,
	kAKSGetSystem        = 14,
	kAKSGetDeviceState   = 17,
	kAKSGetExtDeviceState = 35,     /* aks_get_extended_device_state */
	kAKSAssertHold       = 26,
	kAKSAssertDrop       = 27,
	kAKSUnloadSessionBags = 55,
	kAKSNotifyStateSet   = 102,
	kAKSNotifyStateIsSet = 103,
	kAKSAssertConsume    = 101,     /* aks_assert_consume */
};

/* Tag in front of a "wrapped" key (wrap = tag + key, as AES key wrap adds 8 bytes). */
static const uint8_t sweet_aks_wrap_tag[8] = { 'S', '9', 'A', 'K', 'S', 'W', '0', '1' };
#define SWEET_AKS_MAX_KEY       128

/* The system (device) bag handle. */
#define SWEET_AKS_SYSTEM_HANDLE 0

/*
 * aks lock state bits (aks_get_lock_state): bit 2 is "unlocked since boot"
 * (MobileKeyBag MKBDeviceUnlockedSinceBoot); clear lock bits = unlocked.
 */
#define SWEET_AKS_LOCK_STATE    0x4

class AppleKeyStore : public IOService {
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	registerService();
	IOLog("SWEET AKS: AppleKeyStore registered (no passcode, unlocked)\n");
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* Copy len bytes to the caller's output structure (inband or descriptor). */
static IOReturn
sweet_aks_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
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

/* Append SEQUENCE { UTF8String key, INTEGER value } (short forms only). */
static uint32_t
sweet_aks_der_pair(uint8_t *p, const char *key, uint8_t value)
{
	uint32_t klen = (uint32_t)strlen(key);
	uint32_t n = 0;
	p[n++] = 0x30;
	p[n++] = (uint8_t)(2 + klen + 3);
	p[n++] = 0x0c;
	p[n++] = (uint8_t)klen;
	memcpy(p + n, key, klen);
	n += klen;
	p[n++] = 0x02;
	p[n++] = 0x01;
	p[n++] = value;
	return n;
}

/*
 * Device state (selector 17): a DER SET of key/INTEGER pairs that libaks
 * decode_extended_state reads: ss state, sls lock state (MKBGetDeviceLockState:
 * 0 = unlocked), sb backoff, sfa failed attempts, sgs generation, sas
 * assertions, sgpe grace period, srcd recovery countdown.
 */
static IOReturn
sweet_aks_device_state(IOExternalMethodArguments *args)
{
	static const struct {
		const char *key;
		uint8_t value;
	} fields[] = {
		{ "ss", SWEET_AKS_LOCK_STATE }, { "sls", 0 }, { "sb", 0 }, { "sfa", 0 },
		{ "sgs", 0 }, { "sas", 0 }, { "sgpe", 0 }, { "srcd", 0 },
	};
	uint8_t buf[128];
	uint32_t n = 2;
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		n += sweet_aks_der_pair(buf + n, fields[i].key, fields[i].value);
	}
	buf[0] = 0x31;
	buf[1] = (uint8_t)(n - 2);
	return sweet_aks_struct_out(args, buf, n);
}

/* Structure input (inband or descriptor) into buf; its length, or -1 if larger than max. */
static int32_t
sweet_aks_struct_in(IOExternalMethodArguments *args, uint8_t *buf, uint32_t max)
{
	if (args->structureInputDescriptor != NULL) {
		uint32_t len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len > max) {
			return -1;
		}
		args->structureInputDescriptor->readBytes(0, buf, len);
		return (int32_t)len;
	}
	if (args->structureInputSize > max) {
		return -1;
	}
	if (args->structureInputSize > 0) {
		memcpy(buf, args->structureInput, args->structureInputSize);
	}
	return (int32_t)args->structureInputSize;
}

/* aks_wrap_key: scalars [handle, class], key in; class out, tag + key out. */
static IOReturn
sweet_aks_wrap(IOExternalMethodArguments *args)
{
	uint8_t buf[sizeof(sweet_aks_wrap_tag) + SWEET_AKS_MAX_KEY];
	int32_t len = sweet_aks_struct_in(args, buf + sizeof(sweet_aks_wrap_tag), SWEET_AKS_MAX_KEY);
	if (len <= 0 || args->scalarInputCount < 2 || args->scalarOutputCount < 1) {
		return kIOReturnBadArgument;
	}
	memcpy(buf, sweet_aks_wrap_tag, sizeof(sweet_aks_wrap_tag));
	IOReturn ret = sweet_aks_struct_out(args, buf, sizeof(sweet_aks_wrap_tag) + (uint32_t)len);
	if (ret == kIOReturnSuccess) {
		args->scalarOutput[0] = args->scalarInput[1];
		args->scalarOutputCount = 1;
	}
	bzero(buf, sizeof(buf));
	return ret;
}

/* aks_unwrap_key: scalars [handle, class], tag + key in; key out. */
static IOReturn
sweet_aks_unwrap(IOExternalMethodArguments *args)
{
	uint8_t buf[sizeof(sweet_aks_wrap_tag) + SWEET_AKS_MAX_KEY];
	int32_t len = sweet_aks_struct_in(args, buf, sizeof(buf));
	args->scalarOutputCount = 0;
	if (len <= (int32_t)sizeof(sweet_aks_wrap_tag) ||
	    memcmp(buf, sweet_aks_wrap_tag, sizeof(sweet_aks_wrap_tag)) != 0) {
		return kIOReturnBadArgument;
	}
	IOReturn ret = sweet_aks_struct_out(args, buf + sizeof(sweet_aks_wrap_tag),
	    (uint32_t)len - sizeof(sweet_aks_wrap_tag));
	bzero(buf, sizeof(buf));
	return ret;
}

static uint64_t sweet_aks_logged;       /* selectors already logged (< 64) */
static uint32_t sweet_aks_failures_logged;

static uint32_t
sweet_aks_struct_out_size(IOExternalMethodArguments *args)
{
	return args->structureOutputDescriptor != NULL ?
	       (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint32_t in_scalars = args->scalarInputCount;
	uint32_t in_struct = args->structureInputDescriptor != NULL ?
	    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t out_scalars = args->scalarOutputCount;
	uint32_t out_struct = sweet_aks_struct_out_size(args);
	uint64_t arg0 = in_scalars > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kAKSOpen:
	case kAKSUnloadBag:
	case kAKSSetSystem:
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
	case kAKSLockBag:
	case kAKSAssertHold:
	case kAKSAssertDrop:
	case kAKSAssertConsume:
	case kAKSUnloadSessionBags:
	case kAKSNotifyStateSet:
		args->scalarOutputCount = 0;
		break;
	case kAKSGetSystem:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_SYSTEM_HANDLE;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetLockState:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_LOCK_STATE;
		args->scalarOutputCount = 1;
		break;
	case kAKSNotifyStateIsSet:
		/* Nothing was ever set: answer "not set". */
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = 0;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetDeviceState:
	case kAKSGetExtDeviceState:
		args->scalarOutputCount = 0;
		ret = sweet_aks_device_state(args);
		break;
	case kAKSWrapKey:
		ret = sweet_aks_wrap(args);
		break;
	case kAKSUnwrapKey:
		ret = sweet_aks_unwrap(args);
		break;
	default:
		ret = kIOReturnUnsupported;
		break;
	}

	bool first = selector >= 64 || !(sweet_aks_logged & (1ULL << selector));
	if (first || (ret != kIOReturnSuccess && sweet_aks_failures_logged < 32)) {
		if (selector < 64) {
			sweet_aks_logged |= 1ULL << selector;
		}
		if (ret != kIOReturnSuccess) {
			sweet_aks_failures_logged++;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    arg0, in_scalars, in_struct, out_scalars, out_struct, ret);
	}
	return ret;
}
#endif /* SWEET */
