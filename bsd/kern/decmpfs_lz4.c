/*
 * Copyright (c) 2026 Scion Project. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * decmpfs compression type 16 for iOS 27 on the S9: files that iOS compressed in place, whose data is LZ4 in 64 KiB
 * chunks in the resource fork. On Apple's systems a closed kernel extension registers the decompressors of every type
 * but 1; this kernel registers type 1 only, so such files cannot be read here: iOS 27 writes its app snapshots this way
 * (SplashBoard's .ktx files, which the app switcher shows; scion-kernel docs/ios27/RENDERING.md).
 *
 * The layout, as read from the files themselves (not from a specification; every part is checked and anything else is
 * refused, never guessed at):
 *   - com.apple.decmpfs: the 16-byte header only (magic "cmpf", type 16, the uncompressed size);
 *   - the resource fork: a table of n + 1 little-endian 32-bit offsets into the fork, n = the uncompressed size in
 *     64 KiB chunks rounded up; the first entry is the table's own length, 4 (n + 1), the last the fork's length, and
 *     chunk i lies between entries i and i + 1;
 *   - a chunk: blocks of Apple's LZ4 framing, each a 4-byte magic and little-endian 32-bit sizes: "bv41" (the decoded
 *     size, the encoded size, then an LZ4 block), "bv4-" (the size, then that many bytes stored), and "bv4$" (the end,
 *     which must be the chunk's last 4 bytes); the chunk decodes to 64 KiB, the last one to what remains.
 * The LZ4 block itself is the public LZ4 block format (a token, literals, a 2-byte offset, a match length), decoded
 * here from that description with every length and offset checked against the input and the chunk's output.
 *
 * Registered only with the boot argument decmpfs_lz4=1 (off by default: without it nothing here runs). Tested on a Mac
 * against libcompression's own LZ4 streams and against real files of this type, under the address and undefined
 * behaviour sanitizers (bsd/kern/decmpfs_lz4_test.c); not run on a phone.
 */

#ifdef DECMPFS_LZ4_HOST_TEST
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#else
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/namei.h>
#include <sys/xattr.h>
#include <sys/uio_internal.h>
#include <sys/decmpfs.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#include <libkern/OSAtomic.h>
#endif

#define LZ4C_TYPE        16
#define LZ4C_CHUNK       65536u
/* the largest file taken: its table is then at most 64 KiB */
#define LZ4C_MAX_SIZE    (1ull << 30)
/* the largest chunk taken: 64 KiB stored, or LZ4's worst case for 64 KiB, with the framing, is well under this */
#define LZ4C_MAX_IN      (2 * LZ4C_CHUNK)

#define LZ4C_MAGIC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define LZ4C_BLOCK       LZ4C_MAGIC('b', 'v', '4', '1')
#define LZ4C_STORED      LZ4C_MAGIC('b', 'v', '4', '-')
#define LZ4C_END         LZ4C_MAGIC('b', 'v', '4', '$')

static inline uint32_t
lz4c_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The number of 64 KiB chunks of a file of `size` bytes. */
static inline uint64_t
lz4c_chunks(uint64_t size)
{
	return (size + LZ4C_CHUNK - 1) / LZ4C_CHUNK;
}

/*
 * One LZ4 block of src_len bytes, decoded into exactly dst_len bytes at dst; a match may reach back to `base` (the
 * start of the chunk's output, at or before dst). 0, or EINVAL for anything that does not fit exactly.
 */
