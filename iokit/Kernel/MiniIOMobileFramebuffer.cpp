/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of Darwin userland 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/thread_policy.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void hakios_vgpu3d_display_surface(uint32_t sid);       /* bsd/dev/hakios.vgpu3d_xnu.c */
extern "C" bool hakios_vgpu3d_host_owns(uint32_t sid);

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
extern "C" void hakios_display_power(int on);   /* GenericARMPE.cpp (patch 1517) */
extern "C" int hakios_panel_brightness(int level);       /* drivers/display/panel/hakios.backlight.cpp: 0 if a panel driver took it, else ENODEV */
extern "C" void hakios_backlight_publish(IOService * display);  /* the same: AppleARMBacklight, with boot-arg backlight= */

/*
 * Scion shared framebuffer (SCFB), for the Android Virtualization Framework: crosvm has no ramfb,
 * and an ordinary app cannot take crosvm's virtio-gpu display. Instead the app lends the VM a
 * slice of RAM that it maps too (an ashmem region crosvm maps as guest memory), and the boot
 * framebuffer is in it. avfboot (avf/shim) finds it in the kernel parameters
 * scfb.addr=PA scfb.size=N scfb.w=W scfb.h=H, keeps it out of the kernel's memory, writes the
 * header below and points boot_args' video at buffer 0. Here, every frame that has been drawn
 * into the panel's memory is published: frame_seq goes up, and the app copies the buffer to its
 * screen. There is one buffer, drawn in place (the layers of a swap only redraw what they cover),
 * so it is guarded like a seqlock: before drawing we wait while the app is copying
 * (host_reading, 4 ms at most) and set drawing; the app waits while drawing is set and copies
 * again if a frame began meanwhile. No half-drawn frame reaches the screen. The header layout is
 * the app's (display/SCFB.md in scion-avf); only the fields used are named here.
 */
#define SCFB_MAGIC              0x42464353u     /* "SCFB" */
#define SCFB_F_READY            1u              /* the header is valid */
#define SCFB_F_POLL             2u              /* nothing publishes frames yet: copy now and then */
#define SCFB_F_DAMAGE           4u              /* dirty[] is exact: an empty rectangle means nothing changed */
struct scfb_header {
	uint32_t        magic, version;
	uint32_t        width, height;
	uint32_t        stride, format;
	uint32_t        buf_offset[2];
	uint32_t        nbufs, flags;
	uint32_t        front;
	uint32_t        drawing;                /* 1 while a frame is drawn into the buffer (seqlock with host_reading) */
	uint64_t        frame_seq;              /* frames published */
	uint64_t        input_seq;              /* (input reports consumed: not known here, 0) */
	uint64_t        guest_ns;               /* when the frame was published */
	uint32_t        dirty[4];               /* x, y, width, height that changed */
	uint64_t        host_seq;               /* the app: last frame copied */
	uint64_t        host_reading;
	uint64_t        host_ns;
};
/*
 * virtio-gpu 2D (bsd/dev/hakios.vgpu.c): the panel when the loader left no framebuffer, e.g.
 * QEMU with -device virtio-gpu-pci and no ramfb. Each frame is then sent with hakios_vgpu_flush.
 */
extern "C" int hakios_vgpu_display(void ** fb, uint32_t * width, uint32_t * height, uint32_t * stride);
extern "C" int hakios_vgpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
static bool gVGPU;

static TUNABLE(uint64_t, gSCFBAddr, "scfb.addr", 0);
static volatile struct scfb_header * gSCFB;
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static void
scfbAttach(void)
{
	volatile struct scfb_header * h;

	if (gSCFBAddr == 0) {
		return;
	}
	h = (volatile struct scfb_header *) ml_io_map((vm_offset_t) gSCFBAddr, PAGE_SIZE);
	if (h == NULL || h->magic != SCFB_MAGIC) {
		IOLog("MiniIOMobileFramebuffer: no shared framebuffer header at 0x%llx\n", gSCFBAddr);
		return;
	}
	IOLog("MiniIOMobileFramebuffer: shared framebuffer at 0x%llx: %ux%u, %u bytes a row, format 0x%x, app at frame %llu\n",
	    gSCFBAddr, h->width, h->height, h->stride, h->format, h->host_seq);
	gSCFB = h;
}

/* A frame is about to be drawn into the panel's memory: wait for the app to finish copying. */
static void
scfbBeginDraw(void)
{
	volatile struct scfb_header * h = gSCFB;
	uint64_t deadline;

	if (h == NULL) {
		return;
	}
	clock_interval_to_deadline(4, NSEC_PER_MSEC, &deadline);
	for (;;) {
		while (h->host_reading != 0 && mach_absolute_time() < deadline) {
		}
		h->drawing = 1;
		__builtin_arm_dsb(0xb);
		if (h->host_reading == 0 || mach_absolute_time() >= deadline) {
			break;
		}
		h->drawing = 0;                 /* both at once: the app goes first */
		__builtin_arm_dsb(0xb);
	}
}

/*
 * The frame is in the panel's memory: tell the app what changed. exact: the rectangle is the
 * whole change (empty when nothing did), as the shadow comparison in drawRows finds it.
 */
static void
scfbPublish(uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool exact)
{
	volatile struct scfb_header * h = gSCFB;
	uint64_t nanos;

	if (h == NULL) {
		return;
	}
	__builtin_arm_dsb(0xf);                 /* (SY) the frame's write-combined stores are out */
	absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
	h->dirty[0] = x;
	h->dirty[1] = y;
	h->dirty[2] = width;
	h->dirty[3] = height;
	h->guest_ns = nanos;
	h->front = 0;
	__builtin_arm_dmb(0xb);
	h->drawing = 0;
	h->frame_seq = h->frame_seq + 1;
	h->flags = SCFB_F_READY | (exact ? SCFB_F_DAMAGE : 0);   /* from now on every frame says so itself */
	__builtin_arm_dsb(0xb);
}

/* IOConnectCallMethod selectors of the framebuffer user client (Darwin userland 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and Darwin draws whatever size of screen IOMobileFramebuffer reports: UIKit and
 * SpringBoard lay themselves out in points, as wide and as tall as the display is. What is fixed is
 * the status bar: UIKit picks it by the screen's exact geometry, and has one for a phone with the
 * iPhone X's sensor housing only at Apple's widths (_UIStatusBarVisualProvider_Split1125, 1080, 1170,
 * 1242, 828, 1284); at any other it draws a red bar saying "rdar://45025538" ("No status bar visual
 * provider for screen ..."). UIKit takes the one named by UIStatusBarVisualProviderClassName in the
 * defaults instead, which user space sets from the display's width (userland's libhakios.sb).
 * MobileGestalt's main-screen-class is -1 for any size not Apple's (MGIOMFBSupport.c: "screen
 * parameters are unexpected"), and MobileGestalt answers from its cache on /var, made at an earlier
 * boot, until that is made again: main-screen-width and -height can be an earlier size's.
 *
 * So by default (fb_native=1), on a panel wider than 1125 pixels whose shape is not the iPhone's,
 * the display is 375 points wide, the iPhone's own width, and as many points tall as the panel's
 * shape makes, at 3 pixels a point. On the Galaxy S9's 1440 x 2960 (18.5:9; the S9+'s and the Note9's
 * are the same) that is 375 x 771 points, 1125 x 2313 pixels, enlarged by the DPU (fb_dpp) 1.28 times
 * across and 1.2797 down: 0.02 % apart, nothing visibly squeezed and no black bars. The iPhone's own
 * 1125 x 2436 (19.5:9) on that panel was stretched 1.28 across and 1.215 down.
 * Why not fewer points: at 360 x 740 points (1080 x 2220, the panel's FHD+ and exactly its shape) the
 * keyboard is cut off on the right, "p", backspace and "search" with it (the S9, 3 November). The
 * width of 375 points is one the keyboard is whole at.
 * In QEMU (the loader makes the panel 1125 x 2436) and on panels 1125 pixels wide or less the display
 * stays the iPhone's 1125 x 2436 (untried there; fbwidth= and fbheight= give any size).
 * Boot-args fbwidth= and fbheight= give the size outright (the iPhone's for the one not given).
 * Boot-arg fb_native=0: the iPhone's 1125 x 2436 on every panel, as before (the picture stretched to the panel's shape).
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458
#define kFBNativePointsWide     375                     /* the iPhone's own width; narrower cuts the keyboard off */

static TUNABLE(uint32_t, gFBWidth, "fbwidth", 0);
static TUNABLE(uint32_t, gFBHeight, "fbheight", 0);
static TUNABLE(uint32_t, gFBNative, "fb_native", 1);
/*
 * Boot-arg fbscale=2: the same points (375 x 812, or fb_native's) drawn at two pixels a point instead of
 * three. Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/*
 * Boot-arg fbsubtype=N with fbscale and fbwidth/fbheight: a screen of another iPhone's class -- its pixels (fbwidth x
 * fbheight, as given, not 375 x 812 points at fbscale), its scale and its artwork-device-subtype (the device tree's
 * /product, what MobileGestalt's screen class, corner radius and artwork come from). UIKit picks the status bar's look
 * by width, scale and corner radius together and has none for the iPhone X's 375 points at 2 ("No status bar visual
 * provider ... rdar://45025538", a red bar), and PosterBoard drew no wallpaper there (2 November). fbwidth=828
 * fbheight=1792 fbscale=2 fbsubtype=1792 is the iPhone XR's: 1.85 times fewer pixels than this one's to draw.
 */
static TUNABLE(uint32_t, gFBSubtype, "fbsubtype", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had Darwin killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 2);
/*
 * fb_filter=2 (the default): nearest neighbour while frames come, bilinear once they stop. Bilinear costs: a frame
 * took 3.6 ms to scale onto the panel as a rule and 11 ms for one in ten, at any priority (fb_pri), against 1.7 and
 * 4.3 by nearest neighbour (touch_lat=1, 30 October) -- and at 120 Hz a refresh is 8.3 ms. What nearest neighbour
 * gets wrong (a column in 25 and a row in 67 left out) does not show while the picture moves; so a frame drawn so is
 * drawn again, filtered, when no other has come for SETTLE_MS: what stands still on the screen is the bilinear one.
 *
 * Frames that come slowly are filtered at once: a clock asking for 10 frames a second had each drawn by nearest neighbour and
 * 40 ms later by bilinear, every edge of a still screen moving back and forth. Nearest neighbour is for FAST_FRAMES in a row and
 * on. (Upstream hakios-xnu-kernel 539d35e, seen there on an Adreno phone; not yet on the S9.)
 */
#define SETTLE_MS       40
#define FAST_FRAMES     2
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
/*
 * Boot-arg fb_ca_hz=N: the refresh rate told to CoreAnimation (IOMFBDisplayRefresh) when it is not to be fb_hz's. On
 * this system CoreAnimation draws at half the rate it is told unless something asks it for more: told 120 it gave 60
 * (tools/frames.py: frames every 16.7 ms, with or without CADisableMinimumFrameDurationOnPhone, CA_FORCE_120HZ and
 * CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0, 30 October). The refreshes themselves still come at fb_hz.
 * fb_ca_hz=120 on the S9 (fb_hz 60, fb_zc=1; the USB console's "swipe", 8 strokes): refreshes missed while the finger moved
 * the home screen 3 to 9 in about 115 frames, against 16 to 25 in 99 told 60; in all 17 to 24 in 390 frames of motion
 * against 43 to 53 in 355. What is left is where CoreAnimation goes to 30 a second by itself: the last 300 ms of a page's
 * slide (SpringBoard's commits 33.3 ms apart there).
 * That 30 is SpringBoard's own choice, not CoreAnimation's judgement of this display: the crossfade of the page dots and
 * the search pill that follows every slide is set up with CAFrameRateRangeMake(20, 30, 0) (-[SBFolderScrollAccessoryView
 * _setUpAnimation]); the swaps' own timestamps ("tl: t") are 33.3 ms apart there and none of those frames is late.
 * userland/src/inject_sb.c raises such ranges to the panel's rate (sb_fps). What fb_ca_hz=120 does is the rest: told 60,
 * CoreAnimation starts a frame about 3 ms after the panel's refresh and has the 13 ms left to it for the render thread
 * (8 to 15 ms a frame here) and the GPU (5 ms), and a frame that takes longer misses that refresh (21 refreshes missed
 * while the finger moved in about 97 frames, 3 November); told 120 it draws every other 8.3 ms tick and the swap queue
 * (fb_queue, two) takes up the difference (3 to 11 in about 115). Nothing here reads a refresh notification
 * (IOMobileFramebuffer's selector 72 is never called) and nothing reports when a frame was shown.
 */
