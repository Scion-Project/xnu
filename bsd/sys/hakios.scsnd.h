/*
 * SCSND: iOS's sound played by the Scion app (patch 1401, for the Android Virtualization Framework).
 *
 * AVF's own sound device (crosvm's vhost-user virtio-snd) failed to start two times in three on the
 * Pixel 10, and the VM went with it. Instead the app lends the guest a region of RAM, as it lends
 * the framebuffer's (SCFB), the camera's (SCCAM) and the GPU's (SCGPU), and says where with
 * scsnd.addr= scsnd.size= in the kernel parameters; avfboot keeps it out of the kernel's memory.
 * The region begins with the header below, which the app writes before the VM starts, and holds a
 * ring of samples in /dev/hakios-audio's one format (sys/hakios.audio.h: 48000 frames a second,
 * 16-bit, left then right). The kernel's sink (bsd/dev/hakios.scsnd.c) puts in what the audio server
 * writes; the app takes it out and plays it through Android's audio (AAudio).
 *
 * The ring: ring_bytes (a power of two, whole frames) at ring_off from the region's start. head and
 * tail count bytes, free-running, modulo 2^32: the guest alone moves head (after the samples are in,
 * with a barrier), the app alone moves tail (after it has taken them). What is in: head - tail. The
 * guest never waits: what does not fit is lost (counted in dropped). The app never lets more than a
 * little build up: if it finds too much, it skips ahead (counted in skipped).
 *
 * The volume iOS sets on its speaker (HAKIOS_AUDIO_SET_VOLUME) is passed on in below/muted, and the
 * app scales by it: iOS's volume is then a part of Android's media volume.
 */
#ifndef _SYS_HAKIOS_SCSND_H_
#define _SYS_HAKIOS_SCSND_H_

#include <stdint.h>

#define SCSND_MAGIC     0x4e534353u    /* "SCSN" */
#define SCSND_VERSION   1u

struct scsnd_header {
	uint32_t magic, version;        /* the app's */
	uint32_t ring_off, ring_bytes;  /* the app's: where the ring is, and how big */
	uint32_t rate, channels;        /* the app's: 48000, 2 (what it plays) */
	uint32_t head;                  /* the guest's: bytes put in */
	uint32_t tail;                  /* the app's: bytes taken out */
	uint32_t below, muted;          /* the guest's: iOS's volume, eighths of a dB under the loudest; muted */
	uint32_t guest_ready;           /* the guest's: 1 once its sink is in place */
	uint32_t dropped;               /* the guest's: frames that did not fit */
	uint32_t skipped;               /* the app's: frames it skipped to catch up */
	uint32_t underruns;             /* the app's: times it had less than it was asked for */
	uint32_t reserved[18];          /* (128 bytes in all) */
};

#endif
