/*
 * SCGPU: the GPU of the Scion app, for an XNU guest under the Android Virtualization Framework.
 *
 * crosvm's virtio-gpu draws with the phone's GPU through gfxstream, but it can't show a picture to
 * a sideloaded app (avf/DISPLAY.md), so every frame had to come back into guest memory and go out
 * again through the shared framebuffer (SCFB): a readback of the GPU's picture for every frame, then
 * two copies. Here the app runs gfxstream's host renderer itself (tools/gfxstream/mk-host-android.sh)
 * and presents the guest's colour buffers straight into its window: nothing comes back.
 *
 * The guest still speaks virtio-gpu to it -- the same commands /dev/vgpu3d sends to crosvm
 * (bsd/dev/hakios.vgpu3d.c), the same gfxstream pipe on top -- but through a region of guest RAM the
 * app lends and maps too, as SCFB and SCCAM are lent (a CustomMemoryBackingFile). The kernel learns
 * where from scgpu.addr= and scgpu.size=; avfboot keeps the region out of XNU's memory.
 *
 *   +0           struct scgpu_header (one page)
 *   +4 KiB       the command: one virtio-gpu control command, as it would go on the control queue
 *   +64 KiB      its reply
 *   +128 KiB...  the pool: memory the guest hands out and names by guest physical address, as it would
 *                to a virtio device (ATTACH_BACKING of the pipe buffers, SCGPU_CMD_TRANSFER's staging);
 *                the app reaches it at the same offset in its own mapping
 *
 * One command at a time, as on a control queue: the guest writes the command and req_len, then bumps
 * req_seq (a release); the app sees it (an acquire), runs it, writes the reply, resp_len and status,
 * then sets resp_seq = req_seq (a release).
 *
 * Waiting: each side spins a little, then sleeps on a doorbell -- a virtio-input device named
 * "scion-scgpu" (SCGPU_BELL_NAME) whose socket is the app's. The guest rings by putting an event on
 * its status queue (crosvm writes it into the socket), the app by writing one into the socket (crosvm
 * puts it on the event queue: an interrupt). A side rings only when the other has said it sleeps
 * (host_sleeping, guest_waiting), and says so before its last look at the sequence it waits for, with
 * a full barrier on both sides, so one of the two always sees the other. Without the device (an older
 * app) both sides poll as before.
 *
 * The stream ring: what user space writes to the pipe (the GL stream, write(2) on /dev/vgpu3d) does
 * not go through the mailbox, one command per write and a wait for each, but into a byte ring in the
 * pool (ring_off, ring_size, a power of two) as records -- struct scgpu_record, then the bytes, then
 * padding to 16 -- and the guest goes on at once. ring_head (the guest's) and ring_tail (the app's) count
 * bytes forever; their difference is what waits. The app hands each record to its resource (the
 * context's pipe) and moves ring_tail on, and it does so with everything the ring holds before it runs
 * any mailbox command: so a reply, an upload or a read still comes after every byte written before it.
 * The guest waits only for room (and rings the app, or is rung, as for the mailbox).
 *
 * With SCGPU_F_RING_CMDS from the app, a record of kind SCGPU_REC_CMD holds a virtio-gpu command
 * instead of stream bytes, one whose reply nothing waits for: RESOURCE_CREATE_3D,
 * RESOURCE_ATTACH_BACKING, CTX_ATTACH_RESOURCE, CTX_DETACH_RESOURCE, RESOURCE_UNREF, and uploads:
 * SCGPU_CMD_TRANSFER to the host with staging 0, the rows (staging_len bytes, at the resource's pitch)
 * right after it in the record. The app runs it where it is in the ring, in order with the stream
 * around it, and only logs a failure. A colour buffer then costs the guest no round trip to make, fill
 * (up to a quarter of the ring) or let go (it was five and one a fill, each a wait for the app).
 *
 * Commands are virtio-gpu's (virtio 1.2, 5.7.6), with these meanings at the app:
 *   GET_CAPSET_INFO, GET_CAPSET            gfxstream's capsets (stream_renderer_get_cap_set/fill_caps)
 *   CTX_CREATE, CTX_DESTROY                stream_renderer_context_create/destroy
 *   RESOURCE_CREATE_3D, RESOURCE_UNREF     stream_renderer_resource_create/unref
 *   RESOURCE_ATTACH_BACKING                the entries are in the pool (none: no backing)
 *   CTX_ATTACH_RESOURCE, CTX_DETACH_RESOURCE
 *   TRANSFER_TO_HOST_3D, TRANSFER_FROM_HOST_3D   on the attached backing (the pipe)
 *   RESOURCE_FLUSH                         present the resource (a colour buffer) in the app's window:
 *                                          stream_renderer_flush; SET_SCANOUT is not needed
 * and one of its own:
 *   SCGPU_CMD_TRANSFER                     a transfer through a staging buffer in the pool instead of
 *                                          a backing attached for good, so that the hundreds of
 *                                          colour buffers CoreAnimation shares cost no guest memory
 */
