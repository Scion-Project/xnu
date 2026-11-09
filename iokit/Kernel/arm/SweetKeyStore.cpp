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
 *
 * iOS 27 (24A446): the selector numbers and argument shapes are the ones above (read from
 * AppleKeyStore.framework, MobileKeyBag and keybagd in its shared cache: every aks_* function still
 * calls the number it called on 16.x; the new ones only add to the list). What was wrong for it were
 * the answers. Boot-arg sweetspeed&0x400000 (off by default) gives the contract of a device with no
 * passcode in full:
 *   - the state says "no passcode" (flags bit 1, lock state 3 "disabled") and not, as before,
 *     "a passcode is set and it is unlocked" (lock state 0): MobileKeyBag's MKBGetDeviceLockState is
 *     the "sls" value, and ManagedConfiguration's "is a passcode set" is "sls is not 3". With 0,
 *     Settings offered to turn a passcode off that nobody had set, and SpringBoard asked to lock the
 *     device at every screen-off (8, 32), which failed;
 *   - lock (8, 32) does nothing and succeeds, as it does with no passcode;
 *   - the configuration (30, 31), the bag's UUID (23), the generation (40), assertion promote (100),
 *     prewarm (115), verify password (42) and "can the SE be reset in restore mode" (107) answer;
 *   - "the first-unlock notification has been posted" (102, 103) is remembered.
 * Selectors that need a Secure Enclave or a paired device (135 beacon keys, 57 remote peers) still
 * answer kIOReturnUnsupported.
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
	kAKSLockDevice       = 8,       /* aks_lock_device: 8, or 32 for "now" */
	kAKSUnlockDevice     = 9,
	kAKSWrapKey          = 10,
	kAKSUnwrapKey        = 11,
	kAKSUnlockBag        = 12,
	kAKSLockBag          = 13,
	kAKSGetSystem        = 14,
	kAKSGetDeviceState   = 17,
	kAKSGetExtDeviceState = 35,     /* aks_get_extended_device_state */
	kAKSGetBagUUID       = 23,
	kAKSAssertHold       = 26,
	kAKSAssertDrop       = 27,
	kAKSSetConfiguration = 30,      /* in: DER, as 31's answer */
	kAKSGetConfiguration = 31,      /* in: handle; out: DER */
	kAKSLockDeviceNow    = 32,
	kAKSGeneration       = 40,
	kAKSVerifyPassword   = 42,
	kAKSAssertPromote    = 100,
	kAKSSESupportInRMIsSet = 107,   /* aks_se_support_in_rm_is_set: out scalar */
	kAKSPrewarm          = 115,
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

/*
 * The answers of a device with no passcode (boot-arg sweetspeed&0x400000; the head of this file).
 * State flags: bit 0 locked, bit 1 no passcode, bit 2 unlocked since boot. Lock state ("sls"):
 * 0 unlocked, 1 locked, 2 locking, 3 disabled (no passcode).
 */
#define SWEET_SPEED_AKS_NOPASSCODE      0x400000
#define SWEET_AKS_STATE_NOPASSCODE      0x6
#define SWEET_AKS_SLS_DISABLED          3
#define SWEET_AKS_MAX_UNLOCK_ATTEMPTS   10      /* "mua"; nothing here wipes */

extern "C" uint32_t sweet_speed_flags(void);    /* boot-arg sweetspeed (osfmk/arm64/sweet_a64e.c) */

static bool
sweet_aks_nopasscode(void)
{
	return (sweet_speed_flags() & SWEET_SPEED_AKS_NOPASSCODE) != 0;
}

static uint32_t sweet_aks_grace_period;         /* "gp", as set by selector 30; kept for this boot */
static bool sweet_aks_notify_state;             /* selector 102 was called */

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
	IOLog("SWEET AKS: AppleKeyStore registered (no passcode, unlocked%s)\n",
	    sweet_aks_nopasscode() ? "; lock state 3, sweetspeed&0x400000" : "");
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
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

/*
 * aks_register_for_notifications (IOConnectSetNotificationPort). The state never changes without a
 * passcode, so nothing is ever sent; MKBEventsRegister uses IOService's general interest instead.
 */
IOReturn
AppleKeyStoreUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	if (!sweet_aks_nopasscode()) {
		return IOUserClient::registerNotificationPort(port, type, refCon);
	}
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
	bool nopasscode = sweet_aks_nopasscode();
	uint8_t buf[128];
	uint32_t n = 2;
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		uint8_t value = fields[i].value;
		if (nopasscode && i == 0) {
			value = SWEET_AKS_STATE_NOPASSCODE;
		} else if (nopasscode && i == 1) {
			value = SWEET_AKS_SLS_DISABLED;
		}
		n += sweet_aks_der_pair(buf + n, fields[i].key, value);
	}
	buf[0] = 0x31;
	buf[1] = (uint8_t)(n - 2);
	return sweet_aks_struct_out(args, buf, n);
}

