/*
 * minicrypto: a minimal stand-in for Apple's corecrypto kext, enough for the
 * open-source kernel to boot without it. See mc_provider.c.
 */
#ifndef _MINICRYPTO_H_
#define _MINICRYPTO_H_

#include <corecrypto/ccdigest.h>

extern const struct ccdigest_info mc_md5_di;
extern const struct ccdigest_info mc_sha1_di;
extern const struct ccdigest_info mc_sha384_di;
extern const struct ccdigest_info mc_sha512_di;

void mc_digest(const struct ccdigest_info *di, size_t len, const void *data, void *digest);
const char *mc_digest_selftest(void);

/* AES (mc_aes.c) */
struct ccmode_ecb;
struct ccmode_cbc;
struct ccmode_ctr;
extern const struct ccmode_ecb mc_aes_ecb_encrypt, mc_aes_ecb_decrypt;
extern const struct ccmode_cbc mc_aes_cbc_encrypt, mc_aes_cbc_decrypt;
extern const struct ccmode_ctr mc_aes_ctr_crypt;
const char *mc_aes_selftest(void);

/* The ARMv8 crypto instructions (mc_ce.c). mc_ce_on: set once at boot, after their self-test */
extern int mc_ce_on, mc_ce_have;
void mc_ce_start(void);
void mc_ce_bench(char *text, size_t size);
const struct ccdigest_info *mc_ce_sha256_di(const struct ccdigest_info *plain);
void mc_ce_sha1_compress(uint32_t state[5], size_t nblocks, const void *in);
void mc_ce_aes_encrypt(const uint8_t *keys, uint32_t rounds, const uint8_t *in, uint8_t *out, size_t nblocks);
void mc_ce_aes_decrypt(const uint8_t *keys, uint32_t rounds, const uint8_t *in, uint8_t *out, size_t nblocks);
void mc_ce_aes_decrypt_schedule(uint8_t *decrypt_keys, const uint8_t *keys, uint32_t rounds);

#endif /* _MINICRYPTO_H_ */
