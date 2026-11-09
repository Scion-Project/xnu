/*
 * PORT NOTE (Scion's iOS 27 tree, the fork's scion/xnu-12377, xnu-12377 STARLTE): from Scion-Project/scion-kernel drivers/audio/
 * hakios.audio.c (APSL; HakiOS/Scion). Kept as Scion has them: the write path, the sink, the end-of-stretch log line,
 * HAKIOS_AUDIO_SET_VOLUME (and its cap) and HAKIOS_AUDIO_GET_ROUTE (always ENOTTY here: no driver knows the jack).
 * Left out: rec_sound/rec_sound_fed/rec_sound_mic (they hand memory to Scion's loader through loader_sound, which this
 * path has not), /dev/hakios-mic (its only feeder is ABOX's capture of the CS47L93 codec, not ported) and the calls
 * into the POCO X5 Pro's and the Scion app's drivers. hakios_audio_init() is called by ABOX's driver at abox=8 only
 * (Scion calls it on every boot), and makes the node once.
 *
 * /dev/hakios-audio: the sound that Darwin plays (see sys/hakios.audio.h). What is written goes to a sink, a driver
 * that plays it (here ABOX's step 8: RDMA0 -> UAIF1 -> the two MAX98512s).
 *
 * When the writer has stopped, a line in the log says how long it wrote, at what rate, how much
 * of it was not silence and how loud: the only way to "hear" a machine one cannot listen to.
 * (The audio server writes for as long as its device runs, a second or two beyond the sound.)
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/hakios.audio.h>
#include <sys/systm.h>
#include <sys/uio.h>

#define CHUNK_FRAMES    1024            /* what one pass of write() moves */
#define QUIET_MS        500             /* no write for this long: the writer has stopped */
#define SILENCE         8               /* samples no larger than this are silence */

static LCK_GRP_DECLARE(audio_lck_grp, "hakios-audio");
static LCK_MTX_DECLARE(audio_lock, &audio_lck_grp);

static hakios_audio_sink_t audio_sink;

/* the stretch of writing in progress */
static uint64_t stretch_first, stretch_last;    /* mach_absolute_time() of its first and its latest write */
static uint64_t stretch_frames, stretch_latest; /* frames written, and how many of them by the latest write */
static uint64_t stretch_sound, stretch_lost;    /* frames in writes that were not silence; frames the sink did not take */
static uint32_t stretch_peak;                   /* the largest sample, as an absolute value */
static thread_call_t report_call;
static bool report_armed;

static void
audio_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t frames, latest, sound, lost, deadline, since = 0, lasted = 0;
	uint32_t loudest;

	lck_mtx_lock(&audio_lock);
	absolutetime_to_nanoseconds(mach_absolute_time() - stretch_last, &since);
	if (since < QUIET_MS * NSEC_PER_MSEC) {
		/* still writing: look again later */
		clock_interval_to_deadline(2 * QUIET_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(report_call, deadline);
		lck_mtx_unlock(&audio_lock);
		return;
	}
	absolutetime_to_nanoseconds(stretch_last - stretch_first, &lasted);
	frames = stretch_frames;
	latest = stretch_latest;
	sound = stretch_sound;
	lost = stretch_lost;
	loudest = stretch_peak;
	stretch_frames = stretch_sound = stretch_lost = 0;
	stretch_peak = 0;
	report_armed = false;
	lck_mtx_unlock(&audio_lock);

	printf("hakios-audio: %llu.%02llu s written", frames / HAKIOS_AUDIO_RATE,
	    frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE);
	if (lasted >= NSEC_PER_SEC) {
		/* what was written before the latest write took the time between the first and the latest */
		printf(" at %llu frames a second", (frames - latest) * NSEC_PER_SEC / lasted);
	}
	printf(", %llu.%02llu s of it sound, peak %u%% of full scale", sound / HAKIOS_AUDIO_RATE,
	    sound % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE, loudest * 100 / 32768);
	if (lost) {
		printf(", %llu ms lost (the sink was full)", lost * 1000 / HAKIOS_AUDIO_RATE);
	}
	printf("\n");
}

