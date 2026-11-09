/*
 * Data-protection classes on the built-in HFS Plus, as labels.
 *
 * iOS gives every file and directory of the data volume a protection class (A, B, C, D, and CX;
 * NSFileProtection*), and its programs read the class back: getattrlist(ATTR_CMN_DATA_PROTECT_FLAGS),
 * fcntl(F_GETPROTECTIONCLASS), NSURLFileProtectionKey. A file system that answers "no such attribute"
 * is not "NSFileProtectionNone" to them: PosterBoard, for one, calls its data store unusable when a
 * directory it has just marked None does not read back as None.
 *
 * Apple's HFS keeps the class in the content-protection attribute together with the file's wrapped
 * key, and the storage driver encrypts with that key (CONFIG_PROTECT). None of that exists on these
 * boards: no AppleKeyStore key hierarchy for files, no volume formatted for it, no inline encryption.
 * So this is the bookkeeping half only:
 *
 *   - a class that is set (fcntl F_SETPROTECTIONCLASS, setattrlist, open_dprotected_np, a create with
 *     va_dataprotect_class) is kept in the attribute com.apple.system.scion.pclass (a "system"
 *     attribute: user space can neither list, read nor change it through the xattr calls);
 *   - a new file or directory takes the class its directory was given, as on iOS;
 *   - what was never given a class reads back as D (NSFileProtectionNone), which is the truth here.
 *
 * NOTHING IS ENCRYPTED. A class is a label that programs can set and read; a file marked A is as
 * readable from the flash, and while the device is locked, as one marked D. docs/ios27/KEYSTORE.md
 * (scion-kernel) says what that means.
 *
 * Boot-arg sweetspeed&0x100000 turns it on (off by default); sweetspeed&0x200000 also marks the
 * mount MNT_CPROTECT, which is what MKBDeviceFormattedForContentProtection() and SQLite look at;
 * sweetspeed&0x1000000 accepts APFS's "mark purgeable" calls (the end of this file's header).
 *
 * Locking: sweet_hfs_pclass_get and _newnode are called with the cnode(s) locked (hfs_vnop_getattr,
 * hfs_makenode); sweet_hfs_pclass_set and _get_vp take the lock themselves.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <sys/content_protection.h>

#include "hfs.h"
#include "hfs_cnode.h"
#include "sweet_hfs_pclass.h"

extern uint32_t sweet_speed_flags(void);        /* boot-arg sweetspeed (osfmk/arm64/sweet_a64e.c) */

#define SWEET_SPEED_PCLASS      0x100000
#define SWEET_SPEED_CPROTECT    0x200000

#define SWEET_PCLASS_XATTR      "com.apple.system.scion.pclass"
#define SWEET_PCLASS_VERSION    1
#define SWEET_PCLASS_MASK       0x1f            /* the class without its flags, as CP_CLASS() */

/* c_sweet_pclass: 0 = not looked up yet, else SWEET_PCLASS_KNOWN | the class that was set (0: none) */
#define SWEET_PCLASS_KNOWN      0x80

struct sweet_pclass_rec {
	uint8_t  version;
	uint8_t  pclass;
	uint16_t reserved;
};

int
sweet_hfs_pclass_enabled(void)
{
	return (sweet_speed_flags() & SWEET_SPEED_PCLASS) != 0;
}

int
sweet_hfs_pclass_mntflag(void)
{
	return (sweet_speed_flags() & (SWEET_SPEED_PCLASS | SWEET_SPEED_CPROTECT)) ==
	       (SWEET_SPEED_PCLASS | SWEET_SPEED_CPROTECT);
}

/* APFS's purgeable fsctl()s are accepted (core/hfs_readwrite.c, hfs_vnop_ioctl) */
#define SWEET_SPEED_PURGEABLE   0x1000000

int
sweet_hfs_purgeable_enabled(void)
{
	return (sweet_speed_flags() & SWEET_SPEED_PURGEABLE) != 0;
}

/* As HFS's cp_is_valid_class, with CX and without E: a directory may be "none" but never F. */
static int
sweet_pclass_valid(int isdir, uint32_t pclass, int creating)
{
	if (isdir) {
		return pclass <= PROTECTION_CLASS_D;
	}
	if (pclass == PROTECTION_CLASS_F) {
		return creating;                /* a file is made F, it cannot become F */
	}
	return (pclass >= PROTECTION_CLASS_A && pclass <= PROTECTION_CLASS_D) || pclass == PROTECTION_CLASS_CX;
}

