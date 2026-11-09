/*
 * Darwin-on-hakios: a file of an ext4 file system, read only, by its path. For the drivers that load the user's own
 * firmware from the phone's own partitions at run time (the Galaxy S9's VENDOR: the BCM4361's Wi-Fi firmware, NVRAM and
 * CLM, its Bluetooth patch), so that nothing of the vendor's is put into an image or a repository.
 *
 * What it reads, from the ext4 documentation (the kernel.org "ext4 Data Structures and Algorithms" pages) and nothing
 * else: the superblock at byte 1024; the group descriptors (32 or 64 bytes) in the block after it; an inode by its group
 * and index; a file's blocks through its extent tree (any depth; an extent not yet written reads as zeroes) or, without
 * extents, through ext2/3's block map (12 direct, single, double and triple indirect; a hole reads as zeroes); a
 * directory's entries read in order, block by block (an htree directory is also a list of ordinary blocks, its index
 * blocks looking like one empty entry, so no hash is needed); a symbolic link, short (in the inode) or long (in a block),
 * relative or, when it begins with the mount point given (the VENDOR partition is mounted at /vendor), absolute;
 * data in the inode (inline_data) when the whole file or directory fits there (60 bytes).
 *
 * Nothing is written, nothing is trusted: every number read is checked against the file system's size before it is used,
 * and a feature it does not know how to read (meta_bg, compression, a journal device; an encrypted file) is refused,
 * not guessed at. Checksums are not checked (metadata_csum is read as if absent): this is a reader of a partition Android
 * itself mounts read only.
 *
 * Nothing but C here, so that tools/ext4-test.py compiles it on the machine the kernel is built on and reads file
 * systems made by mke2fs with it. The kernel's side (drivers/storage/hakios.ext4.c) gives it a way to read the partition
 * and to allocate.
 *
 * Use: struct hakios_ext4 fs; hakios_ext4_open(&fs, read, context, mount, alloc, free) (0 or an errno);
 * hakios_ext4_file(&fs, "/etc/wifi/bcmdhd_sta.bin_b2", max, &data, &size) gives the file in memory from alloc (the
 * caller frees it with free(data, size)), or an errno: ENOENT not there, EFBIG larger than max, EFTYPE not a file,
 * ENOTSUP a feature not read, EIO a read or a structure that is not right, ELOOP too many links.
 */
#ifndef _SYS_HAKIOS_EXT4_H_
#define _SYS_HAKIOS_EXT4_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef EFTYPE
#define EFTYPE 79
#endif

#define HAKIOS_EXT4_MAGIC               0xEF53
/*
 * incompat features read (FILETYPE, RECOVER, EXTENTS, 64BIT, MMP, FLEX_BG, CSUM_SEED, LARGEDIR, INLINE_DATA, ENCRYPT).
 * ENCRYPT only says that some files and directories may be encrypted (Samsung sets it on the S9's EFS); those, the
 * ones whose inode has EXT4_ENCRYPT_FL, are refused, and the rest read as ever.
 */
#define HAKIOS_EXT4_INCOMPAT_READ       (0x0002u | 0x0004u | 0x0040u | 0x0080u | 0x0100u | 0x0200u | 0x2000u | 0x4000u | \
	                                 0x8000u | 0x10000u)
#define HAKIOS_EXT4_INCOMPAT_FILETYPE   0x0002u
#define HAKIOS_EXT4_INCOMPAT_RECOVER    0x0004u
#define HAKIOS_EXT4_INCOMPAT_64BIT      0x0080u
#define HAKIOS_EXT4_ENCRYPT_FL          0x00000800u
#define HAKIOS_EXT4_EXTENTS_FL          0x00080000u
#define HAKIOS_EXT4_INLINE_DATA_FL      0x10000000u
#define HAKIOS_EXT4_EXTENT_MAGIC        0xF30A
#define HAKIOS_EXT4_ROOT                2
#define HAKIOS_EXT4_LINKS               8               /* symbolic links followed in one lookup */
#define HAKIOS_EXT4_CHUNK               (1u << 20)      /* the most one read asks for */