static int
lz4c_block(const uint8_t *src, size_t src_len, const uint8_t *base, uint8_t *dst, size_t dst_len)
{
	const uint8_t *ip = src;
	const uint8_t *const iend = src + src_len;
	uint8_t *op = dst;
	uint8_t *const oend = dst + dst_len;

	for (;;) {
		if (ip == iend && op == oend) {
			break;
		}
		if (ip >= iend) {
			return EINVAL;
		}
		const unsigned token = *ip++;
		size_t literals = token >> 4;
		if (literals == 15) {
			unsigned more;
			do {
				if (ip >= iend) {
					return EINVAL;
				}
				more = *ip++;
				literals += more;
				if (literals > dst_len) {
					return EINVAL;
				}
			} while (more == 255);
		}
		if (literals > (size_t)(iend - ip) || literals > (size_t)(oend - op)) {
			return EINVAL;
		}
		memcpy(op, ip, literals);
		op += literals;
		ip += literals;
		if (ip == iend) {
			/* the last sequence: literals only */
			break;
		}
		if (iend - ip < 2) {
			return EINVAL;
		}
		const size_t offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
		ip += 2;
		if (offset == 0 || offset > (size_t)(op - base)) {
			return EINVAL;
		}
		size_t match = token & 15;
		if (match == 15) {
			unsigned more;
			do {
				if (ip >= iend) {
					return EINVAL;
				}
				more = *ip++;
				match += more;
				if (match > dst_len) {
					return EINVAL;
				}
			} while (more == 255);
		}
		match += 4;
		if (match > (size_t)(oend - op)) {
			return EINVAL;
		}
		/* byte by byte: the copy may overlap what it writes (offset < match) */
		const uint8_t *from = op - offset;
		for (size_t i = 0; i < match; i++) {
			op[i] = from[i];
		}
		op += match;
	}
	return op == oend ? 0 : EINVAL;
}

/* One chunk of src_len bytes, decoded into exactly `want` bytes at dst. 0 or EINVAL. */
static int
lz4c_chunk(const uint8_t *src, size_t src_len, uint8_t *dst, size_t want)
{
	const uint8_t *ip = src;
	const uint8_t *const iend = src + src_len;
	size_t done = 0;

	for (;;) {
		if (iend - ip < 4) {
			return EINVAL;
		}
		const uint32_t magic = lz4c_le32(ip);
		if (magic == LZ4C_END) {
			ip += 4;
			break;
		}
		if (magic == LZ4C_BLOCK) {
			if (iend - ip < 12) {
				return EINVAL;
			}
			const uint32_t decoded = lz4c_le32(ip + 4), encoded = lz4c_le32(ip + 8);
			ip += 12;
			if (decoded > want - done || encoded > (size_t)(iend - ip)) {
				return EINVAL;
			}
			int err = lz4c_block(ip, encoded, dst, dst + done, decoded);
			if (err != 0) {
				return err;
			}
			ip += encoded;
			done += decoded;
		} else if (magic == LZ4C_STORED) {
			if (iend - ip < 8) {
				return EINVAL;
			}
			const uint32_t stored = lz4c_le32(ip + 4);
			ip += 8;
			if (stored > want - done || stored > (size_t)(iend - ip)) {
				return EINVAL;
			}
			memcpy(dst + done, ip, stored);
			ip += stored;
			done += stored;
		} else {
			return EINVAL;
		}
	}
	return (done == want && ip == iend) ? 0 : EINVAL;
}

/*
 * Entries [first, first + count) of a file's table, given as `table` (little-endian words), against the file's size
 * and its fork's length: entry 0 the table's length, each chunk between 4 bytes and LZ4C_MAX_IN, the last entry the
 * fork's length. 0 or EINVAL.
 */
static int
lz4c_table_check(const uint8_t *table, uint64_t first, uint64_t count, uint64_t size, uint64_t fork)
{
	const uint64_t n = lz4c_chunks(size);
	if (size > LZ4C_MAX_SIZE || count == 0 || first + count > n + 1) {
		return EINVAL;
	}
	uint64_t previous = 0;
	for (uint64_t i = 0; i < count; i++) {
		const uint64_t entry = lz4c_le32(table + 4 * i), at = first + i;
		if (at == 0 && entry != 4 * (n + 1)) {
			return EINVAL;
		}
		if (at == n && entry != fork) {
			return EINVAL;
		}
		if (entry < 4 * (n + 1) || entry > fork) {
			return EINVAL;
		}
		if (i > 0 && (entry < previous + 4 || entry - previous > LZ4C_MAX_IN)) {
			return EINVAL;
		}
		previous = entry;
	}
	return 0;
}

#ifndef DECMPFS_LZ4_HOST_TEST

#if FS_COMPRESSION

static SInt32 lz4c_refusals;