static TUNABLE(uint32_t, gCARefreshHz, "fb_ca_hz", 0);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_tcoal=1: the kernel's timer coalescing left on. By default this driver turns it off when it starts
 * (mach_timer_coalescing_enabled, the switch of sysctl kern.timer_coalescing_enabled). CoreAnimation draws a frame when a
 * timer of its own fires, not when this driver's refresh notifications come (it never asks for them, see vsync()), and
 * the kernel lets a timer fire late by as much as its thread's class allows: up to 100 ms for a thread in the background
 * (DARWIN_BG; timer_coalesce_bg_ns_max), 75 ms and more for the low latency tiers, nothing only for real-time threads,
 * which the window server's are on an iPhone. Here they are not, and on the S9 with fb_dpp=1 the frames came exactly
 * 100 ms apart, ten a second whatever was on the screen (main-1102, 2 November: "fps: 10 presented, 10 drawn, 10 flips,
 * 0 lost, 0 held; drawing 1600 us a frame; 60 refreshes", 58 seconds of 60): a frame took 1.6 ms here, so SwapEnd never
 * held the render thread, and every frame waited for its timer. Drawn on the processor (14 ms a frame) the swaps were
 * held (fb_queue) and woke the render thread themselves, 15 to 35 a second, and 10 when nothing held them. fps_log says
 * which thread hands the swaps over and how the kernel classes it ("fps: the swaps come from ...").
 */
static TUNABLE(uint32_t, gTimerCoalescing, "fb_tcoal", 0);
extern "C" boolean_t mach_timer_coalescing_enabled;
extern "C" int proc_get_effective_thread_policy(thread_t thread, int flavor);
extern "C" kern_return_t thread_policy_get(thread_t thread, thread_policy_flavor_t flavor, thread_policy_t policy_info,
    mach_msg_type_number_t * count, boolean_t * get_default);
extern "C" kern_return_t thread_policy_set(thread_t thread, thread_policy_flavor_t flavor, thread_policy_t policy_info,
    mach_msg_type_number_t count);
extern "C" uint64_t thread_get_runtime_self(void);
extern "C" int cpu_number(void);
extern "C" void hakios_mali_frame(void);        /* hakios.mali.c (stubs.c without it): mali_gov_frame's floor */
#define POLICY_DARWIN_BG        0x21                    /* TASK_POLICY_DARWIN_BG (kern/policy_internal.h, IMP_TASK_POLICY_*) */
#define POLICY_LATENCY_QOS      0x2F                    /* TASK_POLICY_LATENCY_QOS */
/* the refresh notifications sent (vsync()), all user clients together: for fps_log */
static volatile SInt32 gVSyncsTold;
/*
 * Boot-arg fb_pri=N: the scheduling priority of the threads that put frames on the panel (the presenter and its
 * helpers); 0 is the kernel's default for its threads. By default 92 (BASEPRI_PREEMPT): at the kernel's they lost the
 * processors to the window server's own real-time threads under load, and a frame took 3.6 ms to put on the panel
 * as a rule but 10.9 for one in ten (touch_lat=1, 30 October), more than the whole of a refresh at 120 Hz.
 */
static TUNABLE(uint32_t, gThreadPriority, "fb_pri", 92);
extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void * parameter, integer_t priority,
    thread_t * new_thread);
/* a thread of the panel's, at fb_pri */
static kern_return_t
panel_thread_start(thread_continue_t continuation, void * parameter, thread_t * thread)
{
	if (gThreadPriority >= 1 && gThreadPriority <= 95) {
		return kernel_thread_start_priority(continuation, parameter, (integer_t) gThreadPriority, thread);
	}
	return kernel_thread_start(continuation, parameter, thread);
}
/* kern.hakios.fb_surface: the IOSurface on the panel, for a picture of what is there (SpringBoard's wallpaper_freeze=1) */
SYSCTL_DECL(_kern_hakios);
static uint32_t gShownSurface;
SYSCTL_UINT(_kern_hakios, OID_AUTO, fb_surface, CTLFLAG_RD | CTLFLAG_LOCKED, &gShownSurface, 0, "the IOSurface last put on the panel");

#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: drivers/input/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);
/*
 * Boot-args fb_flip=1 and fb_dpp=1: every frame drawn into a buffer of its own and shown from the display controller's
 * next refresh on, never drawn into the buffer the controller is reading (which tears: half a frame and half the next
 * reach the panel). The Galaxy S9 only, through its DPU (drivers/display/dpu/hakios.exynos_dpu.c, which says how; elsewhere the
 * stand-ins of drivers/core/hakios.stubs.c say no and nothing changes).
 *   fb_flip=1  three buffers of the panel's size (the boot framebuffer and two more); the scaling on the processor, as
 *              without it.
 *   fb_dpp=1   three buffers of the display's size (on the S9 1125 x 2313, fb_native): a frame is copied into one, not scaled,
 *              and the DPU's VGF0 scaler enlarges it to the panel. fb_filter means nothing then (the scaler filters).
 * A frame is flipped only once the flip before it has been taken (at a refresh), so frames reach the panel at most once
 * a refresh, and a buffer is drawn into only when it is neither shown nor about to be. Either mode falls back to
 * drawing into the boot framebuffer, as without them: when the DPU is not as expected, when the scaler reports an
 * error, or when a flip is not taken within FLIP_STUCK_MS.
 * Boot-arg fps_log=1: a line a second while frames come ("fps: ...", in the loader's log and the kernel's): swaps
 * presented, frames drawn, flips taken, swaps lost and held, the drawing (scaling or copy) time and the wait for the
 * flip before, and the display controller's own refresh count (the S9's DECON: hakios_scanout_refreshes(); 0 elsewhere).
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
static TUNABLE(uint32_t, gDPP, "fb_dpp", 0);
static TUNABLE(uint32_t, gFpsLog, "fps_log", 0);
/*
 * Boot-arg fb_zc=1 (with fb_dpp=1): nothing copied. The window server's display surfaces -- the display's size, one plane
 * of 'BGRA', made by backboardd -- are made of the DPU driver's scan-out pool (MiniIOSurface.cpp, MiniIOSurfaceScanout():
 * physically contiguous, below 4 GB, write-combined), the GPU draws CoreAnimation's frame into one, and a frame whose
 * only layer is such a surface over the whole display is shown by pointing VGF0 at it (hakios_scanout_show()) at the
 * next refresh, after the flip before has been taken: no copy into a buffer of the scan-out's, which took 1.5 to 4 ms of
 * the presenter's every frame. The surface is held, and its use count raised (IOSurfaceIsInUse()), from its flip until
 * another one's has been taken. A frame of any other kind -- more than one layer, a part of the display, a surface of
 * ordinary memory, or the very surface on the panel drawn into again -- is copied as before. fps_log says how many went
 * which way ("zc: N in place, N copied, N into the one shown").
 */
static TUNABLE(uint32_t, gZeroCopy, "fb_zc", 0);
/*
 * fb_zc's surfaces, as the iPhone's display driver keeps them (boot-arg fb_zc_hold, kern.hakios.fb_zc_hold while it runs;
 * a sum, 3 by default; 0 is how fb_zc first was):
 *   1  in use from SwapEnd on: a display surface's use count (IOSurfaceIsInUse()) is raised when its swap is handed over,
 *      not only once it is on the panel, and comes down when the presenter is done with the frame; it stays up while the
 *      surface is shown, as an iPhone's driver keeps it. (CoreAnimation did not make a third surface for it: two in turn
 *      either way, fps_log's "N surfaces in turn". It goes by SwapWait, see fb_swapwait.)
 *   2  let go at the refresh: the presenter watches for the flip it asked for to be taken (every FLIP_POLL_US while it
 *      has no frame to show), and lets the surface shown before go then, not when the next frame comes; and a frame drawn
 *      into the surface on the panel (or about to be) is not copied: it is already where the panel reads, and a copy,
 *      read back from write-combined memory, took 5 to 9 ms of the presenter's. Without it about half the frames were
 *      counted "into the one shown" and copied (PR 160's first run); they were not: the surface's flip had been taken
 *      long before, it was only let go of at the next frame. With it, on the S9 swiping the home screen (the USB console's
 *      "swipe"), 0 frames of 564 copied and 0 into the one shown.
 */
static uint32_t gZcHold = 3;
#define FLIP_POLL_US    1000
/*
 * Boot-arg fb_rt=1 (kern.hakios.fb_rt): the thread that hands the swaps over (backboardd's render thread, CoreAnimation's
 * frames) is made real-time at its first swap, as an iPhone's is (a period of a refresh, fb_rt_us of work in it, default
 * 8000): the scheduler gives it a processor at once and the faster cores first. 0 by default.
 */
static uint32_t gRenderRT = 0, gRenderRTMicros = 8000;
/*
 * Boot-arg fb_swapwait (kern.hakios.fb_swapwait; 1 when fb_zc scans out the window server's surfaces, 0 otherwise): SwapWait
 * as the iPhone's driver answers it -- not at once, but once the swap it names is on the panel (its flip taken at a
 * refresh, or the frame drawn where the panel reads it), two refreshes at most. backboardd asks it with the number of a
 * swap it handed over, for the one just handed over and for the one before, and draws its next frame when answered, into
 * the other of its two surfaces: the one the panel still shows until that swap's flip is taken. Answered at once
 * (fb_swapwait=0), the GPU draws into a surface while the DECON sends it to the panel, a transfer of 9.6 ms of each 16.7 ms
 * refresh: tearing. A build that sampled the surface on the panel saw it change before the next flip was even asked in
 * 69 of 1683 frames, and between that flip being asked and taken in 438 more (a sparse sample, so at least that many;
 * the S9, the USB console's "swipe", 4 November). With fb_swapwait=1, 0 and 4 of 1745, at no cost in pacing: refreshes
 * missed in 11 s of swiping 27, 28 and 34, against 24, 73 and 84 answered at once. With fb_zc off nothing draws into what
 * the DECON reads, and it is answered at once. Samsung's driver has the same rule: a window's release fence fires only
 * once the frame after it has been taken (decon_update_regs(), dpu_9810), and the GPU does not draw into a buffer until then.
 */
static uint32_t gSwapWait = 0;
SYSCTL_DECL(_kern_hakios);
SYSCTL_UINT(_kern_hakios, OID_AUTO, fb_zc_hold, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gZcHold, 0,
    "fb_zc: 1 surfaces in use from SwapEnd on, 2 let go at the refresh and not copied when drawn into again (3: both)");
SYSCTL_UINT(_kern_hakios, OID_AUTO, fb_swapwait, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapWait, 0,
    "SwapWait waits until the swap is on the panel (1)");
SYSCTL_UINT(_kern_hakios, OID_AUTO, fb_rt, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gRenderRT, 0,
    "the render thread made real-time at its next swap (1)");
/* kern.hakios.ksample: a thread's stacks while it runs, for what holds frames up (osfmk/arm64/user_exception_trace.c) */
extern "C" int hakios_ksample_get(void);
extern "C" bool hakios_ksample_set(int value);
static int
ksampleSysctl(__unused struct sysctl_oid * oidp, __unused void * arg1, __unused int arg2, struct sysctl_req * req)
{
	int value = hakios_ksample_get(), error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if ((error = SYSCTL_IN(req, &value, sizeof(value))) != 0) {
		return error;
	}
	return hakios_ksample_set(value) ? 0 : EINVAL;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, ksample, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    ksampleSysctl, "I", "sample a thread while it runs: 1 SpringBoard's main, 2 backboardd's render thread, N process N's main");
#define SCAN_BUFFERS    3
#define FLIP_STUCK_MS   100
#define NO_BUFFER       (~0u)
extern "C" {
int hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);
int hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t * phys);
int hakios_scanout_attach(int mode, const uint64_t * phys, unsigned count, uint32_t width, uint32_t height,
    uint32_t rowbytes, unsigned shown);
int hakios_scanout_flip(unsigned index);
int hakios_scanout_show(uint64_t phys, uint32_t rowbytes);
int hakios_mali_second(uint64_t * busy_ns, uint32_t * jobs, uint64_t * queued_ns, uint32_t * mhz, uint32_t * switches,
    int32_t * g3d_mc);
int hakios_scanout_latched(void);
void hakios_scanout_detach(const char * why);
uint32_t hakios_scanout_refreshes(void);
void loader_log_line(const char * line);
void flush_dcache64(addr64_t addr, unsigned count, int phys);
vm_offset_t ml_vtophys(vm_offset_t vaddr);
}

static void scanSay(const char * format, ...) __printflike(1, 2);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

/*
 * Boot-arg fb_measure=1: how often the panel is really refreshed. The boot loader leaves the display controller running
 * and this driver only writes into the memory it scans, so fb_hz says how often frames are made, not how often the
 * panel shows one. The frame counter of the controller's DSI interface (SM7325's DPU: INTF_1 at 0x0ae36000,
 * INTF_FRAME_COUNT at 0xac, as Linux's catalogue for sc7280 has them) is read twice, a second apart, and the
 * difference printed. Reads only, once, a minute after the driver starts (what the kernel says in its first minute
 * does not come back with the phone's log), in a thread of its own; off by default.
 */
static TUNABLE(uint32_t, gMeasure, "fb_measure", 0);
#define DPU_SC7280_INTF1        0x0ae36000UL            /* SM7325 (sc7280): INTF_1, what the panel of redwood is on */
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0                   /* the line the interface is scanning (Linux's dpu_hw_intf.c) */
/*
 * Boot-arg fb_beam=0: frames written to the panel's memory whenever their beat comes, as before. The panel reads the
 * very memory this driver writes (one buffer, and it takes the scan 8.3 ms to read it, this driver 3 to write it): a
 * frame written across the line being read came out torn, a piece of the last frame and a piece of this one, moving
 * bands with doubled icons and text wherever something moved (3 November, seen on the phone and in screenshots).
 * Now each thread's share of the rows waits for the scan to have passed it and is written behind the line being read,
 * in time for the next scan to read all of the new frame: that scan shows the whole frame, nothing is ever read half
 * written. The line comes from the interface's own counter; reads only. It is looked at a minute after the driver
 * starts, with fb_measure's reads, and if it does not count as a line counter should, frames stay on their beat.
 */
static TUNABLE(uint32_t, gBeam, "fb_beam", 1);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
static inline uint32_t refreshHz(void);