typedef int (*hakios_ext4_read_t)(void *context, uint64_t offset, void *dst, uint64_t length);
typedef void *(*hakios_ext4_alloc_t)(uint64_t size);
typedef void (*hakios_ext4_free_t)(void *p, uint64_t size);

struct hakios_ext4 {
	hakios_ext4_read_t read;
	void *context;
	hakios_ext4_alloc_t alloc;
	hakios_ext4_free_t free;
	const char *mount;              /* where the partition is mounted ("/vendor"), for absolute links; or NULL */
	uint64_t blocks;                /* the file system's size in blocks */
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t inodes;
	uint32_t inodes_per_group;
	uint32_t groups;
	uint32_t desc_size;
	uint32_t first_data_block;
	uint32_t incompat;
	uint8_t *block;                 /* one block, for directories and the block map's indirect blocks */
	uint8_t *block2;                /* and one more, for the extent tree's inner nodes */
};

struct hakios_ext4_inode {
	uint32_t number;
	uint16_t mode;
	uint32_t flags;
	uint64_t size;
	uint8_t i_block[60];
};

static inline uint16_t
hakios_ext4_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t
hakios_ext4_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int
hakios_ext4_read_block(struct hakios_ext4 *fs, uint64_t block, uint8_t *dst)
{
	if (block >= fs->blocks) {
		return 5;       /* EIO */
	}
	return fs->read(fs->context, block * fs->block_size, dst, fs->block_size) == 0 ? 0 : 5;
}

static inline void
hakios_ext4_close(struct hakios_ext4 *fs)
{
	if (fs->block != NULL) {
		fs->free(fs->block, fs->block_size);
		fs->block = NULL;
	}
	if (fs->block2 != NULL) {
		fs->free(fs->block2, fs->block_size);
		fs->block2 = NULL;
	}
}

static inline int
hakios_ext4_open(struct hakios_ext4 *fs, hakios_ext4_read_t read, void *context, const char *mount,
    hakios_ext4_alloc_t alloc, hakios_ext4_free_t free_)
{
	uint8_t sb[1024];
	uint32_t log, blocks_per_group;

	memset(fs, 0, sizeof(*fs));
	fs->read = read;
	fs->context = context;
	fs->mount = mount;
	fs->alloc = alloc;
	fs->free = free_;
	if (read(context, 1024, sb, sizeof(sb)) != 0) {
		return 5;
	}
	if (hakios_ext4_le16(sb + 56) != HAKIOS_EXT4_MAGIC) {
		return 79;      /* EFTYPE: not ext2/3/4 */
	}
	log = hakios_ext4_le32(sb + 24);
	if (log > 6) {          /* 1 KB to 64 KB */
		return 5;
	}
	fs->block_size = 1024u << log;
	fs->incompat = hakios_ext4_le32(sb + 96);
	if (fs->incompat & ~HAKIOS_EXT4_INCOMPAT_READ) {
		return 45;      /* ENOTSUP */
	}
	fs->blocks = hakios_ext4_le32(sb + 4);
	if (fs->incompat & HAKIOS_EXT4_INCOMPAT_64BIT) {
		fs->blocks |= (uint64_t)hakios_ext4_le32(sb + 0x150) << 32;
		fs->desc_size = hakios_ext4_le16(sb + 254);
		if (fs->desc_size < 32 || fs->desc_size > 1024 || (fs->desc_size & (fs->desc_size - 1)) != 0) {
			return 5;
		}
	} else {
		fs->desc_size = 32;
	}
	fs->inodes = hakios_ext4_le32(sb + 0);
	fs->first_data_block = hakios_ext4_le32(sb + 20);
	blocks_per_group = hakios_ext4_le32(sb + 32);
	fs->inodes_per_group = hakios_ext4_le32(sb + 40);
	fs->inode_size = hakios_ext4_le32(sb + 76) == 0 ? 128 : hakios_ext4_le16(sb + 88);
	if (fs->blocks == 0 || blocks_per_group == 0 || fs->inodes_per_group == 0 || fs->inode_size < 128 ||
	    fs->inode_size > fs->block_size || (fs->inode_size & (fs->inode_size - 1)) != 0 ||
	    fs->first_data_block >= fs->blocks) {
		return 5;
	}
	fs->groups = (uint32_t)((fs->blocks - fs->first_data_block + blocks_per_group - 1) / blocks_per_group);
	if ((uint64_t)fs->groups * fs->inodes_per_group < fs->inodes) {
		return 5;
	}
	fs->block = (uint8_t *)alloc(fs->block_size);
	fs->block2 = (uint8_t *)alloc(fs->block_size);
	if (fs->block == NULL || fs->block2 == NULL) {
		hakios_ext4_close(fs);
		return 12;      /* ENOMEM */
	}
	return 0;
}

