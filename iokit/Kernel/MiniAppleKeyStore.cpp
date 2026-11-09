/*
 * MiniAppleKeyStore: a stand-in for Apple's AppleKeyStore (the kernel side of the keybag,
 * which on real hardware talks to the Secure Enclave), as seen by MobileKeyBag.framework
 * and libaks.
 *
 * What it keeps: whether the device has a passcode, and if so a verifier of it, the lock state,
 * the count of failed attempts and the delay they impose, the grace period, and the opaque
 * "system secret blob" that ManagedConfiguration stores with a passcode (what kind of passcode it is:
 * the lock screen's keypad is chosen from it). With no passcode it answers as it always did: "no
 * passcode, unlocked since boot" (lock state 3, disabled).
 *
 * WHAT THIS IS NOT. There is no Secure Enclave and no hardware key. The passcode gates the user
 * interface -- the lock screen, Settings, MC's checks -- and nothing else: no data is encrypted with
 * it, the keychain's "wrapping" below is a reversible transformation of no secrecy, the data volume is
 * plain HFS Plus that anyone with the flash can read, and the verifier file can be deleted by anyone
 * who can write /private/var (root) or the flash. Treat it as a screen lock, not data protection.
 *
 * The passcode
 *   - Verifier: PBKDF2-HMAC-SHA256 of the passcode's bytes with a random 16-byte salt, 100000
 *     iterations (both kept with it, so they can change later), compared in constant time. The
 *     key derivation is checked against RFC 7914's test vector before a passcode is set or tried;
 *     a kernel whose SHA-256 gives another answer refuses to set one, and treats a stored one as
 *     absent (better open than shut for good).
 *   - Kept in /private/var/hakios-passcode (USERDATA on the phone: kept between boots; in memory on
 *     a RAM boot and in QEMU). The file has two 4 KB slots; a change writes the slot that does not
 *     hold the newest record, with a generation one higher and a SHA-256 over it, and syncs. A torn
 *     write damages only the slot being written, and reading takes the valid slot with the highest
 *     generation. A file that cannot be read or has no valid slot means "no passcode".
 *   - Read on the first call any process makes to the key store: process 1 (userland/src/init.c)
 *     mounts /private/var before it starts launchd, and nothing reaches the key store before launchd.
 *
 * Escape hatch: the boot-argument passcode_reset=1 discards the stored passcode at the first call of
 * the boot (both slots are overwritten with a "no passcode" record), and says so in the log. Flashing
 * a boot image with it is always possible (TWRP or download mode), whatever the lock screen does. The kernel's
 * command line holds 607 characters and what does not fit is cut silently, the end first: leave room for it
 * (docs/passcode.md). Without a boot image, deleting /private/var/hakios-passcode from the data volume does the same.
 *
 * Lock states (MobileKeyBag's, what MKBGetDeviceLockState returns): 0 unlocked, 1 locked, 3 disabled
 * (no passcode). With a passcode the device starts locked and not "unlocked since boot". A lock request
 * locks at once. The grace period ManagedConfiguration sets (Settings' "Require Passcode") is kept and reported but not
 * applied: a longer one asks for the passcode sooner than chosen, never later. Lock assertions are refused while
 * locked (kAKSAssertHold below).
 * Every change is sent to the clients of IOGeneralInterest (MKBEventsRegister: SpringBoard), and the
 * first unlock of the boot as well.
 *
 * Failed attempts: the delays of Apple's Platform Security guide, after the 5th failed attempt 1 minute,
 * the 6th 5 minutes, the 7th and 8th 15 minutes, the 9th and every one after 1 hour. Never more, and never
 * "permanently blocked", never a wipe: the 10th attempt is another hour. Attempts made while a delay runs
 * are refused without being counted. The count and the end of the delay are kept with the verifier, so
 * a restart does not clear them (a delay read back after a restart is cut to its length, if the clock
 * says more is left).
 *
 * keystore_real=1 (off by default until it has been proven on a phone): the real key bag below, under "the key bag". Class keys
 * are random, wrapped with AES-256 key wrap under Argon2id of the passcode, and dropped from memory as iOS's classes say; wrap
 * and unwrap are real, a wrong passcode yields no key, damage fails closed. Without it everything here is as described above.
 *
 * The protocol was worked out from the user-space side of Darwin userland 16.5 (MobileKeyBag.framework,
 * SpringBoardFoundation, ManagedConfiguration): selectors, the DER-encoded answers ("SET of SEQUENCE {
 * UTF8String key, INTEGER value }"), and how arguments are passed. The passcode is changed through
 * keybagd's XPC service, which userland/src/passcoded.m stands in for; it reaches this file with the
 * selectors kHKChangeSecret and kHKCopyBlob below, which are this port's own. kHKChangeSecret is for root (passcoded
 * is): Apple's key store has entitlements for this, and this one has no such check. Nothing limits who may try a
 * passcode (kAKSUnlockDevice, kAKSVerifyPassword), so any program can run up the failed attempts and their delay.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <sys/kauth.h>
#include <IOKit/IOBSD.h>
#include <kern/clock.h>
#include <sys/sysctl.h>

#include "MiniDrivers.h"

extern "C" {
extern vfs_context_t vfs_context_kernel(void);
extern vnode_t proc_getexecutablevnode(proc_t);
}

#include "../../osfmk/minicrypto/mc_keywrap.h"
#include "../../bsd/dev/hakios.drivers.h"       /* HAKIOS_DEVICE */

enum {
	kAKSOpenCheck           = 0x00,
	kAKSGetLockState        = 0x07,         /* in: handle; out: state flags */
	kAKSLockDevice          = 0x08,         /* in: handle */
	kAKSUnlockDevice        = 0x09,         /* in: handle, the passcode's address in the caller, its length */
	kAKSWrapKey             = 0x0a,         /* in: handle, class; a key; out: the class, the key wrapped */
	kAKSUnwrapKey           = 0x0b,         /* in: handle, class; a wrapped key; out: the key */
	kAKSUnlockBag           = 0x0c,
	kAKSGetSystem           = 0x0e,         /* out: the handle of the system keybag */
	kAKSGetDeviceState      = 0x11,         /* in: handle; out: DER */
	kAKSGetBagUUID          = 0x17,         /* out: 16 bytes */
	kAKSAssertHold          = 0x1a,
	kAKSAssertDrop          = 0x1b,
	kAKSSetConfiguration    = 0x1e,         /* in: DER, as kAKSGetConfiguration's answer */
	kAKSGetConfiguration    = 0x1f,         /* in: handle; out: DER (a big buffer, so out of line) */
	kAKSLockDeviceNow       = 0x20,
	kAKSGetExtendedState    = 0x23,         /* as kAKSGetDeviceState */
	kAKSGeneration          = 0x28,
	kAKSVerifyPassword      = 0x2a,         /* in: handle; { u32 count; { u32 length; bytes, padded to 4 } ... }, the passcode first */
	kAKSAssertPromote       = 0x64,
	kAKSAssertConsume       = 0x65,
	kAKSNotifyStateSet      = 0x66,         /* "the first-unlock notification has been posted" */
	kAKSNotifyStateIsSet    = 0x67,
	kAKSPrewarm             = 0x73,

	/* this port's own, for userland/src/passcoded.m (the stand-in for keybagd's XPC service) */
	kHKChangeSecret         = 0x100,        /* in: struct HKChangeSecret and its bytes */
	kHKCopyBlob             = 0x101,        /* out: the opaque data stored with the passcode */
	kHKStatus               = 0x102,        /* out: has passcode, lock state, failed attempts, seconds of delay left, generation */
};

/* the messages MKBEventsRegister's callback knows (SpringBoardFoundation: 0 first unlock, 1 status changed) */
#define kAKSMessageStatusChanged        0xe3ffd00d
#define kAKSMessageFirstUnlock          0xe3ffd00f

/* state flags ("ss", and kAKSGetLockState's answer): bit 0 locked, bit 1 no passcode, bit 2 unlocked since boot */
#define kAKSStateLocked         0x1
#define kAKSStateNoPasscode     0x2
#define kAKSStateUnlockedOnce   0x4
/* lock state ("sls"): 0 unlocked, 1 locked, 2 locking, 3 disabled (no passcode) */
#define kAKSLockUnlocked        0
#define kAKSLockLocked          1
#define kAKSLockDisabled        3

/* answers that MobileKeyBag turns into its own codes: NotReadable is -3 (a wrong passcode), NotPermitted -2 */
#define kAKSWrongPasscode       kIOReturnNotReadable
#define kAKSBlocked             kIOReturnNotPermitted

/* the real key bag's answers to wrap and unwrap: what securityd's ks_crypt turns into errSecInteractionNotAllowed ("locked"),
 * errSecNotAvailable ("no key for this class") and errSecDecode ("can't be decrypted on this device, ever, so drop the item") */
#define kAKSNoPermission        kIOReturnNotPermitted
#define kAKSNoClassKey          kIOReturnNotFound
#define kAKSDecodeError         kIOReturnError   /* not AKS's own 0xe007c008: ks_crypt gives errSecNotAvailable for that, and the item stays and fails for ever */

#define kHKMaxSecret            256
#define kHKMaxBlob              2048
#define kHKIterations           100000
#define kHKMaxUnlockAttempts    10              /* what "mua" says; nothing here wipes, whatever it is */

struct HKChangeSecret {
	uint32_t magic;                         /* 'HKcs' */
	uint32_t oldLength;
	uint32_t newLength;
	uint32_t blobLength;
	/* then the old passcode, the new one (none: no passcode from now on), the opaque data */
};
#define kHKChangeMagic          0x484b6373

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the record on disk */

#define kHKPath                 "/private/var/hakios-passcode"
#define kHKSlotSize             4096
#define kHKRecordMagic          0x4850434b      /* 'HPCK' */
#define kHKRecordVersion        1
#define kHKBagPath              "/private/var/hakios-keybag"
#define kHKBagVersion           2

/* The key bag's secrets: a root key for each group of classes. D's is not protected by anything: there is no hardware key. */
enum { kTierA, kTierB, kTierC, kTierP, kHKTiers };
#define kHKRootSize             32
#define kHKWrappedRootSize      (kHKRootSize + MC_KW_OVERHEAD)
/* Argon2id's cost, kept in the bag. Measured on the phone: see docs/passcode.md. */
#define kHKKdfMemKiB            65536
#define kHKKdfPasses            3
#define kHKKdfMaxMemKiB         (512 * 1024)
#define kHKKdfMaxPasses         64

struct HKBag {
	uint32_t kdfMemKiB;
	uint32_t kdfPasses;
	uint8_t  kdfSalt[32];
	uint32_t wrappedTiers;                  /* a bit for each tier that has a wrapped root below */
	uint32_t migrating;                     /* the verifier above is the old file's: the next right passcode makes the bag */
	uint8_t  wrapped[kHKTiers][kHKWrappedRootSize];
	uint8_t  deviceRoot[kHKRootSize];       /* class D, in the clear: see the head of "the key bag" */
	uint8_t  seed[32];                      /* stirred into the kernel's generator at the next boot, then replaced */
};

struct HKRecord {
	uint32_t magic;
	uint32_t version;
	uint64_t generation;
	uint8_t  digest[32];                    /* SHA-256 of everything after it, to the end of the blob */
	uint32_t hasPasscode;
	uint32_t iterations;
	uint8_t  salt[16];
	uint8_t  verifier[32];
	uint32_t failedAttempts;
	uint32_t gracePeriod;                   /* seconds */
	uint64_t blockedUntil;                  /* calendar seconds; 0: no delay */
	uint32_t blobLength;
	uint32_t reserved;
	uint8_t  blob[kHKMaxBlob];
	HKBag    bag;                           /* the key bag's file only (the old file's records end at blob) */
};

#define kHKV1Size               __builtin_offsetof(HKRecord, bag)

static_assert(sizeof(HKRecord) <= kHKSlotSize, "a record fits in a slot");

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the state */

static IOLock *         gAKSLock;
static IOService *      gAKSService;
static uint64_t         gAKSLogged[4];  /* selectors already reported */
static bool             gAKSNotifyState;

static struct {
	bool     loaded;                        /* the file has been read (or found absent) */
	bool     kdfGood;                       /* PBKDF2 passed its self-test */
	HKRecord record;                        /* the current record, as in memory */
	uint64_t generation;                    /* the newest generation on disk */
	int      newestSlot;                    /* where it is; -1 none */
	bool     locked;
	bool     unlockedOnce;                  /* since boot */
	uint64_t blockedUntilUptime;            /* uptime, ns; 0: no delay */
	bool     real;                          /* keystore_real=1: the key bag */
	bool     damaged;                       /* a key bag that cannot be read: nothing opens, nothing is overwritten */
	bool     persisted;                     /* the bag in memory is on disk (no class key is handed out before it is) */
	bool     kdfBusy;                       /* a key derivation is running, without the lock */
	uint32_t kdfMilliseconds;               /* the last one, for tuning */
	bool     have[kHKTiers];
	uint8_t  roots[kHKTiers][kHKRootSize];
} gKS;

static void realLoad(void);
static IOReturn tryPasscodeReal(const uint8_t * passcode, size_t length, bool unlock, bool * changed, bool * first, uint8_t (*keep)[kHKRootSize]);
static IOReturn changeSecretReal(const uint8_t * oldSecret, size_t oldLength, const uint8_t * newSecret, size_t newLength,
    const uint8_t * blob, size_t blobLength, bool * changed, bool * first);
static void dropLockedRoots(void);