/*
 * Boot-arg fb_intf=PHYSICAL: the registers of the display interface the panel is on, for a port. Without it the SoC the kernel was
 * configured for says (hakios_kver): SM7325's address read on another SoC is somebody else's registers, or an abort, so on a SoC
 * not known here nothing is read. (Upstream hakios-xnu-kernel f8976bc.)
 */
static TUNABLE(uint64_t, gIntfPhysical, "fb_intf", 0);

/* (Scion: on a SoC with no interface known here, as the Galaxy S9, fb_beam starts no look and says nothing: its frames are as before) */
static bool
beamWanted(void)
{
	char version[128] = "";

	return gBeam != 0 && (gIntfPhysical != 0 || (PE_parse_boot_argn("hakios_kver", version, sizeof(version)) &&
	    strnstr(version, "-sc7280-", sizeof(version)) != NULL));
}

static vm_offset_t
panelInterface(void)
{
	char version[128] = "";
	uint64_t physical = gIntfPhysical;

	if (physical == 0 && PE_parse_boot_argn("hakios_kver", version, sizeof(version)) && strnstr(version, "-sc7280-", sizeof(version)) != NULL) {
		physical = DPU_SC7280_INTF1;
	}
	return physical != 0 ? ml_io_map((vm_offset_t) (physical & ~0x3fffULL), 0x4000) + (vm_offset_t) (physical & 0x3fffULL) : 0;
}