/* inode `number` (1 is the first) */
static inline int
hakios_ext4_inode(struct hakios_ext4 *fs, uint32_t number, struct hakios_ext4_inode *inode)
{
	uint32_t group, index;
	uint64_t table, where;
	uint8_t desc[64], raw[128];

	if (number == 0 || number > fs->inodes) {
		return 5;
	}
	group = (number - 1) / fs->inodes_per_group;
	index = (number - 1) % fs->inodes_per_group;
	if (group >= fs->groups) {
		return 5;
	}
	/* the descriptors begin in the block after the superblock's (block 2 for 1 KB blocks, else block 1) */
	where = (uint64_t)(fs->first_data_block + 1) * fs->block_size + (uint64_t)group * fs->desc_size;
	if (fs->read(fs->context, where, desc, fs->desc_size >= 64 ? 64 : 32) != 0) {
		return 5;
	}
	table = hakios_ext4_le32(desc + 8);
	if (fs->desc_size >= 64) {
		table |= (uint64_t)hakios_ext4_le32(desc + 0x28) << 32;
	}
	if (table == 0 || table >= fs->blocks) {
		return 5;
	}
	if (fs->read(fs->context, table * fs->block_size + (uint64_t)index * fs->inode_size, raw, sizeof(raw)) != 0) {
		return 5;
	}
	inode->number = number;
	inode->mode = hakios_ext4_le16(raw + 0);
	inode->flags = hakios_ext4_le32(raw + 32);
	if (inode->flags & HAKIOS_EXT4_ENCRYPT_FL) {
		return 45;      /* ENOTSUP: an encrypted file or directory (INCOMPAT_ENCRYPT) */
	}
	inode->size = hakios_ext4_le32(raw + 4) | ((uint64_t)hakios_ext4_le32(raw + 108) << 32);
	memcpy(inode->i_block, raw + 40, 60);
	return 0;
}

/*
 * The physical block of the file's logical block `logical`, 0 for a hole or an extent not yet written (zeroes), and in
 * *run how many blocks from there on are contiguous both in the file and on the disk (at least 1), so that a file is read
 * a piece at a time and not a block at a time.
 */