static uint64_t
uptimeNanoseconds(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

static uint64_t
calendarSeconds(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	clock_get_calendar_microtime(&secs, &usecs);
	return (uint64_t) secs;
}

/* The delay after the n-th failed attempt, in seconds (Apple Platform Security, "Passcodes"). */
static uint32_t
delayAfter(uint32_t failed)
{
	if (failed < 5) {
		return 0;
	}
	switch (failed) {
	case 5:  return 60;
	case 6:  return 5 * 60;
	case 7:
	case 8:  return 15 * 60;
	default: return 60 * 60;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * PBKDF2-HMAC-SHA256 */

struct HMACKey {
	SHA256_CTX inner, outer;                /* after the padded key */
};

static void
hmacKey(HMACKey * k, const uint8_t * key, size_t keyLength)
{
	uint8_t block[64], hashed[32];

	if (keyLength > sizeof(block)) {
		SHA256_CTX c;
		SHA256_Init(&c);
		SHA256_Update(&c, key, keyLength);
		SHA256_Final(hashed, &c);
		key = hashed;
		keyLength = sizeof(hashed);
	}
	memset(block, 0, sizeof(block));
	memcpy(block, key, keyLength);
	for (size_t i = 0; i < sizeof(block); i++) {
		block[i] ^= 0x36;
	}
	SHA256_Init(&k->inner);
	SHA256_Update(&k->inner, block, sizeof(block));
	for (size_t i = 0; i < sizeof(block); i++) {
		block[i] ^= 0x36 ^ 0x5c;
	}
	SHA256_Init(&k->outer);
	SHA256_Update(&k->outer, block, sizeof(block));
	mc_wipe(block, sizeof(block));
	mc_wipe(hashed, sizeof(hashed));
}

static void
hmac(const HMACKey * k, const uint8_t * a, size_t aLength, const uint8_t * b, size_t bLength, uint8_t out[32])
{
	SHA256_CTX c = k->inner;
	uint8_t inner[32];

	SHA256_Update(&c, a, aLength);
	if (bLength) {
		SHA256_Update(&c, b, bLength);
	}
	SHA256_Final(inner, &c);
	c = k->outer;
	SHA256_Update(&c, inner, sizeof(inner));
	SHA256_Final(out, &c);
	mc_wipe(&c, sizeof(c));
	mc_wipe(inner, sizeof(inner));
}

static void
pbkdf2(const uint8_t * password, size_t passwordLength, const uint8_t * salt, size_t saltLength, uint32_t iterations,
    uint8_t * out, size_t outLength)
{
	HMACKey k;

	hmacKey(&k, password, passwordLength);
	for (uint32_t block = 1; outLength > 0; block++) {
		uint8_t count[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t) block };
		uint8_t u[32], t[32];
		size_t n = outLength < 32 ? outLength : 32;

		hmac(&k, salt, saltLength, count, sizeof(count), u);
		memcpy(t, u, sizeof(t));
		for (uint32_t i = 1; i < iterations; i++) {
			hmac(&k, u, sizeof(u), NULL, 0, u);
			for (size_t j = 0; j < sizeof(t); j++) {
				t[j] ^= u[j];
			}
		}
		memcpy(out, t, n);
		out += n;
		outLength -= n;
		mc_wipe(u, sizeof(u));
		mc_wipe(t, sizeof(t));
	}
	mc_wipe(&k, sizeof(k));
}

/* RFC 7914 section 11: PBKDF2-HMAC-SHA256 (P="passwd", S="salt", c=1), the first 32 of its 64 bytes */
static bool
pbkdf2SelfTest(void)
{
	static const uint8_t expected[32] = {
		0x55, 0xac, 0x04, 0x6e, 0x56, 0xe3, 0x08, 0x9f, 0xec, 0x16, 0x91, 0xc2, 0x25, 0x44, 0xb6, 0x05,
		0xf9, 0x41, 0x85, 0x21, 0x6d, 0xde, 0x04, 0x65, 0xe6, 0x8b, 0x9d, 0x57, 0xc2, 0x0d, 0xac, 0xbc,
	};
	uint8_t out[32];

	pbkdf2((const uint8_t *) "passwd", 6, (const uint8_t *) "salt", 4, 1, out, sizeof(out));
	return memcmp(out, expected, sizeof(out)) == 0;
}

static bool
sameBytes(const uint8_t * a, const uint8_t * b, size_t length)
{
	uint8_t d = 0;

	for (size_t i = 0; i < length; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the file */

static void
recordDigest(const HKRecord * r, uint8_t out[32])
{
	SHA256_CTX c;
	const uint8_t * from = (const uint8_t *) &r->hasPasscode;
	size_t length = __builtin_offsetof(HKRecord, blob) - __builtin_offsetof(HKRecord, hasPasscode) + (r->blobLength <= kHKMaxBlob ? r->blobLength : 0);

	SHA256_Init(&c);
	SHA256_Update(&c, from, length);
	if (r->version == kHKBagVersion) {
		SHA256_Update(&c, &r->bag, sizeof(r->bag));
	}
	SHA256_Final(out, &c);
}

static bool
recordValid(const HKRecord * r, uint32_t version)
{
	uint8_t digest[32];

	if (r->magic != kHKRecordMagic || r->version != version || r->blobLength > kHKMaxBlob) {
		return false;
	}
	recordDigest(r, digest);
	if (!sameBytes(digest, r->digest, sizeof(digest))) {
		return false;
	}
	if (version == kHKRecordVersion && r->hasPasscode && (r->iterations == 0 || r->iterations > 10000000)) {
		return false;
	}
	return true;
}

static int
fileIOAt(const char * path, size_t size, enum uio_rw rw, int slot, HKRecord * r)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int resid = 0, error;
	int fmode = rw == UIO_READ ? FREAD : (FREAD | FWRITE | O_CREAT);

	error = vnode_open(path, fmode, 0600, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
	if (error) {
		return error;
	}
	error = vn_rdwr(rw, vp, (caddr_t) r, (int) size, (off_t) slot * kHKSlotSize, UIO_SYSSPACE,
	    rw == UIO_WRITE ? IO_SYNC | IO_NOAUTH : IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (error == 0 && resid != 0) {
		error = rw == UIO_READ ? ENOENT : EIO;      /* a short slot: never written */
	}
	if (error == 0 && rw == UIO_WRITE) {
		error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
	}
	vnode_close(vp, fmode, ctx);
	return error;
}

static int
fileIO(enum uio_rw rw, int slot, HKRecord * r)
{
	return gKS.real ? fileIOAt(kHKBagPath, sizeof(*r), rw, slot, r) : fileIOAt(kHKPath, kHKV1Size, rw, slot, r);
}

/* Write r as the next generation into the slot that does not hold the newest one. */
static int
persist(HKRecord * r)
{
	int slot = gKS.newestSlot == 0 ? 1 : 0;
	int error;

	if (gKS.damaged) {
		return EROFS;                           /* whatever is left of it stays for someone to look at */
	}
	r->magic = kHKRecordMagic;
	r->version = gKS.real ? kHKBagVersion : kHKRecordVersion;
	r->generation = gKS.generation + 1;
	recordDigest(r, r->digest);
	error = fileIO(UIO_WRITE, slot, r);
	if (error) {
		printf("MiniAppleKeyStore: cannot write %s slot %d (error %d)\n", gKS.real ? kHKBagPath : kHKPath, slot, error);
		return error;
	}
	gKS.generation = r->generation;
	gKS.newestSlot = slot;
	return 0;
}

static void
setBlockedUntil(HKRecord * r, uint32_t seconds)
{
	if (seconds == 0) {
		r->blockedUntil = 0;
		gKS.blockedUntilUptime = 0;
	} else {
		r->blockedUntil = calendarSeconds() + seconds;
		gKS.blockedUntilUptime = uptimeNanoseconds() + (uint64_t) seconds * NSEC_PER_SEC;
	}
}

static uint32_t
secondsBlocked(void)
{
	uint64_t now = uptimeNanoseconds();

	if (gKS.blockedUntilUptime == 0 || now >= gKS.blockedUntilUptime) {
		return 0;
	}
	return (uint32_t) ((gKS.blockedUntilUptime - now + NSEC_PER_SEC - 1) / NSEC_PER_SEC);
}

static void
noPasscode(HKRecord * r)
{
	uint32_t grace = r->gracePeriod;

	memset(r, 0, sizeof(*r));
	r->gracePeriod = grace;
}

/* The first call of the boot: read the record (with the key store's lock held). */
static void
load(void)
{
	HKRecord * slots[2] = { NULL, NULL };
	uint32_t reset = 0;
	int error[2] = { 0, 0 };

	gKS.loaded = true;
	gKS.newestSlot = -1;
	gKS.unlockedOnce = true;
	if (gKS.real) {
		realLoad();
		return;
	}
	gKS.kdfGood = pbkdf2SelfTest();
	if (!gKS.kdfGood) {
		printf("MiniAppleKeyStore: PBKDF2-HMAC-SHA256 fails its test vector: no passcode can be set or used\n");
	}
	for (int i = 0; i < 2; i++) {
		slots[i] = IONew(HKRecord, 1);
		if (slots[i] == NULL) {
			error[i] = ENOMEM;
			continue;
		}
		memset(slots[i], 0, sizeof(HKRecord));
		error[i] = fileIO(UIO_READ, i, slots[i]);
		if (error[i] == 0 && !recordValid(slots[i], kHKRecordVersion)) {
			error[i] = EINVAL;
		}
		if (error[i] == 0 && slots[i]->generation >= gKS.generation) {
			gKS.generation = slots[i]->generation;
			gKS.newestSlot = i;
		}
	}
	if (gKS.newestSlot >= 0) {
		gKS.record = *slots[gKS.newestSlot];
	}
	for (int i = 0; i < 2; i++) {
		if (slots[i] != NULL) {
			memset(slots[i], 0, sizeof(HKRecord));
			IODelete(slots[i], HKRecord, 1);
		}
	}

	if (PE_parse_boot_argn("passcode_reset", &reset, sizeof(reset)) && reset != 0) {
		bool had = gKS.newestSlot >= 0 && gKS.record.hasPasscode;

		noPasscode(&gKS.record);
		/* both slots, so that no older record with a passcode is left to come back */
		for (int i = 0; i < 2; i++) {
			if (persist(&gKS.record) != 0) {
				break;
			}
		}
		printf("MiniAppleKeyStore: passcode_reset=%u: %s; %s now holds no passcode\n", reset,
		    had ? "the stored passcode is discarded" : "there was no passcode", kHKPath);
	}

	if (gKS.newestSlot < 0) {
		printf("MiniAppleKeyStore: %s: %s (slot errors %d, %d): no passcode\n", kHKPath,
		    error[0] == ENOENT && error[1] == ENOENT ? "absent" : "no valid record", error[0], error[1]);
		noPasscode(&gKS.record);
		return;
	}
	if (gKS.record.hasPasscode && !gKS.kdfGood) {
		printf("MiniAppleKeyStore: the stored passcode is ignored (the key derivation is broken): no passcode\n");
		noPasscode(&gKS.record);
		return;
	}
	if (gKS.record.hasPasscode) {
		uint64_t now = calendarSeconds();
		uint32_t delay = 0;

		gKS.locked = true;
		gKS.unlockedOnce = false;
		if (gKS.record.blockedUntil > now) {
			uint64_t left = gKS.record.blockedUntil - now;
			uint32_t most = delayAfter(gKS.record.failedAttempts);

			delay = left > most ? most : (uint32_t) left;       /* a clock that went back does not lengthen it */
			gKS.blockedUntilUptime = uptimeNanoseconds() + (uint64_t) delay * NSEC_PER_SEC;
		}
		printf("MiniAppleKeyStore: passcode set (generation %llu, slot %d), locked; %u failed attempts, %u s of delay left\n",
		    gKS.generation, gKS.newestSlot, gKS.record.failedAttempts, delay);
	} else {
		printf("MiniAppleKeyStore: %s generation %llu: no passcode\n", kHKPath, gKS.generation);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the state machine (lock held) */

static uint32_t
stateFlags(void)
{
	uint32_t flags = 0;

	if (!gKS.record.hasPasscode) {
		return kAKSStateNoPasscode | kAKSStateUnlockedOnce;
	}
	if (gKS.locked) {
		flags |= kAKSStateLocked;
	}
	if (gKS.unlockedOnce) {
		flags |= kAKSStateUnlockedOnce;
	}
	return flags;
}

static uint32_t
lockState(void)
{
	if (!gKS.record.hasPasscode) {
		return kAKSLockDisabled;
	}
	return gKS.locked ? kAKSLockLocked : kAKSLockUnlocked;
}

/* A passcode try. *changed says whether the lock state changed, *first whether this is the first unlock. */
static IOReturn
tryPasscode(const uint8_t * passcode, size_t length, bool unlock, bool * changed, bool * first)
{
	uint8_t derived[32];
	bool good;

	if (gKS.real) {
		return tryPasscodeReal(passcode, length, unlock, changed, first, NULL);
	}
	if (!gKS.record.hasPasscode) {
		return kIOReturnSuccess;
	}
	if (secondsBlocked() > 0) {
		return kAKSBlocked;
	}
	if (length == 0 || !gKS.kdfGood) {
		return kAKSWrongPasscode;                        /* not counted: nothing was tried */
	}
	pbkdf2(passcode, length, gKS.record.salt, sizeof(gKS.record.salt), gKS.record.iterations, derived, sizeof(derived));
	good = sameBytes(derived, gKS.record.verifier, sizeof(derived));
	mc_wipe(derived, sizeof(derived));

	if (!good) {
		uint32_t delay;

		if (gKS.record.failedAttempts < 1000) {
			gKS.record.failedAttempts++;
		}
		delay = delayAfter(gKS.record.failedAttempts);
		setBlockedUntil(&gKS.record, delay);
		persist(&gKS.record);                            /* if it cannot be written, memory still counts it */
		printf("MiniAppleKeyStore: wrong passcode (%u failed attempts; %u s before the next)\n", gKS.record.failedAttempts, delay);
		*changed = true;                                 /* the failed count and the delay are state too */
		return kAKSWrongPasscode;
	}
	if (gKS.record.failedAttempts != 0 || gKS.record.blockedUntil != 0) {
		gKS.record.failedAttempts = 0;
		setBlockedUntil(&gKS.record, 0);
		persist(&gKS.record);
		*changed = true;
	}
	if (unlock) {
		if (gKS.locked) {
			gKS.locked = false;
			*changed = true;
			printf("MiniAppleKeyStore: unlocked with the passcode\n");
		}
		if (!gKS.unlockedOnce) {
			gKS.unlockedOnce = true;
			*first = true;
		}
	}
	return kIOReturnSuccess;
}

static IOReturn
changeSecret(const uint8_t * oldSecret, size_t oldLength, const uint8_t * newSecret, size_t newLength,
    const uint8_t * blob, size_t blobLength, bool * changed, bool * first)
{
	HKRecord * next;
	IOReturn ret;
	int error;

	if (gKS.real) {
		return changeSecretReal(oldSecret, oldLength, newSecret, newLength, blob, blobLength, changed, first);
	}
	if (newLength > 0 && !gKS.kdfGood) {
		return kIOReturnUnsupported;
	}
	if (gKS.record.hasPasscode) {
		ret = tryPasscode(oldSecret, oldLength, false, changed, first);
		if (ret != kIOReturnSuccess) {
			return ret;
		}
	}
	next = IONew(HKRecord, 1);
	if (next == NULL) {
		return kIOReturnNoMemory;
	}
	*next = gKS.record;
	if (newLength == 0) {
		noPasscode(next);
	} else {
		next->hasPasscode = 1;
		next->iterations = kHKIterations;
		read_random(next->salt, sizeof(next->salt));
		pbkdf2(newSecret, newLength, next->salt, sizeof(next->salt), next->iterations, next->verifier, sizeof(next->verifier));
		next->failedAttempts = 0;
		next->blockedUntil = 0;
	}
	next->blobLength = (uint32_t) blobLength;
	memset(next->blob, 0, sizeof(next->blob));
	if (blobLength) {
		memcpy(next->blob, blob, blobLength);
	}
	error = persist(next);
	if (error) {
		/* nothing changes unless it is kept: a passcode that would be gone at the next start is not set */
		memset(next, 0, sizeof(*next));
		IODelete(next, HKRecord, 1);
		return kIOReturnIOError;
	}
	gKS.record = *next;
	memset(next, 0, sizeof(*next));
	IODelete(next, HKRecord, 1);
	gKS.blockedUntilUptime = 0;
	gKS.locked = false;
	if (!gKS.unlockedOnce) {
		gKS.unlockedOnce = true;
		*first = true;
	}
	*changed = true;
	printf("MiniAppleKeyStore: passcode %s (generation %llu)\n", newLength ? "set" : "removed", gKS.generation);
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the key bag (keystore_real=1) */

/*
 * What this protects, and from whom, is in docs/keystore.md; read that before believing anything below. In short: there is no
 * Secure Enclave and (so far) no hardware key, so the only secret is the passcode, and a passcode's strength is all there is.
 *
 *   tier   classes (AppleKeyStore's numbers)                  root available
 *   A      1 file A; 6 ak, 9 aku (keychain "when unlocked")   while unlocked; wiped from memory at each lock
 *   B      2 file B                                           as A for now (the asymmetric part goes with file protection)
 *   C      3 file C; 7 ck, 10 cku ("after first unlock")      from the first unlock until the restart
 *   P      12 akpu ("when passcode set")                      as A, and only while there is a passcode; destroyed when it is removed
 *   D      4 file D; 8 dk, 11 dku ("always")                  always. In the clear in the file: it can only ever be thrown away.
 *
 * Each root is random; A, B, C and P are kept wrapped (AES-256 key wrap) under a key made from the passcode:
 *     Argon2id(passcode, a random salt) -> hwTangle() -> HMAC-SHA-256 -> KEK
 * RFC 3394's integrity check is the verifier: there is nothing else to compare with, so a wrong passcode is known only by the
 * keys not coming out. With no passcode the KEK comes from the device root instead, which is to say the file holds the keys in
 * all but name, as iOS's would with no passcode if it had no hardware key. A class's key is HMAC-SHA-256 of its tier's root
 * and its number.
 *
 * Fail closed. A file that does not exist is a new bag with new keys (what was wrapped before stays unreadable: deleting the
 * file gets no one anything). A file that exists and cannot be read, or that no key comes out of, is damage: the device is
 * locked, no passcode opens it, nothing is overwritten, and only passcode_reset=1 gets out, by destroying the keys.
 */

static const uint8_t kKEKLabel[] = "HakiOS key bag KEK 2";
static const uint8_t kNoPasscodeLabel[] = "HakiOS key bag no passcode 2";
static const uint8_t kClassLabel[] = "HakiOS key bag class 2";

static int
classTier(uint32_t keyclass)
{
	switch (keyclass & 0x1f) {
	case 1: case 6: case 9:                 return kTierA;
	case 2:                                 return kTierB;
	case 3: case 7: case 10:                return kTierC;
	case 12:                                return kTierP;
	case 4: case 8: case 11:                return kHKTiers;    /* D */
	default:                                return -1;
	}
}

/*
 * Where a key from outside the kernel would go in. Today: nothing is mixed in, so the result depends on the passcode and the salt
 * alone (see docs/keystore.md on the TEE route). Changing what this does makes every existing bag unreadable.
 */
static bool
hwTangle(const uint8_t in[32], uint8_t out[32])
{
	memcpy(out, in, 32);
	return true;
}

static void
hmacOne(const uint8_t * key, size_t keyLength, const uint8_t * data, size_t dataLength, const uint8_t * data2, size_t data2Length, uint8_t out[32])
{
	HMACKey k;

	hmacKey(&k, key, keyLength);
	hmac(&k, data, dataLength, data2, data2Length, out);
	mc_wipe(&k, sizeof(k));
}

/* The passcode's KEK, from Argon2id. Long: the caller has let go of the lock. */
static IOReturn
kekFromPasscode(const uint8_t * passcode, size_t length, uint32_t memKiB, uint32_t passes, const uint8_t * salt, size_t saltLength, uint8_t kek[32])
{
	size_t size = (size_t) memKiB * MC_ARGON2_BLOCK;
	void * work = IOMallocAligned(size, PAGE_SIZE);
	uint8_t derived[32], tangled[32];
	uint64_t started;
	bool good;

	if (work == NULL) {
		printf("MiniAppleKeyStore: no %u KiB for the key derivation\n", memKiB);
		return kIOReturnNoMemory;
	}
	started = uptimeNanoseconds();
	mc_argon2id(derived, work, memKiB, passes, passcode, length, salt, saltLength);
	gKS.kdfMilliseconds = (uint32_t) ((uptimeNanoseconds() - started) / NSEC_PER_MSEC);
	IOFreeAligned(work, size);
	printf("MiniAppleKeyStore: Argon2id %u KiB x %u: %u ms\n", memKiB, passes, gKS.kdfMilliseconds);
	good = hwTangle(derived, tangled);
	if (good) {
		hmacOne(tangled, sizeof(tangled), kKEKLabel, sizeof(kKEKLabel) - 1, NULL, 0, kek);
	}
	mc_wipe(derived, sizeof(derived));
	mc_wipe(tangled, sizeof(tangled));
	return good ? kIOReturnSuccess : kIOReturnNotReady;
}

static void
kekWithoutPasscode(const HKRecord * r, uint8_t kek[32])
{
	hmacOne(r->bag.deviceRoot, sizeof(r->bag.deviceRoot), kNoPasscodeLabel, sizeof(kNoPasscodeLabel) - 1, NULL, 0, kek);
}

/* The roots out of a bag: every root it says it holds, or none. */
static bool
unwrapRoots(const HKRecord * r, const uint8_t kek[32], uint8_t (*roots)[kHKRootSize], bool * have)
{
	for (int t = 0; t < kHKTiers; t++) {
		have[t] = false;
		if (!(r->bag.wrappedTiers & (1u << t))) {
			continue;
		}
		if (mc_aes256_kw_unwrap(kek, r->bag.wrapped[t], kHKWrappedRootSize, roots[t]) != 0) {
			mc_wipe(roots, kHKTiers * kHKRootSize);
			for (int u = 0; u < kHKTiers; u++) {
				have[u] = false;
			}
			return false;
		}
		have[t] = true;
	}
	return r->bag.wrappedTiers != 0;
}

static void
wrapRoots(HKRecord * r, const uint8_t kek[32], const uint8_t (*roots)[kHKRootSize], const bool * have)
{
	r->bag.wrappedTiers = 0;
	memset(r->bag.wrapped, 0, sizeof(r->bag.wrapped));
	for (int t = 0; t < kHKTiers; t++) {
		if (have[t] && mc_aes256_kw_wrap(kek, roots[t], kHKRootSize, r->bag.wrapped[t]) == 0) {
			r->bag.wrappedTiers |= 1u << t;
		}
	}
}

/* What the machine has to offer beyond the generator's state: the old seed, and how late a few delays run. See docs/keystore.md. */
static void
stirEntropy(const uint8_t * seed, size_t seedLength)
{
	uint64_t samples[64];

	if (seed != NULL) {
		write_random((void *) seed, (u_int) seedLength);
	}
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		IODelay(20 + (i & 7));
		samples[i] = mach_absolute_time();
	}
	write_random(samples, sizeof(samples));
	mc_wipe(samples, sizeof(samples));
}

static void
kdfCost(uint32_t * memKiB, uint32_t * passes)
{
	uint32_t phase = 0, kib = 0;

	*memKiB = kHKKdfMemKiB;
	*passes = kHKKdfPasses;
	/* the test's shortcut, QEMU being slow: never without the test's own boot-argument */
	if (PE_parse_boot_argn("passcode_selftest", &phase, sizeof(phase)) && phase != 0 &&
	    PE_parse_boot_argn("keystore_kdf_kib", &kib, sizeof(kib)) && kib >= 8 && kib <= kHKKdfMaxMemKiB) {
		*memKiB = kib;
		*passes = 1;
	}
}

static bool
bagCostValid(const HKRecord * r)
{
	return r->bag.kdfMemKiB >= 8 && r->bag.kdfMemKiB <= kHKKdfMaxMemKiB && r->bag.kdfPasses >= 1 && r->bag.kdfPasses <= kHKKdfMaxPasses;
}

static void
wipeRoots(void)
{
	mc_wipe(gKS.roots, sizeof(gKS.roots));
	for (int t = 0; t < kHKTiers; t++) {
		gKS.have[t] = false;
	}
}

/* At a lock: what iOS's classes A and B and "when passcode set" give up. C stays until the restart. */
static void
dropLockedRoots(void)
{
	static const int tiers[] = { kTierA, kTierB, kTierP };

	if (!gKS.real) {
		return;
	}
	for (size_t i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
		mc_wipe(gKS.roots[tiers[i]], kHKRootSize);
		gKS.have[tiers[i]] = false;
	}
}

/* Overwrite the old file's passcode verifier: left there, it is a far cheaper way to test guesses than the bag. */
static void
wipeOldFile(void)
{
	HKRecord * r = IONew(HKRecord, 1);

	if (r == NULL) {
		return;
	}
	memset(r, 0, sizeof(*r));
	r->magic = kHKRecordMagic;
	r->version = kHKRecordVersion;
	r->generation = 1;
	recordDigest(r, r->digest);
	for (int i = 0; i < 2; i++) {
		int error = fileIOAt(kHKPath, kHKV1Size, UIO_WRITE, i, r);

		if (error != 0 && error != ENOENT) {
			printf("MiniAppleKeyStore: cannot overwrite %s slot %d (error %d)\n", kHKPath, i, error);
		}
	}
	mc_wipe(r, sizeof(*r));
	IODelete(r, HKRecord, 1);
}

/* Whether the old kind of file holds a passcode; old receives the newer of its slots. */
static bool
oldFilePasscode(HKRecord * old)
{
	HKRecord * other = IONew(HKRecord, 1);
	bool oldGood, otherGood;

	if (other == NULL) {
		return false;
	}
	memset(old, 0, sizeof(*old));
	memset(other, 0, sizeof(*other));
	oldGood = fileIOAt(kHKPath, kHKV1Size, UIO_READ, 0, old) == 0 && recordValid(old, kHKRecordVersion);
	otherGood = fileIOAt(kHKPath, kHKV1Size, UIO_READ, 1, other) == 0 && recordValid(other, kHKRecordVersion);
	if (otherGood && (!oldGood || other->generation > old->generation)) {
		memcpy(old, other, sizeof(*old));
		oldGood = true;
	}
	mc_wipe(other, sizeof(*other));
	IODelete(other, HKRecord, 1);
	return oldGood && old->hasPasscode;
}

/* The old file's passcode is in force from now until the first unlock, which makes the bag's keys wrapped under it (migrateBag). */
static void
takeOldPasscode(const HKRecord * old)
{
	gKS.record.bag.migrating = 1;
	gKS.record.hasPasscode = 1;
	gKS.record.iterations = old->iterations;
	memcpy(gKS.record.salt, old->salt, sizeof(gKS.record.salt));
	memcpy(gKS.record.verifier, old->verifier, sizeof(gKS.record.verifier));
	gKS.record.failedAttempts = old->failedAttempts;
	gKS.record.gracePeriod = old->gracePeriod;
	gKS.record.blobLength = old->blobLength;
	memcpy(gKS.record.blob, old->blob, sizeof(gKS.record.blob));
	gKS.locked = true;
	gKS.unlockedOnce = false;
}

/* New keys for everything, no passcode: whatever was wrapped before is gone. In memory; the caller keeps the record. */
static void
newBag(HKRecord * r)
{
	uint32_t grace = r->gracePeriod;
	uint8_t kek[32];
	bool have[kHKTiers] = {};

	stirEntropy(NULL, 0);
	memset(r, 0, sizeof(*r));
	r->gracePeriod = grace;
	wipeRoots();
	read_random(r->bag.deviceRoot, sizeof(r->bag.deviceRoot));
	read_random(r->bag.seed, sizeof(r->bag.seed));
	for (int t = 0; t < kHKTiers; t++) {
		if (t != kTierP) {
			read_random(gKS.roots[t], kHKRootSize);
			gKS.have[t] = have[t] = true;
		}
	}
	kekWithoutPasscode(r, kek);
	wrapRoots(r, kek, gKS.roots, have);
	mc_wipe(kek, sizeof(kek));
}

static void
damagedBag(const char * why)
{
	printf("MiniAppleKeyStore: %s: %s; locked, and no passcode opens it. Only passcode_reset=1 gets out, and it destroys the keys.\n", kHKBagPath, why);
	wipeRoots();
	memset(&gKS.record, 0, sizeof(gKS.record));
	gKS.record.hasPasscode = 1;
	gKS.damaged = true;
	gKS.locked = true;
	gKS.unlockedOnce = false;
}

/* The first call of the boot, with keystore_real=1. */
static void
realLoad(void)
{
	HKRecord * slots[2] = { NULL, NULL };
	uint32_t reset = 0;
	int error[2] = { 0, 0 };

	gKS.damaged = false;
	gKS.persisted = false;
	gKS.generation = 0;
	wipeRoots();
	gKS.kdfGood = pbkdf2SelfTest() && mc_keywrap_selftest();
	for (int i = 0; i < 2; i++) {
		slots[i] = IONew(HKRecord, 1);
		if (slots[i] == NULL) {
			error[i] = ENOMEM;
			continue;
		}
		memset(slots[i], 0, sizeof(HKRecord));
		error[i] = fileIOAt(kHKBagPath, sizeof(HKRecord), UIO_READ, i, slots[i]);
		if (error[i] == 0 && !recordValid(slots[i], kHKBagVersion)) {
			error[i] = EINVAL;
		}
		if (error[i] == 0 && slots[i]->generation >= gKS.generation) {
			gKS.generation = slots[i]->generation;
			gKS.newestSlot = i;
		}
	}
	if (gKS.newestSlot >= 0) {
		gKS.record = *slots[gKS.newestSlot];
	}
	for (int i = 0; i < 2; i++) {
		if (slots[i] != NULL) {
			mc_wipe(slots[i], sizeof(HKRecord));
			IODelete(slots[i], HKRecord, 1);
		}
	}

	if (!gKS.kdfGood) {
		damagedBag("the key derivation or key wrap fails its known answers");
		return;
	}
	if (PE_parse_boot_argn("passcode_reset", &reset, sizeof(reset)) && reset != 0) {
		/* both slots, so that no older record with the old keys is left to come back */
		newBag(&gKS.record);
		for (int i = 0; i < 2; i++) {
			if (persist(&gKS.record) != 0) {
				break;
			}
		}
		gKS.persisted = gKS.newestSlot >= 0;
		wipeOldFile();
		printf("MiniAppleKeyStore: passcode_reset=%u: the key bag's keys are destroyed, and what they protected with them; %s now holds new keys and no passcode\n",
		    reset, kHKBagPath);
		return;
	}
	if (gKS.newestSlot < 0) {
		if ((error[0] != ENOENT && error[0] != 0) || (error[1] != ENOENT && error[1] != 0)) {
			printf("MiniAppleKeyStore: slot errors %d, %d\n", error[0], error[1]);
			damagedBag("no valid record");
			return;
		}
		/* No bag. The old file's passcode, if there is one, stays in force until the first unlock makes the bag from it. */
		HKRecord * old = IONew(HKRecord, 1);
		bool migrate = old != NULL && oldFilePasscode(old);

		newBag(&gKS.record);
		if (migrate) {
			wipeRoots();
			gKS.record.bag.wrappedTiers = 0;
			memset(gKS.record.bag.wrapped, 0, sizeof(gKS.record.bag.wrapped));
			takeOldPasscode(old);
		}
		if (old != NULL) {
			mc_wipe(old, sizeof(*old));
			IODelete(old, HKRecord, 1);
		}
		gKS.persisted = persist(&gKS.record) == 0;
		printf("MiniAppleKeyStore: %s: new key bag (%s)%s\n", kHKBagPath, migrate ? "the old passcode stays until the first unlock" : "no passcode",
		    gKS.persisted ? "" : ", not written yet");
		return;
	}
	gKS.persisted = true;
	stirEntropy(gKS.record.bag.seed, sizeof(gKS.record.bag.seed));
	read_random(gKS.record.bag.seed, sizeof(gKS.record.bag.seed));
	if (!gKS.record.bag.migrating) {
		/*
		 * A passcode in the old file beside a bag: an older kernel, or this one with keystore_real=0, set it after the bag was made
		 * (everything here that makes a bag's passcode empties that file). With no passcode in the bag, the phone must not
		 * come up open: that passcode holds, over the bag's keys. With one, the bag's holds, and the file is only a cheap way to test guesses.
		 */
		HKRecord * old = IONew(HKRecord, 1);

		if (old != NULL) {
			if (oldFilePasscode(old)) {
				if (gKS.record.hasPasscode) {
					wipeOldFile();
					printf("MiniAppleKeyStore: the bag's passcode holds; the passcode in %s is overwritten\n", kHKPath);
				} else {
					takeOldPasscode(old);
					printf("MiniAppleKeyStore: %s holds a passcode and the bag none: it stays until the first unlock\n", kHKPath);
				}
			}
			mc_wipe(old, sizeof(*old));
			IODelete(old, HKRecord, 1);
		}
	}
	if (gKS.record.hasPasscode) {
		if (!gKS.record.bag.migrating && !bagCostValid(&gKS.record)) {
			damagedBag("an impossible key derivation cost");
			return;
		}
		if (!gKS.record.bag.migrating && (gKS.record.bag.wrappedTiers & (1u << kTierA | 1u << kTierC)) != (1u << kTierA | 1u << kTierC)) {
			damagedBag("a passcode, and no keys under it");
			return;
		}
		uint64_t now = calendarSeconds();
		uint32_t delay = 0;

		gKS.locked = true;
		gKS.unlockedOnce = false;
		if (gKS.record.blockedUntil > now) {
			uint64_t left = gKS.record.blockedUntil - now;
			uint32_t most = delayAfter(gKS.record.failedAttempts);

			delay = left > most ? most : (uint32_t) left;
			gKS.blockedUntilUptime = uptimeNanoseconds() + (uint64_t) delay * NSEC_PER_SEC;
		}
		printf("MiniAppleKeyStore: key bag generation %llu, slot %d: passcode set, locked; %u failed attempts, %u s of delay left\n",
		    gKS.generation, gKS.newestSlot, gKS.record.failedAttempts, delay);
	} else {
		uint8_t kek[32];

		kekWithoutPasscode(&gKS.record, kek);
		if (!unwrapRoots(&gKS.record, kek, gKS.roots, gKS.have)) {
			mc_wipe(kek, sizeof(kek));
			damagedBag("no passcode, and the keys do not come out");
			return;
		}
		mc_wipe(kek, sizeof(kek));
		printf("MiniAppleKeyStore: key bag generation %llu, slot %d: no passcode\n", gKS.generation, gKS.newestSlot);
	}
	persist(&gKS.record);                           /* the new seed */
}

/* The KDF on what a record says, without the lock. kdfBusy keeps the other tries and changes out meanwhile. */
static IOReturn
kdfUnlocked(const uint8_t * passcode, size_t length, uint32_t memKiB, uint32_t passes, const uint8_t * salt, size_t saltLength, uint8_t kek[32])
{
	IOReturn ret;

	gKS.kdfBusy = true;
	IOLockUnlock(gAKSLock);
	ret = kekFromPasscode(passcode, length, memKiB, passes, salt, saltLength, kek);
	IOLockLock(gAKSLock);
	gKS.kdfBusy = false;
	return ret;
}

/* The first right passcode after keystore_real=1 meets the old file: the bag's keys, wrapped from now on. Nothing wrapped before this has any secrecy to keep. */
static IOReturn
migrateBag(const uint8_t * passcode, size_t length)
{
	uint8_t kek[32];
	bool have[kHKTiers] = {};
	uint32_t memKiB, passes;
	IOReturn ret;

	/* a bag that already holds keys (under no passcode) keeps them, with what is wrapped under them; otherwise new ones */
	if (gKS.record.bag.wrappedTiers != 0) {
		bool good;

		kekWithoutPasscode(&gKS.record, kek);
		good = unwrapRoots(&gKS.record, kek, gKS.roots, gKS.have);
		mc_wipe(kek, sizeof(kek));
		if (!good) {
			damagedBag("no passcode in it, and the keys do not come out");
			return kAKSNoPermission;
		}
	}
	kdfCost(&memKiB, &passes);
	read_random(gKS.record.bag.kdfSalt, sizeof(gKS.record.bag.kdfSalt));
	ret = kdfUnlocked(passcode, length, memKiB, passes, gKS.record.bag.kdfSalt, sizeof(gKS.record.bag.kdfSalt), kek);
	if (ret != kIOReturnSuccess) {
		wipeRoots();
		return ret;
	}
	stirEntropy(NULL, 0);
	for (int t = 0; t < kHKTiers; t++) {
		if (!gKS.have[t]) {
			read_random(gKS.roots[t], kHKRootSize);
		}
		gKS.have[t] = have[t] = true;
	}
	gKS.record.bag.kdfMemKiB = memKiB;
	gKS.record.bag.kdfPasses = passes;
	gKS.record.bag.migrating = 0;
	gKS.record.iterations = 0;
	mc_wipe(gKS.record.salt, sizeof(gKS.record.salt));
	mc_wipe(gKS.record.verifier, sizeof(gKS.record.verifier));
	wrapRoots(&gKS.record, kek, gKS.roots, have);
	mc_wipe(kek, sizeof(kek));
	if (persist(&gKS.record) != 0) {
		return kIOReturnIOError;
	}
	wipeOldFile();
	printf("MiniAppleKeyStore: the key bag has its keys, wrapped under the passcode; the old file's verifier is gone\n");
	return kIOReturnSuccess;
}

/*
 * A passcode try. keep, if given, receives the roots (and the caller wipes them); otherwise an unlock installs them and a verify
 * forgets them. The attempt is counted and written before the key derivation runs, so that cutting the power during it does not
 * make it free; the right passcode then clears the count.
 */
static IOReturn
tryPasscodeReal(const uint8_t * passcode, size_t length, bool unlock, bool * changed, bool * first, uint8_t (*keep)[kHKRootSize])
{
	uint8_t kek[32], roots[kHKTiers][kHKRootSize];
	bool have[kHKTiers] = {};
	bool good;
	IOReturn ret;

	if (gKS.damaged) {
		printf("MiniAppleKeyStore: the key bag is damaged: no passcode opens it (passcode_reset=1 destroys the keys)\n");
		return kAKSWrongPasscode;
	}
	if (!gKS.record.hasPasscode) {
		return kIOReturnSuccess;
	}
	if (gKS.kdfBusy) {
		return kIOReturnBusy;
	}
	if (secondsBlocked() > 0) {
		return kAKSBlocked;
	}
	if (length == 0) {
		return kAKSWrongPasscode;                       /* not counted: nothing was tried */
	}
	if (gKS.record.failedAttempts < 1000) {
		gKS.record.failedAttempts++;
	}
	persist(&gKS.record);                                   /* if it cannot be written, memory still counts it */

	if (gKS.record.bag.migrating) {
		uint8_t derived[32];

		pbkdf2(passcode, length, gKS.record.salt, sizeof(gKS.record.salt), gKS.record.iterations, derived, sizeof(derived));
		good = sameBytes(derived, gKS.record.verifier, sizeof(derived));
		mc_wipe(derived, sizeof(derived));
		ret = good ? migrateBag(passcode, length) : kIOReturnSuccess;
		if (ret != kIOReturnSuccess) {
			return ret;
		}
		if (good) {
			memcpy(roots, gKS.roots, sizeof(roots));
			memcpy(have, gKS.have, sizeof(have));
		}
	} else {
		ret = kdfUnlocked(passcode, length, gKS.record.bag.kdfMemKiB, gKS.record.bag.kdfPasses, gKS.record.bag.kdfSalt,
		    sizeof(gKS.record.bag.kdfSalt), kek);
		if (ret != kIOReturnSuccess) {
			gKS.record.failedAttempts--;            /* nothing was tried: no memory for it */
			persist(&gKS.record);
			return ret;
		}
		good = unwrapRoots(&gKS.record, kek, roots, have);
		mc_wipe(kek, sizeof(kek));
	}

	if (!good) {
		uint32_t delay = delayAfter(gKS.record.failedAttempts);

		setBlockedUntil(&gKS.record, delay);
		persist(&gKS.record);
		printf("MiniAppleKeyStore: wrong passcode, no keys (%u failed attempts; %u s before the next)\n", gKS.record.failedAttempts, delay);
		*changed = true;
		return kAKSWrongPasscode;
	}
	gKS.record.failedAttempts = 0;
	setBlockedUntil(&gKS.record, 0);
	persist(&gKS.record);
	*changed = true;
	if (keep != NULL) {
		memcpy(keep, roots, sizeof(roots));
	}
	if (unlock) {
		memcpy(gKS.roots, roots, sizeof(roots));
		memcpy(gKS.have, have, sizeof(have));
		if (gKS.locked) {
			gKS.locked = false;
			printf("MiniAppleKeyStore: unlocked with the passcode; the class keys are in memory\n");
		}
		if (!gKS.unlockedOnce) {
			gKS.unlockedOnce = true;
			*first = true;
		}
	} else if (gKS.locked && !gKS.unlockedOnce) {
		wipeRoots();                                    /* a verify before the first unlock installs nothing (a migration had filled them) */
	}
	mc_wipe(roots, sizeof(roots));
	return kIOReturnSuccess;
}

static IOReturn
changeSecretReal(const uint8_t * oldSecret, size_t oldLength, const uint8_t * newSecret, size_t newLength,
    const uint8_t * blob, size_t blobLength, bool * changed, bool * first)
{
	uint8_t roots[kHKTiers][kHKRootSize], kek[32], salt[sizeof(gKS.record.bag.kdfSalt)];
	bool have[kHKTiers] = {};
	HKRecord * next;
	uint32_t memKiB, passes;
	IOReturn ret;
	int error;

	if (gKS.damaged || !gKS.kdfGood) {
		return kAKSNoPermission;
	}
	if (gKS.kdfBusy) {
		return kIOReturnBusy;
	}
	if (gKS.record.hasPasscode) {
		ret = tryPasscodeReal(oldSecret, oldLength, false, changed, first, roots);
		if (ret != kIOReturnSuccess) {
			mc_wipe(roots, sizeof(roots));
			return ret;
		}
		for (int t = 0; t < kHKTiers; t++) {
			have[t] = (gKS.record.bag.wrappedTiers & (1u << t)) != 0;
		}
	} else {
		memcpy(roots, gKS.roots, sizeof(roots));
		memcpy(have, gKS.have, sizeof(have));
		if (!have[kTierA] || !have[kTierC]) {
			mc_wipe(roots, sizeof(roots));
			return kAKSNoPermission;
		}
	}
	if (newLength == 0) {
		/* "when passcode set" items are to die with the passcode */
		mc_wipe(roots[kTierP], kHKRootSize);
		have[kTierP] = false;
		kekWithoutPasscode(&gKS.record, kek);
	} else {
		if (!have[kTierP]) {
			stirEntropy(NULL, 0);
			read_random(roots[kTierP], kHKRootSize);
			have[kTierP] = true;
		}
		kdfCost(&memKiB, &passes);
		read_random(salt, sizeof(salt));
		ret = kdfUnlocked(newSecret, newLength, memKiB, passes, salt, sizeof(salt), kek);
		if (ret != kIOReturnSuccess) {
			mc_wipe(roots, sizeof(roots));
			return ret;
		}
	}
	next = IONew(HKRecord, 1);
	if (next == NULL) {
		mc_wipe(roots, sizeof(roots));
		mc_wipe(kek, sizeof(kek));
		return kIOReturnNoMemory;
	}
	*next = gKS.record;
	wrapRoots(next, kek, roots, have);
	mc_wipe(kek, sizeof(kek));
	next->failedAttempts = 0;
	next->blockedUntil = 0;
	if (newLength == 0) {
		next->hasPasscode = 0;
		next->bag.kdfMemKiB = next->bag.kdfPasses = 0;
		memset(next->bag.kdfSalt, 0, sizeof(next->bag.kdfSalt));
	} else {
		next->hasPasscode = 1;
		next->bag.kdfMemKiB = memKiB;
		next->bag.kdfPasses = passes;
		memcpy(next->bag.kdfSalt, salt, sizeof(salt));
	}
	next->blobLength = (uint32_t) blobLength;
	memset(next->blob, 0, sizeof(next->blob));
	if (blobLength) {
		memcpy(next->blob, blob, blobLength);
	}
	error = persist(next);
	if (error) {
		mc_wipe(next, sizeof(*next));
		IODelete(next, HKRecord, 1);
		mc_wipe(roots, sizeof(roots));
		return kIOReturnIOError;
	}
	gKS.persisted = true;
	gKS.record = *next;
	mc_wipe(next, sizeof(*next));
	IODelete(next, HKRecord, 1);
	/*
	 * The previous slot holds the keys under the old passcode (or under none at all) until it is written over: write the
	 * same record again, over it. It is what can be done from here; the flash's own remapping is another matter (docs/keystore.md).
	 */
	persist(&gKS.record);
	memcpy(gKS.roots, roots, sizeof(roots));
	memcpy(gKS.have, have, sizeof(have));
	mc_wipe(roots, sizeof(roots));
	gKS.blockedUntilUptime = 0;
	gKS.locked = false;
	if (!gKS.unlockedOnce) {
		gKS.unlockedOnce = true;
		*first = true;
	}
	*changed = true;
	printf("MiniAppleKeyStore: passcode %s; the class keys wrapped again (generation %llu)\n", newLength ? "set" : "removed", gKS.generation);
	return kIOReturnSuccess;
}

/* The key of a class, or why not: what securityd reads as "locked" or "no such key". */
static IOReturn
classKey(uint32_t keyclass, uint8_t key[32])
{
	int tier = classTier(keyclass);
	uint8_t number = (uint8_t) (keyclass & 0x1f);
	const uint8_t * root;

	if (gKS.damaged) {
		return kAKSNoPermission;
	}
	if (tier < 0) {
		return kAKSNoClassKey;
	}
	if (!gKS.persisted) {
		/* a key that would not be there after a restart protects nothing and loses what it wraps */
		if (persist(&gKS.record) != 0) {
			return kAKSNoPermission;
		}
		gKS.persisted = true;
	}
	if (tier == kHKTiers) {
		root = gKS.record.bag.deviceRoot;
	} else if (gKS.have[tier]) {
		root = gKS.roots[tier];
	} else if (tier == kTierP && !gKS.record.hasPasscode) {
		return kAKSNoClassKey;
	} else {
		return kAKSNoPermission;
	}
	hmacOne(root, kHKRootSize, kClassLabel, sizeof(kClassLabel) - 1, &number, 1, key);
	return kIOReturnSuccess;
}

static IOReturn
bagWrap(uint32_t keyclass, const uint8_t * in, size_t length, uint8_t * out)
{
	uint8_t key[32];
	IOReturn ret = classKey(keyclass, key);

	if (ret != kIOReturnSuccess) {
		return ret;
	}
	ret = mc_aes256_kw_wrap(key, in, length, out) == 0 ? kIOReturnSuccess : kIOReturnBadArgument;
	mc_wipe(key, sizeof(key));
	return ret;
}

static IOReturn
bagUnwrap(uint32_t keyclass, const uint8_t * in, size_t length, uint8_t * out)
{
	uint8_t key[32];
	IOReturn ret = classKey(keyclass, key);

	if (ret != kIOReturnSuccess) {
		return ret;
	}
	ret = mc_aes256_kw_unwrap(key, in, length, out) == 0 ? kIOReturnSuccess : kAKSDecodeError;
	mc_wipe(key, sizeof(key));
	return ret;
}

static void
announce(bool changed, bool first)
{
	if (gAKSService == NULL) {
		return;
	}
	if (changed || first) {
		gAKSService->messageClients(kAKSMessageStatusChanged);
	}
	if (first) {
		gAKSService->messageClients(kAKSMessageFirstUnlock);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * DER */

/* Append SEQUENCE { UTF8String key, INTEGER value }; returns the new length. */
static size_t
derPut(uint8_t * buf, size_t length, const char * key, uint64_t value)
{
	size_t keyLength = strlen(key);
	uint8_t bytes[9];
	size_t n = 0;

	/* INTEGER: big-endian, the fewest bytes, a leading zero when the top bit is set (it is not negative) */
	for (int shift = 56; shift >= 0; shift -= 8) {
		uint8_t b = (uint8_t)(value >> shift);
		if (n == 0 && b == 0 && shift != 0) {
			continue;
		}
		if (n == 0 && (b & 0x80)) {
			bytes[n++] = 0;
		}
		bytes[n++] = b;
	}
	buf[length++] = 0x30;
	buf[length++] = (uint8_t) (keyLength + n + 4);
	buf[length++] = 0x0c;
	buf[length++] = (uint8_t) keyLength;
	memcpy(buf + length, key, keyLength);
	length += keyLength;
	buf[length++] = 0x02;
	buf[length++] = (uint8_t) n;
	memcpy(buf + length, bytes, n);
	return length + n;
}

/* The SET's header, once its contents (written from buf + 4) are known; returns the whole length. */
static size_t
derSet(uint8_t * buf, size_t length)
{
	size_t body = length - 4;

	if (body < 128) {
		buf[0] = 0x31;
		buf[1] = (uint8_t) body;
		memmove(buf + 2, buf + 4, body);
		return body + 2;
	}
	buf[0] = 0x31;
	buf[1] = 0x82;
	buf[2] = (uint8_t) (body >> 8);
	buf[3] = (uint8_t) body;
	return body + 4;
}

/* The answer to "device state" (sls is the lock state, ss the flags, sb the seconds of delay left, sfa the failed attempts). */
static size_t
deviceState(uint8_t * buf)
{
	size_t length = 4;

	length = derPut(buf, length, "sb", secondsBlocked());
	length = derPut(buf, length, "sr", 0);
	length = derPut(buf, length, "ss", stateFlags());
	length = derPut(buf, length, "sas", 0);
	length = derPut(buf, length, "sfa", gKS.record.hasPasscode ? gKS.record.failedAttempts : 0);
	length = derPut(buf, length, "sgs", 0);
	length = derPut(buf, length, "sls", lockState());
	length = derPut(buf, length, "sgpe", gKS.record.hasPasscode && gKS.record.gracePeriod != 0);
	length = derPut(buf, length, "srcd", 0);
	return derSet(buf, length);
}

static size_t
configuration(uint8_t * buf)
{
	size_t length = 4;

	length = derPut(buf, length, "bd", 0);
	length = derPut(buf, length, "gp", gKS.record.gracePeriod);
	length = derPut(buf, length, "mua", kHKMaxUnlockAttempts);
	return derSet(buf, length);
}

/* One tag-length-value; returns false if it does not fit. */
static bool
derNext(const uint8_t ** p, const uint8_t * end, uint8_t * tag, const uint8_t ** value, size_t * length)
{
	const uint8_t * q = *p;
	size_t n;

	if (end - q < 2) {
		return false;
	}
	*tag = *q++;
	n = *q++;
	if (n & 0x80) {
		size_t bytes = n & 0x7f;
		if (bytes == 0 || bytes > 2 || (size_t)(end - q) < bytes) {
			return false;
		}
		n = 0;
		while (bytes--) {
			n = (n << 8) | *q++;
		}
	}
	if ((size_t)(end - q) < n) {
		return false;
	}
	*value = q;
	*length = n;
	*p = q + n;
	return true;
}

/* kAKSSetConfiguration: the grace period ("gp") is the one thing kept; the rest is noted. */
static void
setConfiguration(const uint8_t * der, size_t size)
{
	const uint8_t * p = der, * end = der + size, * value;
	uint8_t tag;
	size_t length;

	if (!derNext(&p, end, &tag, &value, &length) || (tag != 0x31 && tag != 0x30)) {
		return;
	}
	p = value;
	end = value + length;
	while (p < end) {
		const uint8_t * item, * key, * number, * q;
		size_t itemLength, keyLength, numberLength;
		uint8_t t1, t2;
		uint64_t v = 0;

		if (!derNext(&p, end, &tag, &item, &itemLength) || tag != 0x30) {
			return;
		}
		q = item;
		if (!derNext(&q, item + itemLength, &t1, &key, &keyLength) || t1 != 0x0c ||
		    !derNext(&q, item + itemLength, &t2, &number, &numberLength) || t2 != 0x02 || numberLength > 8) {
			continue;
		}
		for (size_t i = 0; i < numberLength; i++) {
			v = (v << 8) | number[i];
		}
		if (keyLength == 2 && memcmp(key, "gp", 2) == 0) {
			if (gKS.record.gracePeriod != (uint32_t) v) {
				gKS.record.gracePeriod = v > 4 * 3600 ? 4 * 3600 : (uint32_t) v;
				persist(&gKS.record);
				printf("MiniAppleKeyStore: grace period %u s\n", gKS.record.gracePeriod);
			}
		} else {
			printf("MiniAppleKeyStore: configuration %.*s = %llu (not kept)\n", (int) keyLength, key, v);
		}
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * who may call what */

/*
 * With keystore_real=1. The entitlements are the ones 16.5's programs carry (docs/keystore.md): a selector not listed here is open, as a
 * question about the state is on iOS. What this kernel can check is limited: it verifies no code signature, so a program can list any
 * entitlement it likes in its own signature (libkern/amfi/miniamfi_entitlements.cpp). So the entitlement counts only if the program runs
 * from a read-only volume as well: what a process with root writes to /private/var and starts holds nothing. It does not stop root, which
 * can attach to a program that holds the entitlement, or write to the flash.
 *
 * keystore_entcheck: 0 no check; 1 (the default) say what would be refused, once for each program and selector, and allow it; 2 refuse.
 */
#define kEntDevice      "com.apple.keystore.device"
#define kEntVerify      "com.apple.keystore.device.verify"
#define kEntKeychain    "com.apple.keystore.access-keychain-keys"
#define kEntAssert      "com.apple.keystore.lockassertion"
#define kEntConfig      "com.apple.keystore.config.set"
#define kEntHakiOS      "com.hakios.keystore.change"        /* userland/src/passcoded.entitlements */

struct KSAccess {
	uint32_t     selector;
	const char * entitlements[2];                       /* either */
};

static const KSAccess kKSAccess[] = {
	{ kAKSLockDevice, { kEntDevice, NULL } },
	{ kAKSLockDeviceNow, { kEntDevice, NULL } },
	{ kAKSUnlockDevice, { kEntDevice, NULL } },
	{ kAKSVerifyPassword, { kEntVerify, kEntDevice } },
	{ kAKSWrapKey, { kEntKeychain, NULL } },
	{ kAKSUnwrapKey, { kEntKeychain, NULL } },
	{ kAKSAssertHold, { kEntAssert, kEntDevice } },
	{ kAKSAssertDrop, { kEntAssert, kEntDevice } },
	{ kAKSAssertPromote, { kEntAssert, kEntDevice } },
	{ kAKSAssertConsume, { kEntAssert, kEntDevice } },
	{ kAKSSetConfiguration, { kEntConfig, kEntDevice } },
	{ kAKSNotifyStateSet, { kEntDevice, kEntHakiOS } },
	{ kHKChangeSecret, { kEntHakiOS, NULL } },
	{ kHKCopyBlob, { kEntHakiOS, NULL } },
};

enum { kAccessAllowed, kAccessNoEntitlement, kAccessWritableProgram };

static uint32_t gEntCheck = 1;

static bool
currentProgramIsOnReadOnlyVolume(void)
{
	proc_t self = proc_self();
	vnode_t vp = NULLVP;
	bool readOnly = false;

	if (self != NULL) {
		vp = proc_getexecutablevnode(self);
		proc_rele(self);
	}
	if (vp != NULLVP) {
		mount_t mp = vnode_mount(vp);

		readOnly = mp != NULL && (vfs_flags(mp) & MNT_RDONLY) != 0;
		vnode_put(vp);
	}
	return readOnly;
}

/* Whether the calling process may use this selector; wanted says which entitlement it lacks. */
static int
accessCheck(uint32_t selector, const char ** wanted)
{
	for (size_t i = 0; i < sizeof(kKSAccess) / sizeof(kKSAccess[0]); i++) {
		const KSAccess * a = &kKSAccess[i];

		if (a->selector != selector) {
			continue;
		}
		*wanted = a->entitlements[0];
		if (!IOCurrentTaskHasEntitlement(a->entitlements[0]) &&
		    (a->entitlements[1] == NULL || !IOCurrentTaskHasEntitlement(a->entitlements[1]))) {
			return kAccessNoEntitlement;
		}
		return currentProgramIsOnReadOnlyVolume() ? kAccessAllowed : kAccessWritableProgram;
	}
	return kAccessAllowed;
}

/* Said once for each program and selector, up to a limit: the list to look at when deciding to turn on keystore_entcheck=2. */
#define kAccessLogged 64
static struct {
	char     name[20];
	uint32_t selector;
} gAccessLogged[kAccessLogged];
static uint32_t gAccessLoggedCount;

static void
accessLog(uint32_t selector, const char * wanted, int verdict)
{
	char name[20] = "?";

	proc_selfname(name, sizeof(name));
	for (uint32_t i = 0; i < gAccessLoggedCount; i++) {
		if (gAccessLogged[i].selector == selector && strncmp(gAccessLogged[i].name, name, sizeof(name)) == 0) {
			return;
		}
	}
	if (gAccessLoggedCount < kAccessLogged) {
		strlcpy(gAccessLogged[gAccessLoggedCount].name, name, sizeof(name));
		gAccessLogged[gAccessLoggedCount].selector = selector;
		gAccessLoggedCount++;
	}
	printf("MiniAppleKeyStore: %s[%d] selector 0x%x: %s %s (%s)\n", name, proc_selfpid(), selector, gEntCheck >= 2 ? "refused:" : "would be refused:",
	    verdict == kAccessNoEntitlement ? "it lacks" : "it runs from a writable volume, so it does not count:", wanted,
	    gEntCheck >= 2 ? "keystore_entcheck=2" : "keystore_entcheck=1");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the self-test (QEMU) */

/*
 * kern.hakios.passcode_selftest: written 1 by qemu/run-passcode-test.sh's process 1 once /private/var is mounted,
 * with the boot-argument passcode_selftest=N saying which boot of the sequence this is. Off (EPERM) without the
 * boot-argument: it sets and removes passcodes. Each check logs "MiniAppleKeyStore: selftest N: ..."; reading the
 * sysctl afterwards gives 1 if every check passed, 2 if one failed.
 *   1  no passcode yet; set 123456; lock; four wrong tries (no delay), a fifth (a minute's delay); the right one is
 *      refused during the delay and not counted; the delay cleared (the test's own shortcut), the right one unlocks;
 *      a change with a wrong old passcode is refused; changed to abcdef with new metadata. Left set.
 *   2  after a restart: set, locked, not unlocked since boot, the metadata kept; a wrong try counts one; abcdef
 *      unlocks (the first unlock); removed. Then set 999999 and left set, for 3.
 *   3  with passcode_reset=1 as well: no passcode.
 */
static int gSelftestResult;

#define CHECK(what, cond) do { bool ok_ = (cond); printf("MiniAppleKeyStore: selftest %u: %s %s\n", phase, ok_ ? "ok  " : "FAIL", what); \
	if (!ok_) { failed = true; } } while (0)

static bool
selftest(uint32_t phase)
{
	bool failed = false, changed = false, first = false;
	const uint8_t * none = (const uint8_t *) "";

	if (!gKS.loaded) {
		load();
	}
	switch (phase) {
	case 1:
		CHECK("PBKDF2's test vector", gKS.kdfGood);
		CHECK("no passcode at the start", !gKS.record.hasPasscode && lockState() == kAKSLockDisabled);
		CHECK("set 123456", changeSecret(none, 0, (const uint8_t *) "123456", 6, (const uint8_t *) "BLOB1", 5, &changed, &first) == kIOReturnSuccess);
		CHECK("set and unlocked", gKS.record.hasPasscode && lockState() == kAKSLockUnlocked);
		gKS.locked = true;
		CHECK("locked", lockState() == kAKSLockLocked && (stateFlags() & kAKSStateLocked));
		for (int i = 1; i <= 4; i++) {
			CHECK("a wrong try is refused", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode);
		}
		CHECK("four failed attempts, no delay", gKS.record.failedAttempts == 4 && secondsBlocked() == 0);
		CHECK("a fifth wrong try is refused", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode);
		CHECK("five failed attempts, about a minute's delay", gKS.record.failedAttempts == 5 && secondsBlocked() > 55 && secondsBlocked() <= 60);
		CHECK("the right one during the delay is refused", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kAKSBlocked);
		CHECK("... and not counted", gKS.record.failedAttempts == 5 && lockState() == kAKSLockLocked);
		gKS.blockedUntilUptime = 0;
		CHECK("the right one unlocks", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kIOReturnSuccess);
		CHECK("unlocked, no failed attempts", lockState() == kAKSLockUnlocked && gKS.record.failedAttempts == 0 && gKS.record.blockedUntil == 0);
		CHECK("a change with a wrong old passcode is refused", changeSecret((const uint8_t *) "111111", 6, (const uint8_t *) "abcdef", 6,
		    (const uint8_t *) "BLOB2", 5, &changed, &first) == kAKSWrongPasscode);
		CHECK("changed to abcdef", changeSecret((const uint8_t *) "123456", 6, (const uint8_t *) "abcdef", 6,
		    (const uint8_t *) "BLOB2", 5, &changed, &first) == kIOReturnSuccess);
		CHECK("the old one no longer verifies", tryPasscode((const uint8_t *) "123456", 6, false, &changed, &first) == kAKSWrongPasscode);
		CHECK("the new one verifies", tryPasscode((const uint8_t *) "abcdef", 6, false, &changed, &first) == kIOReturnSuccess);
		break;
	case 2:
		CHECK("set after the restart", gKS.record.hasPasscode);
		CHECK("locked, not unlocked since boot", lockState() == kAKSLockLocked && !(stateFlags() & kAKSStateUnlockedOnce));
		CHECK("the metadata kept", gKS.record.blobLength == 5 && memcmp(gKS.record.blob, "BLOB2", 5) == 0);
		CHECK("a wrong try is refused", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kAKSWrongPasscode);
		CHECK("one failed attempt", gKS.record.failedAttempts == 1);
		CHECK("abcdef unlocks, the first unlock", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess && first);
		CHECK("unlocked since boot", lockState() == kAKSLockUnlocked && (stateFlags() & kAKSStateUnlockedOnce));
		CHECK("removed", changeSecret((const uint8_t *) "abcdef", 6, none, 0, none, 0, &changed, &first) == kIOReturnSuccess);
		CHECK("no passcode", lockState() == kAKSLockDisabled && !gKS.record.hasPasscode);
		CHECK("set 999999 for the reset", changeSecret(none, 0, (const uint8_t *) "999999", 6, none, 0, &changed, &first) == kIOReturnSuccess);
		break;
	case 3:
		CHECK("passcode_reset=1 left no passcode", !gKS.record.hasPasscode && lockState() == kAKSLockDisabled);
		break;
	default:
		CHECK("a known phase", false);
		break;
	}
	printf("MiniAppleKeyStore: selftest %u: %s (generation %llu)\n", phase, failed ? "FAILED" : "ALL OK", gKS.generation);
	return !failed;
}

/*
 * The key bag's self-test: keystore_real=1 passcode_selftest=N, N >= 10 (qemu/run-passcode-test.sh real, and migrate). Blobs wrapped in
 * one boot are kept in /private/var/hakios-selftest for the next. kTestKey is the key they wrap, and the script looks for it on
 * the volume afterwards: it must not be there.
 *   10  a new bag: known answers; no passcode: wrap and unwrap work, no "when passcode set" key; a blob damaged, or of another class, or
 *       the old reversible kind, is refused; passcode set; a lock wipes A's root: class A neither wraps nor unwraps, C and D still
 *       do; a wrong passcode brings no key and counts; the right one brings A back and the same blob opens; delays; a change with
 *       a wrong old passcode refused, then accepted, and the same blobs still open.
 *   11  after a restart: locked and not unlocked since boot: A, C and P closed, D open; a wrong try counts; the right one is the first
 *       unlock and every blob of the last boot opens to the same key; a lock leaves C open and shuts A; removal: P's blob can no longer
 *       be used, A and C still open; a new passcode has a new P.
 *   12  damage: both slots written with garbage, the bag read again: locked, no class opens (not even D, whose root was in it), the
 *       right passcode gets nothing and is not counted, nothing can be set, and the garbage is still there.
 *   13  with passcode_reset=1 too: out of the damage, with no passcode, and the old blobs answer "never" (the data is lost, which is the point).
 *       Sets a passcode and wraps again, for 14.
 *   14  with passcode_reset=1 again, an intact bag this time: the same.
 *   15  (migrate) the old kind of file holds abcdef: locked; no key before the first unlock; D open; a wrong try counts; abcdef opens, and
 *       the old file's verifier is gone.
 *   16  (migrate) after a restart: the bag holds; abcdef opens; the blob of 15 opens.
 *   17  (entitlements, in a test program signed with com.apple.keystore.device and nothing else, on a read-only root): that is honoured; it may
 *       unlock and not wrap, nor change the passcode; the state is open.
 *   18  (the same, the test program unsigned): nothing but the state.
 */
#define kSelftestPath   "/private/var/hakios-selftest"

static const uint8_t kTestKey[32] = {
	0x4b, 0x45, 0x59, 0x53, 0x54, 0x4f, 0x52, 0x45, 0x2d, 0x54, 0x45, 0x53, 0x54, 0x2d, 0x4b, 0x45,
	0x59, 0x2d, 0x4d, 0x55, 0x53, 0x54, 0x2d, 0x4e, 0x4f, 0x54, 0x2d, 0x4c, 0x45, 0x41, 0x4b, 0x21,
};
enum { kBlobA, kBlobC, kBlobD, kBlobP, kTestBlobs };
static const uint32_t kBlobClass[kTestBlobs] = { 6, 7, 8, 12 };

struct SelftestBlobs {
	uint8_t blob[kTestBlobs][32 + MC_KW_OVERHEAD];
};

static int
selftestFile(enum uio_rw rw, SelftestBlobs * b)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int resid = 0, fmode = rw == UIO_READ ? FREAD : (FREAD | FWRITE | O_CREAT);
	int error = vnode_open(kSelftestPath, fmode, 0600, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);

	if (error) {
		return error;
	}
	error = vn_rdwr(rw, vp, (caddr_t) b, (int) sizeof(*b), 0, UIO_SYSSPACE, rw == UIO_WRITE ? IO_SYNC | IO_NOAUTH : IO_NOAUTH,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (error == 0 && resid != 0) {
		error = EIO;
	}
	vnode_close(vp, fmode, ctx);
	return error;
}

static bool
opens(uint32_t keyclass, const uint8_t * blob)
{
	uint8_t out[64];
	bool good = bagUnwrap(keyclass, blob, 32 + MC_KW_OVERHEAD, out) == kIOReturnSuccess && sameBytes(out, kTestKey, 32);

	mc_wipe(out, sizeof(out));
	return good;
}

static bool
rootIsWiped(int tier)
{
	uint8_t d = 0;

	for (size_t i = 0; i < kHKRootSize; i++) {
		d |= gKS.roots[tier][i];
	}
	return d == 0 && !gKS.have[tier];
}

/* What a lock request does. */
static void
selftestLock(void)
{
	gKS.locked = true;
	dropLockedRoots();
}

static bool
realSelftest(uint32_t phase)
{
	bool failed = false, changed = false, first = false;
	const uint8_t * none = (const uint8_t *) "";
	uint8_t out[32 + MC_KW_OVERHEAD];
	SelftestBlobs * saved = IONew(SelftestBlobs, 1);
	int error;

	if (saved == NULL || !gKS.real) {
		printf("MiniAppleKeyStore: selftest %u: needs keystore_real=1\n", phase);
		return false;
	}
	memset(saved, 0, sizeof(*saved));
	if (!gKS.loaded) {
		load();
	}
	if (phase != 10 && phase != 15 && phase != 17 && phase != 18 && phase != 19) {
		error = selftestFile(UIO_READ, saved);
		CHECK("the blobs of the last boot are there", error == 0);
	}
	switch (phase) {
	case 10: {
		uint8_t legacy[40] = { 'H', 'K', 'w', 'r', 7 };

		CHECK("the known answers", gKS.kdfGood && mc_keywrap_selftest());
		CHECK("a new bag, no passcode", !gKS.damaged && !gKS.record.hasPasscode && lockState() == kAKSLockDisabled);
		CHECK("no key for \"when passcode set\" without a passcode", bagWrap(12, kTestKey, 32, out) == kAKSNoClassKey);
		for (int i = 0; i < kBlobP; i++) {
			CHECK("wrap", bagWrap(kBlobClass[i], kTestKey, 32, saved->blob[i]) == kIOReturnSuccess);
			CHECK("the wrapped key is not the key", !sameBytes(saved->blob[i] + 8, kTestKey, 32) && !sameBytes(saved->blob[i], kTestKey, 32));
			CHECK("unwrap", opens(kBlobClass[i], saved->blob[i]));
		}
		CHECK("two classes, two wrappings", !sameBytes(saved->blob[kBlobA], saved->blob[kBlobC], 40));
		memcpy(out, saved->blob[kBlobA], sizeof(out));
		out[13] ^= 0x40;
		CHECK("a damaged blob is refused", bagUnwrap(6, out, sizeof(out), out) == kAKSDecodeError);
		CHECK("another class's blob is refused", bagUnwrap(7, saved->blob[kBlobA], 40, out) == kAKSDecodeError);
		CHECK("the old reversible kind is refused", bagUnwrap(7, legacy, sizeof(legacy), out) == kAKSDecodeError);
		CHECK("a wrong length is refused", bagUnwrap(6, saved->blob[kBlobA], 39, out) == kAKSDecodeError);
		CHECK("an unknown class has no key", bagWrap(29, kTestKey, 32, out) == kAKSNoClassKey);

		CHECK("set 123456", changeSecret(none, 0, (const uint8_t *) "123456", 6, (const uint8_t *) "BLOB1", 5, &changed, &first) == kIOReturnSuccess);
		CHECK("set and unlocked, the same keys", lockState() == kAKSLockUnlocked && opens(6, saved->blob[kBlobA]) && opens(7, saved->blob[kBlobC]));
		CHECK("now \"when passcode set\" has a key", bagWrap(12, kTestKey, 32, saved->blob[kBlobP]) == kIOReturnSuccess && opens(12, saved->blob[kBlobP]));

		selftestLock();
		CHECK("locked", lockState() == kAKSLockLocked);
		CHECK("the lock wiped A's and P's roots", rootIsWiped(kTierA) && rootIsWiped(kTierP) && rootIsWiped(kTierB));
		CHECK("class A does not unwrap while locked", bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSNoPermission);
		CHECK("class A does not wrap while locked", bagWrap(6, kTestKey, 32, out) == kAKSNoPermission);
		CHECK("class P does not unwrap while locked", bagUnwrap(12, saved->blob[kBlobP], 40, out) == kAKSNoPermission);
		CHECK("class C still opens: unlocked once", opens(7, saved->blob[kBlobC]));
		CHECK("class D still opens", opens(8, saved->blob[kBlobD]));

		CHECK("a wrong passcode is refused", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode);
		CHECK("... it brought no key", rootIsWiped(kTierA) && bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSNoPermission && lockState() == kAKSLockLocked);
		CHECK("... and it counted", gKS.record.failedAttempts == 1);
		CHECK("the right one unlocks", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kIOReturnSuccess && lockState() == kAKSLockUnlocked);
		CHECK("the same blob opens again", opens(6, saved->blob[kBlobA]) && opens(12, saved->blob[kBlobP]));
		CHECK("no failed attempts", gKS.record.failedAttempts == 0);

		selftestLock();
		for (int i = 1; i <= 5; i++) {
			CHECK("a wrong try", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode);
		}
		CHECK("five: a minute's delay", gKS.record.failedAttempts == 5 && secondsBlocked() > 40 && secondsBlocked() <= 60);
		CHECK("the right one is refused during it, not counted", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kAKSBlocked && gKS.record.failedAttempts == 5);
		gKS.blockedUntilUptime = 0;
		CHECK("after it, the right one unlocks", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kIOReturnSuccess);

		CHECK("a change with a wrong old passcode is refused", changeSecret((const uint8_t *) "111111", 6, (const uint8_t *) "abcdef", 6,
		    (const uint8_t *) "BLOB2", 5, &changed, &first) == kAKSWrongPasscode);
		CHECK("changed to abcdef", changeSecret((const uint8_t *) "123456", 6, (const uint8_t *) "abcdef", 6,
		    (const uint8_t *) "BLOB2", 5, &changed, &first) == kIOReturnSuccess);
		CHECK("the same blobs open", opens(6, saved->blob[kBlobA]) && opens(7, saved->blob[kBlobC]) && opens(12, saved->blob[kBlobP]));
		CHECK("the old one no longer opens anything", tryPasscode((const uint8_t *) "123456", 6, false, &changed, &first) == kAKSWrongPasscode);
		CHECK("the new one does", tryPasscode((const uint8_t *) "abcdef", 6, false, &changed, &first) == kIOReturnSuccess);
		CHECK("the blobs are kept for the restart", selftestFile(UIO_WRITE, saved) == 0);
		break;
	}
	case 11:
		CHECK("set after the restart", gKS.record.hasPasscode && !gKS.damaged);
		CHECK("locked, not unlocked since boot", lockState() == kAKSLockLocked && !(stateFlags() & kAKSStateUnlockedOnce));
		CHECK("nothing but D in memory", rootIsWiped(kTierA) && rootIsWiped(kTierB) && rootIsWiped(kTierC) && rootIsWiped(kTierP));
		CHECK("class A is shut", bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSNoPermission);
		CHECK("class C is shut before the first unlock", bagUnwrap(7, saved->blob[kBlobC], 40, out) == kAKSNoPermission);
		CHECK("class P is shut", bagUnwrap(12, saved->blob[kBlobP], 40, out) == kAKSNoPermission);
		CHECK("class D opens", opens(8, saved->blob[kBlobD]));
		CHECK("the metadata kept", gKS.record.blobLength == 5 && memcmp(gKS.record.blob, "BLOB2", 5) == 0);
		CHECK("a wrong try is refused", tryPasscode((const uint8_t *) "123456", 6, true, &changed, &first) == kAKSWrongPasscode);
		CHECK("... no key, one attempt", rootIsWiped(kTierC) && gKS.record.failedAttempts == 1);
		CHECK("abcdef unlocks, the first unlock", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess && first);
		for (int i = 0; i < kTestBlobs; i++) {
			CHECK("last boot's blob opens to the same key", opens(kBlobClass[i], saved->blob[i]));
		}
		selftestLock();
		CHECK("a lock: C stays, A goes", opens(7, saved->blob[kBlobC]) && bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSNoPermission);
		CHECK("unlock again", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess);
		CHECK("removed", changeSecret((const uint8_t *) "abcdef", 6, none, 0, none, 0, &changed, &first) == kIOReturnSuccess);
		CHECK("no passcode", lockState() == kAKSLockDisabled);
		CHECK("A and C still open, no passcode now guards them", opens(6, saved->blob[kBlobA]) && opens(7, saved->blob[kBlobC]));
		CHECK("P died with the passcode", bagUnwrap(12, saved->blob[kBlobP], 40, out) == kAKSNoClassKey);
		CHECK("set 999999", changeSecret(none, 0, (const uint8_t *) "999999", 6, none, 0, &changed, &first) == kIOReturnSuccess);
		CHECK("a new P: the old blob says never", bagUnwrap(12, saved->blob[kBlobP], 40, out) == kAKSDecodeError);
		break;
	case 12: {
		HKRecord * garbage = IONew(HKRecord, 1);

		if (garbage != NULL) {
			memset(garbage, 0x5a, sizeof(*garbage));
			CHECK("damage both slots", fileIOAt(kHKBagPath, sizeof(*garbage), UIO_WRITE, 0, garbage) == 0 && fileIOAt(kHKBagPath, sizeof(*garbage), UIO_WRITE, 1, garbage) == 0);
			gKS.loaded = false;
			load();
			CHECK("damaged: locked", gKS.damaged && lockState() == kAKSLockLocked);
			CHECK("no key in memory", rootIsWiped(kTierA) && rootIsWiped(kTierC));
			CHECK("A is shut, and not called corrupt", bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSNoPermission);
			CHECK("C is shut", bagUnwrap(7, saved->blob[kBlobC], 40, out) == kAKSNoPermission);
			CHECK("D is shut", bagUnwrap(8, saved->blob[kBlobD], 40, out) == kAKSNoPermission);
			CHECK("the right passcode gets nothing", tryPasscode((const uint8_t *) "999999", 6, true, &changed, &first) == kAKSWrongPasscode && lockState() == kAKSLockLocked);
			CHECK("nor is it counted", gKS.record.failedAttempts == 0);
			CHECK("nothing can be set", changeSecret((const uint8_t *) "999999", 6, (const uint8_t *) "1", 1, none, 0, &changed, &first) == kAKSNoPermission);
			memset(garbage, 0, sizeof(*garbage));
			CHECK("the garbage is still there", fileIOAt(kHKBagPath, sizeof(*garbage), UIO_READ, 0, garbage) == 0 && garbage->magic == 0x5a5a5a5a);
			IODelete(garbage, HKRecord, 1);
		} else {
			CHECK("memory", false);
		}
		break;
	}
	case 13:
		CHECK("passcode_reset=1: out of the damage", !gKS.damaged && !gKS.record.hasPasscode && lockState() == kAKSLockDisabled);
		CHECK("the old blobs say never: the data is gone", bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSDecodeError && bagUnwrap(7, saved->blob[kBlobC], 40, out) == kAKSDecodeError);
		CHECK("the old D blob too", bagUnwrap(8, saved->blob[kBlobD], 40, out) == kAKSDecodeError);
		CHECK("new wraps work", bagWrap(6, kTestKey, 32, saved->blob[kBlobA]) == kIOReturnSuccess && opens(6, saved->blob[kBlobA]));
		CHECK("set 222222", changeSecret(none, 0, (const uint8_t *) "222222", 6, none, 0, &changed, &first) == kIOReturnSuccess);
		CHECK("kept for the next boot", selftestFile(UIO_WRITE, saved) == 0);
		break;
	case 14:
		CHECK("passcode_reset=1 again: no passcode", !gKS.record.hasPasscode && !gKS.damaged);
		CHECK("the keys are new: the blob says never", bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSDecodeError);
		break;
	case 15:
		CHECK("the old file's passcode is in force", gKS.record.hasPasscode && gKS.record.bag.migrating && lockState() == kAKSLockLocked);
		CHECK("no key yet", rootIsWiped(kTierA) && rootIsWiped(kTierC));
		CHECK("A is shut", bagWrap(6, kTestKey, 32, out) == kAKSNoPermission);
		CHECK("D works from the start", bagWrap(8, kTestKey, 32, saved->blob[kBlobD]) == kIOReturnSuccess);
		CHECK("a wrong try is refused and counts", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode && gKS.record.failedAttempts >= 1);
		CHECK("abcdef opens", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess && first && lockState() == kAKSLockUnlocked);
		CHECK("the bag holds wrapped keys now", !gKS.record.bag.migrating && (gKS.record.bag.wrappedTiers & 0xf) == 0xf && gKS.record.iterations == 0);
		CHECK("wrap", bagWrap(6, kTestKey, 32, saved->blob[kBlobA]) == kIOReturnSuccess && opens(6, saved->blob[kBlobA]));
		{
			HKRecord * old = IONew(HKRecord, 1);

			if (old != NULL) {
				memset(old, 0, sizeof(*old));
				CHECK("the old file holds no verifier", fileIOAt(kHKPath, kHKV1Size, UIO_READ, 0, old) == 0 && !old->hasPasscode && old->iterations == 0 &&
				    old->verifier[0] == 0 && old->verifier[31] == 0);
				IODelete(old, HKRecord, 1);
			}
		}
		CHECK("kept for the next boot", selftestFile(UIO_WRITE, saved) == 0);
		break;
	case 19:
		CHECK("a new bag, no passcode", !gKS.record.hasPasscode && !gKS.record.bag.migrating && lockState() == kAKSLockDisabled);
		CHECK("A wraps", bagWrap(6, kTestKey, 32, saved->blob[kBlobA]) == kIOReturnSuccess);
		CHECK("D wraps", bagWrap(8, kTestKey, 32, saved->blob[kBlobD]) == kIOReturnSuccess);
		CHECK("kept for the next boot", selftestFile(UIO_WRITE, saved) == 0);
		break;
	case 20:
		CHECK("the old file's passcode holds over a bag with none", gKS.record.hasPasscode && gKS.record.bag.migrating && lockState() == kAKSLockLocked);
		CHECK("no key yet", rootIsWiped(kTierA) && rootIsWiped(kTierC));
		CHECK("A is shut", bagUnwrap(6, saved->blob[kBlobA], 40, out) == kAKSNoPermission);
		CHECK("D stays open", opens(8, saved->blob[kBlobD]));
		CHECK("a wrong try is refused and counts", tryPasscode((const uint8_t *) "000000", 6, true, &changed, &first) == kAKSWrongPasscode && gKS.record.failedAttempts >= 1);
		CHECK("abcdef opens", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess && first && lockState() == kAKSLockUnlocked);
		CHECK("the bag holds wrapped keys now", !gKS.record.bag.migrating && (gKS.record.bag.wrappedTiers & 0xf) == 0xf && gKS.record.iterations == 0);
		CHECK("what was wrapped before it opens", opens(6, saved->blob[kBlobA]) && opens(8, saved->blob[kBlobD]));
		break;
	case 17: {
		const char * wanted = NULL;

		CHECK("the program is on a read-only volume", currentProgramIsOnReadOnlyVolume());
		CHECK("its entitlement is seen", IOCurrentTaskHasEntitlement(kEntDevice));
		CHECK("and one it does not have is not", !IOCurrentTaskHasEntitlement(kEntKeychain));
		CHECK("unlock: allowed", accessCheck(kAKSUnlockDevice, &wanted) == kAccessAllowed);
		CHECK("verify: allowed by the second of two", accessCheck(kAKSVerifyPassword, &wanted) == kAccessAllowed);
		CHECK("wrap: refused", accessCheck(kAKSWrapKey, &wanted) == kAccessNoEntitlement);
		CHECK("unwrap: refused", accessCheck(kAKSUnwrapKey, &wanted) == kAccessNoEntitlement);
		CHECK("changing the passcode: refused", accessCheck(kHKChangeSecret, &wanted) == kAccessNoEntitlement);
		CHECK("the state: open", accessCheck(kAKSGetDeviceState, &wanted) == kAccessAllowed);
		break;
	}
	case 18: {
		const char * wanted = NULL;

		CHECK("no entitlements", !IOCurrentTaskHasEntitlement(kEntDevice));
		CHECK("unlock: refused", accessCheck(kAKSUnlockDevice, &wanted) == kAccessNoEntitlement);
		CHECK("verify: refused", accessCheck(kAKSVerifyPassword, &wanted) == kAccessNoEntitlement);
		CHECK("wrap: refused", accessCheck(kAKSWrapKey, &wanted) == kAccessNoEntitlement);
		CHECK("lock: refused", accessCheck(kAKSLockDevice, &wanted) == kAccessNoEntitlement);
		CHECK("changing the passcode: refused", accessCheck(kHKChangeSecret, &wanted) == kAccessNoEntitlement);
		CHECK("the state: open", accessCheck(kAKSGetDeviceState, &wanted) == kAccessAllowed);
		CHECK("the lock state: open", accessCheck(kAKSGetLockState, &wanted) == kAccessAllowed);
		break;
	}
	case 16:
		CHECK("after the restart: the bag, locked", gKS.record.hasPasscode && !gKS.record.bag.migrating && lockState() == kAKSLockLocked);
		CHECK("abcdef opens", tryPasscode((const uint8_t *) "abcdef", 6, true, &changed, &first) == kIOReturnSuccess);
		CHECK("A's blob and D's open", opens(6, saved->blob[kBlobA]) && opens(8, saved->blob[kBlobD]));
		break;
	default:
		CHECK("a known phase", false);
		break;
	}
	mc_wipe(out, sizeof(out));
	mc_wipe(saved, sizeof(*saved));
	IODelete(saved, SelftestBlobs, 1);
	printf("MiniAppleKeyStore: selftest %u: %s (generation %llu, kdf %u ms)\n", phase, failed ? "FAILED" : "ALL OK", gKS.generation, gKS.kdfMilliseconds);
	return !failed;
}

static int
selftestSysctl(__unused struct sysctl_oid * oidp, __unused void * arg1, __unused int arg2, struct sysctl_req * req)
{
	uint32_t phase = 0;
	int value = gSelftestResult, error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if ((error = SYSCTL_IN(req, &value, sizeof(value))) != 0) {
		return error;
	}
	if (!PE_parse_boot_argn("passcode_selftest", &phase, sizeof(phase)) || phase == 0 || gAKSLock == NULL) {
		return EPERM;
	}
	IOLockLock(gAKSLock);
	gSelftestResult = (phase >= 10 ? realSelftest(phase) : selftest(phase)) ? 1 : 2;
	IOLockUnlock(gAKSLock);
	return 0;
}
/* kern.hakios.passcode: the state, for the USB console ("sysctl kern.hakios.passcode") and for logs; nothing secret */
static int
stateSysctl(__unused struct sysctl_oid * oidp, __unused void * arg1, __unused int arg2, struct sysctl_req * req)
{
	char line[160];

	if (gAKSLock == NULL) {
		return ENOENT;
	}
	IOLockLock(gAKSLock);
	if (!gKS.loaded) {
		snprintf(line, sizeof(line), "not read yet (nothing has asked the key store)");
	} else {
		snprintf(line, sizeof(line), "%spasscode %s, lock state %u, %s, %u failed attempts, %u s of delay, grace %u s, generation %llu, kdf %u ms",
		    !gKS.real ? "" : gKS.damaged ? "key bag DAMAGED, " : "key bag, ",
		    gKS.record.hasPasscode ? "set" : "none", lockState(), gKS.unlockedOnce ? "unlocked since boot" : "not unlocked since boot",
		    gKS.record.failedAttempts, secondsBlocked(), gKS.record.gracePeriod, gKS.generation, gKS.kdfMilliseconds);
	}
	IOLockUnlock(gAKSLock);
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}
SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, passcode, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    stateSysctl, "A", "the key store's passcode state (MiniAppleKeyStore.cpp)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, passcode_selftest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    selftestSysctl, "I", "the key store's passcode self-test (QEMU; needs the boot-argument passcode_selftest=N)");

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the service and its user client */

class AppleKeyStore : public IOService
{
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	setName("AppleKeyStore");
	registerService();
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

	static IOReturn answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length);
	static IOReturn input(IOExternalMethodArguments * args, uint8_t * buf, size_t size, size_t * length);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* keybagd asks to be told about keys to back up and bags to save; there never are any. */
IOReturn
AppleKeyStoreUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	releaseNotificationPort(port);
	return kIOReturnSuccess;
}

/*
 * kAKSWrapKey and kAKSUnwrapKey without keystore_real=1. A wrapped key is 8 bytes longer than the key, as with AES key
 * wrap (RFC 3394), which is what callers size their buffers for: a mark, the class, and the key
 * mixed with a fixed pattern. Not encryption: there is no secret here to encrypt with, and the
 * passcode is not one (see the head of this file). With keystore_real=1 this is not reached.
 */
static const uint8_t kWrapMark[4] = { 'H', 'K', 'w', 'r' };

static void
wrapMix(uint8_t * bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		bytes[i] ^= (uint8_t)(0x5a + 37 * i);
	}
}

/* A structure answer; a big buffer on the other side arrives as a memory descriptor. */
IOReturn
AppleKeyStoreUserClient::answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length)
{
	if (args->structureOutputDescriptor != NULL) {
		IOMemoryDescriptor * out = args->structureOutputDescriptor;
		IOReturn ret;

		if (out->getLength() < length) {
			return kIOReturnNoSpace;
		}
		ret = out->prepare();
		if (ret == kIOReturnSuccess) {
			out->writeBytes(0, bytes, length);
			out->complete();
			args->structureOutputDescriptorSize = (uint32_t) length;
		}
		return ret;
	}
	if (args->structureOutput == NULL || args->structureOutputSize < length) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, bytes, length);
	args->structureOutputSize = (uint32_t) length;
	return kIOReturnSuccess;
}

/* The structure input, inline or (a big one) as a memory descriptor, copied into buf. */
IOReturn
AppleKeyStoreUserClient::input(IOExternalMethodArguments * args, uint8_t * buf, size_t size, size_t * length)
{
	if (args->structureInputDescriptor != NULL) {
		IOMemoryDescriptor * in = args->structureInputDescriptor;
		size_t n = in->getLength();
		IOReturn ret;

		if (n > size) {
			return kIOReturnBadArgument;
		}
		ret = in->prepare();
		if (ret != kIOReturnSuccess) {
			return ret;
		}
		*length = in->readBytes(0, buf, n);
		in->complete();
		return *length == n ? kIOReturnSuccess : kIOReturnIOError;
	}
	if (args->structureInput == NULL || args->structureInputSize > size) {
		return kIOReturnBadArgument;
	}
	memcpy(buf, args->structureInput, args->structureInputSize);
	*length = args->structureInputSize;
	return kIOReturnSuccess;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	static const uint8_t bagUUID[16] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64, 0x00, 0x6b, 0x65, 0x79, 0x62, 0x61, 0x67, 0x00, 0x01 };
	uint8_t der[256];
	uint64_t * out = args->scalarOutput;
	bool changed = false, first = false;
	IOReturn ret = kIOReturnUnsupported;
	bool handled = true;

	if (gKS.real && gEntCheck != 0) {
		const char * wanted = NULL;
		int verdict = accessCheck(selector, &wanted);

		if (verdict != kAccessAllowed) {
			IOLockLock(gAKSLock);
			accessLog(selector, wanted, verdict);
			IOLockUnlock(gAKSLock);
			if (gEntCheck >= 2) {
				return kIOReturnNotPrivileged;
			}
		}
	}

	IOLockLock(gAKSLock);
	if (!gKS.loaded) {
		load();
	}

	switch (selector) {
	case kAKSUnlockBag:
		/* escrow and backup bags do not exist with the key bag (docs/keystore.md); without it, "opened" meant nothing either */
		ret = gKS.real ? kAKSNoPermission : kIOReturnSuccess;
		break;

	case kAKSOpenCheck:
	case kAKSAssertDrop:
	case kAKSPrewarm:
		ret = kIOReturnSuccess;
		break;

	case kAKSAssertHold:
	case kAKSAssertPromote:
	case kAKSAssertConsume:
		/*
		 * A lock assertion ("keep the device unlocked") can only be taken while it is unlocked. SpringBoard takes one
		 * to ask "is the passcode needed?" (SBFUserAuthenticationController, "Checking if passcode locked"): granted
		 * while locked, it lets the lock screen in without its keypad. Holding one does not keep the device from
		 * locking here.
		 */
		if (gKS.record.hasPasscode && gKS.locked) {
			ret = kAKSBlocked;
		} else {
			ret = kIOReturnSuccess;
		}
		break;

	case kAKSLockDevice:
	case kAKSLockDeviceNow:
		/* at once, whatever the grace period (see the head of this file) */
		if (gKS.record.hasPasscode && !gKS.locked) {
			char procname[32] = "?";

			gKS.locked = true;
			dropLockedRoots();
			changed = true;
			proc_selfname(procname, sizeof(procname));
			printf("MiniAppleKeyStore: locked (%s[%d], %s)\n", procname, proc_selfpid(), selector == kAKSLockDeviceNow ? "now" : "lock");
		}
		ret = kIOReturnSuccess;
		break;

	case kAKSUnlockDevice: {
		uint8_t passcode[kHKMaxSecret];
		size_t length = 0;

		if (args->scalarInputCount >= 3 && args->scalarInput[1] != 0 && args->scalarInput[2] != 0) {
			length = (size_t) args->scalarInput[2];
			if (length > sizeof(passcode)) {
				ret = kIOReturnBadArgument;
				break;
			}
			if (copyin((user_addr_t) args->scalarInput[1], passcode, length) != 0) {
				ret = kIOReturnBadArgument;
				break;
			}
		} else if (args->structureInput != NULL && args->structureInputSize <= sizeof(passcode)) {
			length = args->structureInputSize;
			memcpy(passcode, args->structureInput, length);
		}
		ret = tryPasscode(passcode, length, true, &changed, &first);
		mc_wipe(passcode, sizeof(passcode));
		break;
	}

	case kAKSVerifyPassword: {
		size_t size = 8 + kHKMaxSecret + 1024, length = 0;
		uint8_t * buf = IONew(uint8_t, size);
		uint32_t count = 0, passcodeLength = 0;

		if (buf == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		ret = input(args, buf, size, &length);
		if (ret == kIOReturnSuccess && length >= 8) {
			memcpy(&count, buf, 4);
			memcpy(&passcodeLength, buf + 4, 4);
		}
		if (ret == kIOReturnSuccess && (length < 8 || count < 1 || passcodeLength > kHKMaxSecret || passcodeLength > length - 8)) {
			ret = kIOReturnBadArgument;
		}
		if (ret == kIOReturnSuccess) {
			ret = tryPasscode(buf + 8, passcodeLength, false, &changed, &first);
		}
		mc_wipe(buf, size);
		IODelete(buf, uint8_t, size);
		break;
	}

	case kAKSGetLockState:
		if (args->scalarOutputCount < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = stateFlags();
		args->scalarOutputCount = 1;
		ret = kIOReturnSuccess;
		break;

	case kAKSGetSystem:
	case kAKSGeneration:
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		ret = kIOReturnSuccess;
		break;

	case kAKSGetDeviceState:
	case kAKSGetExtendedState:
		ret = answer(args, der, deviceState(der));
		break;

	case kAKSGetConfiguration:
		ret = answer(args, der, configuration(der));
		break;

	case kAKSSetConfiguration: {
		uint8_t buf[256];
		size_t length = 0;

		ret = input(args, buf, sizeof(buf), &length);
		if (ret == kIOReturnSuccess) {
			setConfiguration(buf, length);
		}
		break;
	}

	case kAKSGetBagUUID:
		ret = answer(args, bagUUID, sizeof(bagUUID));
		break;

	case kAKSNotifyStateSet:
		gAKSNotifyState = true;
		ret = kIOReturnSuccess;
		break;

	case kAKSNotifyStateIsSet:
		if (args->scalarOutputCount < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = gAKSNotifyState;
		args->scalarOutputCount = 1;
		ret = kIOReturnSuccess;
		break;

	case kAKSWrapKey: {
		uint8_t wrapped[8 + 64];
		size_t length = args->structureInputSize;

		if (args->structureInput == NULL || length == 0 || length > 64 || args->scalarInputCount < 2) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (gKS.real) {
			ret = bagWrap((uint32_t) args->scalarInput[1], (const uint8_t *) args->structureInput, length, wrapped);
			if (ret == kIOReturnSuccess) {
				if (args->scalarOutputCount >= 1) {
					out[0] = args->scalarInput[1];
					args->scalarOutputCount = 1;
				}
				ret = answer(args, wrapped, length + MC_KW_OVERHEAD);
			}
			mc_wipe(wrapped, sizeof(wrapped));
			break;
		}
		memcpy(wrapped, kWrapMark, sizeof(kWrapMark));
		wrapped[4] = (uint8_t)args->scalarInput[1];
		wrapped[5] = wrapped[6] = wrapped[7] = 0;
		memcpy(wrapped + 8, args->structureInput, length);
		wrapMix(wrapped + 8, length);
		if (args->scalarOutputCount >= 1) {
			out[0] = args->scalarInput[1];
			args->scalarOutputCount = 1;
		}
		ret = answer(args, wrapped, 8 + length);
		break;
	}

	case kAKSUnwrapKey: {
		uint8_t key[64];
		const uint8_t * wrapped = (const uint8_t *)args->structureInput;
		size_t length = args->structureInputSize;

		if (gKS.real) {
			if (wrapped == NULL || args->scalarInputCount < 2 || length > sizeof(key) + MC_KW_OVERHEAD) {
				ret = kIOReturnBadArgument;
				break;
			}
			ret = bagUnwrap((uint32_t) args->scalarInput[1], wrapped, length, key);
			if (ret == kIOReturnSuccess) {
				ret = answer(args, key, length - MC_KW_OVERHEAD);
			}
			mc_wipe(key, sizeof(key));
			break;
		}
		if (wrapped == NULL || length <= 8 || length > 8 + sizeof(key) || memcmp(wrapped, kWrapMark, sizeof(kWrapMark)) != 0) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(key, wrapped + 8, length - 8);
		wrapMix(key, length - 8);
		ret = answer(args, key, length - 8);
		break;
	}

	case kHKChangeSecret: {
		uint8_t * buf;
		size_t size = sizeof(HKChangeSecret) + 2 * kHKMaxSecret + kHKMaxBlob, length = 0;
		HKChangeSecret head = {};

		if (kauth_getuid() != 0) {
			ret = kIOReturnNotPrivileged;
			break;
		}
		buf = IONew(uint8_t, size);
		if (buf == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		ret = input(args, buf, size, &length);
		if (ret == kIOReturnSuccess) {
			if (length >= sizeof(head)) {
				memcpy(&head, buf, sizeof(head));
			}
			if (length < sizeof(head) || head.magic != kHKChangeMagic || head.oldLength > kHKMaxSecret ||
			    head.newLength > kHKMaxSecret || head.blobLength > kHKMaxBlob ||
			    sizeof(head) + head.oldLength + head.newLength + head.blobLength != length) {
				ret = kIOReturnBadArgument;
			} else {
				const uint8_t * p = buf + sizeof(head);
				ret = changeSecret(p, head.oldLength, p + head.oldLength, head.newLength,
				    p + head.oldLength + head.newLength, head.blobLength, &changed, &first);
			}
		}
		mc_wipe(buf, size);
		IODelete(buf, uint8_t, size);
		break;
	}

	case kHKCopyBlob:
		if (!gKS.record.hasPasscode || gKS.record.blobLength == 0) {
			ret = kIOReturnNotFound;
			break;
		}
		ret = answer(args, gKS.record.blob, gKS.record.blobLength);
		break;

	case kHKStatus:
		if (args->scalarOutputCount < 5) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = gKS.record.hasPasscode;
		out[1] = lockState();
		out[2] = gKS.record.failedAttempts;
		out[3] = secondsBlocked();
		out[4] = gKS.generation;
		args->scalarOutputCount = 5;
		ret = kIOReturnSuccess;
		break;

	default:
		handled = false;
		break;
	}
	IOLockUnlock(gAKSLock);

	announce(changed, first);
	if (handled) {
		return ret;
	}

	if (selector < 256 && !(gAKSLogged[selector / 64] & (1ULL << (selector % 64)))) {
		char procname[32] = "?";

		gAKSLogged[selector / 64] |= 1ULL << (selector % 64);
		proc_selfname(procname, sizeof(procname));
		printf("MiniAppleKeyStore: %s[%d] selector 0x%x not implemented (scalars %u in, %u out; structure %u in, %u out)\n",
		    procname, proc_selfpid(), selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize,
		    args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniAppleKeyStoreStart(void)
{
	/* user space looks for it by class, and by the path IOService:/IOResources/AppleKeyStore */
	IOService * resources = IOService::getResourceService();
	AppleKeyStore * store = OSTypeAlloc(AppleKeyStore);

	/* on by default on the Galaxy S9 alone, and off with keystore_real=0: compiled in, because its boot line has no room (docs/keystore.md) */
	uint32_t real = strcmp(HAKIOS_DEVICE, "starlte") == 0;

	PE_parse_boot_argn("keystore_real", &real, sizeof(real));
	gKS.real = real != 0;
	PE_parse_boot_argn("keystore_entcheck", &gEntCheck, sizeof(gEntCheck));
	gAKSLock = IOLockAlloc();
	if (gAKSLock == NULL || resources == NULL || store == NULL || !store->init() || !store->attach(resources)) {
		OSSafeReleaseNULL(store);
		printf("MiniAppleKeyStore: cannot create the service\n");
		return;
	}
	if (!store->start(resources)) {
		store->detach(resources);
		store->release();
		return;
	}
	gAKSService = store;            /* the registry holds it; so does this */
}