static void
measurePanel(__unused void * arg, __unused wait_result_t wr)
{
	vm_offset_t intf;
	uint32_t on, first, second;

	IOSleep(60000);
	intf = panelInterface();
	if (intf == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_measure: no display interface known for this SoC (fb_intf=)\n");
		return;
	}
	on = *(volatile uint32_t *)(intf + INTF_TIMING_ENGINE_EN);
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	IOSleep(1000);
	second = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	IOLog("MiniIOMobileFramebuffer: the panel: DSI interface's timing engine %s, %u refreshes in one second "
	    "(frames are made at %u a second: fb_hz)\n", (on & 1) ? "on" : "OFF", second - first, refreshHz());
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fPointsWide, fPointsHigh;   /* the display in points, at kFBDefaultScale (fbscale draws them at another) */
	bool            fNative;                    /* fb_native chose the display's shape */
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * With the shared framebuffer (SCFB): a cached copy of what the panel's memory holds. drawRows
	 * compares each new row with it and writes only what differs to both, so the app is told
	 * exactly what changed (and reads of the write-combined panel are never needed). Each thread
	 * keeps its own bounds and folds them into fDamage* once per share of rows.
	 */
	uint32_t *      fShadow;
	bool            fShadowStale;           /* the panel was drawn without it (text console): rewrite all */
	uint32_t        fDamageX0, fDamageY0, fDamageX1, fDamageY1;     /* inclusive; X0 > X1: nothing */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	uint8_t         fSwapInUse[SWAP_QUEUE];                 /* ... the layers whose use count SwapEnd raised (fb_zc_hold 1) */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	vm_offset_t     fBeamIntf;              /* fb_beam: the DSI interface's registers, once they were seen to count */
	uint32_t        fBeamLines;             /* ... lines of a scan, blanking included */
	uint64_t        fBeamPeriod;            /* ... how long a scan takes */
	uint64_t        fBeamScan;              /* ... when the scan that shows the frame being written begins */
	uint64_t        fBeamShare;             /* ... how long a thread's share of the rows takes to write */
	uint32_t        fStatScans, fStatScansMissed;   /* ... frames in time for the next scan, and those that were not */
	/* ... for each thread's share of the rows: how many, their time, begun after the scan reached them or not done before it left them, and done after it reached them */
	uint64_t        fStatShareTime[8];
	uint32_t        fStatShares[8], fStatShareTorn[8], fStatShareClose[8];
	uint32_t        fStatSettleReused;      /* ... frames not drawn again filtered: the GPU had their surface for the next frame */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint32_t        fStatSettled;           /* frames drawn again, filtered, once they stood still (fb_filter=2) */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */
	uint64_t        fRecCost, fRecCostMax;  /* what copying the recorded frames took (mach_absolute_time), and the worst */
	uint32_t        fRecCosted;             /* ... of how many frames: a line every 120 */
	/* fb_flip / fb_dpp: the scan-out's buffers */
	int             fScanMode;              /* 0 the boot framebuffer; 1 flips; 2 flips, the DPU scales; -1, -2 set up, not yet taken */
	unsigned        fScanCount, fScanFront, fScanPending, fScanTarget;
	uint8_t *       fScanBase[SCAN_BUFFERS];
	uint64_t        fScanPhys[SCAN_BUFFERS];
	IOMemoryDescriptor * fScanMemory[SCAN_BUFFERS];  /* (IOKit's buffers, or the DPU driver's memory described) */
	IOMemoryMap *   fScanMap[SCAN_BUFFERS];
	uint8_t *       fBootBase;              /* the boot framebuffer, as it was before fBase pointed elsewhere */
	uint32_t        fScanWidth, fScanHeight, fScanRowBytes;
	uint32_t        fBootWidth, fBootHeight, fBootRowBytes;
	bool            fScanSaid;              /* the first frame into a scan-out buffer has been told of */
	/* fb_zc: the surface VGF0 scans out now, and the one flipped to and not yet taken (each held, its use count raised) */
	MiniIOSurface * fZcFront, * fZcPending;
	bool            fZcSaid;
	bool            fZcFrame;               /* show(): this frame is scanned out as it is: drawLayer() draws nothing */
	uint32_t        fFpsZc, fFpsZcCopied, fFpsZcBusy;       /* fps_log: frames shown in place, copied, drawn into the one shown */
	uint32_t        fFpsZcAgain;                            /* ... of those drawn into the one shown, how many not copied (fb_zc_hold 2) */
	uint32_t        fFpsZcSurfaces[4], fFpsZcSurfaceCount;  /* ... the different surfaces this second's frames came from (up to 4) */
	/* fps_log: the render thread (the one that hands the swaps over): its processor time from one swap to the next, the
	 * processors it handed them over on (a bit for each, and how many on the slower cores) */
	thread_t        fCpuThread;
	uint64_t        fCpuLast, fFpsCpu, fFpsCpuMax;
	uint32_t        fFpsCpuFrames, fFpsCpuMask, fFpsCpuSlow;
	thread_t        fRTThread;                              /* (fb_rt: the thread last made real-time, or tried) */
	/* SwapWait (fb_swapwait): the last swap on the panel, the one whose flip waits to be taken; under fSwapLock */
	uint32_t        fShownID, fFlipID, fLastSwapID;
	uint32_t        fFpsWaits, fFpsWaitLast, fFpsWaitBefore, fFpsWaitOther, fFpsWaitsSlept, fFpsWaitsTimedOut;
	uint64_t        fFpsWaitSlept;
	/* fps_log: refreshes a moving picture missed (no new frame though frames were coming), and the longest gap */
	uint64_t        fFpsLastShow, fFpsGapMax;
	uint32_t        fFpsMissed;
	/* fps_log: this second's counts */
	uint32_t        fFpsPresents, fFpsShown, fFpsFlips, fFpsLost, fFpsHeld, fFpsPartial, fFpsIdle, fFpsRefreshes;
	thread_t        fSwapThread;            /* fps_log: the thread the last swap came from, and how the kernel classed it */
	uint32_t        fSwapClass;
	SInt32          fFpsVSyncs;
	uint32_t        fFpsSameSurface, fFpsUndrawn;   /* fps_log: frames from the surface of the frame before; shown with the GPU not done */
	uint32_t        fLastSurface;
	uint64_t        fFpsDraw, fFpsDrawMax, fFpsWait, fFpsWaitMax;
	thread_call_t   fFpsCall;

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	void chooseDisplaySize(void);
	void publishRefresh(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void settle(const uint8_t * swap, MiniIOSurface ** surfaces);
	bool fSettling;                 /* drawing the last frame again, filtered (fb_filter=2) */
	bool fFilterFrame;              /* this frame came slowly: filtered as it is (fb_filter=2) */
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	void damage(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
	static void helperMain(void * self, wait_result_t result);
	static void beamProbeMain(void * self, wait_result_t result);
	void beamProbe(void);
	uint64_t beamNextScan(uint64_t now);
	void beamWait(const struct RowJob * job, int32_t lastRow);
	void beamShareDone(const struct RowJob * job, unsigned share, int32_t firstRow, int32_t lastRow, uint64_t began);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
	void scanSetup(void);
	bool scanBegin(bool covered);
	bool scanEnd(void);
	bool scanLatch(void);
	void scanTaken(void);
	bool scanPoll(void);
	bool flipAsked(void) { return fScanPending != NO_BUFFER || fZcPending != NULL; }
	void renderThread(void);
	void markShown(uint32_t id);
	void swapWait(uint32_t id);
	bool zcShow(const uint8_t * swap, MiniIOSurface ** surfaces);
	void zcLetGo(MiniIOSurface ** which);
	void scanDrop(const char * why);
	void drawLayers(const uint8_t * swap, MiniIOSurface ** surfaces);
	static void fpsFired(thread_call_param_t self, thread_call_param_t unused);
	void fpsLine(void);
	void swapSource(void);
	int scanned(uintptr_t * base, uint32_t * width, uint32_t * height, uint32_t * rowbytes, unsigned * index);
};

/* the one framebuffer, for the USB console's shot (hakios_fb_scanned) */
static IOMobileFramebuffer * gScanOwner;

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

/*
 * The display's size (see fb_native above): fbwidth and fbheight as given, else the panel's own shape 375 points wide at
 * 3 pixels a point where the panel is wider than that and not already the iPhone's shape, else the iPhone's 1125 x 2436.
 */
void
IOMobileFramebuffer::chooseDisplaySize(void)
{
	/* the panel's shape against the iPhone's: w / h against 1125 / 2436, as w * 2436 against h * 1125 */
	uint64_t across = (uint64_t) fPanelWidth * kFBDefaultHeight, down = (uint64_t) fPanelHeight * kFBDefaultWidth;
	uint64_t apart = across > down ? across - down : down - across;
	uint32_t points;

	fNative = false;
	fWidth = kFBDefaultWidth;
	fHeight = kFBDefaultHeight;
	if (gFBWidth != 0 || gFBHeight != 0) {
		fWidth = gFBWidth != 0 ? gFBWidth : kFBDefaultWidth;
		fHeight = gFBHeight != 0 ? gFBHeight : kFBDefaultHeight;
	} else if (gFBNative != 0 && fPanelWidth > kFBNativePointsWide * kFBDefaultScale && fPanelHeight > fPanelWidth &&
	    100 * apart > down) {
		/* the panel's shape further than 1 % from the iPhone's: as many points tall as it makes, to the nearest */
		points = (uint32_t) ((2 * (uint64_t) kFBNativePointsWide * fPanelHeight + fPanelWidth) / (2 * (uint64_t) fPanelWidth));
		fWidth = kFBNativePointsWide * kFBDefaultScale;
		fHeight = points * kFBDefaultScale;
		fNative = true;
	}
	fPointsWide = fWidth / kFBDefaultScale;
	fPointsHigh = fHeight / kFBDefaultScale;
	if (fPointsWide == 0 || fPointsHigh == 0) {
		fPointsWide = kFBPointsWide;
		fPointsHigh = kFBPointsHigh;
	}
}

/*
 * The refresh the window server clocks its frames by: the property IOMFBDisplayRefresh, a dictionary of the shortest
 * interval between refreshes, the longest, and the step between them (QuartzCore, windowserver-apple-display.cpp:
 * displayMinRefreshInterval, displayRefreshStepMachTime; CoreMedia's FPSupport: displayMaxRefreshInterval,
 * displayRefreshStep). Without it CoreAnimation takes a built-in panel for a 60 Hz one whatever its timing says: the
 * mode it makes is "internal_panel (fixed)" at 60, UIScreen's maximumFramesPerSecond is 60, and frames came at 60 on
 * the 120 Hz panel (userland/src/fps_info.m, 30 October).
 * The intervals are in units of 2^-32 of a second (32.32 fixed point), not in mach_absolute_time's: given 1/120 s in
 * the phone's ticks (160000 of 19.2 MHz) CoreAnimation answered a refresh rate of 26844 a second, which is
 * 2^32 / 160000. Only the step whose name says MachTime is in ticks. This panel does not vary its rate, so all of
 * them are the one interval, fb_hz's.
 */
void
IOMobileFramebuffer::publishRefresh(void)
{
	OSDictionary * refresh = OSDictionary::withCapacity(4);
	uint32_t told = gCARefreshHz >= 1 && gCARefreshHz <= 480 ? gCARefreshHz : refreshHz();
	uint64_t interval = (1ULL << 32) / told, ticks;

	if (refresh == NULL) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC / told, &ticks);
	setNumber(refresh, "displayMinRefreshInterval", interval);
	setNumber(refresh, "displayMaxRefreshInterval", interval);
	setNumber(refresh, "displayRefreshStep", interval);
	setNumber(refresh, "displayRefreshStepMachTime", ticks);
	setProperty("IOMFBDisplayRefresh", refresh);
	refresh->release();
	IOLog("MiniIOMobileFramebuffer: IOMFBDisplayRefresh: %u Hz (the panel: %u), an interval of %llu (2^-32 s), %llu ticks\n",
	    told, refreshHz(), interval, ticks);
}

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	publishRefresh();
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

/*
 * What iOS sets on the display service -- its brightness, among other things, on a phone whose panel this kernel can
 * dim: hakios_panel_brightness() (drivers/display/panel/hakios.backlight.cpp) hands it to the panel driver that registered there,
 * the Galaxy S9's (drivers/display/panel/hakios.s6e3ha8.c); with none, ENODEV. Which key iOS 16's CoreBrightness uses for an
 * OLED iPhone's display is not known here yet (boot-arg backlight= publishes AppleARMBacklight as well, to find out
 * whether it is that service iOS sets instead): the first calls are said in the log, key by key, with the
 * value of a number, and a key named "brightness" (any case) with a number from 0 to 1 or to 255 is taken as the
 * brightness. Nothing is kept in the registry, and what is not a brightness is refused as IOService refuses it.
 */
static bool
is_brightness(const char * key)
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

IOReturn
IOMobileFramebuffer::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	static unsigned said;
	char who[32] = "?";
	bool taken = false;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSNumber * number = key ? OSDynamicCast(OSNumber, dict->getObject(key)) : NULL;

		if (key == NULL) {
			continue;
		}
		if (said < 64) {
			said++;
			if (number != NULL) {
				IOLog("IOMobileFramebuffer: %s sets %s = %llu\n", who, key->getCStringNoCopy(), number->unsigned64BitValue());
			} else {
				IOLog("IOMobileFramebuffer: %s sets %s (%s)\n", who, key->getCStringNoCopy(),
				    dict->getObject(key) ? dict->getObject(key)->getMetaClass()->getClassName() : "?");
			}
		}
		if (number != NULL && is_brightness(key->getCStringNoCopy())) {
			uint64_t value = number->unsigned64BitValue();

			/* (refused, as before, where no panel driver takes it: every other phone) */
			taken = hakios_panel_brightness(value <= 1 ? (int)(value * 255) : value > 255 ? 255 : (int)value) == 0;
		}
	}
	keys->release();
	return taken ? kIOReturnSuccess : kIOReturnUnsupported;
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	void * vgpuFB = NULL;
	uint32_t vgpuWidth = 0, vgpuHeight = 0, vgpuStride = 0;

	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		if (hakios_vgpu_display(&vgpuFB, &vgpuWidth, &vgpuHeight, &vgpuStride) != 0) {
			IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u), and no virtio-gpu\n",
			    vinfo.v_width, vinfo.v_height, vinfo.v_depth);
			return false;
		}
		gVGPU = true;
		IOLog("MiniIOMobileFramebuffer: the panel is virtio-gpu's scanout, %ux%u\n", vgpuWidth, vgpuHeight);
	}
	if (beamWanted() && kernel_thread_start(beamProbeMain, this, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "the panel's scan: looked at");
		thread_deallocate(thread);
	}
	if (gMeasure && kernel_thread_start(measurePanel, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (panel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (panel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = gVGPU ? (uint8_t *) vgpuFB : (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = gVGPU ? vgpuWidth : vinfo.v_width;
	fPanelHeight = gVGPU ? vgpuHeight : vinfo.v_height;
	fRowBytes = gVGPU ? vgpuStride : vinfo.v_rowbytes;
	chooseDisplaySize();
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		if (gFBSubtype == 0) {
			fWidth = fPointsWide * scale;
			fHeight = fPointsHigh * scale;
		} else {
			IORegistryEntry * product = IORegistryEntry::fromPath("/product", gIODTPlane);
			uint32_t subtype = gFBSubtype;

			if (product != NULL) {
				product->setProperty("artwork-device-subtype", &subtype, sizeof(subtype));
				product->release();
			}
		}
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	scfbAttach();
	if (gSCFB != NULL) {
		fShadow = (uint32_t *) IOMalloc((size_t) fPanelWidth * fPanelHeight * 4);
		fShadowStale = true;
		if (fShadow == NULL) {
			IOLog("MiniIOMobileFramebuffer: no memory for the shadow of the panel: every frame is published whole\n");
		}
	}
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u (%ux%u points) on a panel of %ux%u (%u bytes per row)%s\n", fWidth,
	    fHeight, fPointsWide, fPointsHigh, fPanelWidth, fPanelHeight, fRowBytes, fNative ? ": the panel's own shape (fb_native)" : "");
	fBootBase = fBase;
	fBootWidth = fPanelWidth;
	fBootHeight = fPanelHeight;
	fBootRowBytes = fRowBytes;
	fScanFront = fScanPending = fScanTarget = NO_BUFFER;
	scanSetup();
	gScanOwner = this;
	if (gTimerCoalescing == 0 && mach_timer_coalescing_enabled) {
		mach_timer_coalescing_enabled = FALSE;
		scanSay("MiniIOMobileFramebuffer: the kernel's timer coalescing turned off: the window server's frame timer fires "
		    "when it is due, not up to 100 ms late (fb_tcoal=1 keeps it)\n");
	} else {
		scanSay("MiniIOMobileFramebuffer: the kernel's timer coalescing left %s (fb_tcoal=%u)\n",
		    mach_timer_coalescing_enabled ? "on" : "off", gTimerCoalescing);
	}
	if (gFpsLog) {
		uint64_t deadline;

		fFpsCall = thread_call_allocate(&IOMobileFramebuffer::fpsFired, this);
		clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
		if (fFpsCall != NULL) {
			thread_call_enter_delayed(fFpsCall, deadline);
		}
	}

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	hakios_backlight_publish(this);
	return true;
}

/*
 * Are the first `rows` rows of a surface, `columns` 32-bit pixels of each, inside the memory it
 * has? Its stride, element height and size come from whoever made it, so they are checked here,
 * before anything reads the rows: in 64 bits, so that nothing wraps.
 */
static bool
surfaceHolds(const MiniIOSurface * s, uint64_t rows, uint64_t columns)
{
	if (rows == 0 || columns == 0) {
		return true;
	}
	return (uint64_t) s->info.offset + (rows - 1) * s->info.bytesPerRow + columns * 4 <= s->info.allocSize;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	/* a surface whose rows do not fit in its memory is not read */
	if (!surfaceHolds(s, fHeight, fWidth)) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	/* what the copy cost: it runs in the display's thread, between one frame and the next */
	now = mach_absolute_time() - now;
	fRecCost += now;
	fRecCostMax = now > fRecCostMax ? now : fRecCostMax;
	if (++fRecCosted == 120) {
		IOLog("MiniIOMobileFramebuffer: recording a frame took %llu us on average, %llu at worst (the last 120)\n",
		    micros(fRecCost) / 120, micros(fRecCostMax));
		fRecCost = fRecCostMax = 0;
		fRecCosted = 0;
	}
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
/* A layer's rectangle on the panel, in pixels, further out than this is not drawn (the panel is a few thousand wide). */
#define DST_LIMIT       (1 << 24)

struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         ry0, ry1;       /* the rows of the rectangle that are on the panel: all that is looked at, whatever its size */
	bool            filter, unscaled;
	bool force;              /* SCFB: write every row whole (the shadow does not match the panel) */
	uint64_t        scan;           /* fb_beam: when the scan that is to show the frame begins (0: write at once) */
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;
	uint32_t dx0 = ~0u, dy0 = ~0u, dx1 = 0, dy1 = 0;        /* what this share changed */

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (fShadow != NULL && job->unscaled) {
			/* the changed span of the row, by the shadow; only that goes to the panel */
			const uint32_t * row = (const uint32_t *) (pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4);
			uint32_t * shadow = fShadow + (size_t) y * fPanelWidth + px0;
			int32_t a = 0, b = sw - 1;

			if (!job->force) {
				while (a < sw && shadow[a] == row[a]) {
					a++;
				}
				if (a == sw) {
					continue;
				}
				while (b > a && shadow[b] == row[b]) {
					b--;
				}
			}
			memcpy(shadow + a, row + a, (size_t) (b - a + 1) * 4);
			memcpy(to + px0 + a, row + a, (size_t) (b - a + 1) * 4);
			dx0 = (uint32_t) (px0 + a) < dx0 ? (uint32_t) (px0 + a) : dx0;
			dx1 = (uint32_t) (px0 + b) > dx1 ? (uint32_t) (px0 + b) : dx1;
			dy0 = (uint32_t) y < dy0 ? (uint32_t) y : dy0;
			dy1 = (uint32_t) y;
			continue;
		}
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fShadow != NULL) {
			/* scaled: pixel by pixel against the shadow, only changes written to the panel */
			uint32_t * shadow = fShadow + (size_t) y * fPanelWidth;
			int32_t a = -1, b = -1;

			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t c = fColumn[x], value;

				if (filter) {
					uint32_t w = fColumnWeight[x], p = from[c];

					value = w == 0 || p == from[c + 1] ? p : blend(p, from[c + 1], w);
				} else {
					value = from[c];
				}
				if (job->force || shadow[x] != value) {
					shadow[x] = value;
					to[x] = value;
					a = a < 0 ? x : a;
					b = x;
				}
			}
			if (a >= 0) {
				dx0 = (uint32_t) a < dx0 ? (uint32_t) a : dx0;
				dx1 = (uint32_t) b > dx1 ? (uint32_t) b : dx1;
				dy0 = (uint32_t) y < dy0 ? (uint32_t) y : dy0;
				dy1 = (uint32_t) y;
			}
			continue;
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
	if (dx0 <= dx1) {
		damage(dx0, dy0, dx1, dy1);
	}
}

/* Folds a share's changed bounds into the frame's (shares run on several threads at once). */
static inline void
atomicMin(uint32_t * p, uint32_t v)
{
	uint32_t old = __atomic_load_n(p, __ATOMIC_RELAXED);

	while (v < old && !__atomic_compare_exchange_n(p, &old, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

static inline void
atomicMax(uint32_t * p, uint32_t v)
{
	uint32_t old = __atomic_load_n(p, __ATOMIC_RELAXED);

	while (v > old && !__atomic_compare_exchange_n(p, &old, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

void
IOMobileFramebuffer::damage(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	atomicMin(&fDamageX0, x0);
	atomicMin(&fDamageY0, y0);
	atomicMax(&fDamageX1, x1);
	atomicMax(&fDamageY1, y1);
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->ry1 - job->ry0;
		y0 = job->ry0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->ry0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->beamWait(job, y1);
		{
			uint64_t began = mach_absolute_time();

			fb->drawRows(job, y0, y1, blend);
			fb->beamShareDone(job, index + 1, y0, y1, began);
		}
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/* fb_beam: whether the interface's counters count as they should, and how many lines and how long a scan is. */
void
IOMobileFramebuffer::beamProbeMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->beamProbe();
}

void
IOMobileFramebuffer::beamProbe(void)
{
	vm_offset_t intf;
	uint32_t first, frames, line, lines = 0, smallest = ~0u;
	uint64_t began, ended, until;

	IOSleep(60000);
	intf = panelInterface();
	if (intf == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_beam: no display interface known for this SoC (fb_intf=): frames stay on their beat\n");
		return;
	}
	if (!(*(volatile uint32_t *)(intf + INTF_TIMING_ENGINE_EN) & 1)) {
		IOLog("MiniIOMobileFramebuffer: fb_beam: the interface's timing engine is off: frames stay on their beat\n");
		return;
	}
	/* a whole number of scans, from one frame's start to another's */
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (*(volatile uint32_t *)(intf + INTF_FRAME_COUNT) == first && mach_absolute_time() < until) {
	}
	began = mach_absolute_time();
	first = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &until);
	until += began;
	while (mach_absolute_time() < until) {
		line = *(volatile uint32_t *)(intf + INTF_LINE_COUNT);
		lines = line > lines ? line : lines;
		smallest = line < smallest ? line : smallest;
	}
	IOSleep(450);
	frames = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT);
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (*(volatile uint32_t *)(intf + INTF_FRAME_COUNT) == frames && mach_absolute_time() < until) {
	}
	ended = mach_absolute_time();
	frames = *(volatile uint32_t *)(intf + INTF_FRAME_COUNT) - first;
	if (frames == 0 || lines < fPanelHeight || lines > fPanelHeight * 2 || smallest > fPanelHeight / 4) {
		IOLog("MiniIOMobileFramebuffer: fb_beam: the interface's counters do not count as they should (%u frames, lines %u to %u): "
		    "frames stay on their beat\n", frames, smallest, lines);
		return;
	}
	fBeamLines = lines + 1;
	fBeamPeriod = (ended - began) / frames;
	absolutetime_to_nanoseconds(fBeamPeriod, &began);
	IOLog("MiniIOMobileFramebuffer: fb_beam: the panel's scan is %u lines, %llu us: frames are written behind it from now on\n",
	    fBeamLines, began / NSEC_PER_USEC);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	fBeamIntf = intf;
}

/* When the next scan the frame can be whole for begins: the first share has to be written before that scan reaches it. */
uint64_t
IOMobileFramebuffer::beamNextScan(uint64_t now)
{
	uint32_t line = *(volatile uint32_t *)(fBeamIntf + INTF_LINE_COUNT);
	uint64_t scan = now - fBeamPeriod * (line < fBeamLines ? line : fBeamLines - 1) / fBeamLines + fBeamPeriod;
	/*
	 * The first share is written from its top down about as fast as the scan reads it, so it may begin just before the
	 * scan does: by what it takes longer than the scan takes over the same rows, and a little. (Its whole time as a
	 * margin lost a frame in eight: each too late for one scan waits for the next, and all behind it with it.)
	 */
	uint64_t over = fThreads != 0 ? fBeamPeriod / fThreads : 0;
	uint64_t margin = (fBeamShare > over ? fBeamShare - over : 0) + fBeamPeriod / 16;
	bool missed = false;

	if (now + margin > scan) {
		scan += fBeamPeriod;
		missed = true;
	}
	/* (one frame a scan: the last frame's scan has its own) */
	while (fBeamScan != 0 && scan < fBeamScan + fBeamPeriod / 2) {
		scan += fBeamPeriod;
	}
	fStatScans++;
	fStatScansMissed += missed;
	return scan;
}

/* fb_beam: a share of the rows that ends at lastRow waits for the scan before the frame's to have passed it. */
void
IOMobileFramebuffer::beamWait(const struct RowJob * job, int32_t lastRow)
{
	if (job->scan != 0) {
		/* (the lines of the blanking are counted before the first row, as far as one can tell: taken as a margin either way) */
		uint64_t lines = (uint64_t) lastRow + (fBeamLines - fPanelHeight);
		uint64_t passed = job->scan - fBeamPeriod + fBeamPeriod * lines / fBeamLines;

		if (mach_absolute_time() < passed) {
			clock_delay_until(passed);
		}
	}
}

/* fb_beam: what a share of the rows did against the scan it is for (a frame torn at the share's edge is seen here, not in a screenshot). */
void
IOMobileFramebuffer::beamShareDone(const struct RowJob * job, unsigned share, int32_t firstRow, int32_t lastRow, uint64_t began)
{
	uint64_t now;

	if (fBeamIntf == 0) {
		return;                         /* (no beam, as on the Galaxy S9: nothing is counted) */
	}
	now = mach_absolute_time();
	__atomic_fetch_add(&fStatShareTime[share], now - began, __ATOMIC_RELAXED);
	__atomic_fetch_add(&fStatShares[share], 1, __ATOMIC_RELAXED);
	if (job->scan != 0) {
		uint64_t blank = fBeamLines - fPanelHeight;
		uint64_t top = job->scan + fBeamPeriod * ((uint64_t) firstRow + blank) / fBeamLines;
		uint64_t bottom = job->scan + fBeamPeriod * ((uint64_t) lastRow + blank) / fBeamLines;

		if (began > top || now > bottom) {
			__atomic_fetch_add(&fStatShareTorn[share], 1, __ATOMIC_RELAXED);
		} else if (now > top) {
			__atomic_fetch_add(&fStatShareClose[share], 1, __ATOMIC_RELAXED);
		}
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter == 1 || (gFilter == 2 && (fSettling || fFilterFrame));
	RowJob job;

	if (fZcFrame) {
		return;                         /* (fb_zc: the display controller reads the surface itself) */
	}

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rows and columns named must be in the surface's memory, whatever stride and size it was made with */
	if (!surfaceHolds(s, (uint32_t) (sy + sh), (uint32_t) (sx + sw))) {
		return;
	}
	/*
	 * the rectangle on the panel, worked out in 64 bits: the request's numbers are the caller's, and a
	 * rectangle of billions of rows is not drawn (nor looked at row by row)
	 */
	int64_t qx0 = (int64_t) dx * fPanelWidth / fWidth;
	int64_t qy0 = (int64_t) dy * fPanelHeight / fHeight;
	int64_t qx1 = ((int64_t) dx + dw) * fPanelWidth / fWidth;
	int64_t qy1 = ((int64_t) dy + dh) * fPanelHeight / fHeight;

	if (qx0 < -DST_LIMIT || qy0 < -DST_LIMIT || qx1 > DST_LIMIT || qy1 > DST_LIMIT) {
		return;
	}
	px0 = (int32_t) qx0;
	py0 = (int32_t) qy0;
	px1 = (int32_t) qx1;
	py1 = (int32_t) qy1;
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* nothing of it on the panel: nothing to draw (and the columns below are only worked out for ones that are) */
	if (px1 <= 0 || px0 >= (int32_t) fPanelWidth || py1 <= 0 || py0 >= (int32_t) fPanelHeight) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		bool allocated = fBlendRow != NULL;

		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			allocated = allocated && fHelperBlend[i] != NULL;
		}
		if (!allocated) {
			/* every array that did get made is given back at the size it was made with, so that no later growth frees one with the wrong size */
			if (fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
			for (unsigned i = 0; i + 1 < fThreads; i++) {
				if (fHelperBlend[i] != NULL) {
					IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
					fHelperBlend[i] = NULL;
				}
			}
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.ry0 = py0 < 0 ? 0 : py0;
	job.ry1 = py1 > (int32_t) fPanelHeight ? (int32_t) fPanelHeight : py1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.force = fShadowStale;
	job.scan = fSettling ? 0 : fBeamScan;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, job.ry0, job.ry1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	{
		int32_t last = job.ry0 + (int32_t) ((int64_t) (job.ry1 - job.ry0) / fThreads);
		uint64_t wrote;

		beamWait(&job, last);
		wrote = mach_absolute_time();
		drawRows(&job, job.ry0, last, fBlendRow);
		beamShareDone(&job, 0, job.ry0, last, wrote);
		wrote = mach_absolute_time() - wrote;
		/* (on the average, lately: one preempted now and then is no reason to give up a scan every time) */
		fBeamShare += (int64_t) (wrote - fBeamShare) / 8;
	}
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* the S9's CPU governor's frame boost (drivers/power/exynos/hakios.exynos_cpufreq.c; a stub elsewhere): a stamp, no wait */
extern "C" void hakios_exynos_cpufreq_frame(void);

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
		/* "tl: t ID NOW T1 ... T7": the swap's seven timestamps (IOMobileFramebufferSwapSetTimestamps, types 1 to 7, at its
		 * first 56 bytes), raw, beside mach_absolute_time now */
		uint64_t ts[7];

		memcpy(ts, swap, sizeof(ts));
		hakios_tl("tl: t %u %llu %llu %llu %llu %llu %llu %llu %llu\n", swapWord(swap, SWAP_ID), now, ts[0], ts[1], ts[2],
		    ts[3], ts[4], ts[5], ts[6]);
	}
	if (gFpsLog) {
		swapSource();
	}
	if (gRenderRT) {
		renderThread();
	}
	hakios_exynos_cpufreq_frame();
	hakios_mali_frame();
	thread_t me = current_thread();
	uint64_t runtime = thread_get_runtime_self();
	int cpu = cpu_number();

	IOLockLock(fSwapLock);
	/* fps_log: the render thread's processor time since its swap before (a pause of a second or more is not a frame's) */
	if (me == fCpuThread && fCpuLast != 0 && runtime >= fCpuLast && now - fSwapTime < pause) {
		uint64_t used = runtime - fCpuLast;

		fFpsCpu += used;
		fFpsCpuMax = used > fFpsCpuMax ? used : fFpsCpuMax;
		fFpsCpuFrames++;
	}
	fCpuThread = me;
	fCpuLast = runtime;
	fFpsCpuMask |= cpu >= 0 && cpu < 32 ? 1u << cpu : 0;
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	fFpsHeld += fSwapsWaiting >= swapQueue();
	fFpsPresents++;
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			fFpsLost++;
			IOLockUnlock(fSwapLock);
			markShown(swapWord(swap, SWAP_ID));     /* (never will be: nobody waits for it) */
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	fLastSwapID = swapWord(swap, SWAP_ID);

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
		if (sid != 0) {
			/* (SCGPU: a frame drawn into this surface is shown by the host itself: bsd/dev/hakios.vgpu3d_xnu.c) */
			hakios_vgpu3d_display_surface(sid);
		}
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	/* (fb_zc_hold 1: a display surface in use from now on, as the iPhone's driver has it, until the presenter is done) */
	fSwapInUse[slot] = 0;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		MiniIOSurface * held = fSwapSurfaces[slot][layer];

		if (held != NULL && held->scanPhys != 0 && gZeroCopy && (gZcHold & 1)) {
			MiniIOSurfaceUse(held, 1);
			fSwapInUse[slot] |= (uint8_t) (1u << layer);
		}
	}
	IOLockUnlock(fSwapLock);
}

/*
 * fps_log: which thread hands the swaps over, and how the kernel classes it for its timers (see fb_tcoal): real time or
 * not, in the background (DARWIN_BG) or not, its latency tier. Said when it changes. (On the swapping thread, unlocked.)
 */
void
IOMobileFramebuffer::swapSource(void)
{
	thread_t thread = current_thread();
	thread_time_constraint_policy_data_t rt;
	mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
	boolean_t notRealTime = TRUE;
	uint32_t bg, tier, cls;
	char name[32] = "?";

	if (thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t) &rt, &count, &notRealTime) != KERN_SUCCESS) {
		notRealTime = TRUE;
	}
	bg = proc_get_effective_thread_policy(thread, POLICY_DARWIN_BG) != 0;
	tier = (uint32_t) proc_get_effective_thread_policy(thread, POLICY_LATENCY_QOS);
	cls = (notRealTime ? 0 : 1) | bg << 1 | (tier & 0xff) << 2;
	if (thread == fSwapThread && cls == fSwapClass) {
		return;
	}
	fSwapThread = thread;
	fSwapClass = cls;
	proc_selfname(name, sizeof(name));
	scanSay("fps: the swaps come from %s[%d], thread %#llx: %s, %s, latency tier %u; timer coalescing %s\n", name,
	    proc_selfpid(), thread_tid(thread), notRealTime ? "not real-time" : "real-time", bg ? "in the background (DARWIN_BG)" :
	    "not in the background", tier, mach_timer_coalescing_enabled ? "ON: its timers may fire late" : "off");
}

/*
 * fb_rt=1: the thread handing the swaps over made real-time, once for each such thread: a period of a refresh, fb_rt_us
 * of processor time in it, done within the refresh, preemptible. (On an iPhone backboardd's render thread is so; here
 * it ran at the default priority and could wait behind anything.) The kernel's real-time fail-safe still demotes a
 * thread that keeps a processor for too long.
 */
void
IOMobileFramebuffer::renderThread(void)
{
	thread_t thread = current_thread();
	thread_time_constraint_policy_data_t rt;
	uint64_t period, work;
	kern_return_t kr;
	char name[32] = "?";

	if (thread == fRTThread) {
		return;
	}
	fRTThread = thread;
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	nanoseconds_to_absolutetime((uint64_t) (gRenderRTMicros >= 500 && gRenderRTMicros <= 15000 ? gRenderRTMicros : 8000) *
	    NSEC_PER_USEC, &work);
	work = work < period ? work : period / 2;
	rt.period = (uint32_t) period;
	rt.computation = (uint32_t) work;
	rt.constraint = (uint32_t) period;
	rt.preemptible = TRUE;
	kr = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t) &rt, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	proc_selfname(name, sizeof(name));
	scanSay("MiniIOMobileFramebuffer: fb_rt: %s[%d] thread %#llx made real-time (%llu us of every %llu): %d\n", name,
	    proc_selfpid(), thread_tid(thread), micros(work), micros(period), kr);
}

/* SwapWait: swap `id` is on the panel (or never will be): whoever waits for it, or for one before it, goes on */
void
IOMobileFramebuffer::markShown(uint32_t id)
{
	if (id == 0) {
		return;
	}
	IOLockLock(fSwapLock);
	if ((int32_t) (id - fShownID) > 0) {
		fShownID = id;
	}
	IOLockWakeup(fSwapLock, &fShownID, false);
	IOLockUnlock(fSwapLock);
}

/*
 * SwapWait for swap `id`: counted for fps_log (the last swap handed over, the one before it, or another), and with
 * fb_swapwait=1 waited for until it is on the panel, two refreshes at most.
 */
void
IOMobileFramebuffer::swapWait(uint32_t id)
{
	uint64_t began = mach_absolute_time(), deadline;
	bool slept = false;

	IOLockLock(fSwapLock);
	fFpsWaits++;
	if (id != 0 && id == fLastSwapID) {
		fFpsWaitLast++;
	} else if (id != 0 && id + 1 == fLastSwapID) {
		fFpsWaitBefore++;
	} else {
		fFpsWaitOther++;
	}
	if (gSwapWait && id != 0 && (int32_t) (id - fLastSwapID) <= 0) {
		clock_interval_to_deadline(2 * 1000000 / refreshHz(), NSEC_PER_USEC, &deadline);
		while ((int32_t) (id - fShownID) > 0) {
			slept = true;
			if (IOLockSleepDeadline(fSwapLock, &fShownID, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
				fFpsWaitsTimedOut += (int32_t) (id - fShownID) > 0;
				break;
			}
		}
	}
	if (slept) {
		fFpsWaitsSlept++;
		fFpsWaitSlept += mach_absolute_time() - began;
	}
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS] = { NULL };
	uint8_t inUse = 0;                      /* (fb_zc_hold 1: the layers whose use count SwapEnd raised) */
	bool unsettled = false;                 /* the frame on the panel was drawn unfiltered, and its surfaces are held */
	uint64_t lastFrame = 0, settleTime;
	unsigned fastFrames = 0;
	uint32_t shownDrawn[SWAP_LAYERS] = { 0 };       /* what the GPUs had last been asked to draw into the surfaces on the panel */
	uint64_t shownMali[SWAP_LAYERS] = { 0 };

	nanoseconds_to_absolutetime(SETTLE_MS * NSEC_PER_MSEC, &settleTime);

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			uint64_t deadline;

			/* (fb_zc_hold 2: a flip asked for is watched, and what it replaces let go of, at the refresh it is taken) */
			if ((gZcHold & 2) && gZeroCopy && fScanMode == 2 && flipAsked()) {
				clock_interval_to_deadline(FLIP_POLL_US, NSEC_PER_USEC, &deadline);
				if (IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && fSwapsWaiting == 0) {
					IOLockUnlock(fSwapLock);
					IOLockLock(fLock);              /* (as show() holds it about the scan-out's state) */
					(void) scanPoll();
					IOLockUnlock(fLock);
					IOLockLock(fSwapLock);
				}
				continue;
			}
			if (!unsettled) {
				IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
				continue;
			}
			clock_interval_to_deadline(SETTLE_MS, NSEC_PER_MSEC, &deadline);
			if (IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && fSwapsWaiting == 0) {
				bool reused = false;

				IOLockUnlock(fSwapLock);
				/*
				 * Not from a surface a GPU has been asked to draw into since: CoreAnimation has it back, and the next frame
				 * in it half drawn is what the panel would get (the next frame comes in a moment anyway). (Upstream 539d35e.)
				 */
				for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
					reused = reused || (surfaces[layer] != NULL &&
					    (surfaces[layer]->drawn != shownDrawn[layer] || surfaces[layer]->maliSeq != shownMali[layer]));
				}
				if (reused) {
					fStatSettleReused++;
				} else {
					settle(swap, surfaces);
				}
				unsettled = false;
				IOLockLock(fSwapLock);
			}
		}
		IOLockUnlock(fSwapLock);
		/* the frame before is done with now, settled or not */
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				if (inUse & (1u << layer)) {
					MiniIOSurfaceUse(surfaces[layer], -1);
				}
				MiniIOSurfaceDrop(surfaces[layer]);
				surfaces[layer] = NULL;
			}
		}
		inUse = 0;
		IOLockLock(fSwapLock);
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		inUse = fSwapInUse[fSwapFirst];
		IOLockUnlock(fSwapLock);
		{
			uint64_t now = mach_absolute_time();

			fastFrames = lastFrame != 0 && now - lastFrame <= settleTime ? fastFrames + 1 : 0;
			fFilterFrame = fastFrames < FAST_FRAMES;
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				shownDrawn[layer] = surfaces[layer] != NULL ? surfaces[layer]->drawn : 0;
				shownMali[layer] = surfaces[layer] != NULL ? surfaces[layer]->maliSeq : 0;
			}
			show(swap, surfaces);
			lastFrame = mach_absolute_time();
		}
		unsettled = gFilter == 2 && !fFilterFrame && fScanMode != 2 && fScanMode != -2;     /* (the DPU's scaler filters) */
		if (!unsettled) {
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				if (surfaces[layer] != NULL) {
					if (inUse & (1u << layer)) {
						MiniIOSurfaceUse(surfaces[layer], -1);
					}
					MiniIOSurfaceDrop(surfaces[layer]);
					surfaces[layer] = NULL;
				}
			}
			inUse = 0;
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/*
 * Does the bottom layer of a swap cover the whole display (or is it taken away, the screen black)? Then a scan-out
 * buffer need not hold the frame before it to be drawn into; otherwise the shown one is copied into it first.
 */
