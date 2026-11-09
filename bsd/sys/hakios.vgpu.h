/*
 * virtio-gpu, 2D: a scanout the kernel draws into, for hosts that show one (QEMU with
 * -device virtio-gpu-pci, crosvm with a display). Under the Android Virtualization Framework a
 * sideloaded app cannot see crosvm's display, so there the boot framebuffer is the Scion shared
 * framebuffer instead (avfboot, scfb.*); this is the other way to the screen.
 *
 * MiniIOMobileFramebuffer uses it when the loader left no framebuffer: hakios_vgpu_display()
 * makes resource 1 (B8G8R8X8, what iOS draws) backed by contiguous guest memory and puts it on
 * scanout 0, and after every frame hakios_vgpu_flush() sends TRANSFER_TO_HOST_2D and
 * RESOURCE_FLUSH.
 *
 * For 3D on top (bsd/dev/hakios.vgpu3d.c): the 2D driver negotiates VIRTIO_GPU_F_VIRGL and
 * VIRTIO_GPU_F_CONTEXT_INIT when offered (it doesn't change 2D), keeps resource IDs below
 * HAKIOS_VGPU_3D_FIRST_ID, and lets anyone put one command at a time on the control queue
 * through hakios_vgpu_ctrl().
 */
#ifndef _SYS_HAKIOS_VGPU_H_
#define _SYS_HAKIOS_VGPU_H_

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

#define VIRTIO_GPU_F_VIRGL              0
#define VIRTIO_GPU_F_EDID               1
#define VIRTIO_GPU_F_RESOURCE_UUID      2
#define VIRTIO_GPU_F_RESOURCE_BLOB      3
#define VIRTIO_GPU_F_CONTEXT_INIT       4

#define HAKIOS_VGPU_2D_RESOURCE        1               /* the scanout's */
#define HAKIOS_VGPU_3D_FIRST_ID        0x10000         /* 3D allocates from here up */
#define HAKIOS_VGPU_CTRL_MAX           65536           /* largest command for hakios_vgpu_ctrl */
#define HAKIOS_VGPU_RESP_MAX           4096            /* largest response */

/*
 * The scanout, set up on first call: its memory (kernel address), size and bytes per row.
 * 0, ENODEV (no virtio-gpu or no virtio core), ENOMEM, or EIO (the device said no).
 */
int hakios_vgpu_display(void **fb, uint32_t *width, uint32_t *height, uint32_t *stride);
/* The rectangle changed: copy it to the host's resource and show it. 0 or EIO. */
int hakios_vgpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
/*
 * One command on the control queue, waited for: cmd (starting with struct virtio_gpu_ctrl_hdr)
 * is copied to device-visible memory, the response (resp_len bytes at most) back. Serialized
 * with the 2D driver's own commands; may sleep. 0, EINVAL (too long), ENODEV, or ETIMEDOUT.
 */
int hakios_vgpu_ctrl(const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len);
/* Device features negotiated (VIRTIO_GPU_F_* bits); 0 before the device is set up. */
uint64_t hakios_vgpu_features(void);

__END_DECLS

#endif /* _SYS_HAKIOS_VGPU_H_ */
