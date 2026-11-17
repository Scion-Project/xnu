/*
 * Copyright (c) 2026 Scion Project. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * A test of decmpfs_lz4.c's decoder on a Mac (not part of the kernel build):
 *   cc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o /tmp/lz4t bsd/kern/decmpfs_lz4_test.c -lcompression
 *   /tmp/lz4t                 synthetic files: libcompression's LZ4 streams of chunks of known content, decoded and
 *                             compared; every truncation of a chunk and 20,000 corrupted ones must be refused or stay in
 *                             bounds (the sanitizers stop the run on any read or write outside them)
 *   /tmp/lz4t FILE...         real files of type 16 (on a volume macOS mounts): the fork decoded here against the file
 *                             as macOS reads it, byte for byte
 */
#define DECMPFS_LZ4_HOST_TEST 1
#include "decmpfs_lz4.c"

#include <compression.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)rng;
}

/* content of a given kind: 0 zeros, 1 random, 2 text-like, 3 a picture-like gradient with noise */
static void
fill(uint8_t *p, size_t n, int kind)
{
	for (size_t i = 0; i < n; i++) {
		switch (kind) {
		case 0: p[i] = 0; break;
		case 1: p[i] = (uint8_t)next(); break;
		case 2: p[i] = "the quick brown fox jumps over the lazy dog "[(i * 7 + (next() % 3 == 0)) % 44]; break;
		default: p[i] = (uint8_t)((i / 4) + ((next() & 7) == 0 ? next() : 0)); break;
		}
	}
}

/* A fork for `data` of `size` bytes as iOS lays it out: the table, then each chunk as libcompression's LZ4 stream. */
static uint8_t *
make_fork(const uint8_t *data, uint64_t size, size_t *fork_len)
{
	const uint64_t n = lz4c_chunks(size);
	size_t cap = 4 * (n + 1) + n * (LZ4C_MAX_IN + 64);
	uint8_t *fork = malloc(cap);
	size_t at = 4 * (n + 1);
	for (uint64_t c = 0; c < n; c++) {
		const size_t len = (size - c * LZ4C_CHUNK) < LZ4C_CHUNK ? (size_t)(size - c * LZ4C_CHUNK) : LZ4C_CHUNK;
		const uint32_t entry = (uint32_t)at;
		memcpy(fork + 4 * c, &entry, 4);
		size_t got = compression_encode_buffer(fork + at, LZ4C_MAX_IN, data + c * LZ4C_CHUNK, len, NULL, COMPRESSION_LZ4);
		if (got == 0) {
			printf("libcompression could not encode chunk %llu\n", (unsigned long long)c);
			exit(2);
		}
		at += got;
	}
	const uint32_t last = (uint32_t)at;
	memcpy(fork + 4 * n, &last, 4);
	*fork_len = at;
	return fork;
}

/* The whole file from its fork, as the kernel's fetch does chunk by chunk. 0 or the decoder's error. */
static int
decode_fork(const uint8_t *fork, size_t fork_len, uint64_t size, uint8_t *out)
{
	const uint64_t n = lz4c_chunks(size);
	if (size > LZ4C_MAX_SIZE || fork_len < 4 * (n + 1)) {
		return EINVAL;
	}
	int err = lz4c_table_check(fork, 0, n + 1, size, fork_len);
	if (err != 0) {
		return err;
	}
	for (uint64_t c = 0; c < n; c++) {
		const uint32_t from = lz4c_le32(fork + 4 * c), to = lz4c_le32(fork + 4 * (c + 1));
		const size_t want = (size - c * LZ4C_CHUNK) < LZ4C_CHUNK ? (size_t)(size - c * LZ4C_CHUNK) : LZ4C_CHUNK;
		err = lz4c_chunk(fork + from, to - from, out + c * LZ4C_CHUNK, want);
		if (err != 0) {
			return err;
		}
	}
	return 0;
}

