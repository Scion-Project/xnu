/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 */
#include "SweetSurface.h"
#include "hakios.sweetfb_frameinfo.h"   /* scion-kernel drivers/display/sweetfb, profiles/ios-27.copy */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include "hakios.sweetfb_present.h"   /* sweetfb_nowait's state machine (scion-kernel drivers/display/sweetfb, fork_copy) */
#endif

extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
extern "C" uint64_t a5xelte_power_pressed_at;   /* bsd/dev/a5xelte_keys.c */
#endif

#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/proc.h>
extern "C" uint32_t sweet_speed_flags(void);     /* boot-arg sweetspeed (osfmk/arm64/sweet_a64e.c) */
/*
 * boot-arg sweetfb_prof=1 (off by default): where a frame waits, in the ten-second lines the gap histogram is printed with
 * ("A5XELTE FB prof: ..."): how long a swap call was held (for the chains of the swap before it), how long the tick that
 * took a frame slept for that frame's own chains, the present, the time from the swap to the end of its present, the real
 * time between ticks, and for every tick that found no new frame within six ticks of the last one, what the window server
 * was doing: held in a swap call, GPU chains of it queued or running (the frame is being drawn, its swap has not come), or
 * nothing of it on the GPU at all (it is computing, or waiting for something else). Counters only: two clock reads a swap.
 */
static uint32_t sweet_fb_prof; extern "C" uint64_t sweet_fb_first_present; /* defined under HEROLTE below, at the end for FRANKEL */
static volatile int32_t sweet_fb_pf_inswap;
static struct {
	uint64_t swaps, held_ns, held_max;                      /* swap calls, and how long they were held */
	uint64_t frames, slept_ns, slept_max, present_ns;       /* frames taken by a tick: the sleep for their chains, the present */
	uint64_t shown_ns, shown_max;                           /* swap recorded -> its present done */
	uint64_t ticks, period_ns, period_max, last_tick_at;    /* real time between ticks */
	uint64_t last_shown_tick;
	uint32_t empty_inswap, empty_gpu, empty_idle;           /* ticks without a new frame, by what the window server was at */
	uint64_t empty_depth;
} sweet_fb_pf;
static uint64_t sweet_fb_pf_stamp;                              /* when the pending swap was recorded */
/*
 * boot-arg sweetfb_trace=S (off by default): the order of things, once. A ring of the last 768 frame events with their
 * times; S seconds after the scripted input (sweetspeed&4096) saw its first frame the ring is printed, oldest first, as
 * "FBT: <microseconds since the first one printed><letter><number> ..." (twelve to a line):
 *   T a tick begins (number: app chains queued or running)      F the tick takes a frame (its swap's number)
 *   G that frame's chains are done                              P its present is done
 *   S a swap call comes in (chains queued)                      R the swap is recorded (it was held until now)
 *   B the GPU's queue gets its first chain after being empty    E the queue runs empty (chains run since B)
 * One store and a clock read an event; nothing is printed until the dump.
 */
static uint32_t sweet_fb_trace;
static struct {
	uint64_t at;
	uint32_t arg;
	char what;
} sweet_fb_tr[768];
static volatile uint32_t sweet_fb_tr_n;
static bool sweet_fb_tr_dumped;
extern "C" unsigned long long starlte_touch_script_t0;         /* bsd/dev/starlte_touch.c */
extern "C" void sweet_fb_trace_event(char what, uint32_t arg);
/* every frame presented since boot (one thread counts): bsd/dev/starlte_touch.c prints it between the scripted input's steps */
extern "C" { unsigned long long sweet_fb_frames_shown; }

extern "C" void
sweet_fb_trace_event(char what, uint32_t arg)
{
	if (!sweet_fb_trace || sweet_fb_tr_dumped) {
		return;
	}
	uint32_t at = __atomic_fetch_add(&sweet_fb_tr_n, 1, __ATOMIC_RELAXED) % 768;
	sweet_fb_tr[at].at = mach_absolute_time();
	sweet_fb_tr[at].arg = arg;
	sweet_fb_tr[at].what = what;
}

static void
sweet_fb_trace_dump(void)
{
	sweet_fb_tr_dumped = true;
	uint32_t n = sweet_fb_tr_n, count = n < 768 ? n : 768, first = n < 768 ? 0 : n % 768;
	uint64_t t0 = sweet_fb_tr[first].at;
	char line[200];
	int len = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t at = (first + i) % 768;
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(sweet_fb_tr[at].at - t0, &ns);
		len += snprintf(line + len, sizeof(line) - (size_t)len, " %llu%c%u", ns / 1000, sweet_fb_tr[at].what, sweet_fb_tr[at].arg);
		if ((i % 12) == 11 || i + 1 == count) {
			IOLog("FBT:%s\n", line);
			len = 0;
			if ((i % 96) == 95) {
				IOSleep(20);            /* (the console's reader is slower than this loop) */
			}
		}
	}
	IOLog("FBT: done, %u events\n", count);
}
extern "C" unsigned starlte_gpu_user_pending(void);            /* bsd/dev/starlte_gpu.c: app chains queued or running */

static uint32_t sweet_fb_gaps[5];       /* vsync ticks between presented frames (sweetspeed&16384, see the present path) */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* the S6E3HA8 panel brightness (bsd/dev/hakios.s6e3ha8.c; on with boot-arg sweetspeed&512) */
extern "C" void hakios_s6e3ha8_init(void);
extern "C" int hakios_panel_brightness(int level);
extern "C" int hakios_panel_brightness_nits(uint32_t nits);
/* iOS 27 IOMobileFramebufferSwapSetBrightness (IOMobileFramebuffer kern_SwapSetBrightness): the swap record (the
 * framebuffer object + 0x18) carries a "brightness set" byte at +0x354 and the brightness, a double, at +0x35e.
 * A change goes to the panel (sweetspeed&512): <= 1.0 taken as a fraction, else as nits. Integer decode, no FP in here. */
static uint32_t sweet_fb_sbx;
static uint64_t sweet_fb_last_brightness = ~0ULL;
static void
sweet_fb_swap_brightness(const uint8_t *rec)
{
	uint64_t bits;
	if ((sweet_fb_sbx & 512) == 0 || rec[0x354] == 0) {
		return;
	}
	memcpy(&bits, rec + 0x35e, sizeof(bits));
	if (bits == sweet_fb_last_brightness) {
		return;
	}
	sweet_fb_last_brightness = bits;
	/* value * 1000 as an integer (positive normal doubles only) */
	uint64_t milli = 0;
	int exp = (int)((bits >> 52) & 0x7ff) - 1023;
	if ((bits >> 63) == 0 && exp >= -20 && exp < 40) {
		uint64_t mant = (bits & ((1ULL << 52) - 1)) | (1ULL << 52);
		int shift = 52 - exp;
		milli = shift > 0 ? (mant * 1000) >> shift : 0;
		if (shift > 0 && shift >= 64) {
			milli = 0;
		}
	}
	int level;
	if (milli <= 1000) {
		/* (a floor of 16/255 so a misread value never leaves the panel black) */
		int want = (int)(milli * 255 / 1000);
		want = want < 16 ? 16 : want;
		level = hakios_panel_brightness(want) == 0 ? want : -1;
	} else {
		level = hakios_panel_brightness_nits((uint32_t)(milli / 1000 < 5 ? 5 : milli / 1000));
	}
	static unsigned said;
	if (said < 60) {
		said++;
		IOLog("SWEET FB: swap brightness %llu.%03llu (raw %#llx) -> panel level %d\n", milli / 1000, milli % 1000,
		    bits, level);
	}
}
static bool
sweet_fb_is_brightness(const char *key)
{
	static const char want[] = "brightness";
	for (unsigned i = 0; i < sizeof(want); i++) {
		char c = key[i];
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		if (c != want[i]) {
			return false;
		}
	}
	return true;
}
#endif

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
static void sweet_fb_copy_init(void);
/* No CPU copies to the panel before this mach time (herolte_gpu.c). */
extern "C" uint64_t sweet_fb_hold_until;
uint64_t sweet_fb_hold_until;
/* Last surface copied full-screen to the panel (GPU copy test). */
static uint32_t sweet_fb_last_surface_id;
/* mach_absolute_time() of the first swap that put pixels on the panel (0: none yet), for scripted
 * test input (starlte_touch.c, sweetswipe=1) */
extern "C" uint64_t sweet_fb_first_present;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
extern "C" void hakios_exynos_cpufreq_frame(void);     /* bsd/dev/hakios.exynos_cpufreq.c: the frame boost */
#endif
uint64_t sweet_fb_first_present;
extern "C" bool sweet_fb_last_surface(uint64_t *pages, unsigned max_pages, unsigned *npages,
    uint32_t *offset, uint32_t *stride, uint32_t *width, uint32_t *height, uintptr_t *kva);
/* bsd/dev/herolte_gpu.c: the Mali scales a surface to the whole panel. On
 * STARLTE (whose board config also defines HEROLTE) the S9's Mali-G72 does
 * it instead: bsd/dev/starlte_gpu.c, ready once its boot-time GPU tests
 * have passed. */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#define herolte_gpu_present_slot starlte_gpu_present_slot
#define herolte_gpu_present_map starlte_gpu_present_map
#define herolte_gpu_present_draw starlte_gpu_present_draw
extern "C" int starlte_gpu_present_ready(void);
extern "C" uint64_t starlte_gpu_user_seq(void);
extern "C" int starlte_gpu_user_wait_seq(uint64_t seq, bool block);
extern "C" uint64_t starlte_gpu_user_done(void);
#endif
extern "C" int herolte_gpu_present_slot(uint32_t id, uint64_t first_pa);
extern "C" int herolte_gpu_present_map(uint32_t id, const uint64_t *pages, unsigned n);
extern "C" bool herolte_gpu_present_draw(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h);
#define SWEET_FB_GPU_MAX_PAGES 3072     /* one 12 MiB GPU slot (starlte_gpu.c): 1242x2208 x 5120 B */
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/*
 * boot-arg sweetfb_dpp=1 (off by default; kern.hakios.sweetfb_dpp=0/1 while the system runs, the USB console's "sysctl"):
 * the present by the DPU's scaler. NOT run on a phone. docs/ios27/PRESENT.md (scion-kernel) has the design.
 *
 * Today (sweetfb_scale=1 sweetfb_gpu=1) a frame ends with a GPU job that scales the 828 x 1702 swap surface to the
 * panel's 1440 x 2960 in the boot framebuffer at 0xCC000000, which the DECON reads at every refresh (and which the job
 * draws while it is read: it can tear). With sweetfb_dpp=1 the job copies the frame one pixel for one into one of three
 * buffers of the frame's own size in the display's carve-out (bsd/dev/hakios.exynos_dpu.c, hakios_scanout_memory():
 * physically contiguous, below 4 GB, outside the VM), and the DECON's window reads it through VGF0, whose DPP scales
 * it to the panel (hakios_scanout_attach(2, ...), as the 16.5 kernel's fb_dpp=1 does). The job writes 5.7 MB a frame
 * instead of 17.0 and shades 1.41 million pixels instead of 4.26; the DECON reads 5.7 MB a refresh instead of 17.0.
 *
 * A buffer is drawn only when it is neither the one the DECON shows nor the one a flip waits to show; a flip (the
 * IDMA's base address and a shadow update) is asked for only once the one before it has been taken at a refresh
 * (hakios_scanout_latched()), so nothing is drawn into what is read, and frames reach the panel at most once a refresh.
 *
 * Every way out goes back to today's present (the scaled copy into the boot framebuffer, which the DECON reads again on
 * the boot loader's channel: hakios_scanout_detach()), and says why in the framebuffer's ten-second line:
 *   - the DECON not as expected, the carve-out the VM's, the buffers not mapped for the GPU, the scaler refused or
 *     reporting an error (the DPU driver's own lines say which): given up for the boot (until the sysctl asks again);
 *   - the copy job failed, or a flip not taken in 100 ms: given up likewise;
 *   - a frame the DPP path cannot take (more than one layer, a part of the display, another size or format): left at
 *     that frame, the shown buffer first drawn scaled into the boot framebuffer so that the frame drawn over it is
 *     whole; asked for again after SWEET_FB_DPP_QUIET frames it can take, SWEET_FB_DPP_RETRIES times in a boot;
 *   - the display off (RequestPowerChange 0): left the same way before the fade, asked for again at the next frame;
 *   - kern.hakios.sweetfb_dpp=0: left the same way at the next tick.
 * While the DPP shows the frames, what is written into the boot framebuffer is not seen: the kernel's text console
 * (sweetfb_noconsole=1 is how it is anyway with the GUI) and a panic's screen. The USB console's "shot" reads the
 * buffer VGF0 shows (hakios_fb_scanned(), 828 x 1702, unscaled).
 */
extern "C" int starlte_gpu_present_scan_map(uint64_t pa, uint64_t bytes);
extern "C" bool starlte_gpu_present_copy(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h, uint64_t dst_off,
    uint32_t dst_stride);
extern "C" bool starlte_gpu_present_from_scan(uint64_t src_off, uint32_t stride, uint32_t w, uint32_t h);
extern "C" uint32_t sweet_fb_dpp_want, sweet_fb_dpp_rearm;     /* bsd/dev/starlte_gpu.c (the sysctl) */
extern "C" int hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);
extern "C" int hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys);
extern "C" int hakios_scanout_attach(int mode, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height,
    uint32_t rowbytes, unsigned shown);
