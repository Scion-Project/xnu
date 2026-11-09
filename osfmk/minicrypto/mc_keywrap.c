/*
 * The key store's cryptography: AES-256 key wrap (RFC 3394) and Argon2id (RFC 9106), for
 * iokit/Kernel/MiniAppleKeyStore.cpp.
 *
 * This kernel has no corecrypto AES: Apple's corecrypto kext is closed, and minicrypto's own AES (mc_aes.c) is a
 * byte-table implementation, not constant-time, written so that the TCP stack could boot. So class keys do not go through
 * it. The primitives here are not written here either:
 *
 *   third_party/bearssl_aes_ct64*.c   BearSSL's constant-time bitsliced AES (Thomas Pornin, MIT)
 *   third_party/monocypher.[ch]       Monocypher 4.0.2's Argon2id (Loup Vaillant and others, CC0 or BSD-2-Clause)
 *
 * What is here is the glue: one AES block at a time on BearSSL's bitsliced core, RFC 3394's loop on it, and the known
 * answers. The AES instructions of the CPU are not used: kernel code that uses vector registers must save them, and a
 * key wrap is a few dozen blocks.
 */
#include <string.h>

#include "mc_keywrap.h"
#include "third_party/bearssl_inner.h"
#include "third_party/monocypher.h"

extern int cc_cmp_safe(size_t num, const void *ptr1, const void *ptr2);

