/*
 * A passcode that gates the screen, for the built-in AppleKeyStore of SWEET (SweetKeyStore.cpp).
 * Boot-arg sweetspeed&0x800000 (off by default) switches it on; with the bit clear nothing here runs.
 *
 * What it keeps: whether the device has a passcode and, if so, a verifier of it, the lock state, the
 * count of wrong attempts and the delay they impose, and the grace period ManagedConfiguration sets.
 * What it is not: there is no Secure Enclave and no hardware key. The passcode gates the user
 * interface (the lock screen, Settings, ManagedConfiguration's checks) and nothing else: no data is
 * encrypted with it, and the record can be deleted by anyone who can write /private/var.
 *
 * The record: /private/var/keybags/scion-passcode, two 4096-byte slots. A change writes the slot that
 * does not hold the newest record, with a generation one higher and a SHA-256 over it, and syncs; a
 * torn write damages only the slot being written, and reading takes the valid slot with the highest
 * generation. A file that is missing, unreadable or without a valid slot means "no passcode". The
 * verifier is PBKDF2-HMAC-SHA256 (100000 iterations, 16-byte random salt, both kept with it), checked
 * against RFC 7914's test vector before a passcode is set or tried; if the check fails the stored
 * passcode is ignored (open rather than shut for good). Boot-arg passcode_reset=1 discards the
 * stored passcode when the record is read.
 *
 * Wrong attempts delay the next one as in Apple's Platform Security guide: after the 5th 1 minute, the
 * 6th 5 minutes, the 7th and 8th 15 minutes, the 9th and every later one 1 hour. Never a wipe. An
 * attempt made while a delay runs is refused and not counted. The count and the end of the delay are
 * kept in the record, so a restart does not clear them.
 *
 * Ported from Scion's iOS 16.5 key store (MiniAppleKeyStore): the HMAC/PBKDF2, the record's slots and
 * digest, the delay table, the state machine and the DER answers. Without the key bag, the entitlement
 * checks and the self-tests. The plain functions (sks_*) use no kernel types so that they can be run
 * on a host (SKS_HOST_TEST).
 */
#if defined(SKS_HOST_TEST)
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <libkern/crypto/sha2.h>
#else
#include <pexpert/arm64/board_config.h>
#endif

#if defined(SWEET) || defined(SKS_HOST_TEST)

#if !defined(SKS_HOST_TEST)
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/random.h>
#include "SweetKeyStorePasscode.h"
#endif

#if defined(SKS_HOST_TEST)
#define SKS_API
#else
#define SKS_API static
#endif

#define SKS_PATH                "/private/var/keybags/scion-passcode"
#define SKS_SLOT_SIZE           4096
#define SKS_MAGIC               0x5350434b      /* 'SPCK' */
#define SKS_VERSION             1
#define SKS_ITERATIONS          100000
#define SKS_MAX_ITERATIONS      10000000
#define SKS_MAX_SECRET          256
#define SKS_MAX_UNLOCK_ATTEMPTS 10              /* what "mua" says; nothing here wipes */
#define SKS_MAX_GRACE           (4 * 3600)

/* state flags ("ss" and selector 7): bit 0 locked, bit 1 no passcode, bit 2 unlocked since boot */
#define SKS_STATE_LOCKED        0x1
#define SKS_STATE_NOPASSCODE    0x2
#define SKS_STATE_UNLOCKED_ONCE 0x4
/* lock state ("sls"): 0 unlocked, 1 locked, 3 disabled (no passcode) */
#define SKS_LOCK_UNLOCKED       0
#define SKS_LOCK_LOCKED         1
#define SKS_LOCK_DISABLED       3

struct SKSRecord {
	uint32_t magic;
	uint32_t version;
	uint64_t generation;
	uint8_t  digest[32];            /* SHA-256 of everything after it */
	uint32_t hasPasscode;
	uint32_t iterations;
	uint8_t  salt[16];
	uint8_t  verifier[32];
	uint32_t failedAttempts;
	uint32_t gracePeriod;           /* seconds */
	uint64_t blockedUntil;          /* calendar seconds; 0: no delay */
};

#define SKS_DIGEST_FROM         __builtin_offsetof(SKSRecord, hasPasscode)

#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(SKSRecord) == 120, "the record has no padding");
static_assert(sizeof(SKSRecord) <= SKS_SLOT_SIZE, "a record fits in a slot");
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * plain functions */

static void
sks_wipe(void *p, size_t n)
{
	volatile uint8_t *v = (volatile uint8_t *)p;

	while (n--) {
		*v++ = 0;
	}
}