static bool
coversDisplay(const uint8_t * swap, MiniIOSurface ** surfaces, uint32_t width, uint32_t height)
{
	MiniIOSurface * s = surfaces[0];
	int32_t dst[4];

	if (swapWord(swap, SWAP_SURFACE(0)) == 0) {
		return (swapWord(swap, SWAP_LAYERS_SET) & 1) != 0;
	}
	if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) {
		return false;
	}
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
	if (dst[2] <= 0 || dst[3] <= 0) {
		return true;
	}
	return dst[0] <= 0 && dst[1] <= 0 && (int64_t) dst[0] + dst[2] >= (int64_t) width &&
	       (int64_t) dst[1] + dst[3] >= (int64_t) height;
}

/* fb_flip / fb_dpp / fps_log: a line in the loader's log (which the phone's runs are read from) and the kernel's */
static void scanSay(const char * format, ...) __printflike(1, 2);

static void
scanSay(const char * format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	IOLog("%s", line);
}

/*
 * fb_flip / fb_dpp (see the boot-args): the S9's DPU asked whether it is as expected, and three buffers made for it,
 * physically contiguous and below 4 GB (its DMA takes 32-bit addresses), mapped write-combined as the boot framebuffer
 * is: the processor only writes them, the display controller reads them from memory. Taken at the first frame.
 */