/* The class this item was given, 0 if none. The cnode is locked. */
static uint32_t
sweet_pclass_stored(struct vnode *vp, struct cnode *cp)
{
	uint8_t cached = cp->c_sweet_pclass;
	uint32_t pclass = 0;

	if (cached & SWEET_PCLASS_KNOWN) {
		return cached & SWEET_PCLASS_MASK;
	}
	if (cp->c_attr.ca_recflags & kHFSHasAttributesMask) {
		struct sweet_pclass_rec rec = { 0 };
		size_t size = sizeof(rec);

		if (hfs_xattr_read(vp, SWEET_PCLASS_XATTR, &rec, &size) == 0 && size == sizeof(rec) &&
		    rec.version == SWEET_PCLASS_VERSION && rec.pclass <= PROTECTION_CLASS_MAX) {
			pclass = rec.pclass;
		}
	}
	cp->c_sweet_pclass = (uint8_t)(SWEET_PCLASS_KNOWN | pclass);
	return pclass;
}

/* Keep the class. The cnode is locked exclusive and no transaction is open. */
static int
sweet_pclass_store(struct vnode *vp, struct cnode *cp, uint32_t pclass)
{
	struct sweet_pclass_rec rec = { .version = SWEET_PCLASS_VERSION, .pclass = (uint8_t)pclass };
	int error = hfs_xattr_write(vp, SWEET_PCLASS_XATTR, &rec, sizeof(rec));

	if (error == 0) {
		cp->c_sweet_pclass = (uint8_t)(SWEET_PCLASS_KNOWN | pclass);
	}
	return error;
}

/* What getattr answers: the class that was set, else D. The cnode is locked (shared will do). */
uint32_t
sweet_hfs_pclass_get(struct vnode *vp, struct cnode *cp)
{
	uint32_t pclass = sweet_pclass_stored(vp, cp);

	return pclass != 0 ? pclass : PROTECTION_CLASS_D;
}

/* The same for a caller that holds no lock (getattrlistbulk). */
int
sweet_hfs_pclass_get_vp(struct vnode *vp, uint32_t *pclass)
{
	struct cnode *cp = VTOC(vp);
	int error = hfs_lock(cp, HFS_SHARED_LOCK, HFS_LOCK_DEFAULT);

	if (error != 0) {
		return error;
	}
	*pclass = sweet_hfs_pclass_get(vp, cp);
	hfs_unlock(cp);
	return 0;
}

/* F_SETPROTECTIONCLASS, setattrlist: as HFS's cp_vnode_setclass, without the keys. No lock is held. */
int
sweet_hfs_pclass_set(struct vnode *vp, uint32_t newclass)
{
	struct hfsmount *hfsmp = VTOHFS(vp);
	struct cnode *cp = VTOC(vp);
	int isdir = vnode_isdir(vp);
	int error;

	newclass &= SWEET_PCLASS_MASK;
	if ((!vnode_isreg(vp) && !isdir) || vnode_issystem(vp)) {
		return EBADF;                   /* not something that carries a class */
	}
	if (!sweet_pclass_valid(isdir, newclass, 0)) {
		return EINVAL;
	}
	if (hfsmp->hfs_flags & HFS_READ_ONLY) {
		return EROFS;
	}
	if ((error = hfs_lock(cp, HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT)) != 0) {
		return error;
	}
	if (sweet_pclass_stored(vp, cp) != newclass) {
		error = sweet_pclass_store(vp, cp, newclass);
	}
	hfs_unlock(cp);
	return error;
}

/*
 * A file or directory has just been made (hfs_makenode: both cnodes locked exclusive, its
 * transaction closed). The class the caller asked for, else the one its directory was given; an item
 * with neither gets no attribute and reads back as D. A failure to write the attribute is not a
 * failure to create: the item then reads back as D.
 */
void
sweet_hfs_pclass_newnode(struct vnode *dvp, struct vnode *vp, struct vnode_attr *vap)
{
	int isdir = vnode_isdir(vp);
	uint32_t pclass = 0;

	if ((!vnode_isreg(vp) && !isdir) || (VTOHFS(vp)->hfs_flags & HFS_READ_ONLY)) {
		return;
	}
	if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
		uint32_t asked = vap->va_dataprotect_class & SWEET_PCLASS_MASK;

		if (asked != 0 && sweet_pclass_valid(isdir, asked, 1)) {
			pclass = asked;
		}
	}
	if (pclass == 0 && dvp != NULL) {
		pclass = sweet_pclass_stored(dvp, VTOC(dvp));
		if (!sweet_pclass_valid(isdir, pclass, 1)) {
			pclass = 0;
		}
	}
	if (pclass != 0) {
		(void)sweet_pclass_store(vp, VTOC(vp), pclass);
	} else {
		VTOC(vp)->c_sweet_pclass = SWEET_PCLASS_KNOWN;
	}
}