static void
audio_play(const int16_t *frames, uint32_t count)
{
	uint32_t loudest = 0, taken = 0;
	uint64_t deadline;

	for (uint32_t i = 0; i < count * HAKIOS_AUDIO_CHANNELS; i++) {
		uint32_t level = frames[i] < 0 ? -(int32_t)frames[i] : frames[i];

		if (level > loudest) {
			loudest = level;
		}
	}
	lck_mtx_lock(&audio_lock);
	if (audio_sink != NULL) {
		taken = audio_sink(frames, count);
		stretch_lost += count - taken;
	}
	stretch_last = mach_absolute_time();
	if (stretch_frames == 0) {
		stretch_first = stretch_last;
	}
	stretch_frames += count;
	stretch_latest = count;
	if (loudest > SILENCE) {
		stretch_sound += count;
	}
	if (loudest > stretch_peak) {
		stretch_peak = loudest;
	}
	if (!report_armed && report_call != NULL) {
		report_armed = true;
		clock_interval_to_deadline(2 * QUIET_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(report_call, deadline);
	}
	lck_mtx_unlock(&audio_lock);
}

void
hakios_audio_set_sink(const char *name, hakios_audio_sink_t sink)
{
	lck_mtx_lock(&audio_lock);
	audio_sink = sink;
	lck_mtx_unlock(&audio_lock);
	printf("hakios-audio: sound goes to %s\n", name);
}

static int
audio_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
audio_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
audio_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t frames[CHUNK_FRAMES * HAKIOS_AUDIO_CHANNELS];
	int error = 0;

	while (uio_resid(uio) >= HAKIOS_AUDIO_FRAME) {
		user_ssize_t bytes = uio_resid(uio);

		if (bytes > (user_ssize_t)sizeof(frames)) {
			bytes = sizeof(frames);
		}
		bytes -= bytes % HAKIOS_AUDIO_FRAME;
		error = uiomove((caddr_t)frames, (int)bytes, uio);
		if (error) {
			break;
		}
		audio_play(frames, (uint32_t)(bytes / HAKIOS_AUDIO_FRAME));
	}
	return error;
}

/* the speakers' volume, as Darwin sets it (sys/hakios.audio.h): kept for the sink's driver to look at */
#define HAKIOS_AUDIO_BELOW_MAX  (90 * 8)        /* eighths of a dB: Scion's cap (the POCO amplifiers' lowest volume) */
static uint32_t volume_below, volume_generation;
static bool volume_muted;

static int
audio_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flags, __unused struct proc *p)
{
	const struct hakios_audio_volume *volume = (const struct hakios_audio_volume *)data;
	uint32_t below;

	if (command == HAKIOS_AUDIO_GET_ROUTE) {
		return ENOTTY;          /* (port) no jack known here: the plug-in shows none */
	}
	if (command != HAKIOS_AUDIO_SET_VOLUME) {
		return ENOTTY;
	}
	/*
	 * A larger `below' means nothing to the amplifiers, and from user space (the node is open to all) it would make
	 * sums made with it wrap round to a level far louder than the one asked for: capped.
	 */
	below = volume->below > HAKIOS_AUDIO_BELOW_MAX ? HAKIOS_AUDIO_BELOW_MAX : volume->below;
	lck_mtx_lock(&audio_lock);
	volume_below = below;
	volume_muted = volume->muted != 0;
	volume_generation++;
	lck_mtx_unlock(&audio_lock);
	printf("hakios-audio: the volume: %s%u.%03u dB under the loudest\n", volume->muted ? "muted; " : "", below / 8,
	    below % 8 * 125);
	return 0;
}

bool
hakios_audio_volume(uint32_t *below, bool *muted, uint32_t *generation)
{
	bool known;

	lck_mtx_lock(&audio_lock);
	known = hakios_audio_volume_held(below, muted, generation);
	lck_mtx_unlock(&audio_lock);
	return known;
}

/* The same, for a sink: it is called with audio_lock held (hakios_audio_set_sink). */
bool
hakios_audio_volume_held(uint32_t *below, bool *muted, uint32_t *generation)
{
	*below = volume_below;
	*muted = volume_muted;
	*generation = volume_generation;
	return *generation != 0;
}

static const struct cdevsw audio_cdevsw = {
	.d_open = audio_open, .d_close = audio_close, .d_read = eno_rdwrt, .d_write = audio_write,
	.d_ioctl = audio_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_audio_init(void)
{
	static bool made;
	int major;

	if (made) {
		return;
	}
	made = true;
	major = cdevsw_add(-1, &audio_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-audio") == NULL) {
		printf("hakios-audio: cannot create the device\n");
		return;
	}
	report_call = thread_call_allocate(audio_report, NULL);
	printf("hakios-audio: /dev/hakios-audio made (48 kHz, 2 x 16 bits)\n");
}