void
IOMobileFramebuffer::scanSetup(void)
{
	int want = gDPP ? 2 : gFlip ? 1 : 0;
	const char * name = gDPP ? "fb_dpp" : "fb_flip";
	uint64_t phys = vinfo.v_physaddr != 0 ? vinfo.v_physaddr : ml_vtophys((vm_offset_t) vinfo.v_baseaddr);
	uint64_t carved[SCAN_BUFFERS];
	uint32_t width, height, rowbytes;
	uint64_t size;
	unsigned i, first, own;
	bool carve;

	if (want == 0 && !gFpsLog) {
		return;
	}
	scanSay("MiniIOMobileFramebuffer: scan-out: fb_dpp=%u fb_flip=%u fps_log=%u; the boot framebuffer at %#llx, %ux%u, "
	    "%u bytes a row; the display %ux%u\n", gDPP, gFlip, gFpsLog, phys, fPanelWidth, fPanelHeight, fRowBytes, fWidth,
	    fHeight);
	if (hakios_scanout_probe(phys, fPanelWidth, fPanelHeight, fRowBytes) != 0) {
		scanSay("MiniIOMobileFramebuffer: scan-out: the display controller is not as expected (or not the S9's)%s\n",
		    want != 0 ? ": frames go into the boot framebuffer" : "");
		return;
	}
	if (want == 0) {
		scanSay("MiniIOMobileFramebuffer: scan-out: neither fb_flip nor fb_dpp: frames go into the boot framebuffer\n");
		return;
	}
	if (want == 2 && fWidth <= fPanelWidth && fHeight <= fPanelHeight) {
		width = fWidth;
		height = fHeight;
		rowbytes = (fWidth * 4 + 255) & ~255u;
	} else {
		if (want == 2) {
			scanSay("MiniIOMobileFramebuffer: fb_dpp: the display (%ux%u) is larger than the panel (%ux%u): flips "
			    "(fb_flip) instead\n", fWidth, fHeight, fPanelWidth, fPanelHeight);
		}
		want = 1;
		name = "fb_flip";
		width = fPanelWidth;
		height = fPanelHeight;
		rowbytes = fRowBytes;
	}
	size = round_page((uint64_t) rowbytes * height);
	/* (with flips the boot framebuffer is buffer 0, the one shown now; the others are this driver's) */
	first = want == 1 ? 1 : 0;
	own = SCAN_BUFFERS - first;
	if (first) {
		fScanBase[0] = fBootBase;
		fScanPhys[0] = phys;
	}
	/*
	 * Memory the display controller's DMA reaches (32-bit addresses): on the S9 none of this kernel's own is (its RAM is
	 * above 4 GB), so the DPU driver says where in the lower bank (hakios_scanout_memory); mapped write-combined, as the
	 * boot framebuffer is. Elsewhere IOKit's contiguous memory below 4 GB.
	 */
	carve = hakios_scanout_memory(size, own, carved) == 0;
	for (i = first; i < SCAN_BUFFERS; i++) {
		if (carve) {
			/*
			 * Not ml_io_map_wcomb: io_map takes its addresses from io_submap, 8 MB on arm64 for every ml_io_map of
			 * the kernel's, and allocates with KMA_NOFAIL: a buffer of 11 MB panicked the kernel (60fps-a2,
			 * "kmem_alloc(..., 11239424, 0x100021): failed with 3"). A physical descriptor mapped by IOKit takes
			 * its addresses from the kernel's map and says NULL when it cannot.
			 */
			fScanPhys[i] = carved[i - first];
			fScanMemory[i] = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) fScanPhys[i],
			    (IOByteCount) size, kIODirectionInOut);
			if (fScanMemory[i] == NULL) {
				scanSay("MiniIOMobileFramebuffer: %s: buffer %u at %#llx (%llu bytes): no descriptor\n", name, i,
				    fScanPhys[i], size);
				break;
			}
			fScanMap[i] = fScanMemory[i]->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
			if (fScanMap[i] == NULL) {
				scanSay("MiniIOMobileFramebuffer: %s: buffer %u at %#llx (%llu bytes) cannot be mapped\n", name, i,
				    fScanPhys[i], size);
				break;
			}
			fScanBase[i] = (uint8_t *) fScanMap[i]->getVirtualAddress();
			continue;
		}
		fScanMemory[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x00000000FFFFF000ULL);
		if (fScanMemory[i] == NULL) {
			scanSay("MiniIOMobileFramebuffer: %s: buffer %u: IOKit has no %llu bytes in one piece below 4 GB\n", name, i,
			    size);
			break;
		}
		fScanMap[i] = fScanMemory[i]->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
		if (fScanMap[i] == NULL) {
			scanSay("MiniIOMobileFramebuffer: %s: buffer %u cannot be mapped write-combined\n", name, i);
			break;
		}
		fScanPhys[i] = fScanMemory[i]->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
		/* nothing of the buffer's cached mapping may be written back over what goes through the other one later */
		flush_dcache64((addr64_t) fScanPhys[i], (unsigned) size, TRUE);
		fScanBase[i] = (uint8_t *) fScanMap[i]->getVirtualAddress();
		if (fScanPhys[i] == 0 || fScanPhys[i] + size > (1ULL << 32)) {
			scanSay("MiniIOMobileFramebuffer: %s: buffer %u is at %#llx: not below 4 GB\n", name, i, fScanPhys[i]);
			break;
		}
	}
	if (i < SCAN_BUFFERS) {
		scanSay("MiniIOMobileFramebuffer: %s: no memory for buffer %u (%llu bytes, contiguous, below 4 GB): frames go "
		    "into the boot framebuffer\n", name, i, size);
		for (i = 0; i < SCAN_BUFFERS; i++) {
			OSSafeReleaseNULL(fScanMap[i]);
			OSSafeReleaseNULL(fScanMemory[i]);
			fScanBase[i] = NULL;
		}
		return;
	}
	fScanCount = SCAN_BUFFERS;
	fScanMode = -want;
	fScanFront = want == 1 ? 0 : NO_BUFFER;
	fDiff = false;          /* (fb_diff reads what it draws over: in these buffers, a frame two before) */
	fScanWidth = width;
	fScanHeight = height;
	fScanRowBytes = rowbytes;
	scanSay("MiniIOMobileFramebuffer: %s: %u buffers of %ux%u, %u bytes a row, at %#llx %#llx %#llx (%s); taken at the "
	    "first frame drawn\n", name, SCAN_BUFFERS, width, height, rowbytes, fScanPhys[0], fScanPhys[1], fScanPhys[2],
	    carve ? "the DPU driver's, below 4 GB" : "IOKit's");
	(void) PE_parse_boot_argn("fb_zc_hold", &gZcHold, sizeof(gZcHold));
	(void) PE_parse_boot_argn("fb_rt", &gRenderRT, sizeof(gRenderRT));
	(void) PE_parse_boot_argn("fb_rt_us", &gRenderRTMicros, sizeof(gRenderRTMicros));
	if (gZeroCopy && want == 2 && carve) {
		gSwapWait = 1;                  /* (the GPU must not draw into the surface the DECON reads) */
		MiniIOSurfaceScanout(fWidth, fHeight);
		scanSay("MiniIOMobileFramebuffer: fb_zc: the window server's display surfaces (%ux%u 'BGRA') made of the scan-out "
		    "pool from now on, and scanned out as they are\n", fWidth, fHeight);
	} else if (gZeroCopy) {
		scanSay("MiniIOMobileFramebuffer: fb_zc: needs fb_dpp=1 and the DPU driver's memory: frames are copied\n");
	}
	(void) PE_parse_boot_argn("fb_swapwait", &gSwapWait, sizeof(gSwapWait));
}

/*
 * A frame is about to be drawn: into the buffer that is neither shown nor waiting to be (there is always one of three).
 * If the frame does not cover the display, what is shown is copied into it first. True if fBase is that buffer.
 */
