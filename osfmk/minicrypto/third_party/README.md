# Third-party cryptography for the key store

Used by `../mc_keywrap.c` for `iokit/Kernel/MiniAppleKeyStore.cpp`. Do not edit these; replace them from upstream.

| Files | Upstream | Licence |
|---|---|---|
| `monocypher.c`, `monocypher.h` | [Monocypher](https://monocypher.org) 4.0.2, `src/`, git 0d85f98c9d9b, unchanged | CC0-1.0 or BSD-2-Clause (below) |
| `bearssl_aes_ct64.c`, `bearssl_aes_ct64_enc.c`, `bearssl_aes_ct64_dec.c` | [BearSSL](https://bearssl.org) `src/symcipher/aes_ct64*.c`, git 7bea48e5e850; only `#include "inner.h"` changed | MIT, in each file |
| `bearssl_inner.h` | BearSSL's `inner.h`, cut down to what those three use | MIT, in the file |

Monocypher's licence, in part (its `LICENCE.md`): "Monocypher as a whole is dual-licensed. Choose whichever licence you want from the two licences listed below": 2-clause BSD,
Copyright (c) 2017-2023 Loup Vaillant, Michael Savage, Fabio Scotoni; or CC0 1.0 Universal. This port takes it under CC0-1.0, as its own headers allow.
