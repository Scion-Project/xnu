/*
 * minicrypto: AES, SHA-1 and SHA-256 by the ARMv8 Cryptographic Extension, where the processor has it.
 *
 * Every phone this kernel runs on has these instructions, and iOS's own user-space corecrypto uses them. The kernel, without
 * Apple's corecrypto kext, did not: AES byte by byte in C, SHA-1 and SHA-256 in C. On the S9 isa_floor does not take them away:
 * its floor for ID_AA64ISAR0_EL1 keeps AES+PMULL, SHA1, SHA2 and CRC32 (patch 1510), and both the A55 and the Exynos M3 have
 * them (the S9's Linux /proc/cpuinfo: fp asimd evtstrm aes pmull sha1 sha2 crc32). What it is for: the code signature of every
 * page of a signed program brought in (ubc_subr.c, through g_crypto_funcs' SHA-256 or SHA-1), IOAESAccelerator (AES-CBC), and
 * being constant-time where the C is not.
 *
 * The ID register is read on the boot processor; the cores of one phone agree on these fields. The instructions are checked
 * against the same known answers as the C (mc_digest_selftest, mc_aes_selftest) before anything uses them: any wrong answer and
 * the C stays, with a line in the log. Boot-arg mc_ce=0 stays with the C. No processor without them ever executes one.
 *
 * kern.hakios.crypto_bench (drivers/core/hakios.cryptobench.c): reading it measures SHA-256, SHA-1 and AES-128-CBC over a 4 KB page, both ways, on whichever
 * core it runs, and answers in MB/s.
 *
 * The vector registers are all saved and put back by the assembly (mc_ce_asm.h): the rest of the kernel does not use them
 * and takes no care of them, and they hold user space's until the next switch. Written from Arm's Architecture Reference Manual
 * and FIPS 180-4 / 197; tested against CommonCrypto on an ARM Mac over 200 random messages and keys (all three key sizes,
 * in place too), and at each boot against the vectors.
 */
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccdigest_priv.h>
#include <corecrypto/ccsha2.h>
#include <corecrypto/ccsha1.h>
#include <corecrypto/ccmode.h>
#include <libkern/crypto/aes.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdint.h>
#include <kern/kern_types.h>
#include <kern/misc_protos.h>

#include "minicrypto.h"

int mc_ce_on;   /* the instructions are in use */
int mc_ce_have; /* they are here and right: keys for decrypting get AESD's schedule */

#if defined(__arm64__)

extern const uint32_t ccsha256_K[64];

void mc_ce_sha256_compress(uint32_t state[8], size_t nblocks, const void *in, const uint32_t k[64]);
void mc_ce_aes_imc(uint8_t block[16]);

__asm__(".arch_extension crypto\n"
#include "mc_ce_asm.h"
);

static void
sha256_compress(ccdigest_state_t state, size_t nblocks, const void *in)
{
	mc_ce_sha256_compress(ccdigest_u32(state), nblocks, in, ccsha256_K);
}

static struct ccdigest_info sha256_di;

const struct ccdigest_info *
mc_ce_sha256_di(const struct ccdigest_info *plain)
{
	if (!mc_ce_on) {
		return plain;
	}
	sha256_di = *plain;
	sha256_di.compress = sha256_compress;
	return &sha256_di;
}

/* decrypt_keys and keys do not overlap */
void
mc_ce_aes_decrypt_schedule(uint8_t *decrypt_keys, const uint8_t *keys, uint32_t rounds)
{
	memcpy(decrypt_keys, keys + 16 * rounds, 16);
	for (uint32_t i = 1; i < rounds; i++) {
		memcpy(decrypt_keys + 16 * i, keys + 16 * (rounds - i), 16);
		mc_ce_aes_imc(decrypt_keys + 16 * i);
	}
	memcpy(decrypt_keys + 16 * rounds, keys, 16);
}