/* The delay after the n-th wrong attempt, in seconds (Apple Platform Security, "Passcodes"). */
SKS_API uint32_t
sks_delay_after(uint32_t failed)
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

/* Whole seconds of delay left (rounded up) at uptime now, for a delay that ends at until. */
SKS_API uint32_t
sks_seconds_blocked(uint64_t now_ns, uint64_t until_ns)
{
	if (until_ns == 0 || now_ns >= until_ns) {
		return 0;
	}
	return (uint32_t)((until_ns - now_ns + 1000000000ULL - 1) / 1000000000ULL);
}

struct SKSHmacKey {
	SHA256_CTX inner, outer;        /* after the padded key */
};

static void
sks_hmac_key(SKSHmacKey *k, const uint8_t *key, size_t keyLength)
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
	sks_wipe(block, sizeof(block));
	sks_wipe(hashed, sizeof(hashed));
}

static void
sks_hmac(const SKSHmacKey *k, const uint8_t *a, size_t aLength, const uint8_t *b, size_t bLength, uint8_t out[32])
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
	sks_wipe(&c, sizeof(c));
	sks_wipe(inner, sizeof(inner));
}

SKS_API void
sks_pbkdf2(const uint8_t *password, size_t passwordLength, const uint8_t *salt, size_t saltLength,
    uint32_t iterations, uint8_t *out, size_t outLength)
{
	SKSHmacKey k;

	sks_hmac_key(&k, password, passwordLength);
	for (uint32_t block = 1; outLength > 0; block++) {
		uint8_t count[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block };
		uint8_t u[32], t[32];
		size_t n = outLength < 32 ? outLength : 32;

		sks_hmac(&k, salt, saltLength, count, sizeof(count), u);
		memcpy(t, u, sizeof(t));
		for (uint32_t i = 1; i < iterations; i++) {
			sks_hmac(&k, u, sizeof(u), NULL, 0, u);
			for (size_t j = 0; j < sizeof(t); j++) {
				t[j] ^= u[j];
			}
		}
		memcpy(out, t, n);
		out += n;
		outLength -= n;
		sks_wipe(u, sizeof(u));
		sks_wipe(t, sizeof(t));
	}
	sks_wipe(&k, sizeof(k));
}

