/*
 * STARLTE: install the Starbite iOS overlay into the root before launchd
 * starts (bsd_init, right before load_init_program). The overlay is the
 * "ios.overlay" entry of the Starbite OS store, USERDATA +16 GiB (read
 * through /dev/rufsroot0, the raw USERDATA device of herolte_ufs.c), packed
 * by tools/overlay/sboverlay.py:
 *
 *   "SBOVL001", u32 count, u32 version (1), then count x 256 bytes:
 *   char path[232] (absolute, NUL padded), u32 mode, u32 size, u32 CRC32,
 *   u32 offset (from the overlay start, 4 KiB aligned), 8 bytes zero
 *
 * Each file is read whole, checked against its CRC32 and written over the
 * old one (O_TRUNC); only into directories that already exist. The store
 * entry's CRC is kept in /private/var/db/starbite-overlay: an unchanged
 * overlay is not written again. Needs the root mounted read-write by the
 * kernel (rootrw=1, vfs_subr.c). Boot-arg sboverlay=0 skips it.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/mount_internal.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>

void starlte_overlay_install(void);

#define SO_DEV          "/dev/rufsroot0"
#define SO_STORE_OFF    (16ull << 30)
#define SO_BLOCK        4096u
#define SO_MAX_FILE     (64u << 20)
#define SO_MAX_FILES    256u
#define SO_STAMP        "/private/var/db/starbite-overlay"

struct so_entry {
	char            path[232];
	uint32_t        mode, size, crc, offset;
	uint8_t         zero[8];
};

static uint32_t so_crc_table[256];

static uint32_t
so_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
	if (so_crc_table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			}
			so_crc_table[i] = c;
		}
	}
	crc = ~crc;
	while (n--) {
		crc = so_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	}
	return ~crc;
}

/* Read len bytes (rounded up to 4 KiB) at byte offset off of the raw device. */
static int
so_read(vnode_t dev, vfs_context_t ctx, void *buf, uint32_t len, uint64_t off)
{
	int resid = 0;
	uint32_t whole = (len + SO_BLOCK - 1) & ~(SO_BLOCK - 1);
	int err = vn_rdwr(UIO_READ, dev, (caddr_t)buf, (int)whole, (off_t)off, UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	return err ? err : (resid ? EIO : 0);
}

static int
so_write_file(const char *path, uint32_t mode, const void *data, uint32_t size, vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	int err = vnode_open(path, O_CREAT | O_TRUNC | FWRITE, (int)(mode & 07777), 0, &vp, ctx);
	if (err) {
		return err;
	}
	int resid = 0;
	if (size) {
		err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)data, (int)size, 0, UIO_SYSSPACE,
		    IO_NOAUTH | IO_SYNC, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	}
	if (!err && resid) {
		err = EIO;
	}
	if (!err) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_mode, mode & 07777);
		VATTR_SET(&va, va_uid, 0);
		VATTR_SET(&va, va_gid, 0);
		err = vnode_setattr(vp, &va, ctx);
	}
	(void)vnode_close(vp, FWRITE, ctx);
	return err;
}