extern "C" int hakios_scanout_flip(unsigned index);
extern "C" int hakios_scanout_latched(void);
extern "C" void hakios_scanout_detach(const char *why);
extern "C" uint32_t hakios_scanout_refreshes(void);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
#define SWEET_FB_DPP_BUFFERS    3
#define SWEET_FB_DPP_STUCK_MS   100     /* a flip not taken by then: given up */
#define SWEET_FB_DPP_QUIET      120     /* frames it can take before it is asked for again after one it could not */
#define SWEET_FB_DPP_RETRIES    16
/* how dppLeave() leaves: after a frame it could not take (asked for again after SWEET_FB_DPP_QUIET frames it can, at
 * most SWEET_FB_DPP_RETRIES times a boot), for the display off (again at the next frame), switched off by the sysctl
 * (nothing counted), or given up (until kern.hakios.sweetfb_dpp=1 is written) */
enum { SWEET_FB_DPP_LEAVE_FRAME, SWEET_FB_DPP_LEAVE_AGAIN, SWEET_FB_DPP_LEAVE_OFF, SWEET_FB_DPP_LEAVE_GIVE_UP };
static struct {
	bool tried, ready;              /* set up once: the DECON probed, the buffers had and mapped for the GPU */
	bool attached;                  /* VGF0 scales the buffers to the panel */
	bool given_up;                  /* until kern.hakios.sweetfb_dpp=1 is written */
	bool again_now;                 /* left for the display off: asked for again at the next frame */
	const char *why;                /* why it was last left or given up */
	uint32_t rearm_seen;
	uint32_t quiet, retries;        /* after a frame it could not take: frames it can take since, re-attaches left */
	uint64_t phys[SWEET_FB_DPP_BUFFERS], step;
	uint32_t w, h, rowbytes;
	vm_offset_t kva;                /* the buffers for the CPU (write-combined): the console's shot, the fallback */
	int front, pending;             /* the buffer the DECON shows, the one a flip waits to show (-1: none) */
	int path;                       /* the frame just presented: 0 the CPU, 1 the GPU's scaled copy, 2 the DPP */
	uint32_t last_swap;             /* the swap id of the last SwapEnd */
	/* the ten-second line's counters */
	uint32_t by_dpp, by_gpu, by_cpu, flips, waited, left, attaches, refreshes_at;
	uint64_t wait_abs, wait_max;
	uint32_t sw_calls, sw_last, sw_before, sw_other, sw_logged;
	/* the swap surfaces (layer 0): which ids came in the window, and how often a swap named the one before's again */
	uint32_t sf_ids[4], sf_count, sf_more, sf_same, sf_prev;
} sweet_fb_dp;                                  /* (front, pending -1 and quiet SWEET_FB_DPP_QUIET from start()) */
#endif

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
	kSweetFBChangeFrameInfo         = 73,   /* sweetfb_frameinfo */
	kSweetFBSupportedFrameInfo      = 74,
};

/* the shortest swap record taken: macOS 13 (Ventura) sends 0x468, iOS 16 0x46c, iPadOS 18 0x514; every
 * field read here (swap id 0x50 .. enabled mask 0x104, +0x48 on iPadOS 18) lies far inside it */
#define SWEET_FB_SWAP_REC_SIZE          0x400
#define SWEET_FB_SWAP_REC_MAX           0x600   /* iPadOS 18 sends 0x514 */
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           16      /* IOMFB 27: 0..11 (7 = relbuf info) */
#define SWEET_FB_NOTIFY_RELBUF          7
/* relbuf state (defined with sweet_fb_swap_id() further down) */
static int sweet_fb_relbuf;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/*
 * boot-arg sweetfb_nowait=1 (off by default; NOT run on a phone when written, 18 Nov 2026): the vsync tick never sleeps for the
 * GPU (without it a tick that takes a frame whose chains are still running sleeps until they are done and holds the timer: "tick
 * slept for the GPU avg 11-12 ms", "ticks avg 20 ms" while swiping, 17 Nov). A frame stays recorded until the first tick after
 * its chains are done (the GPU queue's done number, written by the queue's worker as it retires each chain); a newer swap
 * replaces it meanwhile; a frame that waits too long is taken by the old sleeping path, counted, and with the GPU idle logged
 * once as a logic error. The state machine is hakios.sweetfb_present.h (host-tested); docs/ios27/PRESENT.md.
 */
static uint32_t sweet_fb_nowait;
#endif
static uint32_t sweet_fb_relbuf_sent;
/*
 * boot-arg sweetfb_frameinfo=N (off by default; NOT run on a phone when written, 18 Nov 2026): IOMFB's frame info
 * (docs/ios27/PRESENT.md, "Frame info"; the keys and the message in hakios.sweetfb_frameinfo.h). N's bits offer keys:
 * 1 Presentation_time (when the present of a swap finished), 2 Last_Fence_or_Event_Time (when its GPU chains were seen
 * done, STARLTE only). Selector 74 lists them, 73 takes a client's mask, and each shown swap goes to every client that
 * asked as notification type 3. Without it: no key (selector 74 answers a count of 0, as before) and nothing sent.
 */
static uint32_t sweet_fb_fi, sweet_fb_fi_sent;
/*
 * sweetfb_cav=1 (default off): CoreAnimation pacing aids for iOS 27.
 * iOS 27 backboardd does not pace on IOMFB vsync (type 5) or FrameInfo (type 3): IOMFBServer
 * drives renders from its own CADisplayTimer, period = AppleInternalDisplay::heartbeat_rate()
 * (1/60 s unless "IOMFBDisplayRefresh" says otherwise). CA::WindowServer::Server::render_for_time
 * clamps the next update to >= now + 0.1 s (10 Hz) whenever CA's own display power state is not
 * "on" (RequestPowerChange(0) was its last word; a5fb_keep_on keeps the panel lit, CA still
 * throttles). With the boot-arg:
 *  - "IOMFBDisplayRefresh" = { displayMinRefreshInterval = 2^32/60 (1/60 s in 2^-32 s),
 *    displayRefreshStepMachTime = 16.667 ms in mach ticks } is published;
 *  - once a second: "SWEET FB CAV: <n> swaps/s, CA power <on|off>" (CA power = last sel 12 arg);
 *  - every sel 12 call with its uptime; the swap record's brightness fields when they change.
 */
static int sweet_fb_cav;
static uint32_t sweet_fb_cav_swaps;             /* SwapEnd calls since the last rate line */
static int sweet_fb_cav_power = -1;             /* last RequestPowerChange argument, -1 none yet */
/* iOS 27 SwapEnd record = IOMFB's fb + 0x18 (kern_SwapEnd sends 0x6e0 bytes from there);
 * kern_SwapSetBrightness stores a double at fb + 0x376 and sets the byte at fb + 0x36c */
#define SWEET_FB_REC27_BRIGHT_SET       0x354
#define SWEET_FB_REC27_BRIGHT           0x35e
static uint32_t sweet_fb_swap_id(const uint8_t *rec);
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#define SWEET_FB_DOT_PITCH              A5XELTE_FB_DOT_PITCH
#else
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
#endif
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	bool          fScaled;                  /* sweetfb_scale: logical = 2/3 panel, x1.5 */
	bool          fGpu;                     /* sweetfb_gpu: scaled by the Mali, bilinear */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_MAX];      /* latest swap, not yet shown */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	struct sfp    fPres;                                /* sweetfb_nowait: what fPending is waiting for (fDirty == fPres.recorded) */
#endif
	uint64_t      fPendingSeq;                          /* the last GPU chain queued when it came (STARLTE, sweetspeed&262144) */
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	bool          fPerfLog;       /* A5-only copy timing; never on the donor */
	uint64_t      fPerfFrames;
	uint64_t      fPerfBytes;
	uint64_t      fPerfNS;
	uint64_t      fPerfMaxNS;
	uint64_t      fPerfWindowStart;
#endif
	bool          fPowerOff;      /* RequestPowerChange(0) */
	uint32_t      fFadeStep;      /* fade-out steps done while off */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec, uint32_t size);
	void relbuf(uint32_t swapId);
	void frameInfo(uint32_t swapId, uint64_t presented, uint64_t fence);
	uint64_t present(const uint8_t *rec);
	void sweetSaveUnderRect(int32_t dx, int32_t dy, int32_t w, int32_t h);
	void sweetSaveUnder(bool save);
	uint64_t scaledCopy(uintptr_t src, uint32_t srcRowBytes, int32_t dx, int32_t dy,
	    int32_t w, int32_t h);
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	bool gpuPresent(SweetSurface *s, int32_t sx, int32_t sy, int32_t w, int32_t h);
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	bool dppWanted(void);
	bool dppSetup(void);
	bool dppPresent(int slot, uint32_t off, uint32_t stride, uint32_t w, uint32_t h);
	void dppLeave(const char *why, bool restore, int how);
#endif
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];
	uint32_t            fFIMask;        /* sweetfb_frameinfo: the keys this client enabled (selector 73) */

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

/* the panel's memory as start() found it, for hakios_fb_scanned() below */
static uintptr_t sweet_fb_scan_base;
static uint32_t sweet_fb_scan_width, sweet_fb_scan_height, sweet_fb_scan_rowbytes;

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* The loader's DECON handoff scans out this physical XRGB8888 buffer.
	 * It does not fill boot_args.Video, so PE_current_console is empty.
	 * Keep the physical address out of the donor path. */
	fPhys = A5XELTE_FB_PA;
	fWidth = A5XELTE_FB_WIDTH;
	fHeight = A5XELTE_FB_HEIGHT;
	fRowBytes = A5XELTE_FB_STRIDE;
#else
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
#endif
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	/* iPhone X userland on the 1440x2560 panel: its 1125x2436 canvas is
	 * shown centered (fOffX/fOffY are negative), unscaled. */
	if (!native) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	/* Experimental: the CPU renderer (no GPU) draws 960x1706, 2.25x fewer
	 * pixels than the panel, and present() scales it x1.5 (nearest). */
	int scale = 0;
	PE_parse_boot_argn("sweetfb_scale", &scale, sizeof(scale));
	if (scale) {
		fScaled = true;
		fLogicalW = fWidth * 2 / 3;
		fLogicalH = fHeight * 2 / 3;
		/* sweetfb_lw/sweetfb_lh: another logical size (it must match the
		 * ADT canvas). 960 px is 320 points at @3x, narrower than any
		 * iPhone iOS 16 lays out for; 1125x2001 is 375x667 points, the
		 * iPhone 8 / SE screen in the panel's 16:9. */
		uint32_t lw = 0, lh = 0;
		PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw));
		PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh));
		if (lw >= 320 && lw <= fWidth && lh >= 480 && lh <= fHeight) {
			fLogicalW = lw;
			fLogicalH = lh;
		}
		/* sweetfb_gpu=1: full-screen frames go through the Mali instead
		 * (herolte_gpu.c), bilinear; scaledCopy() stays the fallback. */
		int gpu = 0;
		PE_parse_boot_argn("sweetfb_gpu", &gpu, sizeof(gpu));
		fGpu = gpu != 0;
	}
#elif !defined(ARM64_BOARD_CONFIG_A5XELTE)
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
#endif
	fOffX = fScaled ? 0 : (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = fScaled ? 0 : (int32_t)(fLogicalH - fHeight) / 2;
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	/* The default surface must have the presented size (a mismatch left
	 * every swap layer empty). Scaled, it is a buffer of its own, presented
	 * like any other: CoreAnimation double-buffers between it and a surface
	 * of its own, and when it was the panel's top-left part every other
	 * frame landed on the panel unscaled (iPadOS 18 boot52: the doubled
	 * band above the scaled picture). */
	fSurface = fScaled ? sweet_surface_create_buffer(fLogicalW, fLogicalH, SWEET_FB_FORMAT_BGRA) :
	    sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL && !fScaled ? fSurface->kernelAddress() : 0;
	/* Scaled, scaledCopy() writes the whole panel, so it needs a mapping of
	 * all of it (with the panel's top-left part as the default surface, the
	 * lower rows went past its end: black lower third and stray writes, seen
	 * on the S9). */
	if (fScaled && fSurface != NULL) {
		SweetSurface *panel = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes,
		    SWEET_FB_FORMAT_BGRA);
		fPixels = panel != NULL ? panel->kernelAddress() : 0;
		IOLog("SWEET FB: scaled: panel mapping %ux%u for the scaler\n", fWidth, fHeight);
	}
	fWorkLoop = IOWorkLoop::workLoop();
	sweet_fb_scan_base = fPixels;
	sweet_fb_scan_width = fWidth;
	sweet_fb_scan_height = fHeight;
	sweet_fb_scan_rowbytes = fRowBytes;
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 10;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* A5's 1080x1920 panel copies 8.29 MB per full frame. Keep the
	 * donor's 10 Hz default while allowing 30 Hz on this board. */
	hz = 30;
#endif
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	PE_parse_boot_argn("a5fb_hz", &hz, sizeof(hz));
	int perf = 1;
	PE_parse_boot_argn("a5fb_perf", &perf, sizeof(perf));
	fPerfLog = perf != 0;
	fPerfFrames = fPerfBytes = fPerfNS = fPerfMaxNS = 0;
	fPerfWindowStart = mach_absolute_time();