/* RFC 7914 section 11: PBKDF2-HMAC-SHA256 (P="passwd", S="salt", c=1), the first 32 of its 64 bytes */
SKS_API bool
sks_pbkdf2_selftest(void)
{
	static const uint8_t expected[32] = {
		0x55, 0xac, 0x04, 0x6e, 0x56, 0xe3, 0x08, 0x9f, 0xec, 0x16, 0x91, 0xc2, 0x25, 0x44, 0xb6, 0x05,
		0xf9, 0x41, 0x85, 0x21, 0x6d, 0xde, 0x04, 0x65, 0xe6, 0x8b, 0x9d, 0x57, 0xc2, 0x0d, 0xac, 0xbc,
	};
	uint8_t out[32];

	sks_pbkdf2((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out, sizeof(out));
	return memcmp(out, expected, sizeof(out)) == 0;
}

/* Constant-time comparison. */
SKS_API bool
sks_same_bytes(const uint8_t *a, const uint8_t *b, size_t length)
{
	uint8_t d = 0;

	for (size_t i = 0; i < length; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

SKS_API void
sks_record_digest(const SKSRecord *r, uint8_t out[32])
{
	SHA256_CTX c;

	SHA256_Init(&c);
	SHA256_Update(&c, (const uint8_t *)r + SKS_DIGEST_FROM, sizeof(*r) - SKS_DIGEST_FROM);
	SHA256_Final(out, &c);
}

SKS_API bool
sks_record_valid(const SKSRecord *r)
{
	uint8_t digest[32];

	if (r->magic != SKS_MAGIC || r->version != SKS_VERSION) {
		return false;
	}
	sks_record_digest(r, digest);
	if (!sks_same_bytes(digest, r->digest, sizeof(digest))) {
		return false;
	}
	if (r->hasPasscode && (r->iterations == 0 || r->iterations > SKS_MAX_ITERATIONS)) {
		return false;
	}
	return true;
}

/*
 * Which of the two slots holds the newest valid record: the one with the highest generation, the
 * later slot on a tie. Returns -1 if neither is valid; *generation is the newest one's.
 */
SKS_API int
sks_pick_slot(const bool valid[2], const uint64_t generation[2], uint64_t *newest)
{
	int slot = -1;

	*newest = 0;
	for (int i = 0; i < 2; i++) {
		if (valid[i] && generation[i] >= *newest) {
			*newest = generation[i];
			slot = i;
		}
	}
	return slot;
}

/* A write goes to the slot that does not hold the newest record. */
SKS_API int
sks_next_slot(int newestSlot)
{
	return newestSlot == 0 ? 1 : 0;
}

SKS_API uint32_t
sks_state_flags(bool hasPasscode, bool locked, bool unlockedOnce)
{
	uint32_t flags = 0;

	if (!hasPasscode) {
		return SKS_STATE_NOPASSCODE | SKS_STATE_UNLOCKED_ONCE;
	}
	if (locked) {
		flags |= SKS_STATE_LOCKED;
	}
	if (unlockedOnce) {
		flags |= SKS_STATE_UNLOCKED_ONCE;
	}
	return flags;
}

SKS_API uint32_t
sks_lock_state(bool hasPasscode, bool locked)
{
	if (!hasPasscode) {
		return SKS_LOCK_DISABLED;
	}
	return locked ? SKS_LOCK_LOCKED : SKS_LOCK_UNLOCKED;
}

/* Append SEQUENCE { UTF8String key, INTEGER value }; returns the new length. */
SKS_API size_t
sks_der_put(uint8_t *buf, size_t length, const char *key, uint64_t value)
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
	buf[length++] = (uint8_t)(keyLength + n + 4);
	buf[length++] = 0x0c;
	buf[length++] = (uint8_t)keyLength;
	memcpy(buf + length, key, keyLength);
	length += keyLength;
	buf[length++] = 0x02;
	buf[length++] = (uint8_t)n;
	memcpy(buf + length, bytes, n);
	return length + n;
}

/* The SET's header, once its contents (written from buf + 4) are known; returns the whole length. */
SKS_API size_t
sks_der_set(uint8_t *buf, size_t length)
{
	size_t body = length - 4;

	if (body < 128) {
		buf[0] = 0x31;
		buf[1] = (uint8_t)body;
		memmove(buf + 2, buf + 4, body);
		return body + 2;
	}
	buf[0] = 0x31;
	buf[1] = 0x82;
	buf[2] = (uint8_t)(body >> 8);
	buf[3] = (uint8_t)body;
	return body + 4;
}

/* Device state (selectors 17, 35): buf needs 192 bytes. */
SKS_API size_t
sks_der_device_state(uint8_t *buf, uint32_t blocked, uint32_t flags, uint32_t failed, uint32_t lockState, bool graceSet)
{
	size_t length = 4;

	length = sks_der_put(buf, length, "sb", blocked);
	length = sks_der_put(buf, length, "ss", flags);
	length = sks_der_put(buf, length, "sas", 0);
	length = sks_der_put(buf, length, "sfa", failed);
	length = sks_der_put(buf, length, "sgs", 0);
	length = sks_der_put(buf, length, "sls", lockState);
	length = sks_der_put(buf, length, "sgpe", graceSet);
	length = sks_der_put(buf, length, "srcd", 0);
	return sks_der_set(buf, length);
}

/* Configuration (selector 31): buf needs 64 bytes. */
SKS_API size_t
sks_der_configuration(uint8_t *buf, uint32_t gracePeriod)
{
	size_t length = 4;

	length = sks_der_put(buf, length, "bd", 0);
	length = sks_der_put(buf, length, "gp", gracePeriod);
	length = sks_der_put(buf, length, "mua", SKS_MAX_UNLOCK_ATTEMPTS);
	return sks_der_set(buf, length);
}

/* One tag-length-value; returns false if it does not fit. */
static bool
sks_der_next(const uint8_t **p, const uint8_t *end, uint8_t *tag, const uint8_t **value, size_t *length)
{
	const uint8_t *q = *p;
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

/* Selector 30's input: finds "gp" (cut to 4 hours); returns false if there is none. */
SKS_API bool
sks_parse_grace(const uint8_t *der, size_t size, uint32_t *gracePeriod)
{
	const uint8_t *p = der, *end = der + size, *value;
	uint8_t tag;
	size_t length;
	bool found = false;

	if (!sks_der_next(&p, end, &tag, &value, &length) || (tag != 0x31 && tag != 0x30)) {
		return false;
	}
	p = value;
	end = value + length;
	while (p < end) {
		const uint8_t *item, *key, *number, *q;
		size_t itemLength, keyLength, numberLength;
		uint8_t t1, t2;
		uint64_t v = 0;

		if (!sks_der_next(&p, end, &tag, &item, &itemLength) || tag != 0x30) {
			return found;
		}
		q = item;
		if (!sks_der_next(&q, item + itemLength, &t1, &key, &keyLength) || t1 != 0x0c ||
		    !sks_der_next(&q, item + itemLength, &t2, &number, &numberLength) || t2 != 0x02 || numberLength > 8) {
			continue;
		}
		for (size_t i = 0; i < numberLength; i++) {
			v = (v << 8) | number[i];
		}
		if (keyLength == 2 && memcmp(key, "gp", 2) == 0) {
			*gracePeriod = v > SKS_MAX_GRACE ? SKS_MAX_GRACE : (uint32_t)v;
			found = true;
		}
	}
	return found;
}

/*
 * Selector 42's input: { u32 count; { u32 length; bytes, padded to 4 } ... }, the passcode is the
 * first item. Gives its place in buf; false if the shape is wrong or it is longer than 256.
 */
SKS_API bool
sks_parse_verify(const uint8_t *buf, size_t size, size_t *offset, size_t *length)
{
	uint32_t count, passcodeLength;

	if (size < 8) {
		return false;
	}
	memcpy(&count, buf, 4);
	memcpy(&passcodeLength, buf + 4, 4);
	if (count < 1 || passcodeLength > SKS_MAX_SECRET || passcodeLength > size - 8) {
		return false;
	}
	*offset = 8;
	*length = passcodeLength;
	return true;
}

#if !defined(SKS_HOST_TEST)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the state */

static struct {
	IOLock   *lock;
	bool     loaded;                /* the file has been read (or found absent) */
	bool     kdfGood;               /* PBKDF2 passed its self-test */
	bool     writeFailureLogged;
	SKSRecord record;               /* the current record, as in memory */
	uint64_t generation;            /* the newest generation on disk */
	int      newestSlot;            /* where it is; -1 none */
	bool     locked;
	bool     unlockedOnce;          /* since boot */
	uint64_t blockedUntilUptime;    /* uptime, ns; 0: no delay */
	int      systemHandle;
	int      pendingHandle;         /* -1 none */
} gSKS = { .newestSlot = -1, .pendingHandle = -1 };

static uint64_t
sks_uptime_ns(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

static uint64_t
sks_calendar_seconds(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	clock_get_calendar_microtime(&secs, &usecs);
	return (uint64_t)secs;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the file */

static int
sks_file_io(enum uio_rw rw, int slot, SKSRecord *r)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int resid = 0, error;
	int fmode = rw == UIO_READ ? FREAD : (FREAD | FWRITE | O_CREAT);

	error = vnode_open(SKS_PATH, fmode, 0600, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
	if (error) {
		return error;
	}
	error = vn_rdwr(rw, vp, (caddr_t)r, (int)sizeof(*r), (off_t)slot * SKS_SLOT_SIZE, UIO_SYSSPACE,
	    rw == UIO_WRITE ? IO_SYNC | IO_NOAUTH : IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (error == 0 && resid != 0) {
		error = rw == UIO_READ ? ENOENT : EIO;  /* a short slot: never written */
	}
	if (error == 0 && rw == UIO_WRITE) {
		error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
	}
	vnode_close(vp, fmode, ctx);
	return error;
}

/* Write r as the next generation into the slot that does not hold the newest one. If it cannot be written the state stays in memory. */
static int
sks_persist(SKSRecord *r)
{
	int slot = sks_next_slot(gSKS.newestSlot);
	int error;

	r->magic = SKS_MAGIC;
	r->version = SKS_VERSION;
	r->generation = gSKS.generation + 1;
	sks_record_digest(r, r->digest);
	error = sks_file_io(UIO_WRITE, slot, r);
	if (error) {
		if (!gSKS.writeFailureLogged) {
			gSKS.writeFailureLogged = true;
			IOLog("SWEET AKS: cannot write %s slot %d (error %d): the passcode state stays in memory\n",
			    SKS_PATH, slot, error);
		}
		return error;
	}
	gSKS.generation = r->generation;
	gSKS.newestSlot = slot;
	return 0;
}

static void
sks_set_blocked_until(SKSRecord *r, uint32_t seconds)
{
	if (seconds == 0) {
		r->blockedUntil = 0;
		gSKS.blockedUntilUptime = 0;
	} else {
		r->blockedUntil = sks_calendar_seconds() + seconds;
		gSKS.blockedUntilUptime = sks_uptime_ns() + (uint64_t)seconds * NSEC_PER_SEC;
	}
}

static uint32_t
sks_blocked(void)
{
	return sks_seconds_blocked(sks_uptime_ns(), gSKS.blockedUntilUptime);
}

static void
sks_no_passcode(SKSRecord *r)
{
	uint32_t grace = r->gracePeriod;

	memset(r, 0, sizeof(*r));
	r->gracePeriod = grace;
}

/* The first call of the boot: read the record (with the lock held). */
static void
sks_load(void)
{
	SKSRecord slots[2];
	bool valid[2] = { false, false };
	uint64_t generation[2] = { 0, 0 };
	int error[2] = { 0, 0 };
	uint32_t reset = 0;

	gSKS.loaded = true;
	gSKS.newestSlot = -1;
	gSKS.unlockedOnce = true;
	gSKS.kdfGood = sks_pbkdf2_selftest();
	if (!gSKS.kdfGood) {
		IOLog("SWEET AKS: PBKDF2-HMAC-SHA256 fails its test vector: no passcode can be set or used\n");
	}
	for (int i = 0; i < 2; i++) {
		memset(&slots[i], 0, sizeof(slots[i]));
		error[i] = sks_file_io(UIO_READ, i, &slots[i]);
		if (error[i] == 0 && !sks_record_valid(&slots[i])) {
			error[i] = EINVAL;
		}
		valid[i] = error[i] == 0;
		generation[i] = slots[i].generation;
	}
	gSKS.newestSlot = sks_pick_slot(valid, generation, &gSKS.generation);
	if (gSKS.newestSlot >= 0) {
		gSKS.record = slots[gSKS.newestSlot];
	}
	sks_wipe(slots, sizeof(slots));

	if (PE_parse_boot_argn("passcode_reset", &reset, sizeof(reset)) && reset != 0) {
		bool had = gSKS.newestSlot >= 0 && gSKS.record.hasPasscode;

		sks_no_passcode(&gSKS.record);
		/* both slots, so that no older record with a passcode is left to come back */
		for (int i = 0; i < 2; i++) {
			if (sks_persist(&gSKS.record) != 0) {
				break;
			}
		}
		IOLog("SWEET AKS: passcode_reset=%u: %s; %s now holds no passcode\n", reset,
		    had ? "the stored passcode is discarded" : "there was no passcode", SKS_PATH);
	}

	if (gSKS.newestSlot < 0) {
		IOLog("SWEET AKS: %s: %s (slot errors %d, %d): no passcode\n", SKS_PATH,
		    error[0] == ENOENT && error[1] == ENOENT ? "absent" : "no valid record", error[0], error[1]);
		sks_no_passcode(&gSKS.record);
		return;
	}
	if (gSKS.record.hasPasscode && !gSKS.kdfGood) {
		IOLog("SWEET AKS: the stored passcode is ignored (the key derivation is broken): no passcode\n");
		sks_no_passcode(&gSKS.record);
		return;
	}
	if (gSKS.record.hasPasscode) {
		uint64_t now = sks_calendar_seconds();
		uint32_t delay = 0;

		gSKS.locked = true;
		gSKS.unlockedOnce = false;
		if (gSKS.record.blockedUntil > now) {
			uint64_t left = gSKS.record.blockedUntil - now;
			uint32_t most = sks_delay_after(gSKS.record.failedAttempts);

			delay = left > most ? most : (uint32_t)left;    /* a clock that went back does not lengthen it */
			gSKS.blockedUntilUptime = sks_uptime_ns() + (uint64_t)delay * NSEC_PER_SEC;
		}
		IOLog("SWEET AKS: %s loaded: passcode set (generation %llu, slot %d), locked; %u failed attempts, %u s of delay left\n",
		    SKS_PATH, gSKS.generation, gSKS.newestSlot, gSKS.record.failedAttempts, delay);
	} else {
		IOLog("SWEET AKS: %s loaded: generation %llu, no passcode; %u failed attempts\n", SKS_PATH,
		    gSKS.generation, gSKS.record.failedAttempts);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the state machine (lock held) */

/* A passcode try. *changed says whether the lock state changed, *first whether this is the first unlock. */
static IOReturn
sks_try(const uint8_t *passcode, size_t length, bool unlock, bool *changed, bool *first)
{
	uint8_t derived[32];
	bool good;

	if (!gSKS.record.hasPasscode) {
		return kIOReturnSuccess;
	}
	if (sks_blocked() > 0) {
		return kIOReturnNotPermitted;
	}
	if (length == 0 || !gSKS.kdfGood) {
		return kIOReturnNotReadable;    /* not counted: nothing was tried */
	}
	sks_pbkdf2(passcode, length, gSKS.record.salt, sizeof(gSKS.record.salt), gSKS.record.iterations,
	    derived, sizeof(derived));
	good = sks_same_bytes(derived, gSKS.record.verifier, sizeof(derived));
	sks_wipe(derived, sizeof(derived));

	if (!good) {
		uint32_t delay;

		if (gSKS.record.failedAttempts < 1000) {
			gSKS.record.failedAttempts++;
		}
		delay = sks_delay_after(gSKS.record.failedAttempts);
		sks_set_blocked_until(&gSKS.record, delay);
		sks_persist(&gSKS.record);      /* if it cannot be written, memory still counts it */
		IOLog("SWEET AKS: wrong passcode, attempt %u, %u s before the next\n", gSKS.record.failedAttempts, delay);
		*changed = true;                /* the failed count and the delay are state too */
		return kIOReturnNotReadable;
	}
	if (gSKS.record.failedAttempts != 0 || gSKS.record.blockedUntil != 0) {
		gSKS.record.failedAttempts = 0;
		sks_set_blocked_until(&gSKS.record, 0);
		sks_persist(&gSKS.record);
		*changed = true;
	}
	if (unlock) {
		if (gSKS.locked) {
			gSKS.locked = false;
			*changed = true;
			IOLog("SWEET AKS: unlocked with the passcode\n");
		}
		if (!gSKS.unlockedOnce) {
			gSKS.unlockedOnce = true;
			*first = true;
		}
	}
	return kIOReturnSuccess;
}

static IOReturn
sks_change_secret(const uint8_t *oldSecret, size_t oldLength, const uint8_t *newSecret, size_t newLength,
    bool *changed, bool *first)
{
	SKSRecord next;
	bool had = gSKS.record.hasPasscode;
	IOReturn ret;

	if (newLength > 0 && !gSKS.kdfGood) {
		return kIOReturnUnsupported;
	}
	if (had) {
		ret = sks_try(oldSecret, oldLength, false, changed, first);
		if (ret != kIOReturnSuccess) {
			return ret;
		}
	}
	next = gSKS.record;
	if (newLength == 0) {
		sks_no_passcode(&next);
	} else {
		next.hasPasscode = 1;
		next.iterations = SKS_ITERATIONS;
		read_random(next.salt, sizeof(next.salt));
		sks_pbkdf2(newSecret, newLength, next.salt, sizeof(next.salt), next.iterations,
		    next.verifier, sizeof(next.verifier));
		next.failedAttempts = 0;
		next.blockedUntil = 0;
	}
	gSKS.record = next;
	sks_wipe(&next, sizeof(next));
	gSKS.blockedUntilUptime = 0;
	gSKS.locked = false;
	if (!gSKS.unlockedOnce) {
		gSKS.unlockedOnce = true;
		*first = true;
	}
	*changed = true;
	sks_persist(&gSKS.record);              /* if it cannot be written, memory holds it */
	IOLog("SWEET AKS: passcode %s\n", newLength == 0 ? (had ? "removed" : "still none") : (had ? "changed" : "set"));
	return kIOReturnSuccess;
}

static void
sks_lock(uint32_t selector, bool *changed)
{
	if (gSKS.record.hasPasscode && !gSKS.locked) {
		char procname[32] = "?";

		gSKS.locked = true;
		*changed = true;
		proc_selfname(procname, sizeof(procname));
		IOLog("SWEET AKS: locked (%s[%d], %s)\n", procname, proc_selfpid(), selector == 32 ? "now" : "lock");
	}
}

/* Selector 3: a 64-byte blob keybagd keeps in its own file; nobody reads it back. */
static IOReturn
sks_save_bag(IOExternalMethodArguments *args)
{
	uint8_t blob[64];
	uint32_t version = 1, handle = (uint32_t)args->scalarInput[0];
	uint64_t generation = gSKS.record.generation;
	SHA256_CTX c;

	memset(blob, 0, sizeof(blob));
	memcpy(blob, "SCIONBAG", 8);
	memcpy(blob + 8, &version, 4);
	memcpy(blob + 12, &handle, 4);
	memcpy(blob + 16, &generation, 8);
	blob[24] = gSKS.record.hasPasscode ? 1 : 0;
	SHA256_Init(&c);
	SHA256_Update(&c, gSKS.record.digest, sizeof(gSKS.record.digest));
	SHA256_Final(blob + 32, &c);
	return sweet_aks_struct_out(args, blob, sizeof(blob));
}

/* The passcode a caller gives by address and length (scalars i, i + 1); false if it is not usable. */
static bool
sks_copy_passcode(IOExternalMethodArguments *args, unsigned int i, uint8_t *buf, size_t *length)
{
	uint64_t address = args->scalarInput[i], n = args->scalarInput[i + 1];

	if (n > SKS_MAX_SECRET || (n != 0 && address == 0)) {
		return false;
	}
	if (n != 0 && copyin((user_addr_t)address, buf, (size_t)n) != 0) {
		return false;
	}
	*length = (size_t)n;
	return true;
}

static IOReturn
sks_unlock(IOExternalMethodArguments *args, bool *changed, bool *first)
{
	uint8_t passcode[SKS_MAX_SECRET];
	size_t length = 0;
	IOReturn ret;

	if (args->scalarInputCount < 3) {
		return kIOReturnBadArgument;
	}
	if (!gSKS.record.hasPasscode) {
		return kIOReturnSuccess;
	}
	if (!sks_copy_passcode(args, 1, passcode, &length)) {
		sks_wipe(passcode, sizeof(passcode));
		return kIOReturnBadArgument;
	}
	ret = sks_try(passcode, length, true, changed, first);
	sks_wipe(passcode, sizeof(passcode));
	return ret;
}

static IOReturn
sks_verify(IOExternalMethodArguments *args, bool *changed, bool *first)
{
	uint8_t buf[8 + SKS_MAX_SECRET + 1024];
	size_t offset, length;
	int32_t size = sweet_aks_struct_in(args, buf, sizeof(buf));
	IOReturn ret;

	if (size < 0 || !sks_parse_verify(buf, (size_t)size, &offset, &length)) {
		ret = kIOReturnBadArgument;
	} else {
		ret = sks_try(buf + offset, length, false, changed, first);
	}
	sks_wipe(buf, sizeof(buf));
	return ret;
}

static IOReturn
sks_change(IOExternalMethodArguments *args, bool *changed, bool *first)
{
	uint8_t oldSecret[SKS_MAX_SECRET], newSecret[SKS_MAX_SECRET];
	size_t oldLength = 0, newLength = 0;
	uint64_t handle, option;
	IOReturn ret = kIOReturnBadArgument;

	if (args->scalarInputCount < 7) {
		return kIOReturnBadArgument;
	}
	handle = args->scalarInput[0];
	option = args->scalarInput[1];
	if (sks_copy_passcode(args, 3, oldSecret, &oldLength) && sks_copy_passcode(args, 5, newSecret, &newLength)) {
		ret = sks_change_secret(oldSecret, oldLength, newSecret, newLength, changed, first);
	}
	sks_wipe(oldSecret, sizeof(oldSecret));
	sks_wipe(newSecret, sizeof(newSecret));
	if (ret == kIOReturnSuccess && args->scalarOutputCount >= 1) {
		if (option != 0) {
			gSKS.pendingHandle = gSKS.systemHandle + 1;
			args->scalarOutput[0] = (uint64_t)gSKS.pendingHandle;
		} else {
			args->scalarOutput[0] = handle;
		}
		args->scalarOutputCount = 1;
	}
	return ret;
}

static IOReturn
sks_set_configuration(IOExternalMethodArguments *args)
{
	uint8_t der[256];
	int32_t size = sweet_aks_struct_in(args, der, sizeof(der));
	uint32_t grace;

	if (size < 0) {
		return kIOReturnBadArgument;
	}
	if (sks_parse_grace(der, (size_t)size, &grace) && gSKS.record.gracePeriod != grace) {
		gSKS.record.gracePeriod = grace;
		sks_persist(&gSKS.record);
		IOLog("SWEET AKS: grace period %u s\n", grace);
	}
	return kIOReturnSuccess;
}

static IOReturn
sks_scalar_out(IOExternalMethodArguments *args, uint64_t value)
{
	if (args->scalarOutputCount < 1) {
		return kIOReturnBadArgument;
	}
	args->scalarOutput[0] = value;
	args->scalarOutputCount = 1;
	return kIOReturnSuccess;
}

/* The answer to one selector, with the lock held. */
static IOReturn
sks_dispatch(uint32_t selector, IOExternalMethodArguments *args, bool *changed, bool *first)
{
	uint8_t der[192];
	uint32_t blocked;

	switch (selector) {
	case 7:         /* aks_get_lock_state */
		return sks_scalar_out(args, sks_state_flags(gSKS.record.hasPasscode, gSKS.locked, gSKS.unlockedOnce));
	case 17:        /* aks_get_device_state */
	case 35:        /* aks_get_extended_device_state */
		blocked = sks_blocked();
		args->scalarOutputCount = 0;
		return sweet_aks_struct_out(args, der, sks_der_device_state(der, blocked,
		    sks_state_flags(gSKS.record.hasPasscode, gSKS.locked, gSKS.unlockedOnce),
		    gSKS.record.hasPasscode ? gSKS.record.failedAttempts : 0,
		    sks_lock_state(gSKS.record.hasPasscode, gSKS.locked),
		    gSKS.record.hasPasscode && gSKS.record.gracePeriod != 0));
	case 8:         /* aks_lock_device */
	case 32:        /* aks_lock_device_now */
		sks_lock(selector, changed);
		args->scalarOutputCount = 0;
		return kIOReturnSuccess;
	case 9:         /* aks_unlock_device */
		args->scalarOutputCount = 0;
		return sks_unlock(args, changed, first);
	case 42:        /* aks_verify_password */
		args->scalarOutputCount = 0;
		return sks_verify(args, changed, first);
	case 15:        /* aks_change_secret */
		return sks_change(args, changed, first);
	case 3:         /* aks_save_bag */
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		args->scalarOutputCount = 0;
		return sks_save_bag(args);
	case 5:         /* aks_set_system */
		if (gSKS.pendingHandle != -1) {
			gSKS.systemHandle = gSKS.pendingHandle;
			gSKS.pendingHandle = -1;
		}
		args->scalarOutputCount = 0;
		return kIOReturnSuccess;
	case 14:        /* aks_get_system */
		return sks_scalar_out(args, (uint64_t)gSKS.systemHandle);
	case 4:
	case 12:
	case 13:
	case 27:
	case 105:
		args->scalarOutputCount = 0;
		return kIOReturnSuccess;
	case 26:        /* lock assertions: refused while locked */
	case 100:
	case 101:
		args->scalarOutputCount = 0;
		return gSKS.record.hasPasscode && gSKS.locked ? kIOReturnNotPermitted : kIOReturnSuccess;
	case 31:
		args->scalarOutputCount = 0;
		return sweet_aks_struct_out(args, der, sks_der_configuration(der, gSKS.record.gracePeriod));
	case 30:
		args->scalarOutputCount = 0;
		return sks_set_configuration(args);
	case 102:
		sweet_aks_notify_state_set();
		args->scalarOutputCount = 0;
		return kIOReturnSuccess;
	case 103:
		return sks_scalar_out(args, sweet_aks_notify_state_get());
	default:
		return kIOReturnUnsupported;    /* not reached: sks_handles() */
	}
}

static bool
sks_handles(uint32_t selector)
{
	switch (selector) {
	case 3: case 4: case 5: case 7: case 8: case 9: case 12: case 13: case 14: case 15: case 17:
	case 26: case 27: case 30: case 31: case 32: case 35: case 42: case 100: case 101: case 102:
	case 103: case 105:
		return true;
	default:
		return false;
	}
}

/* The lock state changed / the first unlock of the boot: the clients of IOGeneralInterest (MKBEventsRegister). */
static void
sks_announce(bool changed, bool first)
{
	if (changed || first) {
		sweet_aks_message_clients(0xe3ffd00d);
	}
	if (first) {
		sweet_aks_message_clients(0xe3ffd00f);
	}
}

void
sweet_aks_passcode_init(void)
{
	if (gSKS.lock == NULL) {
		gSKS.lock = IOLockAlloc();
	}
}

bool
sweet_aks_passcode_external(uint32_t selector, IOExternalMethodArguments *args, IOReturn *ret)
{
	bool changed = false, first = false;

	if (!sks_handles(selector)) {
		return false;
	}
	if (gSKS.lock == NULL) {
		*ret = kIOReturnNotReady;
		return true;
	}
	IOLockLock(gSKS.lock);
	if (!gSKS.loaded) {
		sks_load();
	}
	*ret = sks_dispatch(selector, args, &changed, &first);
	IOLockUnlock(gSKS.lock);
	sks_announce(changed, first);
	return true;
}

#endif /* !SKS_HOST_TEST */
#endif /* SWEET || SKS_HOST_TEST */
