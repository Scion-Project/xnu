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

#endif
