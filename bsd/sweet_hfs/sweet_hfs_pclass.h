/*
 * Data-protection classes on the built-in HFS Plus, as labels (sweet_hfs_pclass.c).
 */
#ifndef SWEET_HFS_PCLASS_H
#define SWEET_HFS_PCLASS_H

#include <sys/types.h>

struct vnode;
struct cnode;
struct vnode_attr;

int      sweet_hfs_pclass_enabled(void);        /* boot-arg sweetspeed&0x100000 */
int      sweet_hfs_pclass_mntflag(void);        /* ... and sweetspeed&0x200000: the mount says MNT_CPROTECT */
uint32_t sweet_hfs_pclass_get(struct vnode *vp, struct cnode *cp);
int      sweet_hfs_pclass_get_vp(struct vnode *vp, uint32_t *pclass);
int      sweet_hfs_pclass_set(struct vnode *vp, uint32_t newclass);
void     sweet_hfs_pclass_newnode(struct vnode *dvp, struct vnode *vp, struct vnode_attr *vap);

/*
 * APFS's fsctl()s for purgeable files, by number (group 'J'), as iOS 27.0.1's PosterFoundation makes
 * them: is a directory counted (24 bytes out), count it (272 bytes), a file's purgeable flags
 * (48 bytes out), mark it purgeable (48 bytes). Accepted with boot-arg sweetspeed&0x1000000
 * (core/hfs_readwrite.c).
 */
#define SWEET_APFSIOC_DIR_STATS_GET     0x40184a03u
#define SWEET_APFSIOC_DIR_STATS_SET     0xc1104a71u
#define SWEET_APFSIOC_PURGEABLE_GET     0x40304a6du
#define SWEET_APFSIOC_PURGEABLE_MARK    0xc0304a6fu
/* ... and the older one-word form PosterBoard's own -[NSURL pbf_setPurgable:error:] uses (ffsctl, 'J' 68, 8 bytes of flags) */
#define SWEET_APFSIOC_MARK_PURGEABLE    0xc0084a44u
int      sweet_hfs_purgeable_enabled(void);

#endif