#endif
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	if (hz == 0) {
		hz = 30;
	} else if (hz > 60) {
		hz = 60;
	}
	/* Round the tick interval up so 31-59 Hz never silently means 60 Hz. */
	fFlushTicks = (60 + hz - 1) / hz;
#else
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
#endif
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	sweet_fb_copy_init();
#endif
	fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	PE_parse_boot_argn("sweetfb_cav", &sweet_fb_cav, sizeof(sweet_fb_cav));
	{
		/* also on with boot-arg sweetspeed&2048 */
		if ((sweet_speed_flags() & 2048) != 0) {
			sweet_fb_cav = 1;
		}
	}
	if (sweet_fb_cav) {
		uint64_t step = 0;
		nanoseconds_to_absolutetime(16666667ULL, &step);
		OSDictionary *refresh = OSDictionary::withCapacity(2);
		OSNumber *interval = OSNumber::withNumber((4294967296ULL + 30) / 60, 32);  /* 1/60 s, 2^-32 s units */
		OSNumber *ticks = OSNumber::withNumber(step, 32);
		if (refresh != NULL && interval != NULL && ticks != NULL) {
			refresh->setObject("displayMinRefreshInterval", interval);
			refresh->setObject("displayRefreshStepMachTime", ticks);
			setProperty("IOMFBDisplayRefresh", refresh);
		}
		OSSafeReleaseNULL(interval);
		OSSafeReleaseNULL(ticks);
		OSSafeReleaseNULL(refresh);
		IOLog("SWEET FB CAV: sweetfb_cav=1: IOMFBDisplayRefresh published (step %llu ticks), rate log on\n", step);
	}
	{
		/* with async GPU submits (sweetspeed&64) the GPU present waits for the app's chain
		 * itself (starlte_gpu.c sg_run): tell hakios-gl not to glFinish every frame. */
		if ((sweet_speed_flags() & 64) != 0) {
			setProperty("HakiosWaitsForGPU", true);
		}
	}
	PE_parse_boot_argn("sweetfb_relbuf", &sweet_fb_relbuf, sizeof(sweet_fb_relbuf));
	if (PE_parse_boot_argn("sweetfb_frameinfo", &sweet_fb_fi, sizeof(sweet_fb_fi))) {
#if !defined(ARM64_BOARD_CONFIG_STARLTE)
		sweet_fb_fi &= ~(1u << SFI_KEY_FENCE);  /* no GPU completion time known here */
#endif
		sweet_fb_fi = sfi_on(sweet_fb_fi);
		IOLog("SWEET FB: sweetfb_frameinfo=%u: frame info offered, %u key(s)\n", sweet_fb_fi, sfi_count(sweet_fb_fi));
	}
	PE_parse_boot_argn("sweetfb_prof", &sweet_fb_prof, sizeof(sweet_fb_prof));
	PE_parse_boot_argn("sweetfb_trace", &sweet_fb_trace, sizeof(sweet_fb_trace));
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (PE_parse_boot_argn("sweetfb_nowait", &sweet_fb_nowait, sizeof(sweet_fb_nowait)) && sweet_fb_nowait != 0) {
		sweet_fb_nowait = 1;
		IOLog("A5XELTE FB: sweetfb_nowait=1: a tick does not sleep for a frame's GPU chains; a frame not done waits for a "
		    "later tick\n");
	}
	{
		uint32_t dpp = 0;
		if (PE_parse_boot_argn("sweetfb_dpp", &dpp, sizeof(dpp)) && dpp == 1) {
			sweet_fb_dpp_want = 1;
			IOLog("A5XELTE FB dpp: sweetfb_dpp=1: frames of %ux%u to be scaled by the DPU's VGF0 (set up at the first "
			    "frame the GPU presents; kern.hakios.sweetfb_dpp=0 goes back)\n", fLogicalW, fLogicalH);
		}
		sweet_fb_dp.rearm_seen = sweet_fb_dpp_rearm;
		sweet_fb_dp.front = sweet_fb_dp.pending = -1;
		sweet_fb_dp.quiet = SWEET_FB_DPP_QUIET;
	}
#endif
	if (sweet_fb_relbuf) {
		setProperty("SupportRelbufInfoCb", 1ULL, 32);
		IOLog("SWEET FB: sweetfb_relbuf=1: SupportRelbufInfoCb published, relbuf info sent per swap\n");
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	sweet_fb_sbx = sweet_speed_flags();
	hakios_s6e3ha8_init();
#endif
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	IOLog("A5XELTE FB: requested %u Hz, effective %u Hz, copy timing %s\n",
	    hz, 60 / fFlushTicks, fPerfLog ? "on" : "off");
#endif
	return true;
}

/*
 * The panel's memory for the USB console's "shot" (drivers/usb/hakios.dwc3.c): this board has no vinfo for it to fall
 * back on (the shot failed with "a picture of 0x0"). 1 ("the shown buffer"), or 0 before the framebuffer has started.
 */