static void
synthetic(void)
{
	static const uint64_t sizes[] = { 1, 100, 4096, 65535, 65536, 65537, 200000, 1427304 };
	unsigned kinds_seen[3] = { 0 };
	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		for (int kind = 0; kind < 4; kind++) {
			const uint64_t size = sizes[s];
			uint8_t *data = malloc(size), *out = malloc(size);
			fill(data, size, kind);
			size_t fork_len = 0;
			uint8_t *fork = make_fork(data, size, &fork_len);
			/* which block kinds libcompression made */
			for (uint64_t c = 0; c < lz4c_chunks(size); c++) {
				const uint32_t m = lz4c_le32(fork + lz4c_le32(fork + 4 * c));
				kinds_seen[m == LZ4C_BLOCK ? 0 : m == LZ4C_STORED ? 1 : 2]++;
			}
			memset(out, 0xa5, size);
			int err = decode_fork(fork, fork_len, size, out);
			CHECK(err == 0 && memcmp(out, data, size) == 0, "size %llu kind %d: err %d or content differs", (unsigned long long)size, kind, err);
			/* every truncation of the first chunk is refused */
			const uint32_t from = lz4c_le32(fork), to = lz4c_le32(fork + 4);
			const size_t want = size < LZ4C_CHUNK ? (size_t)size : LZ4C_CHUNK;
			uint8_t *scratch = malloc(LZ4C_CHUNK);
			for (uint32_t cut = 0; cut < to - from; cut++) {
				uint8_t *copy = malloc(cut ? cut : 1);
				memcpy(copy, fork + from, cut);
				CHECK(lz4c_chunk(copy, cut, scratch, want) != 0, "size %llu kind %d: a chunk cut to %u of %u bytes was taken",
				    (unsigned long long)size, kind, cut, to - from);
				free(copy);
			}
			/* a chunk with one more byte after its end is refused */
			uint8_t *longer = malloc(to - from + 1);
			memcpy(longer, fork + from, to - from);
			longer[to - from] = 0;
			CHECK(lz4c_chunk(longer, to - from + 1, scratch, want) != 0, "a chunk with a byte after its end was taken");
			free(longer);
			/* corrupted chunks: refused, or decoded within bounds (LZ4 carries no checksum) */
			for (int round = 0; round < 625; round++) {
				uint8_t *copy = malloc(to - from);
				memcpy(copy, fork + from, to - from);
				for (int k = 1 + (int)(next() % 4); k > 0; k--) {
					copy[next() % (to - from)] ^= (uint8_t)(1u << (next() % 8));
				}
				(void)lz4c_chunk(copy, to - from, scratch, want);
				free(copy);
			}
			/* a corrupted table is refused */
			if (lz4c_chunks(size) > 1) {
				uint8_t *bad = malloc(fork_len);
				memcpy(bad, fork, fork_len);
				uint32_t e1 = lz4c_le32(bad + 4) + 1;
				memcpy(bad + 4, &e1, 4);
				CHECK(decode_fork(bad, fork_len, size, out) != 0, "a table with a moved entry was taken");
				free(bad);
			}
			CHECK(decode_fork(fork, fork_len - 1, size, out) != 0, "a fork one byte short was taken");
			free(scratch);
			free(fork);
			free(data);
			free(out);
		}
	}
	/* hand-made blocks: a match before the start of the chunk, a zero offset, a stored block */
	uint8_t out[64];
	const uint8_t before[] = { 'b', 'v', '4', '1', 8, 0, 0, 0, 4, 0, 0, 0, 0x04, 0x05, 0x00, 0x00, 'b', 'v', '4', '$' };
	CHECK(lz4c_chunk(before, sizeof(before), out, 8) != 0, "a match reaching before the chunk was taken");
	const uint8_t stored[] = { 'b', 'v', '4', '-', 3, 0, 0, 0, 'a', 'b', 'c', 'b', 'v', '4', '$' };
	CHECK(lz4c_chunk(stored, sizeof(stored), out, 3) == 0 && memcmp(out, "abc", 3) == 0, "a stored block was not taken");
	const uint8_t unknown[] = { 'b', 'v', 'x', '2', 0, 0, 0, 0, 'b', 'v', '4', '$' };
	CHECK(lz4c_chunk(unknown, sizeof(unknown), out, 0) != 0, "a block of another kind was taken");
	printf("synthetic: libcompression made %u bv41 chunks, %u bv4- chunks, %u others\n", kinds_seen[0], kinds_seen[1], kinds_seen[2]);
}

static void
real(const char *path)
{
	uint8_t header[16];
	ssize_t h = getxattr(path, "com.apple.decmpfs", header, sizeof(header), 0, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION);
	if (h < 16 || lz4c_le32(header) != 0x636d7066 || lz4c_le32(header + 4) != LZ4C_TYPE) {
		printf("skip (not type 16): %s\n", path);
		return;
	}
	uint64_t size;
	memcpy(&size, header + 8, 8);
	ssize_t fork_len = getxattr(path, "com.apple.ResourceFork", NULL, 0, 0, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION);
	uint8_t *fork = malloc(fork_len > 0 ? (size_t)fork_len : 1);
	fork_len = getxattr(path, "com.apple.ResourceFork", fork, (size_t)fork_len, 0, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION);
	uint8_t *mine = malloc(size), *theirs = malloc(size);
	int err = decode_fork(fork, (size_t)fork_len, size, mine);
	int fd = open(path, O_RDONLY);
	ssize_t got = fd >= 0 ? read(fd, theirs, size) : -1;
	if (fd >= 0) {
		close(fd);
	}
	CHECK(err == 0 && got == (ssize_t)size && memcmp(mine, theirs, size) == 0, "real file (%llu bytes, fork %zd): err %d, macOS read %zd, %s",
	    (unsigned long long)size, fork_len, err, got, (err == 0 && got == (ssize_t)size) ? "content differs" : "");
	if (err == 0 && got == (ssize_t)size && memcmp(mine, theirs, size) == 0) {
		printf("ok: %llu bytes from a fork of %zd, same as macOS reads\n", (unsigned long long)size, fork_len);
	}
	free(fork);
	free(mine);
	free(theirs);
}

int
main(int argc, char **argv)
{
	if (argc == 1) {
		synthetic();
	}
	for (int i = 1; i < argc; i++) {
		real(argv[i]);
	}
	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