/* The overlay CRC from the last install, or 0. */
static uint32_t
so_stamp(vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	char text[16] = { 0 };
	int resid = 0;
	if (vnode_open(SO_STAMP, FREAD, 0, 0, &vp, ctx)) {
		return 0;
	}
	int err = vn_rdwr(UIO_READ, vp, text, sizeof(text) - 1, 0, UIO_SYSSPACE, IO_NOAUTH,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	(void)vnode_close(vp, FREAD, ctx);
	uint32_t v = 0;
	for (int i = 0; !err && i < 8; i++) {
		char c = text[i];
		v = v << 4 | (uint32_t)(c >= 'a' ? c - 'a' + 10 : c - '0');
	}
	return err ? 0 : v;
}

void
starlte_overlay_install(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sboverlay", &on, sizeof(on));
	if (!on) {
		return;
	}
	vfs_context_t ctx = vfs_context_kernel();
	if (rootvnode == NULLVP || (vnode_mount(rootvnode)->mnt_flag & MNT_RDONLY)) {
		printf("STARLTE OVERLAY: root is read-only, not installing\n");
		return;
	}
	vnode_t dev = NULLVP;
	int err = vnode_open(SO_DEV, FREAD, 0, 0, &dev, ctx);
	if (err) {
		printf("STARLTE OVERLAY: %s: error %d\n", SO_DEV, err);
		return;
	}
	uint8_t *blk = kalloc_data(SO_BLOCK, Z_WAITOK | Z_ZERO);
	uint8_t *hdr = NULL, *data = NULL;
	uint32_t hdr_len = 0, data_len = 0;
	if (blk == NULL || so_read(dev, ctx, blk, SO_BLOCK, SO_STORE_OFF)) {
		printf("STARLTE OVERLAY: store unreadable\n");
		goto out;
	}
	if (memcmp(blk, "SBSTORE1", 8) != 0) {
		printf("STARLTE OVERLAY: no Starbite store\n");
		goto out;
	}
	uint32_t count = *(uint32_t *)(blk + 12);
	uint64_t ov_off = 0, ov_size = 0;
	uint32_t ov_crc = 0;
	for (uint32_t i = 0; i < count && i < 63; i++) {
		const uint8_t *e = blk + 64 + i * 64;
		if (strncmp((const char *)e, "ios.overlay", 24) == 0) {
			ov_off = *(const uint64_t *)(e + 24);
			ov_size = *(const uint64_t *)(e + 32);
			ov_crc = *(const uint32_t *)(e + 40);
		}
	}
	if (ov_size == 0) {
		printf("STARLTE OVERLAY: no ios.overlay in the store\n");
		goto out;
	}
	if (so_stamp(ctx) == ov_crc) {
		printf("STARLTE OVERLAY: overlay %08x already installed\n", ov_crc);
		goto out;
	}
	/* Header: magic, count, version, then 256-byte entries. */
	if (so_read(dev, ctx, blk, SO_BLOCK, SO_STORE_OFF + ov_off) || memcmp(blk, "SBOVL001", 8)) {
		printf("STARLTE OVERLAY: bad overlay header\n");
		goto out;
	}
	uint32_t files = *(uint32_t *)(blk + 8);
	if (files == 0 || files > SO_MAX_FILES) {
		printf("STARLTE OVERLAY: bad file count %u\n", files);
		goto out;
	}
	hdr_len = 16 + files * (uint32_t)sizeof(struct so_entry);
	hdr_len = (hdr_len + SO_BLOCK - 1) & ~(SO_BLOCK - 1);
	hdr = kalloc_data(hdr_len, Z_WAITOK | Z_ZERO);
	if (hdr == NULL || so_read(dev, ctx, hdr, hdr_len, SO_STORE_OFF + ov_off)) {
		printf("STARLTE OVERLAY: header unreadable\n");
		goto out;
	}
	unsigned written = 0, failed = 0;
	for (uint32_t i = 0; i < files; i++) {
		const struct so_entry *e = (const struct so_entry *)(hdr + 16 + i * sizeof(*e));
		char path[sizeof(e->path) + 1];
		memcpy(path, e->path, sizeof(e->path));
		path[sizeof(e->path)] = 0;
		if (path[0] != '/' || e->size > SO_MAX_FILE || (e->offset & (SO_BLOCK - 1)) ||
		    (uint64_t)e->offset + e->size > ov_size) {
			printf("STARLTE OVERLAY: entry %u (%s) invalid\n", i, path);
			failed++;
			continue;
		}
		uint32_t want = (e->size + SO_BLOCK - 1) & ~(SO_BLOCK - 1);
		if (want > data_len) {
			if (data) {
				kfree_data(data, data_len);
			}
			data_len = want ? want : SO_BLOCK;
			data = kalloc_data(data_len, Z_WAITOK);
			if (data == NULL) {
				data_len = 0;
				failed++;
				break;
			}
		}
		err = e->size ? so_read(dev, ctx, data, e->size, SO_STORE_OFF + ov_off + e->offset) : 0;
		uint32_t crc = err ? 0 : so_crc32(0, data, e->size);
		if (err || crc != e->crc) {
			printf("STARLTE OVERLAY: %s: read error %d or CRC %08x != %08x, skipped\n", path, err,
			    crc, e->crc);
			failed++;
			continue;
		}
		err = so_write_file(path, e->mode, data, e->size, ctx);
		printf("STARLTE OVERLAY: %s (%u bytes, mode %o) %s %d\n", path, e->size, e->mode,
		    err ? "FAILED" : "written", err);
		if (err) {
			failed++;
		} else {
			written++;
		}
	}
	if (failed == 0) {
		char text[9];
		snprintf(text, sizeof(text), "%08x", ov_crc);
		(void)so_write_file(SO_STAMP, 0644, text, 8, ctx);
	}
	printf("STARLTE OVERLAY: overlay %08x: %u files written, %u failed\n", ov_crc, written, failed);
out:
	if (data) {
		kfree_data(data, data_len);
	}
	if (hdr) {
		kfree_data(hdr, hdr_len);
	}
	if (blk) {
		kfree_data(blk, SO_BLOCK);
	}
	(void)vnode_close(dev, FREAD, ctx);
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