extern "C" int hakios_fb_scanned(uintptr_t *base, uint32_t *width, uint32_t *height, uint32_t *rowbytes, unsigned *index);
extern "C" int
hakios_fb_scanned(uintptr_t *base, uint32_t *width, uint32_t *height, uint32_t *rowbytes, unsigned *index)
{
	if (sweet_fb_scan_base == 0) {
		return 0;
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* sweetfb_dpp: the buffer VGF0 reads (the one a flip waited for, once the DECON has taken it); read unlocked */
	if (sweet_fb_dp.attached && sweet_fb_dp.kva != 0) {
		int pending = sweet_fb_dp.pending, front = sweet_fb_dp.front;
		int shown = pending >= 0 && hakios_scanout_latched() ? pending : front;
		if (shown >= 0 && shown < SWEET_FB_DPP_BUFFERS) {
			*base = sweet_fb_dp.kva + (uintptr_t)(sweet_fb_dp.step * (uint64_t)shown);
			*width = sweet_fb_dp.w;
			*height = sweet_fb_dp.h;
			*rowbytes = sweet_fb_dp.rowbytes;
			*index = (unsigned)shown;
			return 2;
		}
	}
#endif
	*base = sweet_fb_scan_base;
	*width = sweet_fb_scan_width;
	*height = sweet_fb_scan_height;
	*rowbytes = sweet_fb_scan_rowbytes;
	*index = 0;
	return 1;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	if (sweet_fb_cav && (count % 60) == 0) {
		uint32_t n = __atomic_exchange_n(&sweet_fb_cav_swaps, 0, __ATOMIC_RELAXED);
		IOLog("SWEET FB CAV: %u swaps/s, CA power %s\n", n,
		    sweet_fb_cav_power < 0 ? "?" : sweet_fb_cav_power ? "on" : "OFF (CA clamps to 10 Hz)");
	}
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	if (fb->fPowerOff) {
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		if (step == 0 && sweet_fb_dp.attached) {
			/* the fade is drawn into the boot framebuffer: the DECON back on it first, with the picture shown */
			fb->dppLeave("the display off", true, SWEET_FB_DPP_LEAVE_AGAIN);
		}
#endif
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
		}
		sender->setTimeoutUS(SWEET_FB_VSYNC_US);
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (sweet_fb_trace && !sweet_fb_tr_dumped) {
		sweet_fb_trace_event('T', starlte_gpu_user_pending());
		uint64_t since = 0;
		if (starlte_touch_script_t0 != 0) {
			absolutetime_to_nanoseconds(now - starlte_touch_script_t0, &since);
			if (since >= (uint64_t)sweet_fb_trace * NSEC_PER_SEC) {
				sweet_fb_trace_dump();
			}
		}
	}
#endif
	uint64_t pf_stamp = 0;
	if (sweet_fb_prof) {
		if (sweet_fb_pf.last_tick_at != 0) {
			uint64_t d = now - sweet_fb_pf.last_tick_at;
			sweet_fb_pf.ticks++;
			sweet_fb_pf.period_ns += d;
			if (d > sweet_fb_pf.period_max) {
				sweet_fb_pf.period_max = d;
			}
		}
		sweet_fb_pf.last_tick_at = now;
		pf_stamp = sweet_fb_pf_stamp;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		if (!flush && sweet_fb_pf.last_shown_tick != 0 && count - sweet_fb_pf.last_shown_tick <= 6) {
			unsigned depth = starlte_gpu_user_pending();
			if (sweet_fb_pf_inswap > 0) {
				sweet_fb_pf.empty_inswap++;
			} else if (depth != 0) {
				sweet_fb_pf.empty_gpu++;
				sweet_fb_pf.empty_depth += depth;
			} else {
				sweet_fb_pf.empty_idle++;
			}
		}
#endif
	}
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	/* bsd/dev/herolte_gpu.c holds the panel for its GPU-drawn test frames. */
	if (flush && now < sweet_fb_hold_until) {
		flush = false;
	}
#endif
	uint8_t *rec = NULL;
	uint64_t recSeq = 0;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (flush && sweet_fb_nowait) {
		/* sweetfb_nowait: taken only when its chains are done (or for the old path: see hakios.sweetfb_present.h) */
		uint32_t pid, idle_before = fb->fPres.idle_fallbacks;
		uint64_t pseq;
		enum sfp_action act = sfp_tick(&fb->fPres, starlte_gpu_user_done(), starlte_gpu_user_pending() == 0, &pid, &pseq);
		flush = act == SFP_SHOW || act == SFP_FALLBACK;
		if (fb->fPres.idle_fallbacks != idle_before && !fb->fPres.warned) {
			fb->fPres.warned = true;
			IOLog("A5XELTE FB nowait: SELF-CHECK: swap %u (GPU chain %llu) not seen done after %u ticks with the GPU idle (done "
			    "%llu): taken by the old path; every such frame is counted on the nowait line\n", pid, pseq, SFP_IDLE_TICKS,
			    starlte_gpu_user_done());
		}
	}
#endif
	if (flush) {
		rec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_MAX);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_MAX);
			recSeq = fb->fPendingSeq;
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	uint64_t fi_fence = 0;  /* sweetfb_frameinfo: when this frame's chains were seen done */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	{
		/* kern.hakios.sweetfb_dpp: off at the next tick; a write of 1 lets a DPP that gave itself up try again */
		uint32_t rearm = __atomic_load_n(&sweet_fb_dpp_rearm, __ATOMIC_ACQUIRE);
		if (rearm != sweet_fb_dp.rearm_seen) {
			sweet_fb_dp.rearm_seen = rearm;
			sweet_fb_dp.given_up = false;
			sweet_fb_dp.quiet = SWEET_FB_DPP_QUIET;
			sweet_fb_dp.retries = 0;
		}
		if (sweet_fb_dp.attached && __atomic_load_n(&sweet_fb_dpp_want, __ATOMIC_ACQUIRE) == 0) {
			fb->dppLeave("switched off (kern.hakios.sweetfb_dpp=0)", true, SWEET_FB_DPP_LEAVE_OFF);
		}
	}
	if (rec != NULL) {
		sweet_fb_trace_event('F', sweet_fb_swap_id(rec));
	}
	if (rec != NULL && recSeq != 0) {
		/* asleep, not spinning, until this frame's own chains are done (not the next frame's, queued since) */
		(void)starlte_gpu_user_wait_seq(recSeq, true);
	}
	if (rec != NULL) {
		sweet_fb_trace_event('G', 0);
		fi_fence = sweet_fb_fi ? mach_continuous_time() : 0;
	}
	uint64_t pf_woke = sweet_fb_prof ? mach_absolute_time() : 0;
#else
	(void)recSeq; (void)fi_fence; uint64_t pf_woke = sweet_fb_prof ? mach_absolute_time() : 0; /* FRANKEL (generic boards): as STARLTE */
#endif
	if (rec != NULL) {
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		uint64_t before = fb->fPerfLog ? mach_absolute_time() : 0;
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		sweet_fb_dp.path = 0;
#endif
		uint64_t bytes = fb->present(rec);
		sweet_fb_frames_shown++;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		if (bytes != 0) {
			if (sweet_fb_dp.path == 2) {
				sweet_fb_dp.by_dpp++;
			} else if (sweet_fb_dp.path == 1) {
				sweet_fb_dp.by_gpu++;
			} else {
				sweet_fb_dp.by_cpu++;
			}
		}
#endif
		sweet_fb_trace_event('P', 0);
		if (sweet_fb_fi) {
			fb->frameInfo(sweet_fb_swap_id(rec), mach_continuous_time(), fi_fence);
		}
		if (sweet_fb_relbuf) {
			fb->relbuf(sweet_fb_swap_id(rec));
		}
		if (sweet_fb_prof) {
			uint64_t done = mach_absolute_time();
			sweet_fb_pf.frames++;
			sweet_fb_pf.last_shown_tick = count;
			sweet_fb_pf.slept_ns += pf_woke - now;
			if (pf_woke - now > sweet_fb_pf.slept_max) {
				sweet_fb_pf.slept_max = pf_woke - now;
			}
			sweet_fb_pf.present_ns += done - pf_woke;
			if (pf_stamp != 0 && done > pf_stamp) {
				sweet_fb_pf.shown_ns += done - pf_stamp;
				if (done - pf_stamp > sweet_fb_pf.shown_max) {
					sweet_fb_pf.shown_max = done - pf_stamp;
				}
			}
		}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		if (fb->fPerfLog) {
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - before, &ns);
			fb->fPerfFrames++;
			{
				/* sweetspeed&16384: vsync ticks between presented frames (1 = 60 fps, 2 = one tick missed, 7+ = idle) */
				static uint64_t last_tick;
				uint64_t gap = count - last_tick;
				last_tick = count;
				if ((sweet_fb_sbx & 16384) != 0) {
					sweet_fb_gaps[gap <= 1 ? 0 : gap == 2 ? 1 : gap == 3 ? 2 : gap <= 6 ? 3 : 4]++;
				}
			}
			fb->fPerfBytes += bytes;
			fb->fPerfNS += ns;
			if (ns > fb->fPerfMaxNS) {
				fb->fPerfMaxNS = ns;
			}
		}
#else
		(void)bytes;
#endif
		IOFreeData(rec, SWEET_FB_SWAP_REC_MAX);
	}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Aggregate after 300 callbacks; measure wall time because expensive
	 * copies can delay the timer and reduce the actual frame rate. */
	if (fb->fPerfLog && (count % 300) == 0) {
		uint64_t elapsedNS = 0;
		uint64_t finished = mach_absolute_time();
		absolutetime_to_nanoseconds(finished - fb->fPerfWindowStart, &elapsedNS);
		if (fb->fPerfFrames != 0 && elapsedNS != 0) {
			uint64_t fpsTenths = fb->fPerfFrames * 10000000000ULL / elapsedNS;
			IOLog("A5XELTE FB: frames=%llu elapsed_ms=%llu fps=%llu.%llu bytes=%llu avg_copy_us=%llu max_copy_us=%llu\n",
			    fb->fPerfFrames, elapsedNS / 1000000, fpsTenths / 10,
			    fpsTenths % 10, fb->fPerfBytes,
			    fb->fPerfNS / fb->fPerfFrames / 1000, fb->fPerfMaxNS / 1000);
			if ((sweet_fb_sbx & 16384) != 0) {
				IOLog("A5XELTE FB gaps: 1:%u 2:%u 3:%u 4-6:%u 7+:%u\n", sweet_fb_gaps[0], sweet_fb_gaps[1], sweet_fb_gaps[2],
				    sweet_fb_gaps[3], sweet_fb_gaps[4]);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
				if (sweet_fb_nowait) {
					struct sfp *ps = &fb->fPres;
					IOLog("A5XELTE FB nowait: frames shown done %u; ticks that left a frame for a later one (its chains not "
					    "done) %u, frames a newer one replaced meanwhile %u; taken by the old path: GPU slow %u, SELF-CHECK %u\n",
					    ps->shown, ps->deferred, ps->replaced, ps->fallbacks, ps->idle_fallbacks);
					ps->shown = ps->deferred = ps->replaced = ps->fallbacks = ps->idle_fallbacks = 0;
				}
#endif
			}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
			if (sweet_fb_dpp_want || sweet_fb_dp.tried) {
				/* sweetfb_dpp: which way the frames went, the flips, the waits for the flip before, and SwapWait */
				uint32_t refreshes = hakios_scanout_refreshes();
				uint64_t wait_ns = 0, wait_max_ns = 0;
				absolutetime_to_nanoseconds(sweet_fb_dp.wait_abs, &wait_ns);
				absolutetime_to_nanoseconds(sweet_fb_dp.wait_max, &wait_max_ns);
				IOLog("A5XELTE FB dpp: %s%s%s; frames by the DPP %u, by the GPU's scaled copy %u, by the CPU %u; flips %u, "
				    "%u found the flip before not yet taken (waited avg %llu us, max %llu us); DECON refreshes %u; left %u, "
				    "attached %u; SwapWait %u (for the last swap %u, the one before %u, other %u); swap surfaces %u (%u %u %u %u, "
				    "%u more), the same as the swap before's %u\n",
				    sweet_fb_dp.attached ? "on, VGF0 scales" : sweet_fb_dp.given_up ? "given up: " : "off",
				    !sweet_fb_dp.attached && sweet_fb_dp.why != NULL && !sweet_fb_dp.given_up ? ", last left: " : "",
				    !sweet_fb_dp.attached && sweet_fb_dp.why != NULL ? sweet_fb_dp.why : "",
				    sweet_fb_dp.by_dpp, sweet_fb_dp.by_gpu, sweet_fb_dp.by_cpu, sweet_fb_dp.flips, sweet_fb_dp.waited,
				    sweet_fb_dp.waited ? wait_ns / sweet_fb_dp.waited / 1000 : 0, wait_max_ns / 1000,
				    refreshes - sweet_fb_dp.refreshes_at, sweet_fb_dp.left, sweet_fb_dp.attaches, sweet_fb_dp.sw_calls,
				    sweet_fb_dp.sw_last, sweet_fb_dp.sw_before, sweet_fb_dp.sw_other, sweet_fb_dp.sf_count,
				    sweet_fb_dp.sf_ids[0], sweet_fb_dp.sf_ids[1], sweet_fb_dp.sf_ids[2], sweet_fb_dp.sf_ids[3], sweet_fb_dp.sf_more,
				    sweet_fb_dp.sf_same);
				sweet_fb_dp.refreshes_at = refreshes;
				bzero(sweet_fb_dp.sf_ids, sizeof(sweet_fb_dp.sf_ids));
				sweet_fb_dp.sf_count = sweet_fb_dp.sf_more = sweet_fb_dp.sf_same = 0;
				sweet_fb_dp.by_dpp = sweet_fb_dp.by_gpu = sweet_fb_dp.by_cpu = sweet_fb_dp.flips = sweet_fb_dp.waited = 0;
				sweet_fb_dp.left = sweet_fb_dp.attaches = 0;
				sweet_fb_dp.wait_abs = sweet_fb_dp.wait_max = 0;
				sweet_fb_dp.sw_calls = sweet_fb_dp.sw_last = sweet_fb_dp.sw_before = sweet_fb_dp.sw_other = 0;
			}
#endif
			if (sweet_fb_prof) {
				uint64_t v[9] = { sweet_fb_pf.held_ns, sweet_fb_pf.held_max, sweet_fb_pf.slept_ns, sweet_fb_pf.slept_max,
					          sweet_fb_pf.present_ns, sweet_fb_pf.shown_ns, sweet_fb_pf.shown_max, sweet_fb_pf.period_ns,
					          sweet_fb_pf.period_max };
				for (unsigned i = 0; i < 9; i++) {
					absolutetime_to_nanoseconds(v[i], &v[i]);
					v[i] /= 1000;
				}
				uint64_t sw = sweet_fb_pf.swaps ? sweet_fb_pf.swaps : 1, fr = sweet_fb_pf.frames ? sweet_fb_pf.frames : 1;
				uint64_t tk = sweet_fb_pf.ticks ? sweet_fb_pf.ticks : 1;
				IOLog("A5XELTE FB prof: %llu swaps held avg %llu us (max %llu) | %llu frames: tick slept for the GPU avg %llu us "
				    "(max %llu), present avg %llu us, swap to shown avg %llu us (max %llu)\n",
				    sweet_fb_pf.swaps, v[0] / sw, v[1], sweet_fb_pf.frames, v[2] / fr, v[3], v[4] / fr, v[5] / fr, v[6]);
				IOLog("A5XELTE FB prof2: ticks avg %llu us (max %llu); with no new frame: in a swap call %u, "
				    "drawing (GPU depth avg %llu) %u, nothing on the GPU %u\n",
				    v[7] / tk, v[8], sweet_fb_pf.empty_inswap,
				    sweet_fb_pf.empty_gpu ? sweet_fb_pf.empty_depth / sweet_fb_pf.empty_gpu : 0, sweet_fb_pf.empty_gpu,
				    sweet_fb_pf.empty_idle);
				uint64_t keep_tick = sweet_fb_pf.last_tick_at, keep_shown = sweet_fb_pf.last_shown_tick;
				bzero(&sweet_fb_pf, sizeof(sweet_fb_pf));
				sweet_fb_pf.last_tick_at = keep_tick;
				sweet_fb_pf.last_shown_tick = keep_shown;
			}
		}
		bzero(sweet_fb_gaps, sizeof(sweet_fb_gaps));
		fb->fPerfFrames = fb->fPerfBytes = fb->fPerfNS = fb->fPerfMaxNS = 0;
		fb->fPerfWindowStart = finished;
	}
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* boot-arg sweetspeed&1024: a fixed 60 Hz cadence. Re-arming 16.7 ms after the present (as below) made the
	 * tick ~19-20 ms plus timer slack, so backboardd's 41-52 swaps/s while swiping showed as 33-36 frames. */
	if ((sweet_fb_sbx & 1024) != 0) {
		static uint64_t interval, next;
		uint64_t t = mach_absolute_time();
		if (interval == 0) {
			nanoseconds_to_absolutetime(16666667ULL, &interval);
		}
		next = next == 0 ? t + interval : next + interval;
		if (next <= t + interval / 8) {
			next = t + interval / 8;        /* late (a long present): no burst, just the next one soon */
		}
		sender->wakeAtTime(next);
		return;
	}
#endif
	sender->setTimeoutUS(SWEET_FB_VSYNC_US);
}

void
AppleCLCD::relbuf(uint32_t swapId)
{
	uint64_t now = mach_absolute_time();
	uint64_t values[5] = { swapId, (uint32_t)now, (uint32_t)(now >> 32), (uint32_t)now, (uint32_t)(now >> 32) };
	IOLockLock(fLock);
	for (unsigned int i = 0; i < fClients->getCount(); i++) {
		((IOMobileFramebufferUserClient *)fClients->getObject(i))->notify(SWEET_FB_NOTIFY_RELBUF, values, 5);
	}
	IOLockUnlock(fLock);
	if (++sweet_fb_relbuf_sent <= 3 || (sweet_fb_relbuf_sent % 1000) == 0) {
		IOLog("SWEET FB: relbuf info for swap %u (%u sent)\n", swapId, sweet_fb_relbuf_sent);
	}
}