static void
lz4c_refused(vnode_t vp, const char *what, uint64_t value, int err)
{
	if (OSIncrementAtomic(&lz4c_refusals) >= 16) {
		return;
	}
	char *path = zalloc(ZV_NAMEI);
	int len = MAXPATHLEN;
	path[0] = 0;
	if (vn_getpath(vp, path, &len) != 0) {
		strlcpy(path, "(no path)", MAXPATHLEN);
	}
	printf("decmpfs lz4: %s %llu refused (%d): %s\n", what, value, err, path);
	zfree(ZV_NAMEI, path);
}

/* The fork's length, or 0 (with an error) when it has none. */
static int
lz4c_fork_length(vnode_t vp, uint64_t *length)
{
	size_t size = 0;
	int err = vn_getxattr(vp, XATTR_RESOURCEFORK_NAME, NULL, &size, XATTR_NOSECURITY, decmpfs_ctx);
	*length = size;
	return err;
}

/* `len` bytes of the fork at `offset`, all of them or an error. */
static int
lz4c_fork_read(vnode_t vp, uint64_t offset, void *buf, size_t len)
{
	char uio_buf[UIO_SIZEOF(1)];
	uio_t uio = uio_createwithbuffer(1, (off_t)offset, UIO_SYSSPACE, UIO_READ, &uio_buf[0], sizeof(uio_buf));
	uio_addiov(uio, CAST_USER_ADDR_T(buf), len);
	size_t got = len;
	int err = vn_getxattr(vp, XATTR_RESOURCEFORK_NAME, uio, &got, XATTR_NOSECURITY, decmpfs_ctx);
	if (err == 0 && uio_resid(uio) != 0) {
		err = EIO;
	}
	return err;
}

/* At open: the header and the whole table. */
static int
lz4c_validate(vnode_t vp, __unused vfs_context_t ctx, decmpfs_header *hdr)
{
	const uint64_t size = hdr->uncompressed_size, n = lz4c_chunks(size);
	uint64_t fork = 0;
	if (size > LZ4C_MAX_SIZE) {
		lz4c_refused(vp, "a file of", size, EFBIG);
		return EINVAL;
	}
	int err = lz4c_fork_length(vp, &fork);
	if (err != 0) {
		return err;
	}
	const size_t table_len = (size_t)(4 * (n + 1));
	if (fork < table_len) {
		lz4c_refused(vp, "a fork of", fork, EINVAL);
		return EINVAL;
	}
	uint8_t *table = kalloc_data(table_len, Z_WAITOK);
	if (table == NULL) {
		return ENOMEM;
	}
	err = lz4c_fork_read(vp, 0, table, table_len);
	if (err == 0) {
		err = lz4c_table_check(table, 0, n + 1, size, fork);
		if (err != 0) {
			lz4c_refused(vp, "the table of a file of", size, err);
		}
	}
	kfree_data(table, table_len);
	return err;
}

/* Copy src[0, len) into the vectors from (*index, *at) on. */
static void
lz4c_out(const uint8_t *src, size_t len, int nvec, decmpfs_vector *vec, int *index, user_ssize_t *at)
{
	while (len > 0 && *index < nvec) {
		const user_ssize_t room = vec[*index].size - *at;
		if (room <= 0) {
			(*index)++;
			*at = 0;
			continue;
		}
		const size_t n = (size_t)room < len ? (size_t)room : len;
		memcpy((uint8_t *)vec[*index].buf + *at, src, n);
		src += n;
		len -= n;
		*at += n;
	}
}