bool
IOMobileFramebuffer::scanBegin(bool covered)
{
	unsigned t;

	if (fScanMode == 0) {
		return false;
	}
	for (t = 0; t < fScanCount && (t == fScanFront || t == fScanPending); t++) {
	}
	fScanTarget = t;
	if (!fScanSaid) {
		fScanSaid = true;
		scanSay("MiniIOMobileFramebuffer: scan-out: the first frame drawn into buffer %u (%#llx, %ux%u)%s; mode %d\n", t,
		    fScanPhys[t], fScanWidth, fScanHeight, covered ? "" : ", what is shown copied in first", fScanMode);
	}
	fBase = fScanBase[t];
	/* (fb_dpp: what is drawn is the display's own size, one pixel for one: the DPU enlarges it) */
	fPanelWidth = fScanWidth;
	fPanelHeight = fScanHeight;
	fRowBytes = fScanRowBytes;
	if (!covered) {
		/* (fb_zc: what is shown may be a surface: the newest flipped to) */
		MiniIOSurface * shown = fZcPending != NULL ? fZcPending : fScanPending == NO_BUFFER ? fZcFront : NULL;

		fFpsPartial++;
		for (uint32_t y = 0; y < fPanelHeight; y++) {
			uint8_t * to = fBase + (size_t) y * fRowBytes;

			if (shown != NULL) {
				memcpy(to, shown->pixels() + (size_t) y * shown->info.bytesPerRow, (size_t) fPanelWidth * 4);
			} else if (fScanFront < fScanCount) {
				memcpy(to, fScanBase[fScanFront] + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
			} else {
				bzero(to, (size_t) fPanelWidth * 4);
			}
		}
	}
	return true;
}

/*
 * The frame is in fScanTarget: once the flip before it has been taken (at a refresh: the DECON clears its shadow
 * request), flip to it. The first time, hand the buffers over (and, for fb_dpp, move the window to the scaler). False
 * if the scan-out was given up meanwhile: the frame has to be drawn into the boot framebuffer.
 */
bool
IOMobileFramebuffer::scanEnd(void)
{
	uint64_t began = mach_absolute_time(), waited;

	__asm__ volatile ("dsb sy" ::: "memory");      /* (the write-combined writes out before the controller is told) */
	if (fScanMode < 0) {
		int want = -fScanMode;

		if (hakios_scanout_attach(want, fScanPhys, fScanCount, fPanelWidth, fPanelHeight, fRowBytes, fScanTarget) != 0) {
			scanDrop("the display controller did not take the buffers");
			return false;
		}
		scanSay("MiniIOMobileFramebuffer: %s: taken: %s from buffer %u on\n", want == 2 ? "fb_dpp" : "fb_flip",
		    want == 2 ? "VGF0 scales the display's frames to the panel" : "frames flipped at the DECON's refresh",
		    fScanTarget);
		fScanMode = want;
		if (want == 2) {
			fScanFront = fScanTarget;               /* (the window was moved and waited for) */
			fFpsFlips++;
		} else {
			fScanPending = fScanTarget;
		}
		return true;
	}
	if (!scanLatch()) {
		return false;
	}
	hakios_scanout_flip(fScanTarget);
	fScanPending = fScanTarget;
	waited = mach_absolute_time() - began;
	fFpsWait += waited;
	fFpsWaitMax = waited > fFpsWaitMax ? waited : fFpsWaitMax;
	return true;
}

/*
 * The flip asked for last -- into a buffer of the scan-out's, or (fb_zc) to a surface -- once the DECON has taken it (at
 * a refresh): what it showed becomes the front, and a surface shown before is let go. False, and the scan-out given up,
 * if it is not taken within FLIP_STUCK_MS.
 */
bool
IOMobileFramebuffer::scanLatch(void)
{
	if (fScanPending == NO_BUFFER && fZcPending == NULL) {
		return true;
	}
	for (unsigned ms = 0; !hakios_scanout_latched(); ms++) {
		if (ms >= FLIP_STUCK_MS) {
			scanDrop("a flip not taken in 100 ms");
			return false;
		}
		IOSleep(1);
	}
	scanTaken();
	return true;
}

/* The flip asked for has been taken: what it showed becomes the front, and a surface shown before is let go. */
void
IOMobileFramebuffer::scanTaken(void)
{
	zcLetGo(&fZcFront);
	markShown(fFlipID);
	if (fScanPending != NO_BUFFER) {
		fScanFront = fScanPending;
		fScanPending = NO_BUFFER;
	} else {
		fZcFront = fZcPending;
		fZcPending = NULL;
		fScanFront = NO_BUFFER;         /* (no buffer of the scan-out's is shown: all three can be drawn into) */
	}
	fFpsFlips++;
}

/*
 * fb_zc_hold 2: the flip asked for, if the DECON has taken it by now (no wait): the surface it replaced is let go of at
 * once, so that CoreAnimation may draw into it again, and a frame from it is not taken for one drawn into what is shown.
 * True if no flip is waiting any more.
 */
bool
IOMobileFramebuffer::scanPoll(void)
{
	if (!flipAsked()) {
		return true;
	}
	if (fScanMode <= 0 || !hakios_scanout_latched()) {
		return false;
	}
	scanTaken();
	return true;
}

/* fb_zc: a surface the panel no longer shows: its use count down, let go of */
void
IOMobileFramebuffer::zcLetGo(MiniIOSurface ** which)
{
	if (*which != NULL) {
		MiniIOSurfaceUse(*which, -1);
		MiniIOSurfaceDrop(*which);
		*which = NULL;
	}
}

/*
 * fb_zc: a frame shown with nothing copied, if it is one that can be: its only layer, the whole of a surface of the
 * scan-out pool, over the whole display, and not the surface on the panel now or about to be (CoreAnimation drawing
 * again into what is shown: that frame is copied, so that what reaches the panel is whole). False: copy it.
 */
bool
IOMobileFramebuffer::zcShow(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	MiniIOSurface * s = surfaces[0];
	int32_t src[4], dst[4];
	uint64_t began, waited;

	if (!gZeroCopy || fScanMode != 2 || s == NULL || s->scanPhys == 0 || s->scanMFC) {
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		if (i == fFpsZcSurfaceCount) {
			fFpsZcSurfaces[fFpsZcSurfaceCount++] = s->sid;
			break;
		}
		if (fFpsZcSurfaces[i] == s->sid) {
			break;
		}
	}
	for (unsigned layer = 1; layer < SWAP_LAYERS; layer++) {
		if (swapWord(swap, SWAP_SURFACE(layer)) != 0) {
			fFpsZcCopied++;
			return false;
		}
	}
	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
	if (s->info.width != fWidth || s->info.height != fHeight || s->info.bytesPerElement != 4 || s->info.planeCount != 0 ||
	    !(src[2] <= 0 || src[3] <= 0 || (src[0] == 0 && src[1] == 0 && (uint32_t) src[2] == fWidth &&
	    (uint32_t) src[3] == fHeight)) || !(dst[2] <= 0 || dst[3] <= 0 || (dst[0] == 0 && dst[1] == 0 &&
	    (uint32_t) dst[2] == fWidth && (uint32_t) dst[3] == fHeight))) {
		fFpsZcCopied++;
		return false;
	}
	if (gZcHold & 2) {
		(void) scanPoll();
	}
	if (s == fZcPending || (s == fZcFront && !flipAsked())) {
		fFpsZcBusy++;
		if (gZcHold & 2) {
			/*
			 * Drawn into the surface the panel reads now, or will from the next refresh on: it is shown already, as it
			 * is. A copy would show the same picture a refresh later, and cost the presenter 5 to 9 ms.
			 */
			fFpsZcAgain++;
			fFpsZc++;
			return true;
		}
		fFpsZcCopied++;
		return false;
	}
	if (s == fZcFront && !(gZcHold & 2)) {
		fFpsZcBusy++;
		fFpsZcCopied++;
		return false;
	}
	/* (fb_zc_hold 2: the surface on the panel, a copy of an older frame flipped to over it: shown again once that is taken) */
	began = mach_absolute_time();
	if (!scanLatch()) {
		return false;
	}
	s = MiniIOSurfaceCopy(s->sid);          /* (held for as long as it is on the panel) */
	if (s == NULL) {
		return false;
	}
	MiniIOSurfaceUse(s, 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (hakios_scanout_show(s->scanPhys + s->info.offset, s->info.bytesPerRow) != 0) {
		MiniIOSurfaceUse(s, -1);
		MiniIOSurfaceDrop(s);
		fFpsZcCopied++;
		return false;
	}
	fZcPending = s;
	fFpsZc++;
	if (!fZcSaid) {
		fZcSaid = true;
		scanSay("MiniIOMobileFramebuffer: fb_zc: the first frame scanned out as it is: surface %u at %#llx, %u bytes a row\n",
		    s->sid, s->scanPhys + s->info.offset, s->info.bytesPerRow);
	}
	waited = mach_absolute_time() - began;
	fFpsWait += waited;
	fFpsWaitMax = waited > fFpsWaitMax ? waited : fFpsWaitMax;
	return true;
}

/* Back to drawing into the boot framebuffer, which the display controller shows again. (The buffers are kept.) */
void
IOMobileFramebuffer::scanDrop(const char * why)
{
	char line[200];

	snprintf(line, sizeof(line), "MiniIOMobileFramebuffer: the scan-out given up (%s): frames go into the boot framebuffer\n",
	    why);
	loader_log_line(line);
	IOLog("%s", line);
	if (fScanMode > 0) {
		hakios_scanout_detach(why);
	}
	fScanMode = 0;
	fBase = fBootBase;
	fPanelWidth = fBootWidth;
	fPanelHeight = fBootHeight;
	fRowBytes = fBootRowBytes;
	fScanFront = fScanPending = fScanTarget = NO_BUFFER;
	zcLetGo(&fZcFront);                     /* (the boot framebuffer is shown now, no surface) */
	zcLetGo(&fZcPending);
}

/*
 * The buffer the display controller scans out now, for the USB console's shot (drivers/usb/hakios.dwc3.c): with fb_dpp
 * one of the three display-sized buffers VGF0 scales (the flip asked for, once the DECON has taken it, else the one
 * before), with fb_flip one of the panel-sized ones. 2 (fb_dpp) or 1 (fb_flip) and where it is; 0 when frames go into
 * the boot framebuffer, which is then what is shown. Read without the lock: the presenter may move on meanwhile, and
 * a shot may then catch a newer frame, or half of one (the buffer is drawn into again two frames later).
 */
int
IOMobileFramebuffer::scanned(uintptr_t * base, uint32_t * width, uint32_t * height, uint32_t * rowbytes, unsigned * index)
{
	int mode = fScanMode;
	unsigned front = fScanFront, pending = fScanPending, shown;

	if (mode <= 0) {
		return 0;
	}
	/* (fb_zc: a surface on the panel; the one asked for once taken. Read unlocked: see above) */
	MiniIOSurface * surface = fZcPending != NULL && hakios_scanout_latched() ? fZcPending : fZcFront;

	if (surface != NULL && pending >= fScanCount) {
		*base = (uintptr_t) surface->pixels();
		*width = surface->info.width;
		*height = surface->info.height;
		*rowbytes = surface->info.bytesPerRow;
		*index = 0;
		return mode;
	}
	shown = pending < fScanCount && hakios_scanout_latched() ? pending : front;
	if (shown >= fScanCount || fScanBase[shown] == NULL) {
		return 0;
	}
	*base = (uintptr_t) fScanBase[shown];
	*width = fScanWidth;
	*height = fScanHeight;
	*rowbytes = fScanRowBytes;
	*index = shown;
	return mode;
}

extern "C" int hakios_fb_scanned(uintptr_t * base, uint32_t * width, uint32_t * height, uint32_t * rowbytes, unsigned * index);

int
hakios_fb_scanned(uintptr_t * base, uint32_t * width, uint32_t * height, uint32_t * rowbytes, unsigned * index)
{
	return gScanOwner != NULL ? gScanOwner->scanned(base, width, height, rowbytes, index) : 0;
}

void
IOMobileFramebuffer::fpsFired(thread_call_param_t self, thread_call_param_t unused __unused)
{
	((IOMobileFramebuffer *) self)->fpsLine();
}

/* fps_log=1: the last second, in one line (and one "idle" line when frames stop) */
void
IOMobileFramebuffer::fpsLine(void)
{
	uint32_t presents, lost, held, shown, flips, partial, same, undrawn, refreshes = hakios_scanout_refreshes(), refreshed;
	uint32_t zc, zcCopied, zcBusy, missed, jobs = 0, mhz = 0, switches = 0;
	uint32_t zcAgain, zcSurfaces, cpuFrames, cpuMask, waits, waitLast, waitBefore, waitOther, waitsSlept, waitsTimedOut;
	uint64_t waitSlept;
	uint64_t cpu, cpuMax;
	uint64_t draw, drawMax, wait, waitMax, deadline, gapMax, busy = 0, queued = 0;
	int32_t g3d = 0;
	int gpu;
	const char * mode;
	char line[400];

	IOLockLock(fSwapLock);
	presents = fFpsPresents;
	lost = fFpsLost;
	held = fFpsHeld;
	fFpsPresents = fFpsLost = fFpsHeld = 0;
	cpu = fFpsCpu;
	cpuMax = fFpsCpuMax;
	cpuFrames = fFpsCpuFrames;
	cpuMask = fFpsCpuMask;
	fFpsCpu = fFpsCpuMax = 0;
	fFpsCpuFrames = fFpsCpuMask = 0;
	waits = fFpsWaits;
	waitLast = fFpsWaitLast;
	waitBefore = fFpsWaitBefore;
	waitOther = fFpsWaitOther;
	waitsSlept = fFpsWaitsSlept;
	waitsTimedOut = fFpsWaitsTimedOut;
	waitSlept = fFpsWaitSlept;
	fFpsWaits = fFpsWaitLast = fFpsWaitBefore = fFpsWaitOther = fFpsWaitsSlept = fFpsWaitsTimedOut = 0;
	fFpsWaitSlept = 0;
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	shown = fFpsShown;
	flips = fFpsFlips;
	partial = fFpsPartial;
	draw = fFpsDraw;
	drawMax = fFpsDrawMax;
	wait = fFpsWait;
	waitMax = fFpsWaitMax;
	same = fFpsSameSurface;
	undrawn = fFpsUndrawn;
	zc = fFpsZc;
	zcCopied = fFpsZcCopied;
	zcBusy = fFpsZcBusy;
	zcAgain = fFpsZcAgain;
	zcSurfaces = fFpsZcSurfaceCount;
	fFpsZcAgain = fFpsZcSurfaceCount = 0;
	missed = fFpsMissed;
	gapMax = fFpsGapMax;
	fFpsZc = fFpsZcCopied = fFpsZcBusy = fFpsMissed = 0;
	fFpsGapMax = 0;
	fFpsShown = fFpsFlips = fFpsPartial = fFpsSameSurface = fFpsUndrawn = 0;
	fFpsDraw = fFpsDrawMax = fFpsWait = fFpsWaitMax = 0;
	mode = fScanMode == 2 ? "dpp: VGF0 scales" : fScanMode == 1 ? "flip" : fScanMode < 0 ? "flip/dpp not yet taken" :
	    "boot framebuffer";
	IOLockUnlock(fLock);
	refreshed = refreshes - fFpsRefreshes;
	fFpsRefreshes = refreshes;
	SInt32 vsyncs = gVSyncsTold, told = vsyncs - fFpsVSyncs;
	fFpsVSyncs = vsyncs;
	if (presents != 0 || shown != 0) {
		fFpsIdle = 0;
		snprintf(line, sizeof(line), "fps: %u presented, %u drawn, %u flips, %u lost, %u held; drawing %llu us a frame, "
		    "%llu at most; waiting for the flip before %llu us, %llu at most; %u refreshes; %u partial; %d refresh "
		    "notifications sent; %u from the surface before, %u not done by the GPU; %s\n", presents, shown, flips, lost,
		    held, shown ? micros(draw) / shown : 0, micros(drawMax), shown ? micros(wait) / shown : 0, micros(waitMax),
		    refreshed, partial, (int) told, same, undrawn, mode);
		loader_log_line(line);
		IOLog("%s", line);
		/*
		 * the second line: refreshes missed while frames came (and the longest gap between two), fb_zc's frames, and
		 * the GPU's second (drivers/gpu/mali/hakios.mali.c): its share of the time running jobs, its time a frame shown, its
		 * jobs and their wait in the queue, its clock and the G3D's temperature
		 */
		gpu = hakios_mali_second(&busy, &jobs, &queued, &mhz, &switches, &g3d);
		snprintf(line, sizeof(line), "fps: %u refreshes missed while moving, the longest gap %llu.%llu ms; zc: %u in "
		    "place, %u copied, %u into the one shown; gpu: %s%llu%% busy, %llu us a frame shown, %u jobs (%llu us each, "
		    "%llu us queued), %u MHz%s, %u switches, G3D %d C\n", missed, gapMax / NSEC_PER_MSEC,
		    gapMax / (NSEC_PER_MSEC / 10) % 10, zc, zcCopied, zcBusy, gpu ? "" : "none; ", busy / (NSEC_PER_SEC / 100),
		    shown ? busy / 1000 / shown : 0, jobs, jobs ? busy / 1000 / jobs : 0, jobs ? queued / 1000 / jobs : 0, mhz,
		    gpu == 2 ? " (mali_gov)" : "", switches, g3d / 1000);
		loader_log_line(line);
		IOLog("%s", line);
		/*
		 * the third: the render thread's processor time a frame against the GPU's (the line before): which of the two a
		 * frame waits for; and where fb_zc's frames came from -- how many surfaces in turn, how many drawn into the one
		 * shown were shown as they were (fb_zc_hold 2) -- and on which processors the swaps were handed over (a bit each)
		 */
		snprintf(line, sizeof(line), "fps: cpu: the render thread %llu us a frame, %llu at most, %u frames, on cpus %#x%s; "
		    "gpu %llu us a frame; zc: %u surfaces in turn, %u drawn into the one shown not copied; fb_zc_hold %u; swapwait: "
		    "%u (%u for the last swap, %u the one before, %u other), %u waited %llu us each, %u for 2 refreshes%s\n",
		    cpuFrames ? micros(cpu) / cpuFrames : 0, micros(cpuMax), cpuFrames, cpuMask, gRenderRT ? " (fb_rt)" : "",
		    shown ? busy / 1000 / shown : 0, zcSurfaces, zcAgain, gZcHold, waits, waitLast, waitBefore, waitOther, waitsSlept,
		    waitsSlept ? micros(waitSlept) / waitsSlept : 0, waitsTimedOut, gSwapWait ? " (fb_swapwait)" : "");
		loader_log_line(line);
		IOLog("%s", line);
	} else if (fFpsIdle++ == 0) {
		snprintf(line, sizeof(line), "fps: idle (%u refreshes in the last second; %s)\n", refreshed, mode);
		loader_log_line(line);
		IOLog("%s", line);
	}
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(fFpsCall, deadline);
}

/*
 * The frame on the panel once more, filtered (fb_filter=2): the same layers from the same surfaces where show() put
 * them, nothing else (no turn to wait for, no swap to hand on, nothing counted). Its surfaces were held since.
 */
void
IOMobileFramebuffer::settle(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	bool scanned;

	if (!fGraphics || fScanMode == 2 || fScanMode == -2) {
		return;
	}
	scanned = scanBegin(coversDisplay(swap, surfaces, fWidth, fHeight));
	fSettling = true;
	drawLayers(swap, surfaces);
	fSettling = false;
	fStatSettled++;
	if (scanned) {
		(void) scanEnd();
	}
}

/* The 32-bit layers of a swap drawn where fBase points, nothing else (settle(), and a frame drawn again by show()). */
void
IOMobileFramebuffer::drawLayers(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	int32_t src[4], dst[4];

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		MiniIOSurface * s = surfaces[layer];

		if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) {
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		drawLayer(s, src, dst);
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	bool scanned;                           /* (fb_flip/fb_dpp: drawn into a buffer of the scan-out's) */
	MiniIOSurface * recorded = NULL;
	uint32_t undrawn = 0;                   /* (fps_log: layers shown before the GPU was done with them) */
	bool inPlace;                           /* (fb_zc: the surface scanned out as it is: drawLayer() draws nothing) */
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		/* (a surface the host shows itself is never waited for, nor drawn here: see below) */
		if (hakios_vgpu3d_host_owns(swapWord(swap, SWAP_SURFACE(layer)))) {
			continue;
		}
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
			undrawn++;
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	fBeamScan = fBeamIntf != 0 ? beamNextScan(began) : 0;
	if (fBeamIntf != 0) {
		/* (on the panel when that scan reads it: that is the beat now, kept by the panel) */
		fStatOnBeat++;
	} else if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	/*
	 * fps_log: refreshes missed while the picture moved -- frames coming, less than FRAME_PAUSE apart, but more than a
	 * refresh apart: each refresh in between showed the frame before again -- and the longest such gap
	 */
	if (fFpsLastShow != 0) {
		uint64_t gap, period = NSEC_PER_SEC / refreshHz();

		absolutetime_to_nanoseconds(began - fFpsLastShow, &gap);
		if (gap < (uint64_t) FRAME_PAUSE * NSEC_PER_MSEC) {
			uint64_t periods = (gap + period / 2) / period;

			fFpsMissed += periods > 1 ? (uint32_t) (periods - 1) : 0;
			fFpsGapMax = gap > fFpsGapMax ? gap : fFpsGapMax;
		}
	}
	fFpsLastShow = began;
	report = fSwapsShown < 8;
	/*
	 * fps_log: a frame from the very surface the frame before came from (CoreAnimation drawing again into what is being
	 * shown: a picture half of one frame and half of the next, or one not drawn yet), and frames shown before the GPU
	 * was done with them -- the ways a stale or partly drawn picture could reach the panel from here
	 */
	fFpsUndrawn += undrawn;
	if (swapWord(swap, SWAP_SURFACE(0)) != 0) {
		fFpsSameSurface += swapWord(swap, SWAP_SURFACE(0)) == fLastSurface;
		fLastSurface = swapWord(swap, SWAP_SURFACE(0));
		gShownSurface = fLastSurface;
	}
	/* (fb_flip/fb_dpp: into a buffer of its own, from the second picture on: the first is the console's end) */
	inPlace = fGraphics && zcShow(swap, surfaces);
	fZcFrame = inPlace;
	scanned = !inPlace && fGraphics && scanBegin(coversDisplay(swap, surfaces, fWidth, fHeight));
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	scfbBeginDraw();
	fDamageX0 = fDamageY0 = ~0u;
	fDamageX1 = fDamageY1 = 0;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
				if (fShadow != NULL) {
					bzero(fShadow, (size_t) fPanelWidth * fPanelHeight * 4);
				}
				damage(0, 0, fPanelWidth - 1, fPanelHeight - 1);
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			/*
			 * SCGPU: the frame in this surface went from the GPU straight to the app's window
			 * (rcFBPost, bsd/dev/hakios.vgpu3d_xnu.c); its memory here is stale, and copying it
			 * to the panel would only cost the milliseconds the next swap waits for.
			 */
			if (hakios_vgpu3d_host_owns(sid)) {
				continue;
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
				gShownSurface = sid;
			}
		}
	}
	drawn = mach_absolute_time();
	if (fShadow != NULL) {
		/* exactly what changed; an empty rectangle when nothing did */
		bool any = fDamageX0 <= fDamageX1;

		if (fShadowStale) {
			/* the first picture after the text console: all of it, as before */
			scfbPublish(0, 0, fPanelWidth, fPanelHeight, false);
		} else {
			scfbPublish(any ? fDamageX0 : 0, any ? fDamageY0 : 0, any ? fDamageX1 - fDamageX0 + 1 : 0,
			    any ? fDamageY1 - fDamageY0 + 1 : 0, true);
		}
		fShadowStale = false;
	} else {
		scfbPublish(0, 0, fPanelWidth, fPanelHeight, false);
	}
	if (gVGPU) {
		hakios_vgpu_flush(0, 0, fPanelWidth, fPanelHeight);
	}
	if (recorded != NULL) {
		record(recorded);
	}
	fZcFrame = false;
	fFpsShown++;
	fFpsDraw += drawn - began;
	fFpsDrawMax = drawn - began > fFpsDrawMax ? drawn - began : fFpsDrawMax;
	if (scanned && !scanEnd()) {
		/* the scan-out given up: this frame again, into the boot framebuffer */
		drawLayers(swap, surfaces);
	}
	fSwapsShown++;
	/* (SwapWait: on the panel now, or once the flip asked for is taken) */
	if (flipAsked()) {
		fFlipID = swapWord(swap, SWAP_ID);
	} else {
		markShown(swapWord(swap, SWAP_ID));
	}
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		char late[48] = "";

		/* (the line as it was on the Galaxy S9, with no beam: what reads these logs does not change) */
		if (fBeamIntf != 0) {
			snprintf(late, sizeof(late), "; %u of %u too late for the next scan", fStatScansMissed, fStatScans);
		}
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves; %u frames drawn again filtered%s\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate, fStatSettled, late);
		fStatScans = fStatScansMissed = 0;
		if (fBeamIntf != 0) {
			char shares[8 * 24] = "";
			size_t at = 0;

			for (unsigned share = 0; share < fThreads && share < 8; share++) {
				uint64_t took;

				absolutetime_to_nanoseconds(fStatShareTime[share], &took);
				at += (size_t) snprintf(shares + at, sizeof(shares) - at, " %llu/%u/%u", fStatShares[share] != 0 ? took / fStatShares[share] / NSEC_PER_USEC : 0,
				    fStatShareTorn[share], fStatShareClose[share]);
				fStatShareTime[share] = 0;
				fStatShares[share] = fStatShareTorn[share] = fStatShareClose[share] = 0;
			}
			IOLog("MiniIOMobileFramebuffer: shares of the rows, us each/torn/close:%s; %u not drawn again, their surface taken for the next frame\n", shares, fStatSettleReused);
			fStatSettleReused = 0;
		}
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = fStatSettled = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		armVSync();
		/*
		 * sent with the lock held: registerNotificationPort() swaps the port under this lock and
		 * only then lets go of the old one, so the port in `ref` cannot be released while it is sent to
		 * (the send does not wait: a full queue drops the message)
		 */
		IOReturn sent = sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
		if (sent == kIOReturnSuccess) {
			OSIncrementAtomic(&gVSyncsTold);
		}
		if (gTouchLat) {
			/* "tl: y COUNT due US at US SENT": the refresh told to the window server, when it was due, when it went, and
			 * whether it went (0) or was dropped on a full queue -- for the window server's idle gaps (tools/frames.py) */
			hakios_tl("tl: y %llu due %llu at %llu %#x\n", fVSyncCount, micros(now), micros(mach_absolute_time()), sent);
		}
	}
	IOLockUnlock(fLock);
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / (fOwner->fPointsWide * kFBDefaultScale);
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* in: the swap; at once, or (fb_swapwait=1) once it is on the panel */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		fOwner->swapWait(args->scalarInputCount > 0 ? (uint32_t) in[0] : 0);
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;
	case kFBMethodRequestPowerChange:
		/*
		 * Scion (patch 1517): backboardd's display power, one scalar (seen at boot: 1, 0, 1). Handed to the platform
		 * expert (GenericARMPE.cpp, hakios_display_power()), which logs each change and, with panel_sleep= or
		 * sleep_cores=, acts on it; without them nothing more happens than before.
		 */
		if (args->scalarInputCount > 0) {
			hakios_display_power(in[0] != 0);
		}
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.darwin, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		uint32_t metal = 0;

		accelerator->setName("sgx");
		/*
		 * Boot-arg metal=1: say where Metal's device is (docs/metal-plan.md, step 1). Metal reads these two
		 * properties, opens /System/Library/Extensions/<name>.bundle and takes the class (userland/src/HakiosMetal.m).
		 * Never by default: with them a program that asks for Metal gets a device, and CoreAnimation would choose
		 * Metal for the screen; until that device can draw, a screen drawn without it is the working one.
		 */
		if (PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0) {
			accelerator->setProperty("MetalPluginName", "HakiosMetal");
			accelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");
			IOLog("MiniIOMobileFramebuffer: metal=1: the accelerator names HakiosMetal as its Metal plug-in\n");
		}
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