/* sweetfb_frameinfo: a shown swap's frame info (type 3) to each client that enabled a key (hakios.sweetfb_frameinfo.h) */
void
AppleCLCD::frameInfo(uint32_t swapId, uint64_t presented, uint64_t fence)
{
	struct sfi_frame f;
	f.swap_id = swapId;
	f.value[SFI_KEY_PRESENTED] = presented;
	f.value[SFI_KEY_FENCE] = fence;
	uint64_t values[SFI_ARGS], said = 0;
	uint32_t n, sent = 0;
	IOLockLock(fLock);
	for (unsigned int i = 0; i < fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *c = (IOMobileFramebufferUserClient *)fClients->getObject(i);
		n = sfi_message(sweet_fb_fi, c->fFIMask, &f, values);
		if (n != 0) {
			c->notify(SFI_TYPE, values, n);
			said = values[0];
			sent = n;
		}
	}
	IOLockUnlock(fLock);
	if (sent != 0 && (++sweet_fb_fi_sent <= 3 || (sweet_fb_fi_sent % 1000) == 0)) {
		IOLog("SWEET FB: frame info for swap %u: mask %#llx, %u values (%u sent)\n", swapId, said, sent - 2,
		    sweet_fb_fi_sent);
	}
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;
static uint64_t sweet_fb_swap_count;
/*
 * sweetfb_relbuf=1 (iOS 27 on STARLTE): publish "SupportRelbufInfoCb" = 1 and send IOMFB's
 * relbuf-info notification (type 7: swap id, release time, release time) for every swap as soon
 * as its surfaces are no longer needed: after present() copied/scaled them to the panel, or when
 * a newer swap replaced it before it was shown. iOS 27's CoreAnimation (IOMFBServer::
 * relbuf_info_callback / IOMFBDisplay::wait_for_relbuf_info) paces frames on these; without the
 * property IOMobileFramebufferRelbufInfo refuses (0xe00002c7) and backboardd swapped about once
 * every 5 s.
 */
static uint32_t
sweet_fb_swap_id(const uint8_t *rec)
{
	const uint32_t sh = get32(rec, 0x98) != 0 && get32(rec, 0x50) == 0 ? 0x48 : 0;
	return get32(rec, 0x50 + sh);
}

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
/*
 * A full 1440x2560 frame is 14.7 MB into write-combined memory: 10-20 ms
 * on one core (a5fb_perf). Helper threads copy row bands next to the
 * caller. Boot-arg sweetfb_copythreads=N helpers (default 3, 0 = off).
 */
#define SWEET_FB_COPY_MAX_HELPERS 7
struct sweet_fb_copy_job {
	uintptr_t       dst;
	uintptr_t       src;
	uint64_t        dstStride, srcStride;
	size_t          rowBytes;
	int32_t         rows;
};
static IOLock *sweet_fb_copy_lock;
static struct sweet_fb_copy_job sweet_fb_copy;
static uint32_t sweet_fb_copy_helpers;
static uint64_t sweet_fb_copy_generation;
static uint32_t sweet_fb_copy_pending;

static void
sweet_fb_copy_band(const struct sweet_fb_copy_job *job, uint32_t band, uint32_t bands)
{
	int32_t first = (int32_t)((int64_t)job->rows * band / bands);
	int32_t last = (int32_t)((int64_t)job->rows * (band + 1) / bands);
	for (int32_t y = first; y < last; y++) {
		memcpy((void *)(job->dst + (uint64_t)y * job->dstStride),
		    (const void *)(job->src + (uint64_t)y * job->srcStride), job->rowBytes);
	}
}

static void
sweet_fb_copy_helper(void *arg, wait_result_t wr __unused)
{
	uint32_t band = (uint32_t)(uintptr_t)arg;
	uint64_t seen = 0;
	for (;;) {
		IOLockLock(sweet_fb_copy_lock);
		while (sweet_fb_copy_generation == seen) {
			IOLockSleep(sweet_fb_copy_lock, &sweet_fb_copy_generation, THREAD_UNINT);
		}
		seen = sweet_fb_copy_generation;
		struct sweet_fb_copy_job job = sweet_fb_copy;
		IOLockUnlock(sweet_fb_copy_lock);

		sweet_fb_copy_band(&job, band, sweet_fb_copy_helpers + 1);

		IOLockLock(sweet_fb_copy_lock);
		if (--sweet_fb_copy_pending == 0) {
			IOLockWakeup(sweet_fb_copy_lock, &sweet_fb_copy_pending, false);
		}
		IOLockUnlock(sweet_fb_copy_lock);
	}
}

static void
sweet_fb_copy_init(void)
{
	uint32_t helpers = 3;
	PE_parse_boot_argn("sweetfb_copythreads", &helpers, sizeof(helpers));
	if (helpers > SWEET_FB_COPY_MAX_HELPERS) {
		helpers = SWEET_FB_COPY_MAX_HELPERS;
	}
	sweet_fb_copy_lock = IOLockAlloc();
	if (sweet_fb_copy_lock == NULL) {
		return;
	}
	uint32_t started = 0;
	for (uint32_t i = 0; i < helpers; i++) {
		thread_t thread;
		/* Band 0 is the caller's; helper i copies band i + 1. */
		if (kernel_thread_start(&sweet_fb_copy_helper, (void *)(uintptr_t)(i + 1), &thread) != KERN_SUCCESS) {
			break;
		}
		thread_deallocate(thread);
		started++;
	}
	sweet_fb_copy_helpers = started;
	IOLog("A5XELTE FB: parallel copy with %u helper threads\n", started);
}

/* Copies rows; returns after every band is done. */
static void
sweet_fb_copy_rows(uintptr_t dst, uint64_t dstStride, uintptr_t src, uint64_t srcStride,
    size_t rowBytes, int32_t rows)
{
	struct sweet_fb_copy_job job = { dst, src, dstStride, srcStride, rowBytes, rows };
	uint32_t helpers = sweet_fb_copy_helpers;
	if (helpers == 0 || rows < 64) {
		sweet_fb_copy_band(&job, 0, 1);
		return;
	}
	IOLockLock(sweet_fb_copy_lock);
	sweet_fb_copy = job;
	sweet_fb_copy_pending = helpers;
	sweet_fb_copy_generation++;
	IOLockWakeup(sweet_fb_copy_lock, &sweet_fb_copy_generation, false);
	IOLockUnlock(sweet_fb_copy_lock);

	sweet_fb_copy_band(&job, 0, helpers + 1);

	IOLockLock(sweet_fb_copy_lock);
	while (sweet_fb_copy_pending != 0) {
		IOLockSleep(sweet_fb_copy_lock, &sweet_fb_copy_pending, THREAD_UNINT);
	}
	IOLockUnlock(sweet_fb_copy_lock);
}
#endif

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg sweetfb_hz,
 * default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec, uint32_t size)
{
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (size >= 0x366) {
		sweet_fb_swap_brightness(rec);
	}
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* with the GPU's chain queue (sweetspeed&262144): the frame is drawn once every chain queued by now is done */
	uint64_t seq = starlte_gpu_user_seq();
	/*
	 * And at most two frames are in flight: this swap returns once the chains of the swap before it are done. The
	 * window server draws the next frame while the GPU works on this one (the overlap the queue is for), but it
	 * cannot run further ahead: with nothing to hold it, a scene the GPU needs more than a refresh for would fill
	 * the queue (256 chains, a dozen frames) and every touch would be answered that many frames late.
	 */
	static uint64_t prevSwapSeq;
	uint64_t prev = __atomic_exchange_n(&prevSwapSeq, seq, __ATOMIC_RELAXED);
	uint64_t pf_t0 = sweet_fb_prof ? mach_absolute_time() : 0;
	sweet_fb_trace_event('S', starlte_gpu_user_pending());
	if (sweet_fb_prof) {
		(void)__atomic_add_fetch(&sweet_fb_pf_inswap, 1, __ATOMIC_RELAXED);
	}
	if (prev != 0 && prev != seq) {
		(void)starlte_gpu_user_wait_seq(prev, true);
	}
	if (sweet_fb_prof) {
		uint64_t pf_t1 = mach_absolute_time();
		(void)__atomic_sub_fetch(&sweet_fb_pf_inswap, 1, __ATOMIC_RELAXED);
		sweet_fb_pf.swaps++;                    /* (one window server: its swaps do not race each other) */
		sweet_fb_pf.held_ns += pf_t1 - pf_t0;
		if (pf_t1 - pf_t0 > sweet_fb_pf.held_max) {
			sweet_fb_pf.held_max = pf_t1 - pf_t0;
		}
		sweet_fb_pf_stamp = pf_t1;
	}
#else
	uint64_t seq = 0;
#endif
	sweet_fb_trace_event('R', sweet_fb_swap_id(rec));
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	__atomic_store_n(&sweet_fb_dp.last_swap, sweet_fb_swap_id(rec), __ATOMIC_RELAXED);
	if (sweet_fb_dpp_want || sweet_fb_dp.tried) {
		/* (one window server: its swaps do not race each other) */
		const uint32_t sh = get32(rec, 0x98) != 0 && get32(rec, 0x50) == 0 ? 0x48 : 0;
		uint32_t id0 = (get32(rec, 0x104 + sh) & 1u) ? get32(rec, 0x54 + sh) : 0;
		if (id0 != 0) {
			unsigned k;
			for (k = 0; k < sweet_fb_dp.sf_count && sweet_fb_dp.sf_ids[k] != id0; k++) {
			}
			if (k == sweet_fb_dp.sf_count) {
				if (k < 4) {
					sweet_fb_dp.sf_ids[sweet_fb_dp.sf_count++] = id0;
				} else {
					sweet_fb_dp.sf_more++;
				}
			}
			if (id0 == sweet_fb_dp.sf_prev) {
				sweet_fb_dp.sf_same++;
			}
			sweet_fb_dp.sf_prev = id0;
		}
	}
#endif
	IOLockLock(fLock);
	uint32_t replaced = fDirty ? sweet_fb_swap_id(fPending) : 0;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (sweet_fb_nowait) {
		(void)sfp_swap(&fPres, sweet_fb_swap_id(rec), seq);
	}
#endif
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_MAX);
	fPendingSeq = seq;
	fDirty = true;
	IOLockUnlock(fLock);
	if (sweet_fb_relbuf && replaced != 0) {
		relbuf(replaced);
	}
#if defined(ARM64_BOARD_CONFIG_HEROLTE) || defined(ARM64_BOARD_CONFIG_STARLTE)
	/* Raw record of the first swaps: offset=value of every non-zero word
	 * (STARLTE: iPadOS 18's record is 0x514 bytes with a new layout). */
	static uint32_t dumped;
	bool has_surface = false;
	for (uint32_t l = 0; l < 4; l++) {
		if (get32(rec, 0x54 + 4 * l) != 0 || get32(rec, 0x9c + 4 * l) != 0) {
			has_surface = true;
		}
	}
	if (has_surface && dumped < 8) {
		char line[160];
		int n = 0;
		dumped++;
		IOLog("HEROLTE FB: rec %llu size 0x%x\n", sweet_fb_swap_count, size);
		for (uint32_t off = 0; off + 4 <= size; off += 4) {
			uint32_t v = get32(rec, off);
			if (v == 0) {
				continue;
			}
			n += snprintf(line + n, sizeof(line) - n, " %x=%x", off, v);
			if (n > (int)sizeof(line) - 24) {
				IOLog("HEROLTE FB: rec %llu%s\n", sweet_fb_swap_count, line);
				n = 0;
			}
		}
		IOLog("HEROLTE FB: rec %llu%s\n", sweet_fb_swap_count, n ? line : " end");
	}
#endif
	if (sweet_fb_cav) {
		__atomic_fetch_add(&sweet_fb_cav_swaps, 1, __ATOMIC_RELAXED);
		static uint64_t lastBright = ~0ULL;
		static unsigned brightLines;
		if (size >= SWEET_FB_REC27_BRIGHT + 8 && rec[SWEET_FB_REC27_BRIGHT_SET] != 0) {
			uint64_t bits;
			memcpy(&bits, rec + SWEET_FB_REC27_BRIGHT, sizeof(bits));
			if (bits != lastBright && brightLines < 200) {
				brightLines++;
				lastBright = bits;
				IOLog("SWEET FB CAV: swap brightness (double bits) %#llx\n", bits);
			}
		}
	}
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

/*
 * sweetfb_scale: the logical rect (dx,dy,w,h) at src goes to panel pixels
 * x1.5, nearest neighbour (two source pixels become three). A panel row
 * whose source row equals the previous one is copied from it. Called only
 * from present(), which the vsync timer serializes.
 */
static bool sweet_fb_blend;	/* scaledCopy() of a layer above the first: blend, do not copy */

uint64_t
AppleCLCD::scaledCopy(uintptr_t src, uint32_t srcRowBytes, int32_t dx, int32_t dy,
    int32_t w, int32_t h)
{
	static uint16_t column[2048];
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* S9 1440x2960: the logical canvas keeps its aspect, scaled to the
	 * full height (or width), centred between black bars. */
	int32_t lw = (int32_t)fLogicalW, lh = (int32_t)fLogicalH;
	int32_t outW = (int32_t)((int64_t)lw * fHeight / lh), outH = (int32_t)fHeight;
	if (outW > (int32_t)fWidth) {
		outW = (int32_t)fWidth;
		outH = (int32_t)((int64_t)lh * fWidth / lw);
	}
	int32_t barX = ((int32_t)fWidth - outW) / 2, barY = ((int32_t)fHeight - outH) / 2;
	static bool barsCleared;
	if (!barsCleared) {
		barsCleared = true;
		memset((void *)fPixels, 0, (size_t)fRowBytes * fHeight);
	}
	int32_t spx0 = barX + (int32_t)((int64_t)dx * outW / lw);
	int32_t spx1 = barX + (int32_t)(((int64_t)(dx + w) * outW + lw - 1) / lw);
	int32_t spy0 = barY + (int32_t)((int64_t)dy * outH / lh);
	int32_t spy1 = barY + (int32_t)(((int64_t)(dy + h) * outH + lh - 1) / lh);
	if (spx1 > barX + outW) {
		spx1 = barX + outW;
	}
	if (spy1 > barY + outH) {
		spy1 = barY + outH;
	}
	int32_t spw = spx1 - spx0;
	if (spw <= 0 || spy1 <= spy0 || spw > (int32_t)(sizeof(column) / sizeof(column[0]))) {
		return 0;
	}
	for (int32_t x = 0; x < spw; x++) {
		int32_t c = (int32_t)((int64_t)(spx0 + x - barX) * lw / outW) - dx;
		column[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));
	}
	int32_t sprev = -1;
	uint32_t *sprevRow = NULL;
	for (int32_t py = spy0; py < spy1; py++) {
		int32_t r = (int32_t)((int64_t)(py - barY) * lh / outH) - dy;
		r = r < 0 ? 0 : (r >= h ? h - 1 : r);
		uint32_t *out = (uint32_t *)(fPixels + (uint64_t)py * fRowBytes + (uint64_t)spx0 * 4);
		if (sweet_fb_blend) {
			/* layers above the first (the pointer, overlays): premultiplied BGRA over what is
			 * there, so a transparent pixel leaves the frame below instead of a black box */
			const uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
			for (int32_t x = 0; x < spw; x++) {
				uint32_t v = in[column[x]], a = v >> 24;
				if (a == 0) {
					continue;
				}
				if (a == 255) {
					out[x] = v;
					continue;
				}
				uint32_t o = out[x], k = 255 - a, res = 0xff000000u;
				for (uint32_t sh = 0; sh < 24; sh += 8) {
					uint32_t c = ((v >> sh) & 0xff) + (((o >> sh) & 0xff) * k + 127) / 255;
					res |= (c > 255 ? 255 : c) << sh;
				}
				out[x] = res;
			}
			continue;
		}
		if (r == sprev && sprevRow != NULL) {
			memcpy(out, sprevRow, (size_t)spw * 4);
		} else {
			const uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
			for (int32_t x = 0; x < spw; x++) {
				out[x] = in[column[x]];
			}
			sprev = r;
		}
		sprevRow = out;
	}
	return (uint64_t)spw * (uint64_t)(spy1 - spy0) * 4;