void
mc_ce_start(void)
{
	uint64_t isar0 = __builtin_arm_rsr64("ID_AA64ISAR0_EL1");
	uint32_t wanted = 1;
	const char *failed;

	PE_parse_boot_argn("mc_ce", &wanted, sizeof(wanted));
	if (!wanted || ((isar0 >> 4) & 0xf) == 0 || ((isar0 >> 8) & 0xf) == 0 || ((isar0 >> 12) & 0xf) == 0) {
		printf("minicrypto: the ARMv8 crypto instructions not used (mc_ce=%u, ISAR0 %#llx)\n", wanted, isar0);
		return;
	}
	mc_ce_have = mc_ce_on = 1;
	/* what is tested is what will run: mc_digest_selftest takes SHA-1 here, and SHA-256 below */
	failed = mc_digest_selftest();
	if (failed == NULL) {
		failed = mc_aes_selftest();
	}
	if (failed == NULL) {
		static const uint8_t abc[64] = { 'a', 'b', 'c', 0x80, [63] = 24 };
		static const uint8_t answer[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae,
			                            0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2,
			                            0x00, 0x15, 0xad };
		uint32_t s[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

		mc_ce_sha256_compress(s, 1, abc, ccsha256_K);
		for (int i = 0; i < 8; i++) {
			if (s[i] != ((uint32_t)answer[4 * i] << 24 | (uint32_t)answer[4 * i + 1] << 16 | (uint32_t)answer[4 * i + 2] << 8 | answer[4 * i + 3])) {
				failed = "sha-256";
			}
		}
	}
	if (failed != NULL) {
		mc_ce_have = mc_ce_on = 0;
		printf("minicrypto: the ARMv8 crypto instructions gave a wrong %s: the C stays\n", failed);
	}
}

/*
 * The answer of kern.hakios.crypto_bench (drivers/core/hakios.cryptobench.c). While the C is measured mc_ce_on is off for
 * everybody for a moment: harmless, both ways give the same answers, and what decrypts goes by its own key (mc_aes.c), not by this.
 */
static uint32_t
mb_per_s(uint32_t pages, uint64_t begun, mach_timebase_info_data_t base)
{
	uint64_t ns = (mach_absolute_time() - begun) * base.numer / base.denom;

	return ns == 0 ? 0 : (uint32_t)((uint64_t)pages * 4096 * 1000 / ns);
}

#define BENCH_WHILE(pages, begun, base, work) do {                                                       \
	(begun) = mach_absolute_time();                                                                  \
	(pages) = 0;                                                                                     \
	do {                                                                                             \
	        for (int bench_i = 0; bench_i < 16; bench_i++, (pages)++) {                              \
	                work;                                                                            \
	        }                                                                                        \
	} while ((mach_absolute_time() - (begun)) * (base).numer / (base).denom < 100000000ULL);         \
} while (0)

static uint32_t
rate(const struct ccdigest_info *di, const uint8_t *page, mach_timebase_info_data_t base)
{
	uint32_t state[64] = { 0 }, pages;
	uint64_t begun;

	BENCH_WHILE(pages, begun, base, di->compress((ccdigest_state_t)(void *)state, 4096 / di->block_size, page));
	return mb_per_s(pages, begun, base);
}

static uint32_t
rate_aes(const uint8_t *page, uint8_t *out, mach_timebase_info_data_t base)
{
	const struct ccmode_cbc *cbc = &mc_aes_cbc_encrypt;
	static const uint8_t key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	uint8_t ctx[1024] __attribute__((aligned(16)));
	uint8_t iv[16] = { 0 };
	uint32_t pages;
	uint64_t begun;

	cbc->init(cbc, (cccbc_ctx *)(void *)ctx, sizeof(key), key);
	BENCH_WHILE(pages, begun, base, cbc->cbc((cccbc_ctx *)(void *)ctx, (cccbc_iv *)(void *)iv, 4096 / 16, page, out));
	return mb_per_s(pages, begun, base);
}

extern const struct ccdigest_info ccsha256_ltc_di;

void
mc_ce_bench(char *text, size_t size)
{
	static uint8_t page[4096], out[4096];
	mach_timebase_info_data_t base;
	uint32_t ce_sha256 = 0, ce_sha1 = 0, ce_aes = 0, c_sha256, c_sha1, c_aes;
	int on = mc_ce_on;

	clock_timebase_info(&base);
	if (on) {
		ce_sha256 = rate(&sha256_di, page, base);
		ce_sha1 = rate(&mc_sha1_di, page, base);
		ce_aes = rate_aes(page, out, base);
	}
	mc_ce_on = 0;
	c_sha256 = rate(&ccsha256_ltc_di, page, base);
	c_sha1 = rate(&mc_sha1_di, page, base);
	c_aes = rate_aes(page, out, base);
	mc_ce_on = on;
	snprintf(text, size, "MB/s: sha256 %u (C %u), sha1 %u (C %u), aes128-cbc-enc %u (C %u); instructions %s",
	    ce_sha256, c_sha256, ce_sha1, c_sha1, ce_aes, c_aes, on ? "on" : "off");
}

#else /* no such instructions */

void
mc_ce_start(void)
{
}

void
mc_ce_bench(char *text, size_t size)
{
	snprintf(text, size, "no ARMv8 crypto instructions on this architecture");
}

const struct ccdigest_info *
mc_ce_sha256_di(const struct ccdigest_info *plain)
{
	return plain;
}

void
mc_ce_sha1_compress(uint32_t state[5] __unused, size_t nblocks __unused, const void *in __unused)
{
}

void
mc_ce_aes_encrypt(const uint8_t *keys __unused, uint32_t rounds __unused, const uint8_t *in __unused, uint8_t *out __unused, size_t nblocks __unused)
{
}

void
mc_ce_aes_decrypt(const uint8_t *keys __unused, uint32_t rounds __unused, const uint8_t *in __unused, uint8_t *out __unused, size_t nblocks __unused)
{
}

void
mc_ce_aes_decrypt_schedule(uint8_t *decrypt_keys __unused, const uint8_t *keys __unused, uint32_t rounds __unused)
{
}
#endif