/*
 * Configuration (selector 31; MKBGetDeviceConfigurations): "gp" the grace period in seconds, "mua"
 * the maximum of unlock attempts, "bd" the backoff delay. All are below 128 here but "gp", which is
 * cut to a byte's worth of minutes by the caller (selector 30 keeps what fits).
 */
static IOReturn
sweet_aks_configuration(IOExternalMethodArguments *args)
{
	uint8_t buf[64];
	uint32_t n = 2;
	n += sweet_aks_der_pair(buf + n, "bd", 0);
	n += sweet_aks_der_pair(buf + n, "gp", (uint8_t)sweet_aks_grace_period);
	n += sweet_aks_der_pair(buf + n, "mua", SWEET_AKS_MAX_UNLOCK_ATTEMPTS);
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

/*
 * Set configuration (selector 30): a DER SET of SEQUENCE { UTF8String key, INTEGER value }. The
 * grace period "gp" is kept (below 128 seconds: the answer's INTEGER is one byte); with no passcode
 * it applies to nothing.
 */
static void
sweet_aks_set_configuration(const uint8_t *der, uint32_t size)
{
	uint32_t i = 0;
	if (size < 2 || (der[0] != 0x31 && der[0] != 0x30)) {
		return;
	}
	i = (der[1] & 0x80) ? 2 + (der[1] & 0x7f) : 2;
	while (i + 2 <= size && der[i] == 0x30 && !(der[i + 1] & 0x80)) {
		uint32_t end = i + 2 + der[i + 1];
		uint32_t k = i + 2;
		if (end > size) {
			return;
		}
		if (k + 2 <= end && der[k] == 0x0c && k + 2 + der[k + 1] + 2 <= end) {
			uint32_t klen = der[k + 1];
			uint32_t v = k + 2 + klen;
			if (der[v] == 0x02 && der[v + 1] >= 1 && der[v + 1] <= 8 && v + 2 + der[v + 1] <= end) {
				uint64_t value = 0;
				for (uint32_t b = 0; b < der[v + 1]; b++) {
					value = (value << 8) | der[v + 2 + b];
				}
				if (klen == 2 && der[k + 2] == 'g' && der[k + 3] == 'p') {
					sweet_aks_grace_period = value > 127 ? 127 : (uint32_t)value;
				}
			}
		}
		i = end;
	}
}

static uint64_t sweet_aks_logged[4];    /* selectors already logged (< 256) */
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
	bool nopasscode = sweet_aks_nopasscode();

	/* the full contract of a device with no passcode (sweetspeed&0x400000): what the switch below does not answer, or answers otherwise */
	if (nopasscode) {
		static const uint8_t bag_uuid[16] = { 0x73, 0x63, 0x69, 0x6f, 0x6e, 0x00, 0x40, 0x00, 0x80, 0x6b, 0x65, 0x79, 0x62, 0x61, 0x67, 0x01 };
		bool handled = true;

		switch (selector) {
		case kAKSLockDevice:
		case kAKSLockDeviceNow:
		case kAKSAssertPromote:
		case kAKSPrewarm:
		case kAKSVerifyPassword:        /* no passcode: nothing to compare with */
			args->scalarOutputCount = 0;
			break;
		case kAKSGetLockState:
			if (out_scalars < 1) {
				ret = kIOReturnBadArgument;
				break;
			}
			args->scalarOutput[0] = SWEET_AKS_STATE_NOPASSCODE;
			args->scalarOutputCount = 1;
			break;
		case kAKSGeneration:
		case kAKSSESupportInRMIsSet:
			if (out_scalars >= 1) {
				args->scalarOutput[0] = 0;
				args->scalarOutputCount = 1;
			}
			break;
		case kAKSGetConfiguration:
			args->scalarOutputCount = 0;
			ret = sweet_aks_configuration(args);
			break;
		case kAKSSetConfiguration: {
			uint8_t der[256];
			int32_t len = sweet_aks_struct_in(args, der, sizeof(der));
			args->scalarOutputCount = 0;
			if (len < 0) {
				ret = kIOReturnBadArgument;
				break;
			}
			sweet_aks_set_configuration(der, (uint32_t)len);
			break;
		}
		case kAKSGetBagUUID:
			args->scalarOutputCount = 0;
			ret = sweet_aks_struct_out(args, bag_uuid, sizeof(bag_uuid));
			break;
		case kAKSNotifyStateSet:
			sweet_aks_notify_state = true;
			args->scalarOutputCount = 0;
			break;
		case kAKSNotifyStateIsSet:
			if (out_scalars < 1) {
				ret = kIOReturnBadArgument;
				break;
			}
			args->scalarOutput[0] = sweet_aks_notify_state;
			args->scalarOutputCount = 1;
			break;
		default:
			handled = false;
			break;
		}
		if (handled) {
			goto done;
		}
	}

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

done:
	bool first = selector >= 256 || !(sweet_aks_logged[selector >> 6] & (1ULL << (selector & 63)));
	if (first || (ret != kIOReturnSuccess && sweet_aks_failures_logged < 32)) {
		if (selector < 256) {
			sweet_aks_logged[selector >> 6] |= 1ULL << (selector & 63);
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