#endif
	int32_t px0 = dx * 3 / 2, px1 = (dx + w) * 3 / 2;
	int32_t py0 = dy * 3 / 2, py1 = (dy + h) * 3 / 2;
	if (px1 > (int32_t)fWidth) {
		px1 = (int32_t)fWidth;
	}
	if (py1 > (int32_t)fHeight) {
		py1 = (int32_t)fHeight;
	}
	int32_t pw = px1 - px0;
	if (pw <= 0 || py1 <= py0 || pw > (int32_t)(sizeof(column) / sizeof(column[0]))) {
		return 0;
	}
	for (int32_t x = 0; x < pw; x++) {
		int32_t c = (px0 + x) * 2 / 3 - dx;
		column[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));
	}
	int32_t previous = -1;
	uint32_t *prevRow = NULL;
	for (int32_t py = py0; py < py1; py++) {
		int32_t r = py * 2 / 3 - dy;
		r = r < 0 ? 0 : (r >= h ? h - 1 : r);
		uint32_t *out = (uint32_t *)(fPixels + (uint64_t)py * fRowBytes + (uint64_t)px0 * 4);
		if (r == previous && prevRow != NULL) {
			memcpy(out, prevRow, (size_t)pw * 4);
		} else {
			const uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
			for (int32_t x = 0; x < pw; x++) {
				out[x] = in[column[x]];
			}
			previous = r;
		}
		prevRow = out;
	}
	return (uint64_t)pw * (uint64_t)(py1 - py0) * 4;
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* 0396: the panel pixels under the pointer (at most 320 x 320 panel pixels: the 128-point pointer at 1.65x) */
#define SWEET_SU_MAX 320
static uint32_t sweet_su_buf[SWEET_SU_MAX * SWEET_SU_MAX];
static int32_t sweet_su_x, sweet_su_y, sweet_su_w, sweet_su_h;
static bool sweet_su_have;
volatile int32_t sweet_fb_cur_x, sweet_fb_cur_y;
volatile uint32_t sweet_fb_cur_seq;

/* the panel rectangle scaledCopy() writes for logical (dx, dy, w, h) */
void
AppleCLCD::sweetSaveUnderRect(int32_t dx, int32_t dy, int32_t w, int32_t h)
{
	int32_t lw = (int32_t)fLogicalW, lh = (int32_t)fLogicalH;
	int32_t outW = (int32_t)((int64_t)lw * fHeight / lh), outH = (int32_t)fHeight;
	if (outW > (int32_t)fWidth) {
		outW = (int32_t)fWidth;
		outH = (int32_t)((int64_t)lh * fWidth / lw);
	}
	int32_t barX = ((int32_t)fWidth - outW) / 2, barY = ((int32_t)fHeight - outH) / 2;
	int32_t x0 = barX + (int32_t)((int64_t)dx * outW / lw);
	int32_t x1 = barX + (int32_t)(((int64_t)(dx + w) * outW + lw - 1) / lw);
	int32_t y0 = barY + (int32_t)((int64_t)dy * outH / lh);
	int32_t y1 = barY + (int32_t)(((int64_t)(dy + h) * outH + lh - 1) / lh);
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > barX + outW ? barX + outW : x1;
	y1 = y1 > barY + outH ? barY + outH : y1;
	sweet_su_x = x0;
	sweet_su_y = y0;
	sweet_su_w = x1 - x0 > SWEET_SU_MAX ? SWEET_SU_MAX : (x1 > x0 ? x1 - x0 : 0);
	sweet_su_h = y1 - y0 > SWEET_SU_MAX ? SWEET_SU_MAX : (y1 > y0 ? y1 - y0 : 0);
}

/* save (true) the panel pixels of the current rectangle, or put the saved ones back (false) */
void
AppleCLCD::sweetSaveUnder(bool save)
{
	if (sweet_su_w <= 0 || sweet_su_h <= 0) {
		sweet_su_have = false;
		return;
	}
	for (int32_t y = 0; y < sweet_su_h; y++) {
		uint32_t *panel = (uint32_t *)(fPixels + (uint64_t)(sweet_su_y + y) * fRowBytes + (uint64_t)sweet_su_x * 4);
		uint32_t *buf = &sweet_su_buf[y * SWEET_SU_MAX];
		if (save) {
			memcpy(buf, panel, (size_t)sweet_su_w * 4);
		} else {
			memcpy(panel, buf, (size_t)sweet_su_w * 4);
		}
	}
	sweet_su_have = save;
}
#endif

uint64_t
AppleCLCD::present(const uint8_t *rec)
{
	/* iPadOS 18 (IOMobileFramebuffer 0x514-byte record): the same fields
	 * 0x48 further in (swap id 0x98, surface ids 0x9c, enabled mask 0x14c);
	 * the record is zero past 0x46c on iOS 16, so the swap id tells. */
	const uint32_t sh = get32(rec, 0x98) != 0 && get32(rec, 0x50) == 0 ? 0x48 : 0;
	uint32_t mask = get32(rec, 0x104 + sh);
	uint64_t copied = 0;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (sweet_fb_dp.attached) {
		/* sweetfb_dpp takes a frame of one layer, layer 0: any other is drawn into the boot framebuffer as before */
		for (uint32_t layer = 1; layer < SWEET_FB_LAYERS; layer++) {
			if ((mask & (1u << layer)) && get32(rec, 0x54 + sh + 4 * layer) != 0) {
				dppLeave("a frame of more than one layer", true, SWEET_FB_DPP_LEAVE_FRAME);
				break;
			}
		}
	}
#endif


	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		if (PE_parse_boot_argn("a5fb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
#else
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
#endif
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			sweet_console_muted = 1;
		}
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + sh + 4 * layer);
		const uint8_t *a = rec + 0x64 + sh + 16 * layer;
		const uint8_t *b = rec + 0xc4 + sh + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50 + sh), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || (s == fSurface && !fScaled)) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		{
			/* sweetfbstats=1: what is on screen, without eyes on the panel. For a full-screen
			 * layer, at most once a second: a 16 x 16 grid of the surface's pixels (BGRA) ->
			 * how many are not black, the average colour, and a 16-character row picture. */
			static int on = -1;
			static uint64_t last;
			if (on < 0) {
				on = 0;
				PE_parse_boot_argn("sweetfbstats", &on, sizeof(on));
			}
			uint64_t now = mach_absolute_time(), ns = 0;
			absolutetime_to_nanoseconds(now - last, &ns);
			if (on && src && bpe == 4 && s->width >= 256 && s->height >= 256 && ns > 1000000000ULL) {
				last = now;
				uint32_t lit = 0;
				uint64_t r = 0, g = 0, bl = 0;
				char rows[16][17];
				for (uint32_t gy = 0; gy < 16; gy++) {
					for (uint32_t gx = 0; gx < 16; gx++) {
						uint32_t px = (gx * 2 + 1) * s->width / 32, py = (gy * 2 + 1) * s->height / 32;
						uint32_t v = *(const volatile uint32_t *)(src + (size_t)py * s->bytesPerRow + px * 4);
						uint32_t B = v & 0xff, G = (v >> 8) & 0xff, R = (v >> 16) & 0xff;
						uint32_t y = (R * 3 + G * 6 + B) / 10;
						lit += y > 16;
						r += R; g += G; bl += B;
						rows[gy][gx] = " .:-=+*#%@"[y * 10 / 256];
					}
					rows[gy][16] = 0;
				}
				IOLog("SWEET FBSTAT: swap %u surface %u %ux%u: %u/256 lit, avg rgb %llu,%llu,%llu\n",
				    get32(rec, 0x50 + sh), id, s->width, s->height, lit, r / 256, g / 256, bl / 256);
				for (uint32_t gy = 0; gy < 16; gy++) {
					IOLog("SWEET FBSTAT: |%s|\n", rows[gy]);
				}
			}
		}
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		{
			/* sweetfbdump=N: once, at the first full-screen present after N s of uptime, the
			 * layer at 1/6 scale as 12-bit colour (3 hex digits a pixel, R G B nibbles), 69
			 * pixels a line: "SWEET FBDUMP W H y x0 hex" (tools/macos/fbdump-png.py). */
			static int at = -1, done;
			if (at < 0) {
				at = 0;
				PE_parse_boot_argn("sweetfbdump", &at, sizeof(at));
			}
			uint64_t up = 0;
			absolutetime_to_nanoseconds(mach_absolute_time(), &up);
			/* up to 3 dumps: the first full-screen present after N, N + 60 and N + 120 s (a still
			 * screen presents nothing, so one late dump often never came) */
			if (at > 0 && done < 3 && src && bpe == 4 && s->width >= 256 && s->height >= 256 &&
			    up / 1000000000ULL >= (uint64_t)at + 60ULL * (uint64_t)done) {
				done++;
				uint32_t W = s->width / 6, H = s->height / 6;
				static const char hx[] = "0123456789abcdef";
				char line[69 * 3 + 1];
				for (uint32_t y = 0; y < H; y++) {
					for (uint32_t x0 = 0; x0 < W; x0 += 69) {
						uint32_t n = 0;
						for (uint32_t x = x0; x < W && x < x0 + 69; x++) {
							uint32_t v = *(const volatile uint32_t *)(src + (size_t)(y * 6 + 3) * s->bytesPerRow + (x * 6 + 3) * 4);
							line[n++] = hx[(v >> 20) & 0xf];
							line[n++] = hx[(v >> 12) & 0xf];
							line[n++] = hx[(v >> 4) & 0xf];
						}
						line[n] = 0;
						IOLog("SWEET FBDUMP %u %u %u %u %s\n", W, H, y, x0, line);
					}
				}
				IOLog("SWEET FBDUMP end %d at %llu s\n", done, up / 1000000000ULL);
				/* sweetfbcrop=X,Y,W,H (0391): with each dump, that rectangle of the layer at full
				 * resolution, "SWEET FBCROP W H y x0 hex" (12-bit colour, 69 pixels a line) */
				char crop[64] = "";
				if (PE_parse_boot_argn("sweetfbcrop", crop, sizeof(crop)) && crop[0]) {
					uint32_t v[4] = { 0, 0, 0, 0 }, k = 0;
					for (const char *c = crop; *c && k < 4; c++) {
						if (*c >= '0' && *c <= '9') {
							v[k] = v[k] * 10 + (uint32_t)(*c - '0');
						} else if (*c == ',') {
							k++;
						}
					}
					uint32_t cx = v[0], cy = v[1], cw = v[2], ch = v[3];
					if (cx < s->width && cy < s->height) {
						if (cx + cw > s->width) {
							cw = s->width - cx;
						}
						if (cy + ch > s->height) {
							ch = s->height - cy;
						}
						for (uint32_t y = 0; y < ch; y++) {
							for (uint32_t x0 = 0; x0 < cw; x0 += 69) {
								uint32_t n = 0;
								for (uint32_t x = x0; x < cw && x < x0 + 69; x++) {
									uint32_t px = *(const volatile uint32_t *)(src + (size_t)(cy + y) * s->bytesPerRow + (cx + x) * 4);
									line[n++] = hx[(px >> 20) & 0xf];
									line[n++] = hx[(px >> 12) & 0xf];
									line[n++] = hx[(px >> 4) & 0xf];
								}
								line[n] = 0;
								IOLog("SWEET FBCROP %u %u %u %u %s\n", cw, ch, y, x0, line);
							}
						}
						IOLog("SWEET FBCROP end %d\n", done);
					}
				}
			}
		}
#endif
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		/* Scaled, dx/dy/w/h stay logical: clip to the logical display. */
		int32_t limitW = (int32_t)(fScaled ? fLogicalW : fWidth);
		int32_t limitH = (int32_t)(fScaled ? fLogicalH : fHeight);
		if (w > limitW - dx) {
			w = limitW - dx;
		}
		if (h > limitH - dy) {
			h = limitH - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		if (sweet_fb_dp.attached && !(src != 0 && bpe == 4 && fGpu && dx == 0 && dy == 0 &&
		    w == (int32_t)fLogicalW && h == (int32_t)fLogicalH)) {
			dppLeave("a frame of a part of the display, or of another size or format", true, SWEET_FB_DPP_LEAVE_FRAME);
		}
#endif
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
		if (src != 0 && bpe == 4 && fGpu && dx == 0 && dy == 0 &&
		    w == (int32_t)fLogicalW && h == (int32_t)fLogicalH && gpuPresent(s, sx, sy, w, h)) {
			copied += (uint64_t)fWidth * fHeight * 4;
			s->release();
			continue;
		}
#endif
#if defined(ARM64_BOARD_CONFIG_STARLTE)
		if (layer > 0 && src != 0) {
			/* where the pointer layer goes (does touch move it?): each new position, up to 400 */
			static int32_t lastX = -1, lastY = -1;
			static uint32_t moves;
			if ((dx != lastX || dy != lastY) && moves < 400) {
				moves++;
				lastX = dx;
				lastY = dy;
				IOLog("SWEET FB: layer %u surface %u at %d,%d %dx%d (swap %u)\n", layer, id, dx, dy, w, h,
				    get32(rec, 0x50 + sh));
			}
		}
		sweet_fb_blend = layer > 0;
		/*
		 * Pointer save-under (0396, replaces 0392's restore from the last layer 0, which WindowServer's
		 * double buffering left stale: trails for ~30 s). Before the pointer layer is blended in, the
		 * panel pixels under it are saved; a pointer-only swap first puts them back. A swap with layer 0
		 * covers everything, so the saved copy is dropped.
		 */
		if (layer > 0 && src != 0 && w <= 256 && h <= 256) {	/* the pointer: whichever upper layer holds it */
			/* 0397: where the pointer layer is drawn (logical), for the touch driver's direct touch */
			extern volatile int32_t sweet_fb_cur_x, sweet_fb_cur_y;
			extern volatile uint32_t sweet_fb_cur_seq;
			sweet_fb_cur_x = dx - sx;      /* unclipped origin: sx grew by what the left edge cut */
			sweet_fb_cur_y = dy - sy;
			__atomic_fetch_add(&sweet_fb_cur_seq, 1, __ATOMIC_RELEASE);
		}
		if (layer > 0 && src != 0 && fScaled && bpe == 4) {
			bool l0 = (mask & 1u) && get32(rec, 0x54 + sh) != 0;
			if (sweet_su_have && !l0) {
				sweetSaveUnder(false);
			}
			sweetSaveUnderRect(dx, dy, w, h);
			sweetSaveUnder(true);
		}
#endif
		if (src != 0 && bpe == 4 && fScaled) {
			copied += scaledCopy(src + s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4,
			    s->bytesPerRow, dx, dy, w, h);
		} else if (src != 0 && bpe == 4) {
			copied += (uint64_t)w * (uint64_t)h * 4;
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
			sweet_fb_copy_rows(fPixels + (uint64_t)dy * fRowBytes + (uint64_t)dx * 4, fRowBytes,
			    src + s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4, s->bytesPerRow,
			    (size_t)w * 4, h);
			if (w == (int32_t)fWidth && h == (int32_t)fHeight) {
				__atomic_store_n(&sweet_fb_last_surface_id, s->id, __ATOMIC_RELEASE);
			}
#else
			for (int32_t y = 0; y < h; y++) {
				memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
				    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
				    (size_t)w * 4);
			}
#endif
		}
		s->release();
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	{
		bool l0 = (mask & 1u) && get32(rec, 0x54 + sh) != 0;
		bool l1 = (mask & 2u) && get32(rec, 0x58 + sh) != 0;
		if (l0 && !l1) {
			sweet_su_have = false;          /* the new frame has no pointer on it */
		} else if (!l0 && !l1 && sweet_su_have) {
			sweetSaveUnder(false);          /* pointer hidden: put the frame back */
			sweet_su_have = false;
		}
	}
