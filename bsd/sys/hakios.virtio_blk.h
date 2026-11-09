/*
 * Virtio block devices: disk images of the host become /dev/rvblkN (and the character
 * devices /dev/rrvblkN) in the guest, over virtio-pci or virtio-mmio (sys/hakios.virtio.h).
 * See bsd/dev/hakios.virtio_blk.c.
 */
#ifndef _SYS_HAKIOS_VIRTIO_BLK_H_
#define _SYS_HAKIOS_VIRTIO_BLK_H_

#include <stdbool.h>
#include <sys/types.h>
#include <sys/cdefs.h>

__BEGIN_DECLS
void hakios_virtio_blk_init(void);       /* idempotent */
bool hakios_virtio_blk_dev(dev_t dev);    /* one of its devices */
/* rd=rvblkN: the block device of that disk (initializing the driver), or an errno */
int hakios_virtio_blk_rootdev(const char *name, dev_t *dev);
/* /dev/NAME and /dev/rNAME for disk rvblkN as well */
int hakios_virtio_blk_alias(const char *disk, const char *name);
__END_DECLS

#endif /* _SYS_HAKIOS_VIRTIO_BLK_H_ */