static inline int
hakios_ext4_map_extents(struct hakios_ext4 *fs, const struct hakios_ext4_inode *inode, uint32_t logical,
    uint64_t *physical, uint32_t *run)
{
	const uint8_t *node = inode->i_block;
	uint32_t room = 60;

	for (unsigned level = 0; level < 6; level++) {
		uint16_t entries, depth;
		int found = -1;

		if (room < 12 || hakios_ext4_le16(node) != HAKIOS_EXT4_EXTENT_MAGIC) {
			return 5;
		}
		entries = hakios_ext4_le16(node + 2);
		depth = hakios_ext4_le16(node + 6);
		if (entries > (room - 12) / 12) {
			return 5;
		}
		/* the last entry that begins at or before the block wanted (entries are in order) */
		for (unsigned e = 0; e < entries; e++) {
			if (hakios_ext4_le32(node + 12 + 12 * e) <= logical) {
				found = (int)e;
			} else {
				break;
			}
		}
		if (depth == 0) {
			const uint8_t *x;
			uint32_t first, length;
			uint64_t start;
			int unwritten;

			if (found < 0) {
				*physical = 0;
				*run = 1;
				return 0;
			}
			x = node + 12 + 12 * found;
			first = hakios_ext4_le32(x);
			length = hakios_ext4_le16(x + 4);
			unwritten = length > 32768;
			if (unwritten) {
				length -= 32768;
			}
			start = ((uint64_t)hakios_ext4_le16(x + 6) << 32) | hakios_ext4_le32(x + 8);
			if (logical - first >= length) {
				*physical = 0;          /* a hole after this extent */
				*run = 1;
				return 0;
			}
			if (start + length > fs->blocks || start < fs->first_data_block) {
				return 5;
			}
			*physical = unwritten ? 0 : start + (logical - first);
			*run = length - (logical - first);
			return 0;
		} else {
			const uint8_t *x;
			uint64_t leaf;

			if (found < 0) {
				*physical = 0;
				*run = 1;
				return 0;
			}
			x = node + 12 + 12 * found;
			leaf = ((uint64_t)hakios_ext4_le16(x + 8) << 32) | hakios_ext4_le32(x + 4);
			if (hakios_ext4_read_block(fs, leaf, fs->block2) != 0) {
				return 5;
			}
			node = fs->block2;
			room = fs->block_size;
			if (hakios_ext4_le16(node + 6) != depth - 1) {
				return 5;
			}
		}
	}
	return 5;
}

/* The same through ext2/3's block map: 12 direct blocks, then single, double and triple indirect ones */
static inline int
hakios_ext4_map_blocks(struct hakios_ext4 *fs, const struct hakios_ext4_inode *inode, uint32_t logical,
    uint64_t *physical, uint32_t *run)
{
	uint32_t per = fs->block_size / 4, block, levels;
	uint64_t index = logical, span = 1;

	*run = 1;
	if (index < 12) {
		block = hakios_ext4_le32(inode->i_block + 4 * index);
		*physical = block;
		return block < fs->blocks ? 0 : 5;
	}
	index -= 12;
	for (levels = 1; levels <= 3; levels++) {
		span *= per;
		if (index < span) {
			break;
		}
		index -= span;
	}
	if (levels > 3) {
		return 5;
	}
	block = hakios_ext4_le32(inode->i_block + 4 * (11 + levels));
	for (uint32_t l = levels; l > 0; l--) {
		span /= per;
		if (block == 0) {
			*physical = 0;
			return 0;
		}
		if (hakios_ext4_read_block(fs, block, fs->block2) != 0) {
			return 5;
		}
		block = hakios_ext4_le32(fs->block2 + 4 * (uint32_t)(index / span));
		index %= span;
	}
	*physical = block;
	return block < fs->blocks ? 0 : 5;
}

static inline int
hakios_ext4_map(struct hakios_ext4 *fs, const struct hakios_ext4_inode *inode, uint32_t logical, uint64_t *physical,
    uint32_t *run)
{
	if (inode->flags & HAKIOS_EXT4_EXTENTS_FL) {
		return hakios_ext4_map_extents(fs, inode, logical, physical, run);
	}
	return hakios_ext4_map_blocks(fs, inode, logical, physical, run);
}