#endif
	if (copied != 0 && sweet_fb_first_present == 0) {
		sweet_fb_first_present = mach_absolute_time();
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (copied != 0) {
		hakios_exynos_cpufreq_frame();  /* the governor's frame boost; nothing without cpufreq_gov=1 */
	}
#endif
	return copied;
}

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
/*
 * sweetfb_gpu: the Mali draws the whole (logical) frame scaled to the panel.
 * The surface is mapped into a GPU slot the first time it is shown (its
 * pages; a slot is keyed by id and first page). The Mali is not IO-coherent,
 * so the frame is cleaned to the point of coherency first. Called only from
 * present(), which the vsync timer serializes. False: use the CPU copy.
 */
bool
AppleCLCD::gpuPresent(SweetSurface *s, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
	static uint64_t pages[SWEET_FB_GPU_MAX_PAGES];
	static uint32_t failures;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* Until the G72 is up (about 8 s into boot) the CPU copies, uncounted. */
	if (!starlte_gpu_present_ready()) {
		return false;
	}
#endif
	if (failures >= 8 || s->memory == NULL) {
		return false;
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* (before the surface's slot is looked up: the DPP's buffers take the last two slots' tables when first set up) */
	bool dpp = dppWanted() && dppSetup();
#endif
	IOByteCount len = 0;
	addr64_t pa0 = s->memory->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	int slot = pa0 ? herolte_gpu_present_slot(s->id, pa0 & ~0xfffULL) : -1;
	if (slot < 0 && pa0 != 0) {
		unsigned n = (unsigned)((s->memory->getLength() + 0xfff) / 0x1000);
		bool ok = n <= SWEET_FB_GPU_MAX_PAGES;
		for (unsigned i = 0; ok && i < n; i++) {
			addr64_t pa = s->memory->getPhysicalSegment((IOByteCount)i * 0x1000, &len,
			    kIOMemoryMapperNone);
			ok = pa != 0;
			pages[i] = pa & ~0xfffULL;
		}
		slot = ok ? herolte_gpu_present_map(s->id, pages, n) : -1;
	}
	if (slot < 0) {
		/* (eight of these and the CPU copies for good: say why, as it used to happen silently) */
		IOLog("SWEET FB: GPU present: surface %u (%llu bytes, first page %#llx) not mapped (%u of 8)\n", s->id,
		    (unsigned long long)s->memory->getLength(), (unsigned long long)pa0, ++failures);
		return false;
	}
	uint64_t off = (uint64_t)s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4;
	uintptr_t start = s->kernelAddress() + (uintptr_t)off;
	uintptr_t end = start + (uintptr_t)(h - 1) * s->bytesPerRow + (uintptr_t)w * 4;
	bool clean = true;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/*
	 * sweetspeed&524288: the clean (6 MiB of DC CVAC a frame, on the vsync thread) only when a CPU may have written
	 * the surface: the first time it is shown, and whenever its seed moved (every CPU writer unlocks with a seed
	 * bump). A frame the GPU rendered (CoreAnimation on GL or Metal) has nothing in the CPU's caches to clean.
	 */
	if ((sweet_fb_sbx & 524288) != 0 && s->state != NULL) {
		static struct { uint32_t id, seed; addr64_t pa; } seen[8];
		static unsigned next;
		static uint64_t skipped, cleaned;
		uint32_t seed = *(volatile uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + 0xc);
		unsigned i;
		for (i = 0; i < 8 && !(seen[i].id == s->id && seen[i].pa == pa0); i++) {
		}
		if (i < 8 && seen[i].seed == seed) {
			clean = false;
			skipped++;
		} else {
			if (i == 8) {
				i = next++ % 8;
			}
			seen[i].id = s->id;
			seen[i].pa = pa0;
			seen[i].seed = seed;
			cleaned++;
		}
		if (((skipped + cleaned) % 3600) == 0) {
			IOLog("SWEET FB: GPU present: %llu frames cleaned first, %llu not (seed unchanged)\n", cleaned, skipped);
		}
	}
#endif
	if (clean) {
		for (uintptr_t p = start & ~(uintptr_t)63; p < end; p += 64) {
			__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
		}
		__asm__ volatile ("dsb sy" ::: "memory");
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (dpp && dppPresent(slot, (uint32_t)off, s->bytesPerRow, (uint32_t)w, (uint32_t)h)) {
		sweet_fb_dp.path = 2;
		return true;
	}
#endif
	bool drawn = herolte_gpu_present_draw(slot, (uint32_t)off, s->bytesPerRow, (uint32_t)w, (uint32_t)h);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (sweet_fb_dp.attached) {
		/* the DPP could not take this frame (dppPresent() says why): it is in the boot framebuffer now, shown from the
		 * next refresh on; if it is not, the shown buffer is drawn there first and the CPU copies the frame over it */
		dppLeave(sweet_fb_dp.why != NULL ? sweet_fb_dp.why : "the frame could not be taken", !drawn,
		    sweet_fb_dp.given_up ? SWEET_FB_DPP_LEAVE_GIVE_UP : SWEET_FB_DPP_LEAVE_FRAME);
	}
	if (drawn) {
		sweet_fb_dp.path = 1;
	}
#endif
	if (!drawn) {
		IOLog("SWEET FB: GPU present: %dx%d stride %u not drawn (%u of 8)\n", w, h, s->bytesPerRow, ++failures);
		return false;
	}
	return true;
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* sweetfb_dpp asked for, not given up, and (after a frame it could not take) enough frames it can take since */
bool
AppleCLCD::dppWanted(void)
{
	if (__atomic_load_n(&sweet_fb_dpp_want, __ATOMIC_ACQUIRE) == 0 || sweet_fb_dp.given_up) {
		return false;
	}
	if (sweet_fb_dp.attached || sweet_fb_dp.again_now) {
		return true;
	}
	if (sweet_fb_dp.quiet < SWEET_FB_DPP_QUIET) {
		sweet_fb_dp.quiet++;
		return false;
	}
	return true;
}

/*
 * Once: the DECON probed (as expected: one window on, reading the boot framebuffer at 1440 x 2960 in command mode),
 * three buffers of the frame's size from the display's carve-out, mapped for the GPU (AS0) and for the CPU. False and
 * given up if any of it is not had; the DPU driver's lines say why.
 */
bool
AppleCLCD::dppSetup(void)
{
	if (sweet_fb_dp.ready) {
		return true;
	}
	if (sweet_fb_dp.tried) {
		return false;
	}
	sweet_fb_dp.tried = true;
	sweet_fb_dp.quiet = SWEET_FB_DPP_QUIET;
	sweet_fb_dp.w = fLogicalW;
	sweet_fb_dp.h = fLogicalH;
	sweet_fb_dp.rowbytes = (fLogicalW * 4 + 63) & ~63u;
	uint64_t bytes = (uint64_t)sweet_fb_dp.rowbytes * fLogicalH;
	sweet_fb_dp.step = (bytes + 0xFFFFULL) & ~0xFFFFULL;   /* as hakios_scanout_memory() lays them out */
	const char *why = NULL;
	if (hakios_scanout_probe(fPhys, fWidth, fHeight, fRowBytes) != 0) {
		why = "the DECON is not as expected (exynos_dpu's lines)";
	} else if (hakios_scanout_memory(bytes, SWEET_FB_DPP_BUFFERS, sweet_fb_dp.phys) != 0) {
		why = "no buffers in the display's carve-out (exynos_dpu's lines)";
	} else if (sweet_fb_dp.phys[1] - sweet_fb_dp.phys[0] != sweet_fb_dp.step ||
	    sweet_fb_dp.phys[2] - sweet_fb_dp.phys[1] != sweet_fb_dp.step) {
		why = "the buffers are not laid out one step apart";
	} else if (starlte_gpu_present_scan_map(sweet_fb_dp.phys[0], 2 * sweet_fb_dp.step + bytes) != 0) {
		why = "the buffers not mapped for the GPU";
	} else if ((sweet_fb_dp.kva = ml_io_map_wcomb((vm_offset_t)sweet_fb_dp.phys[0],
	    (vm_size_t)(2 * sweet_fb_dp.step + bytes))) == 0) {
		why = "the buffers not mapped for the CPU";
	}
	if (why != NULL) {
		sweet_fb_dp.given_up = true;
		sweet_fb_dp.why = why;
		IOLog("A5XELTE FB dpp: not set up: %s; frames go as before\n", why);
		return false;
	}
	sweet_fb_dp.ready = true;
	sweet_fb_dp.refreshes_at = hakios_scanout_refreshes();
	IOLog("A5XELTE FB dpp: 3 buffers of %ux%u, %u bytes a row, at %#llx %#llx %#llx; the first frame copied into one is "
	    "handed to VGF0\n", sweet_fb_dp.w, sweet_fb_dp.h, sweet_fb_dp.rowbytes, sweet_fb_dp.phys[0], sweet_fb_dp.phys[1],
	    sweet_fb_dp.phys[2]);
	return true;
}

/*
 * The frame (slot, offset, stride, w x h: the whole swap surface) copied into the buffer that is neither shown nor
 * waiting to be, and flipped to once the flip before has been taken. The first time, the window is handed to VGF0.
 * False: not taken (sweet_fb_dp.why says why; given up when it is the copy, the scaler or a stuck flip), and the
 * caller draws the frame as before.
 */
bool
AppleCLCD::dppPresent(int slot, uint32_t off, uint32_t stride, uint32_t w, uint32_t h)
{
	if (w != sweet_fb_dp.w || h != sweet_fb_dp.h) {
		sweet_fb_dp.why = "a frame of another size";
		return false;
	}
	if (sweet_fb_dp.attached && sweet_fb_dp.pending >= 0 && hakios_scanout_latched()) {
		sweet_fb_dp.front = sweet_fb_dp.pending;
		sweet_fb_dp.pending = -1;
	}
	int t = 0;
	while (t == sweet_fb_dp.front || t == sweet_fb_dp.pending) {
		t++;
	}
	if (!starlte_gpu_present_copy(slot, off, stride, w, h, sweet_fb_dp.step * (uint64_t)t, sweet_fb_dp.rowbytes)) {
		sweet_fb_dp.given_up = true;
		sweet_fb_dp.why = "the copy job failed (STARLTE GPU PRESENT's line)";
		return false;
	}
	if (!sweet_fb_dp.attached) {
		if (hakios_scanout_attach(2, sweet_fb_dp.phys, SWEET_FB_DPP_BUFFERS, w, h, sweet_fb_dp.rowbytes, (unsigned)t) != 0) {
			sweet_fb_dp.given_up = true;
			sweet_fb_dp.why = "VGF0's scaler not taken (exynos_dpu's lines)";
			return false;
		}
		sweet_fb_dp.attached = true;
		sweet_fb_dp.again_now = false;
		sweet_fb_dp.front = t;
		sweet_fb_dp.pending = -1;
		sweet_fb_dp.attaches++;
		IOLog("A5XELTE FB dpp: VGF0 scales %ux%u to the panel from buffer %d on\n", w, h, t);
		return true;
	}
	if (sweet_fb_dp.pending >= 0) {
		/* the frame before is not on the panel yet: this one waits for its refresh (it never replaces it unseen) */
		uint64_t began = mach_absolute_time();
		for (unsigned ms = 0; !hakios_scanout_latched(); ms++) {
			if (ms >= SWEET_FB_DPP_STUCK_MS) {
				sweet_fb_dp.given_up = true;
				sweet_fb_dp.why = "a flip not taken in 100 ms";
				return false;
			}
			IOSleep(1);
		}
		uint64_t waited = mach_absolute_time() - began;
		sweet_fb_dp.waited++;
		sweet_fb_dp.wait_abs += waited;
		if (waited > sweet_fb_dp.wait_max) {
			sweet_fb_dp.wait_max = waited;
		}
		sweet_fb_dp.front = sweet_fb_dp.pending;
		sweet_fb_dp.pending = -1;
	}
	if (hakios_scanout_flip((unsigned)t) != 0) {
		sweet_fb_dp.given_up = true;
		sweet_fb_dp.why = "the flip refused";
		return false;
	}
	sweet_fb_dp.pending = t;
	sweet_fb_dp.flips++;
	return true;
}

/*
 * Back to today's present: the DECON reads the boot framebuffer again on the boot loader's channel from the next
 * refresh on. `restore`: the picture VGF0 shows drawn scaled into the boot framebuffer first (the GPU's present of
 * that buffer, or the CPU's scaled copy if the GPU cannot), so that what the DECON reads there is the frame on the
 * panel, not an old one. `how`: SWEET_FB_DPP_LEAVE_*, when it is asked for again.
 */
void
AppleCLCD::dppLeave(const char *why, bool restore, int how)
{
	if (!sweet_fb_dp.attached) {
		return;
	}
	int shown = sweet_fb_dp.front;
	if (sweet_fb_dp.pending >= 0) {
		for (unsigned ms = 0; ms < SWEET_FB_DPP_STUCK_MS && !hakios_scanout_latched(); ms++) {
			IOSleep(1);
		}
		if (hakios_scanout_latched()) {
			shown = sweet_fb_dp.pending;
		}
	}
	if (restore && shown >= 0) {
		uint64_t at = sweet_fb_dp.step * (uint64_t)shown;
		if (!starlte_gpu_present_from_scan(at, sweet_fb_dp.rowbytes, sweet_fb_dp.w, sweet_fb_dp.h)) {
			bool blend = sweet_fb_blend;
			sweet_fb_blend = false;
			(void)scaledCopy(sweet_fb_dp.kva + (uintptr_t)at, sweet_fb_dp.rowbytes, 0, 0, (int32_t)sweet_fb_dp.w,
			    (int32_t)sweet_fb_dp.h);
			sweet_fb_blend = blend;
		}
	}
	hakios_scanout_detach(why);
	sweet_fb_dp.attached = false;
	sweet_fb_dp.front = sweet_fb_dp.pending = -1;
	sweet_fb_dp.why = why;
	sweet_fb_dp.left++;
	if (how == SWEET_FB_DPP_LEAVE_GIVE_UP || sweet_fb_dp.given_up) {
		sweet_fb_dp.given_up = true;
	} else if (how == SWEET_FB_DPP_LEAVE_AGAIN) {
		sweet_fb_dp.again_now = true;
	} else if (how == SWEET_FB_DPP_LEAVE_FRAME) {
		/* asked for again after SWEET_FB_DPP_QUIET frames it can take, SWEET_FB_DPP_RETRIES times a boot */
		sweet_fb_dp.quiet = 0;
		if (++sweet_fb_dp.retries > SWEET_FB_DPP_RETRIES) {
			sweet_fb_dp.given_up = true;
			sweet_fb_dp.why = "left too often for frames it could not take";
		}
	}
	IOLog("A5XELTE FB dpp: back to the boot framebuffer (%s)%s\n", why,
	    sweet_fb_dp.given_up ? "; given up until kern.hakios.sweetfb_dpp=1" : "");
}
#endif

/*
 * For the GPU copy test (bsd/dev/herolte_gpu.c): the 4 KiB physical pages
 * of the last full-screen iOS surface, its layout and kernel address. The
 * surface stays alive in the id table while iOS uses it; the test runs once.
 */
extern "C" bool
sweet_fb_last_surface(uint64_t *pages, unsigned max_pages, unsigned *npages,
    uint32_t *offset, uint32_t *stride, uint32_t *width, uint32_t *height, uintptr_t *kva)
{
	uint32_t id = __atomic_load_n(&sweet_fb_last_surface_id, __ATOMIC_ACQUIRE);
	SweetSurface *s = id ? sweet_surface_copy(id) : NULL;
	if (s == NULL) {
		return false;
	}
	bool ok = s->memory != NULL && s->bytesPerElement == 4 && s->elementWidth == 1 &&
	    s->elementHeight == 1;
	unsigned n = ok ? (unsigned)((s->memory->getLength() + 0xfff) / 0x1000) : 0;
	if (ok && n <= max_pages) {
		for (unsigned i = 0; i < n; i++) {
			IOByteCount len = 0;
			addr64_t pa = s->memory->getPhysicalSegment((IOByteCount)i * 0x1000, &len,
			    kIOMemoryMapperNone);
			if (pa == 0) {
				ok = false;
				break;
			}
			pages[i] = pa & ~0xfffULL;
		}
	} else {
		ok = false;
	}
	if (ok) {
		*npages = n;
		*offset = s->offset;
		*stride = s->bytesPerRow;
		*width = s->width;
		*height = s->height;
		*kva = s->kernelAddress();
	}
	s->release();
	return ok && *kva != 0;
}
#endif

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;
	char fiName[SFI_NAME_BYTES];
	bool fiNamed = false;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_MAX];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		uint32_t size = (uint32_t)args->structureInputSize;
		if (size > sizeof(rec)) {
			size = sizeof(rec);
		}
		bzero(rec, sizeof(rec));
		memcpy(rec, args->structureInput, size);
		fFB->swap(rec, size);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBSupportedFrameInfo:
		/* in: the key's index; out: the number of keys, the mode (0: notifications), the name (64 bytes) */
		if (sfi_supported(sweet_fb_fi, args->scalarInputCount > 0 ? args->scalarInput[0] : 0, &out[0], &out[1],
		    fiName) != 0 && sweet_fb_fi != 0) {
			ret = kIOReturnBadArgument;
		}
		outCount = 2;
		fiNamed = sweet_fb_fi != 0;
		break;
	case kSweetFBChangeFrameInfo:
		/* in: 3 (the notification type) and the mask of the keys' indices */
		if (sweet_fb_fi != 0 && args->scalarInputCount >= 2) {
			fFIMask = sfi_mask(sweet_fb_fi, args->scalarInput[1]);
			IOLog("SWEET FB: frame info mask %#x (asked %#llx %#llx)\n", fFIMask, args->scalarInput[0],
			    args->scalarInput[1]);
		}
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3) {
			static uint32_t logged_notify;
			if (logged_notify++ < 40) {
				char who[32];
				proc_selfname(who, sizeof(who));
				IOLog("SWEET FB: %s sel 72 type %llu func %#llx refcon %#llx\n", who,
				    args->scalarInput[2], args->scalarInput[0], args->scalarInput[1]);
			}
		}
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			if (sweet_fb_cav) {
				uint64_t up = 0;
				absolutetime_to_nanoseconds(mach_absolute_time(), &up);
				sweet_fb_cav_power = off ? 0 : 1;
				IOLog("SWEET FB CAV: RequestPowerChange(%llu) at %llu ms: CA renders %s\n", args->scalarInput[0],
				    up / 1000000ULL, off ? "at most 10 Hz from now on (display off)" : "on vsync");
			}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
			int keep_on = 0;
			if (off && PE_parse_boot_argn("a5fb_keep_on", &keep_on, sizeof(keep_on)) && keep_on) {
				/* backboardd also asks for off on its own (right after the
				 * first swap, idle); only a Power press just before is the
				 * user locking the phone (bsd/dev/a5xelte_keys.c). */
				uint64_t pressed = a5xelte_power_pressed_at, since_ns = UINT64_MAX;
				if (pressed) {
					absolutetime_to_nanoseconds(mach_absolute_time() - pressed, &since_ns);
				}
				if (since_ns > 3 * NSEC_PER_SEC) {
					IOLog("A5XELTE FB: display-off request ignored (a5fb_keep_on)\n");
					break;
				}
				IOLog("A5XELTE FB: display-off after Power press\n");
			}
#endif
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
				if (sweet_fb_nowait) {
					sfp_redraw(&fFB->fPres);
				}
#endif
			}
			IOLockUnlock(fFB->fLock);
			IOLog("SWEET FB: display %s\n", off ? "off" : "on");
		}
		break;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	case kSweetFBSwapWait:
		if (sweet_fb_dpp_want || sweet_fb_dp.tried) {
			/*
			 * sweetfb_dpp's evidence for a scan-out of the swap surfaces themselves (docs/ios27/PRESENT.md, stage B):
			 * what iOS 27's CoreAnimation asks SwapWait for. Answered at once, as before; the first 12 calls' scalars
			 * logged, the first compared with the swap ids of the SwapEnds.
			 */
			uint32_t last = __atomic_load_n(&sweet_fb_dp.last_swap, __ATOMIC_RELAXED);
			uint64_t a0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
			sweet_fb_dp.sw_calls++;
			if (a0 == last) {
				sweet_fb_dp.sw_last++;
			} else if (a0 + 1 == last) {
				sweet_fb_dp.sw_before++;
			} else {
				sweet_fb_dp.sw_other++;
			}
			if (sweet_fb_dp.sw_logged < 12) {
				sweet_fb_dp.sw_logged++;
				IOLog("A5XELTE FB dpp: SwapWait %u scalars %#llx %#llx %#llx, the last SwapEnd's swap %u\n",
				    args->scalarInputCount, a0, args->scalarInputCount > 1 ? args->scalarInput[1] : 0ULL,
				    args->scalarInputCount > 2 ? args->scalarInput[2] : 0ULL, last);
			}
		}
		break;