void
mc_wipe(void *p, size_t n)
{
	crypto_wipe(p, n);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * AES-256, a block at a time */

struct mc_aesct {
	uint64_t skey[120];
	unsigned rounds;
};

static void
aesct_init(struct mc_aesct *ctx, const uint8_t key[32])
{
	uint64_t comp[30];

	ctx->rounds = br_aes_ct64_keysched(comp, key, 32);
	br_aes_ct64_skey_expand(ctx->skey, ctx->rounds, comp);
	crypto_wipe(comp, sizeof(comp));
}

static void
aesct_block(const struct mc_aesct *ctx, const uint8_t in[16], uint8_t out[16], int decrypt)
{
	uint32_t w[16] = { 0 };
	uint64_t q[8];

	br_range_dec32le(w, 4, in);
	for (int i = 0; i < 4; i++) {
		br_aes_ct64_interleave_in(&q[i], &q[i + 4], w + (i << 2));
	}
	br_aes_ct64_ortho(q);
	if (decrypt) {
		br_aes_ct64_bitslice_decrypt(ctx->rounds, ctx->skey, q);
	} else {
		br_aes_ct64_bitslice_encrypt(ctx->rounds, ctx->skey, q);
	}
	br_aes_ct64_ortho(q);
	for (int i = 0; i < 4; i++) {
		br_aes_ct64_interleave_out(w + (i << 2), q[i], q[i + 4]);
	}
	for (int i = 0; i < 4; i++) {
		out[4 * i] = (uint8_t)w[i];
		out[4 * i + 1] = (uint8_t)(w[i] >> 8);
		out[4 * i + 2] = (uint8_t)(w[i] >> 16);
		out[4 * i + 3] = (uint8_t)(w[i] >> 24);
	}
	crypto_wipe(w, sizeof(w));
	crypto_wipe(q, sizeof(q));
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * RFC 3394 */

static const uint8_t kKWInitial[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };

static int
kwLength(size_t n)
{
	return n >= 16 && n <= MC_KW_MAX_KEY && (n % 8) == 0;
}

static void
xorCounter(uint8_t a[8], uint64_t t)
{
	for (int i = 7; i >= 0; i--) {
		a[i] ^= (uint8_t)t;
		t >>= 8;
	}
}

int
mc_aes256_kw_wrap(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out)
{
	struct mc_aesct ctx;
	uint8_t b[16];
	size_t words = n / 8;

	if (!kwLength(n)) {
		return -1;
	}
	aesct_init(&ctx, key);
	memmove(out + 8, in, n);
	memcpy(b, kKWInitial, 8);
	for (uint64_t j = 0; j < 6; j++) {
		for (size_t i = 1; i <= words; i++) {
			memcpy(b + 8, out + 8 * i, 8);
			aesct_block(&ctx, b, b, 0);
			memcpy(out + 8 * i, b + 8, 8);
			xorCounter(b, words * j + i);
		}
	}
	memcpy(out, b, 8);
	crypto_wipe(b, sizeof(b));
	crypto_wipe(&ctx, sizeof(ctx));
	return 0;
}

int
mc_aes256_kw_unwrap(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out)
{
	struct mc_aesct ctx;
	uint8_t b[16];
	size_t words;
	int good;

	if (n < MC_KW_OVERHEAD || !kwLength(n - MC_KW_OVERHEAD)) {
		return -1;
	}
	words = (n - MC_KW_OVERHEAD) / 8;
	aesct_init(&ctx, key);
	memcpy(b, in, 8);
	memmove(out, in + 8, n - 8);
	for (uint64_t j = 6; j-- > 0;) {
		for (size_t i = words; i >= 1; i--) {
			xorCounter(b, words * j + i);
			memcpy(b + 8, out + 8 * (i - 1), 8);
			aesct_block(&ctx, b, b, 1);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	good = cc_cmp_safe(8, b, kKWInitial) == 0;
	crypto_wipe(b, sizeof(b));
	crypto_wipe(&ctx, sizeof(ctx));
	if (!good) {
		crypto_wipe(out, n - 8);
		return -1;
	}
	return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * Argon2id */

void
mc_argon2id(uint8_t out[32], void *work, uint32_t memKiB, uint32_t passes, const uint8_t *password, size_t passwordLength,
    const uint8_t *salt, size_t saltLength)
{
	crypto_argon2_config config = { .algorithm = CRYPTO_ARGON2_ID, .nb_blocks = memKiB, .nb_passes = passes, .nb_lanes = 1 };
	crypto_argon2_inputs inputs = { .pass = password, .salt = salt, .pass_size = (uint32_t)passwordLength, .salt_size = (uint32_t)saltLength };

	crypto_argon2(out, 32, work, config, inputs, crypto_argon2_no_extras);
	crypto_wipe(work, (size_t)memKiB * MC_ARGON2_BLOCK);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * the known answers */

static int
aesVector(void)
{
	/* FIPS 197 appendix C.3 */
	static const uint8_t expected[16] = { 0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89 };
	static const uint8_t plain[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	uint8_t key[32], out[16];
	struct mc_aesct ctx;

	for (int i = 0; i < 32; i++) {
		key[i] = (uint8_t)i;
	}
	aesct_init(&ctx, key);
	aesct_block(&ctx, plain, out, 0);
	if (memcmp(out, expected, 16) != 0) {
		return 0;
	}
	aesct_block(&ctx, expected, out, 1);
	return memcmp(out, plain, 16) == 0;
}

static int
kwVector(void)
{
	/* RFC 3394 4.6: 256 bits of key data with a 256-bit KEK */
	static const uint8_t expected[40] = {
		0x28, 0xc9, 0xf4, 0x04, 0xc4, 0xb8, 0x10, 0xf4, 0xcb, 0xcc, 0xb3, 0x5c, 0xfb, 0x87, 0xf8, 0x26,
		0x3f, 0x57, 0x86, 0xe2, 0xd8, 0x0e, 0xd3, 0x26, 0xcb, 0xc7, 0xf0, 0xe7, 0x1a, 0x99, 0xf4, 0x3b,
		0xfb, 0x98, 0x8b, 0x9b, 0x7a, 0x02, 0xdd, 0x21,
	};
	static const uint8_t data[32] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	uint8_t key[32], out[40], bad[40];

	for (int i = 0; i < 32; i++) {
		key[i] = (uint8_t)i;
	}
	if (mc_aes256_kw_wrap(key, data, sizeof(data), out) != 0 || memcmp(out, expected, sizeof(expected)) != 0) {
		return 0;
	}
	if (mc_aes256_kw_unwrap(key, expected, sizeof(expected), out) != 0 || memcmp(out, data, sizeof(data)) != 0) {
		return 0;
	}
	memcpy(bad, expected, sizeof(bad));
	bad[20] ^= 1;
	return mc_aes256_kw_unwrap(key, bad, sizeof(bad), out) != 0;
}

static int
argonVector(void)
{
	/* RFC 9106 5.3: Argon2id, t=3, m=32 KiB, p=4, with a secret and associated data */
	static const uint8_t expected[32] = {
		0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c, 0x08, 0xc0, 0x37, 0xa3, 0x4a, 0x8b, 0x53, 0xc9,
		0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75, 0xb6, 0x5e, 0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59,
	};
	static uint64_t work[32 * MC_ARGON2_BLOCK / sizeof(uint64_t)];
	uint8_t password[32], salt[16], secret[8], ad[12], out[32];
	crypto_argon2_config config = { .algorithm = CRYPTO_ARGON2_ID, .nb_blocks = 32, .nb_passes = 3, .nb_lanes = 4 };
	crypto_argon2_inputs inputs = { .pass = password, .salt = salt, .pass_size = sizeof(password), .salt_size = sizeof(salt) };
	crypto_argon2_extras extras = { .key = secret, .ad = ad, .key_size = sizeof(secret), .ad_size = sizeof(ad) };

	memset(password, 1, sizeof(password));
	memset(salt, 2, sizeof(salt));
	memset(secret, 3, sizeof(secret));
	memset(ad, 4, sizeof(ad));
	crypto_argon2(out, sizeof(out), work, config, inputs, extras);
	return memcmp(out, expected, sizeof(out)) == 0;
}

int
mc_keywrap_selftest(void)
{
	return aesVector() && kwVector() && argonVector();
}