#ifndef _SYS_HAKIOS_SCGPU_H_
#define _SYS_HAKIOS_SCGPU_H_

#include <stdint.h>

#define SCGPU_MAGIC             0x50474353u     /* "SCGP" */
#define SCGPU_VERSION           1u
#define SCGPU_CMD_OFF           0x1000u
#define SCGPU_CMD_MAX           (0x10000u - SCGPU_CMD_OFF)
#define SCGPU_RESP_OFF          0x10000u
#define SCGPU_RESP_MAX          0x1000u
#define SCGPU_POOL_OFF          0x20000u

/* the app's renderer is up and answers the mailbox */
#define SCGPU_F_HOST_READY      (1u << 0)
/* ... and takes the stream ring's records (without it the guest makes no ring: every write by mailbox) */
#define SCGPU_F_RING            (1u << 1)
/*
 * guest_flags: the guest has begun to show frames through the host (rcFBPost): the app can put the
 * renderer's window in front from now on (before, the shared framebuffer had the picture)
 */
#define SCGPU_G_PRESENTING      (1u << 0)

/* flags: the app runs commands from the ring (records of kind SCGPU_REC_CMD) */
#define SCGPU_F_RING_CMDS       (1u << 2)

/* struct scgpu_record's kind */
#define SCGPU_REC_STREAM        0u      /* bytes for the context's pipe */
#define SCGPU_REC_CMD           1u      /* a virtio-gpu command, its reply dropped (SCGPU_F_RING_CMDS) */

/* the doorbell's virtio-input device (its name, ID_NAME) */
#define SCGPU_BELL_NAME         "scion-scgpu"

/* SCGPU_CMD_TRANSFER: outside virtio-gpu's command numbers */
#define SCGPU_CMD_TRANSFER      0x0f00u

struct scgpu_header {
	uint32_t magic, version;
	uint64_t size;                  /* of the whole region */
	uint32_t flags;                 /* SCGPU_F_*, set by the app */
	uint32_t features_lo, features_hi;      /* the virtio-gpu features the "device" offers */
	uint32_t num_capsets;
	/* the mailbox */
	uint32_t req_seq;               /* the guest's, bumped after the command is written */
	uint32_t req_len;               /* bytes at SCGPU_CMD_OFF */
	uint32_t resp_max;              /* bytes the guest takes at SCGPU_RESP_OFF */
	uint32_t resp_seq;              /* the app's: = req_seq once the reply is there */
	uint32_t resp_len;
	int32_t status;                 /* 0, or a negative errno: the app could not run the command */
	/* counters, for the logs of both sides */
	uint64_t commands, presents, host_busy_ns;
	uint32_t guest_flags;           /* SCGPU_G_*, set by the guest */
	uint32_t host_sleeping;         /* the app waits for the doorbell, not polling: ring it */
	uint32_t guest_waiting;         /* the guest waits for a reply (or for room in the ring) asleep: ring it */
	uint32_t ring_off, ring_size;   /* the stream ring, in the region; size 0: no ring (every write by mailbox) */
	uint32_t ring_head, ring_tail;  /* bytes put in (the guest's), bytes taken out (the app's), forever */
	uint32_t reserved[9];
};

/* A record of the stream ring: the bytes follow, then padding to 16. */
struct scgpu_record {
	uint32_t resource_id, ctx_id;   /* the pipe of a context (SCGPU_REC_STREAM) */
	uint32_t len, kind;             /* SCGPU_REC_*: 0 in apps before SCGPU_F_RING_CMDS */
};

/* struct virtio_gpu_transfer_host_3d's twin, with the staging buffer instead of the backing */
struct scgpu_transfer {
	uint32_t type, flags;           /* struct virtio_gpu_ctrl_hdr, type = SCGPU_CMD_TRANSFER */
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx, padding[3];
	uint32_t resource_id;
	uint32_t to_host;               /* 1: guest to host, 0: host to guest */
	uint32_t x, y, w, h;            /* the box, in pixels */
	uint64_t offset;                /* of pixel (x, y) in the resource's rows (width * 4 bytes each) */
	uint64_t staging;               /* guest physical address in the pool: pixel (x, y), then the */
	uint64_t staging_len;           /* rows at the resource's pitch, (h - 1) * pitch + w * 4 bytes */
};

#endif /* _SYS_HAKIOS_SCGPU_H_ */
