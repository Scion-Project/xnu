/*
 * The key store's cryptography (mc_keywrap.c): AES-256 key wrap and Argon2id. See that file.
 */
#ifndef _MINICRYPTO_MC_KEYWRAP_H_
#define _MINICRYPTO_MC_KEYWRAP_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

#define MC_KW_OVERHEAD          8               /* RFC 3394's integrity block */
#define MC_KW_MAX_KEY           64
#define MC_ARGON2_BLOCK         1024            /* the work area is this many bytes per KiB of memory cost */

/* key is the 32-byte key-encrypting key; n a multiple of 8 between 16 and MC_KW_MAX_KEY. 0, or -1 with out wiped. */
int mc_aes256_kw_wrap(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out);
int mc_aes256_kw_unwrap(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out);

/* Argon2id version 1.3, one lane, a 32-byte result. work is memKiB KiB, 8-byte aligned, and is wiped by the call. */
void mc_argon2id(uint8_t out[32], void *work, uint32_t memKiB, uint32_t passes, const uint8_t *password, size_t passwordLength,
    const uint8_t *salt, size_t saltLength);

/* A wipe the compiler cannot remove. */
void mc_wipe(void *p, size_t n);

/* FIPS 197 C.3 (both ways), RFC 3394 4.6 (both ways, and a damaged input refused), RFC 9106 5.3. True if all pass. */
int mc_keywrap_selftest(void);

__END_DECLS

#endif
