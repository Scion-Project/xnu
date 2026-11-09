/*
 * PORT NOTE (Scion's iOS 27 tree, the fork's scion/xnu-12377, xnu-12377 STARLTE): from Scion-Project/scion-kernel xnu/files/bsd/sys/
 * hakios.audio.h (APSL; HakiOS/Scion). Kept: the device's user-space contract (the format, HAKIOS_AUDIO_SET_VOLUME,
 * HAKIOS_AUDIO_GET_ROUTE, /dev/hakios-mic's name, unchanged so that Scion's plug-in builds against it), the sink and
 * volume calls, and the speakers' guard. Left out: the POCO X5 Pro's DSP/speaker/haptic/torch/camera calls, the
 * microphone's kernel side (no CS47L93 codec driver here) and rec_sound (Scion's loader).
 *
 * /dev/hakios-audio: the sound Darwin plays, on its way to whatever can make it heard.
 *
 * Darwin expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them.
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_HAKIOS_AUDIO_H_
#define _SYS_HAKIOS_AUDIO_H_

#include <stdint.h>

#define HAKIOS_AUDIO_DEVICE    "/dev/hakios-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define HAKIOS_AUDIO_RATE      48000
#define HAKIOS_AUDIO_CHANNELS  2
#define HAKIOS_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(HAKIOS_AUDIO_SET_VOLUME): the speakers' volume, as Darwin sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (Darwin's volume at 0). The loudest is the kernel's to say; the plug-in only says how far under it.
 */
#include <sys/ioccom.h>

struct hakios_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define HAKIOS_AUDIO_SET_VOLUME _IOW('r', 1, struct hakios_audio_volume)

/* /dev/hakios-mic (Scion's; not made by this port: no codec driver) */
#define HAKIOS_MIC_DEVICE      "/dev/hakios-mic"
#define HAKIOS_MIC_CHANNELS    1

/*
 * ioctl(HAKIOS_AUDIO_GET_ROUTE) on /dev/hakios-audio: where what is written goes now. In this port no driver knows the
 * jack, so it answers ENOTTY (the plug-in then shows no jack), as Scion does without cs47l93_hp=1.
 */
struct hakios_audio_route {
	uint32_t jack;
	uint32_t headphones;
	uint32_t generation;
};
#define HAKIOS_AUDIO_GET_ROUTE _IOR('r', 2, struct hakios_audio_route)

#ifdef KERNEL
#include <stdbool.h>

/*
 * For drivers. A sink is offered frames as they are written and says how many it took (the
 * rest are lost); it is called from the writer's thread and must not wait.
 */
typedef uint32_t (*hakios_audio_sink_t)(const int16_t *frames, uint32_t count);

void hakios_audio_set_sink(const char *name, hakios_audio_sink_t sink);
/* The volume Darwin has set last (HAKIOS_AUDIO_SET_VOLUME): false if it has set none; *generation counts the settings. */
bool hakios_audio_volume(uint32_t *below, bool *muted, uint32_t *generation);
/* ... for a sink, which is called with the device's lock held (hakios_audio_volume takes it) */
bool hakios_audio_volume_held(uint32_t *below, bool *muted, uint32_t *generation);
/* /dev/hakios-audio made (once); in this port only by abox=8 (bsd/dev/hakios.exynos_abox.c) */
void hakios_audio_init(void);

/*
 * The speakers' guard (bsd/dev/hakios.guard.c): what the samples go through in the feeder on their
 * way to the amplifiers, for want of the DSP's speaker protection. Levels in eighths of a dB.
 */
#define HAKIOS_GUARD_SILENT    0xffffffffu     /* the amplifiers are off: nothing reaches the speakers */
struct hakios_guard_report {
	uint32_t blocks, rms_blocks, fast_blocks, slow_blocks;  /* blocks done, and in how many each average held the level down */
	uint32_t limited_frames;        /* frames in which the peak limiter turned the level down */
	uint32_t peak_in, peak_out;     /* the largest sample that came in and that went out */
	uint32_t least_peak_gain;       /* the peak limiter's least gain, in 1/65536 */
	uint64_t least_gain, gain;      /* the power guard's (with the monitor's) least gain and the latest, Q30 */
	uint64_t rms, fast, slow;       /* the averages now, the louder speaker's: Q30 of a full-scale square wave at Android's level */
};
#define HAKIOS_GUARD_MAX_GAIN  (12 * 8)        /* the most gain before the guard: 12 dB */
void hakios_guard_configure(bool hpf, uint32_t gain);         /* the high-pass filter; `gain' before everything */
void hakios_guard_attenuation(uint32_t below);                 /* the amplifiers' level under Android's (or HAKIOS_GUARD_SILENT):
                                                                 * what the limits are reckoned from */
void hakios_guard_monitor(uint32_t below);                     /* a reduction asked for by the amplifiers' watch */
void hakios_guard_process(int16_t *frames, uint32_t count);    /* stereo frames, in place */
void hakios_guard_take_report(struct hakios_guard_report *report);   /* what happened since the last call */
int hakios_guard_decibels(uint64_t ratio);                     /* 10 log10 of a Q30 ratio, in tenths of a dB */
#endif

#endif /* _SYS_HAKIOS_AUDIO_H_ */