/* Bytes [0, length) of the inode's data into dst (length no more than its size) */
static inline int
hakios_ext4_read_data(struct hakios_ext4 *fs, const struct hakios_ext4_inode *inode, uint8_t *dst, uint64_t length)
{
	uint64_t done = 0;

	if (inode->flags & HAKIOS_EXT4_INLINE_DATA_FL) {
		/* the first 60 bytes are in i_block, the rest in an extended attribute, which is not read */
		if (length > 60) {
			return 45;
		}
		memcpy(dst, inode->i_block, (size_t)length);
		return 0;
	}
	while (done < length) {
		uint64_t physical, bytes;
		uint32_t run, logical = (uint32_t)(done / fs->block_size);

		if (done / fs->block_size > 0xffffffffull) {
			return 5;
		}
		if (hakios_ext4_map(fs, inode, logical, &physical, &run) != 0) {
			return 5;
		}
		bytes = (uint64_t)run * fs->block_size;
		if (bytes > HAKIOS_EXT4_CHUNK) {
			bytes = HAKIOS_EXT4_CHUNK;
		}
		if (bytes > length - done) {
			bytes = length - done;
		}
		if (physical == 0) {
			memset(dst + done, 0, (size_t)bytes);
		} else if (fs->read(fs->context, physical * fs->block_size, dst + done, bytes) != 0) {
			return 5;
		}
		done += bytes;
	}
	return 0;
}

/* The inode of `name` (length bytes, no '/') in the directory `dir`; ENOENT when it is not there */
static inline int
hakios_ext4_lookup(struct hakios_ext4 *fs, const struct hakios_ext4_inode *dir, const char *name, size_t length,
    uint32_t *number)
{
	uint64_t blocks = (dir->size + fs->block_size - 1) / fs->block_size;

	if ((dir->mode & 0xF000) != 0x4000) {
		return 20;      /* ENOTDIR */
	}
	if (dir->flags & HAKIOS_EXT4_INLINE_DATA_FL) {
		/*
		 * a directory in its inode: the parent's number in 4 bytes (for ".."), then entries in the other 56; more than
		 * fits there goes into an extended attribute, which is not read
		 */
		uint32_t at = 4;

		if (dir->size > 60) {
			return 45;
		}
		if (length == 2 && name[0] == '.' && name[1] == '.') {
			*number = hakios_ext4_le32(dir->i_block);
			return 0;
		}
		if (length == 1 && name[0] == '.') {
			*number = dir->number;
			return 0;
		}
		while (at + 8 <= 60) {
			const uint8_t *e = dir->i_block + at;
			uint32_t inode = hakios_ext4_le32(e);
			uint16_t rec = hakios_ext4_le16(e + 4);
			uint16_t len = e[6];

			if (rec < 8 || at + rec > 60 || (uint32_t)len + 8 > rec) {
				return 5;
			}
			if (inode != 0 && len == length && memcmp(e + 8, name, length) == 0) {
				*number = inode;
				return 0;
			}
			at += rec;
		}
		return 2;
	}
	for (uint64_t b = 0; b < blocks && b <= 0xffffffffull; b++) {
		uint64_t physical;
		uint32_t run, at = 0;

		if (hakios_ext4_map(fs, dir, (uint32_t)b, &physical, &run) != 0) {
			return 5;
		}
		if (physical == 0) {
			continue;
		}
		if (hakios_ext4_read_block(fs, physical, fs->block) != 0) {
			return 5;
		}
		while (at + 8 <= fs->block_size) {
			const uint8_t *e = fs->block + at;
			uint32_t inode = hakios_ext4_le32(e);
			uint16_t rec = hakios_ext4_le16(e + 4);
			uint16_t len = (fs->incompat & HAKIOS_EXT4_INCOMPAT_FILETYPE) ? e[6] : hakios_ext4_le16(e + 6);

			if (rec < 8 || (rec & 3) != 0 || at + rec > fs->block_size || (uint32_t)len + 8 > rec) {
				return 5;
			}
			if (inode != 0 && len == length && memcmp(e + 8, name, length) == 0) {
				*number = inode;
				return 0;
			}
			at += rec;
		}
	}
	return 2;               /* ENOENT */
}

/*
 * The inode `path` names (absolute within the file system: "/etc/wifi/x" or "etc/wifi/x"), following symbolic links,
 * at most HAKIOS_EXT4_LINKS of them. `links` counts those followed so far.
 */