#else
	case kSweetFBSwapWait:
#endif
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
		if (fiNamed) {
			memcpy(args->structureOutput, fiName,
			    args->structureOutputSize < SFI_NAME_BYTES ? args->structureOutputSize : SFI_NAME_BYTES);
		}
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	/* (sweetspeed&512) every call of the rarer selectors with its scalar inputs when they change (300 lines in all):
	 * to see which one carries brightness etc. on iOS 27 */
	if ((sweet_fb_sbx & 512) != 0 && selector != 3 && selector != 4 && selector != 5 && selector != 6 && selector < 128) {
		static uint64_t last[128][2];
		static unsigned left = 300;
		uint64_t a0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
		uint64_t a1 = args->scalarInputCount > 1 ? args->scalarInput[1] : 0;
		if (left > 0 && (a0 != last[selector][0] || a1 != last[selector][1])) {
			char who[32];
			left--;
			last[selector][0] = a0;
			last[selector][1] = a1;
			proc_selfname(who, sizeof(who));
			IOLog("SWEET FB ARGS: %s sel %u n %u: %#llx %#llx %#llx %#llx struct %u\n", who, selector,
			    args->scalarInputCount, a0, a1, args->scalarInputCount > 2 ? args->scalarInput[2] : 0ULL,
			    args->scalarInputCount > 3 ? args->scalarInput[3] : 0ULL, args->structureInputSize);
		}
	}
#endif
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* brightness by property (sweetspeed&512) (IOConnectSetCFProperties / IORegistryEntrySetCFProperties): every key logged
 * (first 64); a number under a key starting with "brightness" (0-1 or 0-255) goes to the panel. */
static IOReturn
sweet_fb_set_properties(const char *where, OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	static unsigned said;
	bool taken = false;
	char who[32] = "?";
	if ((sweet_fb_sbx & 512) == 0) {
		return kIOReturnUnsupported;    /* as IOService::setProperties */
	}
	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	OSCollectionIterator *keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	OSObject *next;
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol *key = OSDynamicCast(OSSymbol, next);
		if (key == NULL) {
			continue;
		}
		OSNumber *number = OSDynamicCast(OSNumber, dict->getObject(key));
		if (said < 64) {
			said++;
			if (number != NULL) {
				IOLog("SWEET FB: %s %s sets %s = %llu\n", who, where, key->getCStringNoCopy(), number->unsigned64BitValue());
			} else {
				IOLog("SWEET FB: %s %s sets %s (%s)\n", who, where, key->getCStringNoCopy(),
				    dict->getObject(key) ? dict->getObject(key)->getMetaClass()->getClassName() : "?");
			}
		}
		if (number != NULL && sweet_fb_is_brightness(key->getCStringNoCopy())) {
			uint64_t v = number->unsigned64BitValue();
			taken = hakios_panel_brightness(v <= 1 ? (int)(v * 255) : v > 255 ? 255 : (int)v) == 0;
		}
	}
	keys->release();
	return taken ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn
AppleCLCD::setProperties(OSObject *properties)
{
	return sweet_fb_set_properties("service", properties);
}

IOReturn
IOMobileFramebufferUserClient::setProperties(OSObject *properties)
{
	return sweet_fb_set_properties("client", properties);
}
#endif
#endif /* SWEET || A5XELTE */
#if defined(ARM64_BOARD_CONFIG_FRANKEL)
/* FRANKEL: the first present's time, defined under HEROLTE above for the S9 */
uint64_t sweet_fb_first_present;
/* the pointer layer's position (SweetHID's steering reads it; set by the STARLTE present path only) */
volatile int32_t sweet_fb_cur_x, sweet_fb_cur_y;
volatile uint32_t sweet_fb_cur_seq;
/* no property is taken on this board yet (the S9's go to its panel's brightness): as IOService::setProperties */
IOReturn
AppleCLCD::setProperties(OSObject *properties)
{
	(void)properties;
	return kIOReturnUnsupported;
}
IOReturn
IOMobileFramebufferUserClient::setProperties(OSObject *properties)
{
	(void)properties;
	return kIOReturnUnsupported;
}
#endif
