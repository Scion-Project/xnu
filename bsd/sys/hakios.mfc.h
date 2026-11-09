/*
 * /dev/hakios-mfc: the Exynos 9810's hardware video decoder for user space (bsd/dev/hakios.exynos_mfc.c, boot-arg mfc=5). One client
 * at a time: open(), HAKIOS_MFC_OPEN with the stream's parameter sets and first frame, then HAKIOS_MFC_DECODE with one access
 * unit at a time (Annex B: start codes, parameter sets in-band) and the client's NV12 planes for whatever frame is to be
 * shown; bytes = 0 drains. HAKIOS_MFC_OPEN again starts over (after a drain, the firmware's instance has finished; a stream
 * goes on from a key frame). EBUSY, EIO: use software. ESTALE from a decode: the stream changed size in-band, open again.
 */
#ifndef _SYS_HAKIOS_MFC_H_
#define _SYS_HAKIOS_MFC_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define HAKIOS_MFC_H264 0
#define HAKIOS_MFC_HEVC 1

struct hakios_mfc_open {
	uint32_t codec;                 /* in */
	uint32_t header_bytes;          /* in: Annex B with the parameter sets */
	uint64_t header;                /* in */
	uint32_t width, height;         /* out: the shown size */
	uint32_t flags;                 /* in: HAKIOS_MFC_ZERO_COPY to ask; out: HAKIOS_MFC_ZERO_COPY if frames are to be the client's surfaces */
	uint32_t stride;                /* out, zero copy: the bytes a row a surface must have, both planes */
	uint32_t rows;                  /* out, zero copy: the rows of luma it must have (with the extended pixels); half of them of chroma */
	uint32_t reserved;
};

struct hakios_mfc_decode {
	uint64_t stream;                /* in: one access unit, Annex B */
	uint32_t bytes;                 /* in: 0: drain */
	uint32_t tag;                   /* in: comes back with its frame */
	uint64_t y, uv;                 /* in: where a shown frame's planes go */
	uint32_t y_stride, uv_stride;   /* in */
	uint32_t shown;                 /* out: a frame was written */
	uint32_t shown_tag;             /* out */
	uint32_t width, height;         /* out */
	uint32_t finished;              /* out: drained */
	uint32_t room_width;            /* in: the most the planes above can hold; ERANGE if the picture is larger */
	uint32_t room_height;           /* in */
	uint32_t surface;               /* in, zero copy: the IOSurface (of the block's memory) this frame is decoded into */
	int32_t slot;                   /* out, zero copy: the slot it went to, -1 none. The client keeps a slot's surface until `used` no longer has its bit */
	int32_t shown_slot;             /* out, zero copy: the slot of the shown frame, instead of a copy into y and uv */
	uint32_t used;                  /* out, zero copy: the slots the firmware keeps */
};

#define HAKIOS_MFC_ZERO_COPY    1u

#define HAKIOS_MFC_OPEN         _IOWR('M', 1, struct hakios_mfc_open)
#define HAKIOS_MFC_DECODE       _IOWR('M', 2, struct hakios_mfc_decode)

#endif /* _SYS_HAKIOS_MFC_H_ */