static inline int
hakios_ext4_walk(struct hakios_ext4 *fs, uint32_t start, const char *path, size_t path_length,
    struct hakios_ext4_inode *out, unsigned *links)
{
	struct hakios_ext4_inode at;
	size_t i = 0;
	int error;

	if (path_length > 0 && path[0] == '/') {
		start = HAKIOS_EXT4_ROOT;
	}
	if ((error = hakios_ext4_inode(fs, start, &at)) != 0) {
		return error;
	}
	while (i < path_length) {
		struct hakios_ext4_inode next;
		uint32_t number, parent = at.number;
		size_t begin;

		while (i < path_length && path[i] == '/') {
			i++;
		}
		begin = i;
		while (i < path_length && path[i] != '/') {
			i++;
		}
		if (i == begin) {
			break;
		}
		if (i - begin > 255) {
			return 63;      /* ENAMETOOLONG */
		}
		if ((error = hakios_ext4_lookup(fs, &at, path + begin, i - begin, &number)) != 0) {
			return error;
		}
		if ((error = hakios_ext4_inode(fs, number, &next)) != 0) {
			return error;
		}
		if ((next.mode & 0xF000) == 0xA000) {
			char target[256];
			size_t length, skip = 0, mount_length = fs->mount == NULL ? 0 : strlen(fs->mount);
			uint32_t from = parent;

			if (++*links > HAKIOS_EXT4_LINKS) {
				return 62;      /* ELOOP */
			}
			if (next.size == 0 || next.size >= sizeof(target)) {
				return 5;
			}
			length = (size_t)next.size;
			/* a short link is in the inode itself (and has no blocks); a long one in its first block */
			if (length < 60 && !(next.flags & (HAKIOS_EXT4_EXTENTS_FL | HAKIOS_EXT4_INLINE_DATA_FL))) {
				memcpy(target, next.i_block, length);
			} else if ((error = hakios_ext4_read_data(fs, &next, (uint8_t *)target, length)) != 0) {
				return error;
			}
			target[length] = 0;
			if (target[0] == '/') {
				/* absolute: only within this file system's mount point */
				if (mount_length == 0 || strncmp(target, fs->mount, mount_length) != 0 ||
				    (target[mount_length] != '/' && target[mount_length] != 0)) {
					return 18;      /* EXDEV: it leaves the partition */
				}
				skip = mount_length;
				from = HAKIOS_EXT4_ROOT;
			}
			if ((error = hakios_ext4_walk(fs, from, target + skip, length - skip, &next, links)) != 0) {
				return error;
			}
		}
		at = next;
	}
	*out = at;
	return 0;
}

static inline int
hakios_ext4_file(struct hakios_ext4 *fs, const char *path, uint64_t max, uint8_t **data, uint64_t *size)
{
	struct hakios_ext4_inode inode;
	unsigned links = 0;
	uint8_t *buffer;
	int error;

	*data = NULL;
	*size = 0;
	if ((error = hakios_ext4_walk(fs, HAKIOS_EXT4_ROOT, path, strlen(path), &inode, &links)) != 0) {
		return error;
	}
	if ((inode.mode & 0xF000) != 0x8000) {
		return 79;              /* EFTYPE: not a regular file */
	}
	if (inode.size > max) {
		*size = inode.size;
		return 27;              /* EFBIG */
	}
	buffer = (uint8_t *)fs->alloc(inode.size == 0 ? 1 : inode.size);
	if (buffer == NULL) {
		return 12;
	}
	if ((error = hakios_ext4_read_data(fs, &inode, buffer, inode.size)) != 0) {
		fs->free(buffer, inode.size == 0 ? 1 : inode.size);
		return error;
	}
	*data = buffer;
	*size = inode.size;
	return 0;
}

#endif /* _SYS_HAKIOS_EXT4_H_ */