static int
lz4c_fetch(vnode_t vp, __unused vfs_context_t ctx, decmpfs_header *hdr, off_t offset, user_ssize_t size, int nvec,
    decmpfs_vector *vec, uint64_t *bytes_read)
{
	const uint64_t total = hdr->uncompressed_size;
	uint64_t copied = 0, fork = 0;
	uint8_t *table = NULL, *in = NULL, *out = NULL;
	size_t table_len = 0;
	int err = 0;

	if (offset < 0 || size < 0 || total > LZ4C_MAX_SIZE) {
		err = EINVAL;
		goto done;
	}
	if ((uint64_t)offset >= total || size == 0) {
		goto done;
	}
	const uint64_t end = ((uint64_t)offset + (uint64_t)size < total) ? (uint64_t)offset + (uint64_t)size : total;
	const uint64_t c0 = (uint64_t)offset / LZ4C_CHUNK, c1 = (end - 1) / LZ4C_CHUNK;
	err = lz4c_fork_length(vp, &fork);
	if (err != 0) {
		goto done;
	}
	/* the entries for chunks c0 to c1 and the one after, checked again (the fork is read afresh) */
	table_len = (size_t)(4 * (c1 - c0 + 2));
	table = kalloc_data(table_len, Z_WAITOK);
	in = kalloc_data(LZ4C_MAX_IN, Z_WAITOK);
	out = kalloc_data(LZ4C_CHUNK, Z_WAITOK);
	if (table == NULL || in == NULL || out == NULL) {
		err = ENOMEM;
		goto done;
	}
	err = lz4c_fork_read(vp, 4 * c0, table, table_len);
	if (err == 0) {
		err = lz4c_table_check(table, c0, c1 - c0 + 2, total, fork);
	}
	if (err != 0) {
		lz4c_refused(vp, "the table at chunk", c0, err);
		goto done;
	}
	int index = 0;
	user_ssize_t at = 0;
	for (uint64_t c = c0; c <= c1; c++) {
		const uint32_t from = lz4c_le32(table + 4 * (c - c0)), to = lz4c_le32(table + 4 * (c - c0 + 1));
		const uint64_t start = c * LZ4C_CHUNK;
		const size_t want = (size_t)((total - start) < LZ4C_CHUNK ? (total - start) : LZ4C_CHUNK);
		err = lz4c_fork_read(vp, from, in, to - from);
		if (err == 0) {
			err = lz4c_chunk(in, to - from, out, want);
		}
		if (err != 0) {
			lz4c_refused(vp, "chunk", c, err);
			goto done;
		}
		const uint64_t a = (uint64_t)offset > start ? (uint64_t)offset : start;
		const uint64_t b = end < start + want ? end : start + want;
		lz4c_out(out + (a - start), (size_t)(b - a), nvec, vec, &index, &at);
		copied += b - a;
	}
done:
	if (table != NULL) {
		kfree_data(table, table_len);
	}
	if (in != NULL) {
		kfree_data(in, LZ4C_MAX_IN);
	}
	if (out != NULL) {
		kfree_data(out, LZ4C_CHUNK);
	}
	if (err == 0 && bytes_read != NULL) {
		*bytes_read = copied;
	}
	return err;
}

/* When the file is made uncompressed: its fork, which held the compressed data, goes too. */
static int
lz4c_free(vnode_t vp, __unused vfs_context_t ctx, __unused decmpfs_header *hdr)
{
	int err = vn_removexattr(vp, XATTR_RESOURCEFORK_NAME, 0, decmpfs_ctx);
	return err == ENOATTR ? 0 : err;
}

SECURITY_READ_ONLY_EARLY(static decmpfs_registration) LZ4ChunkReg =
{
	.decmpfs_registration = DECMPFS_REGISTRATION_VERSION,
	.validate          = lz4c_validate,
	.adjust_fetch      = NULL,
	.fetch             = lz4c_fetch,
	.free_data         = lz4c_free,
	.get_flags         = NULL
};

void decmpfs_lz4_init(void);

/* From decmpfs_init(): registers type 16 with the boot argument decmpfs_lz4=1, else nothing. */
void
decmpfs_lz4_init(void)
{
	int on = 0;
	if (!PE_parse_boot_argn("decmpfs_lz4", &on, sizeof(on)) || on == 0) {
		return;
	}
	int err = register_decmpfs_decompressor(LZ4C_TYPE, &LZ4ChunkReg);
	printf("decmpfs: type %d (LZ4 in 64 KiB chunks in the resource fork) %s (decmpfs_lz4=1)\n", LZ4C_TYPE,
	    err == 0 ? "registered" : "NOT registered");
}

#endif /* FS_COMPRESSION */

#endif /* DECMPFS_LZ4_HOST_TEST */
