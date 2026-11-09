/*
 * A USB device ("gadget") link for phones with a DesignWare DWC3: the kernel's log streamed to a host over a CDC-ACM
 * serial port (Linux /dev/ttyACM0, macOS /dev/cu.usbmodem*, Windows COMn), and a small line console back. For a run that
 * has no serial port and gave back only what fits in pstore (32 KB on the Galaxy S9, read in TWRP), this is the whole
 * log, as it is written, for as long as the run lasts, and the panic text at the end.
 *
 * Device-neutral: the DWC3 in device mode, its event buffer, endpoint 0 and the bulk endpoints are here; the CDC-ACM
 * descriptors and requests are plain C in hakios.usb_acm.h (checked on the build machine: tools/usb-acm-test.sh); what
 * the SoC has to do first (clocks, power, the PHY) is a glue's (hakios.usb_glue.h), today only the Exynos 9810's
 * (hakios.exynos_usb.c, the Galaxy S9). Polled from a thread call, no interrupt; one control transfer at a time, one
 * TRB per transfer (LST set), each in its own cache line of memory mapped uncached.
 *
 * Nothing else in the tree provides USB device mode. The POCO loader's fastboot uses Qualcomm's UEFI USB protocol
 * (xnuloader/loader.c, usb_start()); the S9's Silicium UEFI has no USB driver in its firmware volume, and nothing of
 * that survives the hand-over into the kernel. This code is not a port of it.
 *
 * The steps (usb_gadget=N does the first N; each says what it is about to do before it does it):
 *   1  look: the glue reads the SoC's side (PMU isolation, clocks, the PHY's registers, the supplies through ACPM) and,
 *      when the controller has a clock, the DWC3's: its ID (GSNPSID), GCTL, DCTL (RUN_STOP: did S-Boot leave it running?),
 *      DSTS, the event buffer it was given. Reads only.
 *   2  PHY: a controller left running is stopped first (DCTL RUN_STOP clear, DEVCTRLHLT awaited, its events masked),
 *      then the glue's PHY sequence (Exynos 9810: hakios.exynos_usb.c's head, every write read back).
 *   3  core: a device soft reset (DCTL CSFTRST, as dwc3_soft_reset()), whatever the boot left (see below); then the
 *      USB 2.0 PHY interface, GCTL in device mode, bus bursts, the event buffer in this kernel's memory, DCFG high speed,
 *      the device events, endpoint 0 (DEPSTARTCFG, a transfer resource each, DEPCFG) and the first SETUP TRB. The pull-up
 *      stays off: the host sees nothing.
 *   4  enumerate: DCTL RUN_STOP: the host sees a device and asks; endpoint 0 answers (USB 2.0 chapter 9 and CDC's
 *      requests), each request said in the log (the first 64, then every hundredth); SET_CONFIGURATION sets up the bulk
 *      endpoints, nothing is sent on them. The PHY's line state is said every 10 s until a host is seen.
 *   5  ACM: the kernel's log from its first line (the part the kernel keeps of it, then everything new) goes out on
 *      bulk IN, and lines typed on the host come in on bulk OUT to the line console below.
 *
 * Was the controller touched before? Not by UEFI: Silicium's starlte FD has no USB driver (hakios.exynos_usb.c says
 * where that is seen). S-Boot has a download mode and charger detection and may leave it in any state, so step 3
 * always resets it, and step 2 stops it first if it runs; step 1 says what was found. The reset is the one Linux does
 * on every start (dwc3_core_init: dwc3_soft_reset then dwc3_core_soft_reset, both DCTL CSFTRST).
 *
 * The line console (step 5). Lines typed on the host, ended by Enter; the device echoes the line ("usb> ...") and
 * answers. The answers and the echo go into the kernel's log (and so to the host), not to the screen.
 *   help              these
 *   stat              the link's counters
 *   uptime            seconds since boot
 *   sysctl NAME       a sysctl's value (an integer or a string)
 *   sysctl NAME=N     an integer sysctl set to N, then read back. From here only a sysctl marked CTLFLAG_KERN can be
 *                     read or set: XNU's sysctl_root() refuses the others to the kernel with EPERM (whatever user
 *                     space may do), and the answer says so, with the error, instead of "no such name"
 *   bl                the panel's brightness: kern.hakios.brightness (0-255; -1 never set)
 *   bl N              the panel's brightness set to N (0-255) through hakios_panel_brightness(), as iOS's sets reach
 *                     it, and what came back: sent, the DSIM refused it (EIO), or no panel driver (ENODEV). The
 *                     Galaxy S9's driver also logs each set it sent ("s6e3ha8: brightness N sent ...", once a second)
 *   log               the kernel's log again from its start (what the kernel keeps of it: the first 256 KB)
 *   shot [N]          the panel's picture as it is now (what the display controller scans out: the boot framebuffer,
 *                     or with fb_dpp=1 / fb_flip=1 the presenter's buffer shown now, hakios_fb_scanned()), every
 *                     Nth pixel of every Nth row (1-16; 4 when not given), as binary on bulk IN, framed:
 *                       "SHOT w h stride rgb888 bytes\n", then the bytes (rows top to bottom, R G B), then "END\n"
 *                     The log is held back meanwhile (what came before the command is sent first, what comes during
 *                     it after END), so the picture is never mixed with it. tools/usb-shot.py asks for one and writes
 *                     a PNG. The panel is read while the display may be drawing it: a shot can catch half a frame
 *   camshot [N] [tp]  a picture from the Galaxy S9's front camera (hakios.exynos_camera.c, boot-arg cam_s9=6 or 7):
 *                     the camera's thread powers it up, meters and exposes, takes one RAW10 frame through CSIS1's WDMA,
 *                     powers it down (cam_s9=7: the newest frame of the stream, nothing powered up or down), demosaics
 *                     it (the whole 1632 x 1224 at N = 1, every Nth pixel for N of 2-16; 1 when not given) and
 *                     hands it here; it goes out framed as a shot's does, after the camera's log lines. "tp": CSIS1's
 *                     test pattern instead of the sensor. A capture that fails says "usb: camshot failed: ..." with
 *                     the link's and the WDMA's status registers instead. tools/usb-shot.py --cam
 *   irshot [N]        a picture from the Galaxy S9's iris camera (hakios.exynos_iris.c, boot-arg iris_s9=5): its
 *                     thread powers the S5K5F1 up, streams it (its IR LED pulsing with each frame) for one frame at the
 *                     most iris_s9_ms long, powers it down, and hands every Nth pixel (N of 2-8, 2 when not given:
 *                     1200 x 1200) here as grey RGB; it goes out as a camshot's does. tools/usb-shot.py --iris
 *   ufs               what the storage's ufs=3 did (hakios_ufs_summary in hakios.ufs.c): the step reached, the
 *                     self-test's and the fence test's results, the mode (native, or fallen back to ufs=2 and why),
 *                     reads, writes and refusals with the first three refusals' reasons. Said by itself too when
 *                     the host configures the link (ufs=3 only)
 *   wifi ...          the radio and en0: wifi, wifi scan, wifi ip, wifi ping [A.B.C.D] [SIZE], wifi dns NAME [A.B.C.D],
 *                     wifi tcp HOST [PORT [PATH]], wifi stat, wifi names [off] (drivers/wifi/hakios.wifi.c, hakios.sta.c,
 *                     drivers/net/hakios.net.c: what each does is said there)
 *   touch X Y [X2 Y2 [MS]]  a tap, or a swipe over MS milliseconds, given to iOS as the panel's are (X and Y in
 *                     thousandths of the screen's width and height): "shot" shows the result
 *   button NAME       a press of lock, home, up or down, as its driver would give it
 *   usb               the link as it stands: the controller's registers, endpoint 0's state, the restarts, VBUS, the
 *                     USB-C switch (hakios.max77705.c). usb restart: the link restarted, as after a replug. usb drop MS:
 *                     the pull-up off for MS ms and on again, nothing else (the host sees an unplug). usb unplug MS: the
 *                     switch open and VBUS taken as gone for MS ms (an unplug from this side, the port's). usb muic MS:
 *                     the switch open for MS ms and nothing told to the link. usb vbus MS: VBUS taken as gone for MS ms,
 *                     the switch left alone. usb watch off/on: the check for a link
 *                     that gets bus resets and no SETUP (see The cable)
 *   return            end the run: the data volume saved, the drivers given back, the loader restarts the phone (into
 *                     TWRP on the S9). Done on a thread call of its own; should that not have ended the run after
 *                     RETURN_FORCE_SECONDS (the save or a driver's stop waiting on something under load), the poll goes
 *                     back to the loader without them, as a panic does
 * Not a shell: user space is not reached from here.
 *
 * The cable. Nothing in the controller reports the cable going: the PHY's VBUS is forced valid (the VBUS pad is not
 * wired to it, dt is_not_vbus_pad: hakios.exynos_usb.c step i), so there is no disconnect event. On Android the MUIC
 * and charger interrupts make dwc3/otg.c stop the controller and bring it up again from dwc3_core_init on every attach.
 * Without a restart, a link after a replug takes the host's resets but never delivers a SETUP, and the host retries
 * indefinitely. So the link follows Android's two signals and also watches its own state:
 *   - VBUS: hakios.max77705.c reads it (the MUIC's BC_STATUS) four times a second and tells this file
 *     (hakios_usb_gadget_vbus). When it goes: every transfer ended, the pull-up off, the PHY down, the log kept in the
 *     ring (1 MB). When it comes back: the PHY's sequence again, the core reset and set up, the pull-up on (link_restart),
 *     and the log from where the host left it.
 *   - the link's own state: a host's reset without a SETUP after it, four in a row (or twelve resets without a
 *     configuration), means what a replug nobody saw would mean: the same restart, at most every 2 s, doubling to 30 s,
 *     until the host configures it. A USB controller reset on the host's side comes in by this door or by a plain reset.
 *     VBUS there and neither a SOF nor a reset for 5 s, configured or not: the switch closed again and the restart, five
 *     times at most until the next change or configuration, for a host that has stopped trying (macOS gives a port up after
 *     eight failures in some 10 s) and only sees a new pull-up. A wall charger has VBUS too and never talks, and a host
 *     that suspends the port stops its SOFs: both get those five and no more.
 * The restart never waits on VBUS alone: a reading that stops coming puts a stopped link back up after 3 s.
 *
 * Boot-args:
 *   usb_gadget=<1-5>      the steps (0, or none: nothing)
 *   usb_gadget_soc=<name> the glue (exynos9810, the only one; the default)
 *   usb_gadget_clk=1      the glue may open a clock the boot left shut (Android's mux, divider and gate)
 *   usb_gadget_ldo=1      the glue puts the PHY's supplies into Android's USB mode (S9: S2MPS18 LDO9-14 mode 3, read back)
 *   usb_gadget_fs=1       full speed (12 Mbit/s) instead of high speed
 *   usb_gadget_susphy=1   let the link suspend the 2.0 PHY when idle (GUSB2PHYCFG SUSPHY, as Linux does; off by default)
 *   usb_gadget_poll=<ms>  the poll period while a host is attached (1-50; 2)
 *   usb_gadget_drain=<ms> on the way back to the loader, how long to keep sending the rest of the log (0-5000; 1000)
 *   usb_gadget_crlf=0     send the log's line ends as they are (LF) rather than CR LF
 *
 * Registers and fields: Samsung's kernel fork for the Galaxy S9 (Scion-Project/android_kernel_samsung_exynos9810,
 * lineage-24.0, e5906667068e), drivers/usb/dwc3/core.h and gadget.h, at the lines given next to each below; the
 * sequences after core.c dwc3_core_init (795-1000), dwc3_soft_reset (350-375), dwc3_core_config (140-253), dwc3_phy_setup
 * (637-765), gadget.c __dwc3_gadget_start (2025-2110), dwc3_gadget_set_ep_config (564-646), dwc3_gadget_start_config
 * (533-562), dwc3_gadget_run_stop (1733-1800), dwc3_gadget_reset_interrupt (2888), dwc3_gadget_conndone_interrupt (2998),
 * ep0.c (the control transfer's phases, 39-1160). GPL-2.0: read for facts only, no code taken. The event and TRB
 * layouts are those of core.h:1095-1205 and 676-710.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/kalloc.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <vm/memory_types.h>
#include <console/video_console.h>
#include <sys/hakios.input.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

#include "hakios.usb_acm.h"
#include "hakios.usb_glue.h"

extern uint32_t loader_log_tap_set(void (*tap)(char c), void (*history)(const char *text, uint32_t len));
extern void hakios_var_save_on_return(void);
extern void hakios_ufs_summary(const char *when, int how);     /* hakios.ufs.c (or the stub): ufs=3's summary */
extern void hakios_adreno_stop(void);
extern void hakios_wifi_stop(void);
extern void hakios_wifi_report(bool first);
extern void hakios_sta_console_scan(void);
extern void hakios_bt_console(const char *arg);
extern void hakios_net_console_ip(void);            /* hakios.net.c */
extern void hakios_net_console_ping(const char *arg);
extern void hakios_net_console_dns(const char *arg);
extern void hakios_net_console_tcp(const char *arg);
extern void hakios_net_console_stat(void);
extern void hakios_net_console_names(const char *arg);
extern void hakios_input_console_touch(const char *arg);  /* hakios.input.c */
extern void hakios_input_console_button(const char *arg);
extern void hakios_abox_console(const char *arg);  /* sys/hakios.exynos_abox.h */
extern void hakios_cs47l93_console(const char *arg);       /* sys/hakios.cs47l93.h */
extern void hakios_ssp_console(const char *arg);    /* sys/hakios.sensors.h: the S9's sensor hub (shub=N) */
extern void hakios_adsp_stop(void);
extern int hakios_panel_brightness(int level);  /* hakios.backlight.cpp (always built): ENODEV with no panel driver */
extern void hakios_exynos_cpufreq_touch(void);  /* the governor's touch boost, as the touch screen's driver asks it */
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern struct vc_info vinfo;            /* the boot framebuffer: the panel's memory, as the display driver draws on it */
/* the presenter's buffer scanned out now (MiniIOMobileFramebuffer.cpp): 2 fb_dpp, 1 fb_flip, 0 the boot framebuffer */
extern int hakios_fb_scanned(uintptr_t *base, uint32_t *width, uint32_t *height, uint32_t *rowbytes, unsigned *index);
void hakios_usb_gadget_quiesce(void);
void hakios_usb_gadget_vbus(bool present);
extern void hakios_max77705_usb_console(const char *arg);       /* hakios.max77705.c (or the stub) */
extern void hakios_max77705_usb_close(void);                    /* ... the USB-C switch closed again, on its thread */

SYSCTL_DECL(_kern_hakios);

/* DWC3 registers and fields: drivers/usb/dwc3/core.h of the BSP, line numbers given */
#define GSBUSCFG0               0xc100u         /* :83 */
#define  GSBUSCFG0_INCRBRSTEN   (1u << 0)       /* :161 */
#define  GSBUSCFG0_INCR16BRSTEN (1u << 3)       /* :164 */
#define  GSBUSCFG0_REQINFO      ((2u << 16) | (2u << 20) | (2u << 24) | (2u << 28))    /* :169-172 */
#define GSBUSCFG1               0xc104u         /* :84 */
#define  GSBUSCFG1_BREQLIMIT(n) ((n) << 8)      /* :177 */
#define GCTL                    0xc110u         /* :87 */
#define  GCTL_DSBLCLKGTNG       (1u << 0)       /* :227 */
#define  GCTL_SCALEDOWN_MASK    (3u << 4)       /* :222-223 */
#define  GCTL_SOFITPSYNC        (1u << 10)      /* :207, 221 */
#define  GCTL_PRTCAPDIR_MASK    (3u << 12)      /* :215, 218 */
#define  GCTL_PRTCAP_DEVICE     (2u << 12)      /* :215, 217 */
#define GSTS                    0xc118u         /* :89 */
#define GSNPSID                 0xc120u         /* :91; 0x5533xxxx DWC_usb3, 0x3331xxxx DWC_usb31 (core.c:804-807) */
#define GUCTL                   0xc12cu         /* :94 */
#define  GUCTL_USBHSTINAUTORETRYEN (1u << 14)   /* :231 */
#define  GUCTL_REFCLKPER_MASK   (0x3ffu << 22)  /* :229-230 */
#define  GUCTL_REFCLKPER(n)     ((n) << 22)
#define GHWPARAMS0              0xc140u         /* :99 */
#define GHWPARAMS1              0xc144u         /* :100 */
#define GHWPARAMS3              0xc14cu         /* :102 */
#define GUSB2PHYCFG             0xc200u         /* :118, port 0 */
#define  GUSB2_PHYSOFTRST       (1u << 31)      /* :235 */
#define  GUSB2_U2_FREECLK_EXISTS (1u << 30)     /* :236 */
#define  GUSB2_SUSPHY           (1u << 6)       /* :237 */
#define  GUSB2_ENBLSLPM         (1u << 8)       /* :239 */
#define GUSB3PIPECTL            0xc2c0u         /* :123, port 0 */
#define GEVNTADRLO              0xc400u         /* :128, buffer 0 */
#define GEVNTADRHI              0xc404u         /* :129 */
#define GEVNTSIZ                0xc408u         /* :130 */
#define  GEVNTSIZ_INTMASK       (1u << 31)      /* :281 */
#define GEVNTCOUNT              0xc40cu         /* :131 */
#define  GEVNTCOUNT_MASK        0xfffcu         /* :68 */
#define DCFG                    0xc700u         /* :137 */
#define  DCFG_SPEED_MASK        7u              /* :345 */
#define  DCFG_HIGHSPEED         0u              /* :348 */
#define  DCFG_FULLSPEED         1u              /* :349 */
#define  DCFG_DEVADDR_MASK      (0x7fu << 3)    /* :342-343 */
#define  DCFG_DEVADDR(a)        ((uint32_t)(a) << 3)
#define  DCFG_LPM_CAP           (1u << 22)      /* :356 */
#define DCTL                    0xc704u         /* :138 */
#define  DCTL_RUN_STOP          (1u << 31)      /* :359 */
#define  DCTL_CSFTRST           (1u << 30)      /* :360 */
#define  DCTL_KEEP_CONNECT      (1u << 19)      /* :381 */
#define  DCTL_U1U2_MASK         (0xfu << 9)     /* :386-389: INITU2ENA, ACCEPTU2ENA, INITU1ENA, ACCEPTU1ENA */
#define DEVTEN                  0xc708u         /* :139 */
#define  DEVTEN_DISCONNEVTEN    (1u << 0)       /* :416 */
#define  DEVTEN_USBRSTEN        (1u << 1)       /* :415 */
#define  DEVTEN_CONNECTDONEEN   (1u << 2)       /* :414 */
#define  DEVTEN_ULSTCNGEN       (1u << 3)       /* :413 */
#define DSTS                    0xc70cu         /* :140 */
#define  DSTS_CONNECTSPD        7u              /* :439; 0 high, 1 full (and 3), 4 super (:442-444) */
#define  DSTS_USBLNKST(v)       (((v) >> 18) & 0xfu)   /* :431-432 */
#define  DSTS_DEVCTRLHLT        (1u << 22)      /* :429 */
#define DALEPENA                0xc720u         /* :143; bit n: physical endpoint n (:498) */
#define DEP_BASE(n)             (0xc800u + (n) * 0x10u)        /* :145 */
#define  DEPCMDPAR2             0x00u           /* :146 */
#define  DEPCMDPAR1             0x04u           /* :147 */
#define  DEPCMDPAR0             0x08u           /* :148 */
#define  DEPCMD                 0x0cu           /* :149 */
#define  DEPCMD_PARAM(x)        ((uint32_t)(x) << 16)  /* :475-476 */
#define  DEPCMD_RSC_IDX(x)      (((x) >> 16) & 0x7fu)  /* :477 */
#define  DEPCMD_STATUS(x)       (((x) >> 12) & 0xfu)   /* :478 */
#define  DEPCMD_HIPRI_FORCERM   (1u << 11)      /* :479 */
#define  DEPCMD_CMDACT          (1u << 10)      /* :481 */
#define  CMD_SETEPCONFIG        0x01u           /* :495 */
#define  CMD_SETTRANSFRESOURCE  0x02u           /* :494 */
#define  CMD_SETSTALL           0x04u           /* :489 */
#define  CMD_CLEARSTALL         0x05u           /* :488 */
#define  CMD_STARTTRANSFER      0x06u           /* :487 */
#define  CMD_ENDTRANSFER        0x08u           /* :485 */
#define  CMD_DEPSTARTCFG        0x09u           /* :484 */
/* DEPCFG's parameters (gadget.h) */
#define DEPCFG_EP_TYPE(t)       (((t) & 3u) << 1)              /* gadget.h:44 */
#define DEPCFG_MPS(n)           (((n) & 0x7ffu) << 3)          /* gadget.h:45 */
#define DEPCFG_FIFO_NUMBER(n)   (((n) & 0x1fu) << 17)          /* gadget.h:46 */
#define DEPCFG_ACTION_INIT      (0u << 30)                     /* gadget.h:52 */
#define DEPCFG_ACTION_MODIFY    (2u << 30)                     /* gadget.h:54 */
#define DEPCFG_XFER_COMPLETE_EN (1u << 8)                      /* gadget.h:32 */
#define DEPCFG_XFER_IN_PROGRESS_EN (1u << 9)                   /* gadget.h:33 */
#define DEPCFG_XFER_NOT_READY_EN (1u << 10)                    /* gadget.h:34 */
#define DEPCFG_BINTERVAL_M1(n)  (((n) & 0xffu) << 16)          /* gadget.h:37 */
#define DEPCFG_EP_NUMBER(n)     (((n) & 0x1fu) << 25)          /* gadget.h:39 */
#define DEPXFERCFG_NUM_XFER_RES(n) ((n) & 0xffffu)             /* gadget.h:57 */
/* the TRB (core.h:676-710) */
#define TRB_SIZE_MASK           0x00ffffffu     /* :682 */
#define TRB_SIZE_TRBSTS(s)      (((s) >> 28) & 0xfu)   /* :685; SETUP_PENDING 2 (:689) */
#define TRB_CTRL_HWO            (1u << 0)       /* :693 */
#define TRB_CTRL_LST            (1u << 1)       /* :694 */
#define TRB_CTRL_ISP_IMI        (1u << 10)      /* :698 */
#define TRB_CTRL_IOC            (1u << 11)      /* :699 */
#define TRBCTL_NORMAL           (1u << 4)       /* :697, 703-707 */
#define TRBCTL_CONTROL_SETUP    (2u << 4)
#define TRBCTL_CONTROL_STATUS2  (3u << 4)
#define TRBCTL_CONTROL_STATUS3  (4u << 4)
#define TRBCTL_CONTROL_DATA     (5u << 4)
#define TRBSTS_SETUP_PENDING    2u
/* events (core.h:52-66, 1095-1205): bit 0 clear, an endpoint's: [5:1] the endpoint, [9:6] the event, [15:12] status */
#define DEPEVT_XFERCOMPLETE     1u              /* :1103 */
#define DEPEVT_XFERNOTREADY     3u              /* :1105 */
#define DEPEVT_STATUS_CONTROL_DATA   1u         /* :1150 */
#define DEPEVT_STATUS_CONTROL_STATUS 2u         /* :1151 */
#define DEVT_DISCONNECT         0u              /* :56-59 */
#define DEVT_RESET              1u
#define DEVT_CONNECT_DONE       2u
#define DEVT_LINK_STATUS_CHANGE 3u

/* physical endpoints: 0 ep0 OUT, 1 ep0 IN, 2 EP1 OUT, 3 EP1 IN, 5 EP2 IN */
#define PEP_OUT                 2
#define PEP_IN                  3
#define PEP_NOTIFY              5
#define NPEPS                   6

/* memory the controller reaches: one 32 KB block, mapped uncached (see dma_alloc()) */
#define DMA_SIZE                (32 * 1024)
#define EVT_OFF                 0x0000u
#define EVT_SIZE                4096u
#define TRB_OFF(slot)           (0x1000u + (slot) * 64u)       /* one TRB per cache line */
#define EP0_OFF                 0x2000u         /* 512 bytes */
#define OUT_OFF                 0x2400u         /* 512 bytes */
#define IN_OFF                  0x4000u
#define IN_CHUNK                16384u

#define RING_SIZE               (1024u * 1024u)        /* the log on its way out; a power of two */

struct trb {
	uint32_t bpl, bph, size, ctrl;
};

enum { EP0_IDLE, EP0_SETUP, EP0_DATA, EP0_STATUS_WAIT, EP0_STATUS };

static uint32_t steps, poll_ms = 2, drain_ms = 1000, crlf = 1, full_speed, susphy, glue_flags;
static const struct hakios_usb_glue *glue;
static vm_offset_t regs;
static volatile uint8_t *dma;
static uint64_t dma_pa;
static uint32_t ev_pos;
static thread_call_t call;
static int stage;                       /* how far the bring-up got: 0 none ... 5 */
static bool started, connected, high_speed;
static uint32_t busy;                   /* the poll, or the way back to the loader, has the controller */
static bool want_return;                /* the console asked to end the run */
static bool wifi_report_said, want_wifi_report;           /* the console is up: the radio's log said again, once (hakios.lx.c) */
static thread_call_t return_call;       /* ... which end_the_run() does, on a thread of its own */
static uint64_t return_asked_at;        /* when it was started (mach_absolute_time), 0: not yet */
#define RETURN_FORCE_SECONDS    15      /* then the poll returns to the loader itself, without the save and the stops */

/* The cable: see "The cable" in the head comment */
#define VBUS_UNKNOWN            (-1)
#define VBUS_SILENT_SECONDS     3       /* a stopped link goes back up when VBUS has not been heard of for this long */
#define STORM_NO_SETUP          4       /* resets with no SETUP after them */
#define STORM_NO_CONFIG         12      /* resets with no SET_CONFIGURATION after them */
#define RESTART_WAIT_FIRST      2       /* seconds between restarts, doubling ... */
#define RESTART_WAIT_MOST       30      /* ... up to this; back to the first once the host has configured the link */
#define QUIET_SECONDS           5       /* VBUS there and no SOF or reset from a host for this long ... */
#define QUIET_RESTARTS          5       /* ... restarts for that, then none until the next change (a wall charger never talks) */
static int vbus_report = VBUS_UNKNOWN, vbus_seen = VBUS_UNKNOWN;
static uint64_t vbus_heard_at;          /* mach_absolute_time of the last report */
static bool link_stopped;               /* VBUS went: transfers ended, the pull-up off, the PHY down */
static bool watch_storms = true;        /* the console's usb watch */
static bool want_restart;               /* the console's usb restart */
static uint32_t want_drop_ms;           /* the console's usb drop */
static uint32_t resets_no_setup, resets_no_config, quiet_restarts;
static uint64_t last_heard;             /* mach_absolute_time of the host's last SOF or reset, or of the last restart */
static uint32_t last_sof;               /* DSTS's frame number when last looked at */
static uint32_t restart_wait = RESTART_WAIT_FIRST;
static uint64_t restart_after;          /* mach_absolute_time before which no storm restarts the link */
static uint64_t n_restarts;
static uint64_t hold_log_until;         /* after a restart: the log waits for the host to open the port, until this time */
static uint64_t hold_open_at;           /* when the host opened the port, while the log is held */
#define HOLD_LOG_SECONDS        10
static uint32_t say_resets = 16, say_setups = 64;      /* log lines left; a restart gives some back */

static struct usbacm acm;
static struct usbacm_line cline;
static int ep0_state;
static bool ep0_in, ep0_three;          /* the data stage's direction; a data stage there was */
static uint16_t ep0_out_len;
static int ep0_status_pep = -1, ep0_notready_pep = -1;
static struct { bool active; uint32_t rsc; uint32_t len; } pep[NPEPS];

static char *ring;
static uint32_t ring_head, ring_tail, ring_dropped;
static uint64_t n_resets, n_setups, n_stalls, n_in, n_out, n_events, last_phy_say;
static int usb_speed_v;
static char usb_state[24] = "off";

/* a picture of the panel on its way out (the console's shot): the phase, and where in it */
enum { SHOT_OFF, SHOT_LOG, SHOT_HEAD, SHOT_PIXELS, SHOT_END, SHOT_PREP };
static struct {
	int phase;                      /* SHOT_PREP: taken, being set up (nothing is sent meanwhile) */
	uint32_t mark;                  /* the log up to here goes out before the picture */
	uint32_t n, w, h, x, y, rowbytes, sent;
	uintptr_t base;
	bool rgb;                       /* base is w x h RGB888 as it goes out (a camshot), not the panel's BGRA */
	uint64_t start;
} shot;
static uint64_t n_shots;

SYSCTL_NODE(_kern_hakios, OID_AUTO, usb, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the USB gadget link (drivers/usb/hakios.dwc3.c, usb_gadget=N)");
SYSCTL_STRING(_kern_hakios_usb, OID_AUTO, state, CTLFLAG_RD | CTLFLAG_LOCKED, usb_state, 0, "off, phy, core, waiting, default, address, configured");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, bytes_in, CTLFLAG_RD | CTLFLAG_LOCKED, &n_in, "log bytes the host has taken");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, bytes_out, CTLFLAG_RD | CTLFLAG_LOCKED, &n_out, "bytes the host has sent");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, resets, CTLFLAG_RD | CTLFLAG_LOCKED, &n_resets, "bus resets");
SYSCTL_UINT(_kern_hakios_usb, OID_AUTO, dropped, CTLFLAG_RD | CTLFLAG_LOCKED, &ring_dropped, 0, "log bytes dropped: the host did not read fast enough");
SYSCTL_QUAD(_kern_hakios_usb, OID_AUTO, restarts, CTLFLAG_RD | CTLFLAG_LOCKED, &n_restarts, "the link restarted: a replug, or a host that got no answer");

/* the console's answers: into the log (and so to the host) as whole lines, not onto the screen */
static void con(const char *format, ...) __printflike(1, 2);

static void
con(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static void
set_state(const char *s)
{
	strlcpy(usb_state, s, sizeof(usb_state));
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(regs + off);
}

static inline void
wr(uint32_t off, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(regs + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline volatile struct trb *
trb_of(int p)
{
	int slot = p == PEP_NOTIFY ? 4 : p;

	return (volatile struct trb *)(dma + TRB_OFF(slot));
}

static inline uint64_t
trb_pa(int p)
{
	return dma_pa + TRB_OFF(p == PEP_NOTIFY ? 4 : p);
}

/* The log's way out: a ring the log fills (under its lock) and the poll empties. */

static void
tap(char c)
{
	uint32_t h = ring_head, t = os_atomic_load(&ring_tail, acquire);

	if (h - t >= RING_SIZE) {
		ring_dropped++;
		return;
	}
	ring[h & (RING_SIZE - 1)] = c;
	os_atomic_store(&ring_head, h + 1, release);
}

static void
history(const char *text, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		tap(text[i]);
	}
}

/* Endpoint commands. */

static int
depcmd(int p, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *out)
{
	uint32_t base = DEP_BASE((uint32_t)p), v = 0;

	wr(base + DEPCMDPAR2, p2);
	wr(base + DEPCMDPAR1, p1);
	wr(base + DEPCMDPAR0, p0);
	wr(base + DEPCMD, cmd | DEPCMD_CMDACT);
	for (int us = 0; us < 5000; us++) {
		v = rd(base + DEPCMD);
		if (!(v & DEPCMD_CMDACT)) {
			if (out != NULL) {
				*out = v;
			}
			if (DEPCMD_STATUS(v) != 0) {
				say("usb: endpoint %d command %x: status %u\n", p, cmd & 0xf, DEPCMD_STATUS(v));
				return EIO;
			}
			return 0;
		}
		delay(1);
	}
	say("usb: endpoint %d command %x: no answer in 5 ms (DEPCMD %08x)\n", p, cmd & 0xf, v);
	return ETIMEDOUT;
}

static int
start_xfer(int p, uint64_t buf_pa, uint32_t len, uint32_t trbctl)
{
	volatile struct trb *t = trb_of(p);
	uint32_t v = 0;
	uint64_t tp = trb_pa(p);

	t->bpl = (uint32_t)buf_pa;
	t->bph = (uint32_t)(buf_pa >> 32);
	t->size = len & TRB_SIZE_MASK;
	t->ctrl = trbctl | TRB_CTRL_LST | TRB_CTRL_IOC | TRB_CTRL_ISP_IMI | TRB_CTRL_HWO;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (depcmd(p, CMD_STARTTRANSFER, (uint32_t)(tp >> 32), (uint32_t)tp, 0, &v) != 0) {
		return EIO;
	}
	pep[p].active = true;
	pep[p].rsc = DEPCMD_RSC_IDX(v);
	pep[p].len = len;
	return 0;
}

static void
end_xfer(int p)
{
	if (!pep[p].active) {
		return;
	}
	(void)depcmd(p, CMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | DEPCMD_PARAM(pep[p].rsc), 0, 0, 0, NULL);
	delay(100);             /* as gadget.c's dwc3_stop_active_transfer waits */
	pep[p].active = false;
}

static int
ep_config(int p, uint32_t type, uint32_t mps, uint32_t action, uint32_t interval_m1)
{
	uint32_t p0 = DEPCFG_EP_TYPE(type) | DEPCFG_MPS(mps) | action;
	uint32_t p1 = DEPCFG_XFER_COMPLETE_EN | DEPCFG_EP_NUMBER((uint32_t)p);

	if (p & 1) {
		p0 |= DEPCFG_FIFO_NUMBER((uint32_t)p >> 1);
	}
	if (type == 0) {
		p1 |= DEPCFG_XFER_NOT_READY_EN;
	} else {
		p1 |= DEPCFG_XFER_IN_PROGRESS_EN;
	}
	if (type == 3) {
		p1 |= DEPCFG_BINTERVAL_M1(interval_m1);
	}
	return depcmd(p, CMD_SETEPCONFIG, p0, p1, 0, NULL);
}

/* Endpoint 0. */

static void
ep0_setup_start(void)
{
	ep0_state = EP0_SETUP;
	ep0_status_pep = -1;
	ep0_notready_pep = -1;
	if (start_xfer(0, dma_pa + EP0_OFF, 8, TRBCTL_CONTROL_SETUP) != 0) {
		ep0_state = EP0_IDLE;
	}
}

static void
ep0_stall(void)
{
	n_stalls++;
	(void)depcmd(0, CMD_SETSTALL, 0, 0, 0, NULL);
	ep0_setup_start();
}

static void
ep0_try_status(void)
{
	if (ep0_state != EP0_STATUS_WAIT || ep0_notready_pep != ep0_status_pep) {
		return;
	}
	ep0_state = EP0_STATUS;
	if (start_xfer(ep0_status_pep, dma_pa + EP0_OFF, 0, ep0_three ? TRBCTL_CONTROL_STATUS3 : TRBCTL_CONTROL_STATUS2) != 0) {
		ep0_stall();
	}
}

static void bulk_down(void);
static void bulk_up(void);
static void pump_in(void);

static void
apply(const struct usbacm_reply *r)
{
	if (r->effects & USBACM_EFF_ADDRESS) {
		wr(DCFG, (rd(DCFG) & ~DCFG_DEVADDR_MASK) | DCFG_DEVADDR(r->value));    /* before the status stage (ep0.c:553) */
		say("usb: address %u\n", r->value);
		set_state(r->value ? "address" : "default");
	}
	if (r->effects & USBACM_EFF_CONFIG) {
		bulk_down();
		if (r->value) {
			bulk_up();
			set_state("configured");
			resets_no_config = 0;
			quiet_restarts = 0;
			restart_wait = RESTART_WAIT_FIRST;
			say("usb: configured (%s speed): /dev/ttyACM* on a Linux host, /dev/cu.usbmodem* on a Mac\n", high_speed ? "high" : "full");
			hakios_ufs_summary("the USB console is up", 1 | 4);    /* ufs=3 only: what the storage did before the link */
			if (!wifi_report_said) {
				wifi_report_said = true;
				want_wifi_report = true;
			}
		} else {
			set_state("address");
			say("usb: unconfigured\n");
		}
	}
	if (r->effects & (USBACM_EFF_HALT | USBACM_EFF_CLEAR_HALT)) {
		int p = r->ep == USBACM_EP_OUT ? PEP_OUT : r->ep == USBACM_EP_IN ? PEP_IN : PEP_NOTIFY;

		if (r->effects & USBACM_EFF_HALT) {
			end_xfer(p);
			(void)depcmd(p, CMD_SETSTALL, 0, 0, 0, NULL);
		} else {
			(void)depcmd(p, CMD_CLEARSTALL, 0, 0, 0, NULL);
			if (p == PEP_OUT && steps >= 5 && !pep[PEP_OUT].active) {
				(void)start_xfer(PEP_OUT, dma_pa + OUT_OFF, 512, TRBCTL_NORMAL);
			}
		}
	}
	if (r->effects & USBACM_EFF_LINES) {
		say("usb: the host %s the port (DTR %u RTS %u)\n", (acm.lines & 1) ? "opened" : "closed", acm.lines & 1, (acm.lines >> 1) & 1);
	}
}

static void
ep0_complete(int p)
{
	volatile struct trb *t = trb_of(p);
	uint32_t sts = TRB_SIZE_TRBSTS(t->size);
	struct usbacm_reply r;
	uint8_t pkt[8];

	switch (ep0_state) {
	case EP0_SETUP:
		for (int i = 0; i < 8; i++) {
			pkt[i] = dma[EP0_OFF + (uint32_t)i];
		}
		n_setups++;
		resets_no_setup = 0;
		usbacm_setup(&acm, pkt, &r);
		if (say_setups > 0 || n_setups % 100 == 0) {
			say_setups -= say_setups > 0 ? 1 : 0;
			static const char *const what[] = { "stall", "status", "in", "out" };
			say("usb: setup %llu: %02x %02x %02x%02x %02x%02x %02x%02x -> %s %u\n", n_setups, pkt[0], pkt[1], pkt[3], pkt[2],
			    pkt[5], pkt[4], pkt[7], pkt[6], what[r.action & 3], r.len);
		}
		apply(&r);
		switch (r.action) {
		case USBACM_STATUS:
			ep0_three = false;
			ep0_status_pep = 1;
			ep0_state = EP0_STATUS_WAIT;
			ep0_try_status();
			break;
		case USBACM_IN:
			for (uint32_t i = 0; i < r.len; i++) {
				dma[EP0_OFF + i] = r.data[i];
			}
			ep0_in = true;
			ep0_three = true;
			ep0_state = EP0_DATA;
			if (start_xfer(1, dma_pa + EP0_OFF, r.len, TRBCTL_CONTROL_DATA) != 0) {
				ep0_stall();
			}
			break;
		case USBACM_OUT:
			ep0_in = false;
			ep0_three = true;
			ep0_out_len = r.len;
			ep0_state = EP0_DATA;
			/* an OUT data stage is received in whole packets (ep0.c __dwc3_ep0_do_control_data) */
			if (start_xfer(0, dma_pa + EP0_OFF, USBACM_EP0_MPS, TRBCTL_CONTROL_DATA) != 0) {
				ep0_stall();
			}
			break;
		default:
			ep0_stall();
			break;
		}
		break;
	case EP0_DATA:
		if (sts == TRBSTS_SETUP_PENDING) {
			ep0_setup_start();
			break;
		}
		if (!ep0_in) {
			uint32_t got = USBACM_EP0_MPS - (t->size & TRB_SIZE_MASK);
			uint8_t data[USBACM_EP0_MPS];

			for (uint32_t i = 0; i < got && i < sizeof(data); i++) {
				data[i] = dma[EP0_OFF + i];
			}
			if (usbacm_out_done(&acm, data, MIN(got, (uint32_t)ep0_out_len), &r) == USBACM_STALL) {
				ep0_stall();
				break;
			}
			apply(&r);
		}
		ep0_status_pep = ep0_in ? 0 : 1;
		ep0_state = EP0_STATUS_WAIT;
		ep0_try_status();
		break;
	case EP0_STATUS:
		ep0_setup_start();
		break;
	default:
		break;
	}
}

static void
ep0_notready(int p, uint32_t status)
{
	if (status == DEPEVT_STATUS_CONTROL_STATUS) {
		ep0_notready_pep = p;
		ep0_try_status();
	}
}

/* Bulk endpoints. */

static void
bulk_down(void)
{
	end_xfer(PEP_OUT);
	end_xfer(PEP_IN);
	end_xfer(PEP_NOTIFY);
	wr(DALEPENA, rd(DALEPENA) & ~((1u << PEP_OUT) | (1u << PEP_IN) | (1u << PEP_NOTIFY)));
}

static void
bulk_up(void)
{
	uint32_t mps = high_speed ? USBACM_HS_BULK : USBACM_FS_BULK;

	if (ep_config(PEP_OUT, 2, mps, DEPCFG_ACTION_INIT, 0) != 0 || ep_config(PEP_IN, 2, mps, DEPCFG_ACTION_INIT, 0) != 0 ||
	    ep_config(PEP_NOTIFY, 3, 16, DEPCFG_ACTION_INIT, high_speed ? 8 : 0) != 0) {
		say("usb: the bulk endpoints could not be configured\n");
		return;
	}
	wr(DALEPENA, rd(DALEPENA) | (1u << PEP_OUT) | (1u << PEP_IN) | (1u << PEP_NOTIFY));
	if (steps >= 5) {
		(void)start_xfer(PEP_OUT, dma_pa + OUT_OFF, mps, TRBCTL_NORMAL);
		pump_in();
	}
}

/*
 * The console's shot, one bulk IN transfer at a time: first the log as far as it had got when the command came, then
 * the header line, the pixels (as many whole pixels as fit in a transfer), "END". The log that comes meanwhile waits
 * in the ring (1 MB; a shot at N=4 is 0.8 MB and takes some 0.1 s at high speed) and goes out after END.
 */
static uint32_t
shot_fill(void)
{
	volatile uint8_t *out = dma + IN_OFF;
	uint32_t n = 0;

	if (shot.phase == SHOT_LOG) {
		uint32_t t = ring_tail;

		while (t != shot.mark && n + 2 <= IN_CHUNK) {
			char c = ring[t & (RING_SIZE - 1)];

			if (c == '\n' && crlf) {
				out[n++] = '\r';
			}
			out[n++] = (uint8_t)c;
			t++;
		}
		os_atomic_store(&ring_tail, t, release);
		if (t != shot.mark || n != 0) {
			if (t == shot.mark) {
				shot.phase = SHOT_HEAD;
			}
			return n;
		}
		shot.phase = SHOT_HEAD;
	}
	if (shot.phase == SHOT_HEAD) {
		shot.phase = SHOT_PIXELS;
		shot.x = shot.y = 0;
		shot.start = mach_absolute_time();
		return (uint32_t)snprintf((char *)(uintptr_t)out, 96, "SHOT %u %u %u rgb888 %u\n", shot.w, shot.h, shot.w * 3,
		           shot.w * shot.h * 3);
	}
	if (shot.phase == SHOT_PIXELS && shot.rgb) {
		uint32_t total = shot.w * shot.h * 3, take = total - shot.sent;

		take = take > IN_CHUNK ? IN_CHUNK : take;
		memcpy((void *)(uintptr_t)out, (const void *)(shot.base + shot.sent), take);
		shot.sent += take;
		if (shot.sent < total) {
			return take;
		}
		shot.y = shot.h;
		shot.phase = SHOT_END;
		if (take != 0) {
			return take;
		}
	}
	if (shot.phase == SHOT_PIXELS) {
		while (shot.y < shot.h && n + 3 <= IN_CHUNK) {
			const volatile uint32_t *row = (const volatile uint32_t *)(shot.base +
			    (size_t)(shot.y * shot.n + shot.n / 2) * shot.rowbytes);
			uint32_t px = row[shot.x * shot.n + shot.n / 2];      /* B G R A in memory */

			out[n++] = (uint8_t)(px >> 16);
			out[n++] = (uint8_t)(px >> 8);
			out[n++] = (uint8_t)px;
			if (++shot.x == shot.w) {
				shot.x = 0;
				shot.y++;
			}
		}
		shot.sent += n;
		if (shot.y < shot.h) {
			return n;
		}
		shot.phase = SHOT_END;
		if (n != 0) {
			return n;
		}
	}
	/* SHOT_END */
	out[0] = 'E';
	out[1] = 'N';
	out[2] = 'D';
	out[3] = '\n';
	shot.phase = SHOT_OFF;
	n_shots++;
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - shot.start, &ns);
		con("usb: %s %llu sent: %ux%u, %u bytes of pixels in %llu ms\n", shot.rgb ? "camshot" : "shot", n_shots, shot.w,
		    shot.h, shot.sent, ns / NSEC_PER_MSEC);
	}
	return 4;
}

/* the console's "shot [N]": the panel's picture, every Nth pixel, queued behind what the log has already said */
static void
shot_begin(const char *arg)
{
	uint32_t n = 0;

	for (; *arg >= '0' && *arg <= '9' && n < 1000; arg++) {
		n = n * 10 + (uint32_t)(*arg - '0');
	}
	if (n == 0) {
		n = 4;
	}
	if (n > 16 || *arg != 0) {
		con("usb: shot failed: N is a number from 1 to 16 (shot 4: a quarter of the panel's width and height)\n");
		return;
	}
	/*
	 * With fb_dpp=1 the boot framebuffer is no longer shown (it keeps the boot logo): the buffer VGF0 scans out is the
	 * presenter's, the display's own size (1125x2436), and the shot is of that; with fb_flip=1 the flip buffer shown.
	 */
	uintptr_t base = (uintptr_t)vinfo.v_baseaddr;
	uint32_t width = vinfo.v_width, height = vinfo.v_height, rowbytes = vinfo.v_rowbytes;
	unsigned index = 0;
	int scanned = hakios_fb_scanned(&base, &width, &height, &rowbytes, &index);

	if (scanned == 0 && (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32)) {
		con("usb: shot failed: no 32-bit panel framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height, vinfo.v_depth);
		return;
	}
	if (width < n || height < n) {
		con("usb: shot failed: a picture of %ux%u, smaller than every %uth pixel\n", width, height, n);
		return;
	}
	if (!os_atomic_cmpxchg(&shot.phase, SHOT_OFF, SHOT_PREP, acq_rel)) {
		con("usb: shot failed: one is on its way already\n");
		return;
	}
	shot.rgb = false;
	shot.n = n;
	shot.w = width / n;
	shot.h = height / n;
	shot.rowbytes = rowbytes;
	shot.base = base;
	shot.sent = 0;
	if (scanned != 0) {
		con("usb: shot: %s buffer %u (%ux%u, %u bytes a row), every %uth pixel: %ux%u RGB, %u bytes, after this line\n",
		    scanned == 2 ? "fb_dpp: VGF0's" : "fb_flip: the shown", index, width, height, rowbytes, n, shot.w, shot.h,
		    shot.w * shot.h * 3);
	} else {
		con("usb: shot: the panel (%ux%u, %u bytes a row), every %uth pixel: %ux%u RGB, %u bytes, after this line\n",
		    width, height, rowbytes, n, shot.w, shot.h, shot.w * shot.h * 3);
	}
	shot.mark = os_atomic_load(&ring_head, acquire);
	os_atomic_store(&shot.phase, SHOT_LOG, release);
}

int hakios_usb_picture(uintptr_t rgb, uint32_t width, uint32_t height, const char *what);

/*
 * A picture someone else made, w x h RGB888 at rgb (the camera's camshot, from its own thread): sent as a shot is, after
 * what the log has said so far. The memory must stay as it is until "camshot ... sent" (the camera takes no new picture
 * before the console asks for one, and the console asks none while this one is on its way). 0: on its way.
 */
int
hakios_usb_picture(uintptr_t rgb, uint32_t width, uint32_t height, const char *what)
{
	if (steps < 5 || acm.config == 0 || rgb == 0 || width == 0 || height == 0) {
		return -1;
	}
	if (!os_atomic_cmpxchg(&shot.phase, SHOT_OFF, SHOT_PREP, acq_rel)) {
		return -1;
	}
	shot.rgb = true;
	shot.n = 1;
	shot.w = width;
	shot.h = height;
	shot.rowbytes = width * 3;
	shot.base = rgb;
	shot.sent = 0;
	con("usb: camshot: %s, %ux%u RGB, %u bytes, after this line\n", what, width, height, width * height * 3);
	shot.mark = os_atomic_load(&ring_head, acquire);
	os_atomic_store(&shot.phase, SHOT_LOG, release);
	return 0;
}

const char *hakios_exynos_camera_shot(uint32_t n, int pattern);         /* hakios.exynos_camera.c (or the stub) */
const char *hakios_exynos_iris_shot(uint32_t n);                         /* hakios.exynos_iris.c (or the stub) */

/* the console's "irshot [N]": asked of the iris camera's thread, which sends the picture when it has one */
static void
irshot_begin(const char *arg)
{
	uint32_t n = 0;
	const char *no;

	for (; *arg >= '0' && *arg <= '9' && n < 1000; arg++) {
		n = n * 10 + (uint32_t)(*arg - '0');
	}
	if (n == 0) {
		n = 2;
	}
	if (n < 2 || n > 8 || *arg != 0) {
		con("usb: irshot failed: irshot [N]: N from 2 to 8 (2: 1200 x 1200)\n");
		return;
	}
	if (os_atomic_load(&shot.phase, acquire) != SHOT_OFF) {
		con("usb: irshot failed: a picture is on its way already\n");
		return;
	}
	no = hakios_exynos_iris_shot(n);
	if (no != NULL) {
		con("usb: irshot failed: %s\n", no);
		return;
	}
	con("usb: irshot: asked of the iris camera (N = %u); its log, then the picture\n", n);
}

/* the console's "camshot [N] [tp]": asked of the camera's thread, which sends the picture when it has one */
static void
camshot_begin(const char *arg)
{
	uint32_t n = 0;
	int pattern = 0;
	const char *no;

	for (; *arg >= '0' && *arg <= '9' && n < 1000; arg++) {
		n = n * 10 + (uint32_t)(*arg - '0');
	}
	while (*arg == ' ') {
		arg++;
	}
	if (strcmp(arg, "tp") == 0) {
		pattern = 1;
		arg += 2;
	}
	if (n == 0) {
		n = 1;
	}
	if (n > 16 || *arg != 0) {
		con("usb: camshot failed: camshot [N] [tp]: N from 1 to 16 (1: the whole 1632 x 1224, 2: 816 x 612), tp the test pattern\n");
		return;
	}
	if (os_atomic_load(&shot.phase, acquire) != SHOT_OFF) {
		con("usb: camshot failed: a picture is on its way already\n");
		return;
	}
	no = hakios_exynos_camera_shot(n, pattern);
	if (no != NULL) {
		con("usb: camshot failed: %s\n", no);
		return;
	}
	con("usb: camshot: asked of the camera (%s, N = %u); its log, then the picture\n",
	    pattern ? "the test pattern" : "the sensor", n);
}

static void
pump_in(void)
{
	uint32_t n = 0, lost;
	int phase;

	if (steps < 5 || acm.config == 0 || pep[PEP_IN].active || (acm.halted & 2)) {
		return;
	}
	if (hold_log_until != 0) {
		/*
		 * A host that opens the port and closes it before reading would lose what was sent meanwhile, so the log waits
		 * until the port has stayed open for 300 ms, or until the hold runs out. A reader that sets the port up (stty)
		 * closes it again before it starts reading.
		 */
		uint64_t now = mach_absolute_time(), ns = 0;

		if (acm.lines & 1) {
			hold_open_at = hold_open_at != 0 ? hold_open_at : now;
			absolutetime_to_nanoseconds(now - hold_open_at, &ns);
		} else {
			hold_open_at = 0;
		}
		if (ns < 300 * NSEC_PER_MSEC && now < hold_log_until) {
			return;
		}
		hold_log_until = 0;
		hold_open_at = 0;
	}
	phase = os_atomic_load(&shot.phase, acquire);    /* the camera's thread sets a camshot up (hakios_usb_picture) */
	if (phase == SHOT_PREP) {
		return;                 /* a picture being set up: the log waits for it */
	}
	if (phase != SHOT_OFF) {
		n = shot_fill();
		if (n != 0) {
			(void)start_xfer(PEP_IN, dma_pa + IN_OFF, n, TRBCTL_NORMAL);
		}
		return;
	}
	lost = os_atomic_xchg(&ring_dropped, 0, relaxed);
	if (lost != 0) {
		n = (uint32_t)snprintf((char *)(uintptr_t)(dma + IN_OFF), 64, "\r\n[usb: %u bytes of the log lost]\r\n", lost);
	}
	uint32_t t = ring_tail, h = os_atomic_load(&ring_head, acquire);
	while (t != h && n + 2 <= IN_CHUNK) {
		char c = ring[t & (RING_SIZE - 1)];

		if (c == '\n' && crlf) {
			dma[IN_OFF + n++] = '\r';
		}
		dma[IN_OFF + n++] = (uint8_t)c;
		t++;
	}
	os_atomic_store(&ring_tail, t, release);
	if (n != 0) {
		(void)start_xfer(PEP_IN, dma_pa + IN_OFF, n, TRBCTL_NORMAL);
	}
}

static void console(char *line);

static void
bulk_complete(int p)
{
	volatile struct trb *t = trb_of(p);
	uint32_t left = t->size & TRB_SIZE_MASK, done = pep[p].len > left ? pep[p].len - left : 0;

	if (p == PEP_IN) {
		n_in += done;
		pump_in();
	} else if (p == PEP_OUT) {
		n_out += done;
		for (uint32_t i = 0; i < done; i++) {
			if (usbacm_line_feed(&cline, dma[OUT_OFF + i])) {
				console(cline.line);
			}
		}
		if (acm.config != 0 && !(acm.halted & 1)) {
			(void)start_xfer(PEP_OUT, dma_pa + OUT_OFF, high_speed ? USBACM_HS_BULK : USBACM_FS_BULK, TRBCTL_NORMAL);
		}
	}
}

/* Device events. */

/* the log waits, for a while, for the host to have the port open: see pump_in() */
static void
hold_log(void)
{
	uint64_t ns;

	nanoseconds_to_absolutetime((uint64_t)HOLD_LOG_SECONDS * NSEC_PER_SEC, &ns);
	hold_log_until = mach_absolute_time() + ns;
	hold_open_at = 0;
}

static void
on_reset(void)
{
	hold_log();
	n_resets++;
	last_heard = mach_absolute_time();
	resets_no_setup++;
	resets_no_config++;
	if (say_resets > 0) {
		say_resets--;
		say("usb: bus reset %llu\n", n_resets);
	}
	bulk_down();
	for (int p = 2; p < NPEPS; p++) {
		if (p != 4) {
			(void)depcmd(p, CMD_CLEARSTALL, 0, 0, 0, NULL);
		}
	}
	wr(DCFG, rd(DCFG) & ~DCFG_DEVADDR_MASK);
	usbacm_reset(&acm);
	set_state("default");
	if (ep0_state != EP0_SETUP) {
		end_xfer(0);
		end_xfer(1);
		ep0_setup_start();
	}
}

static void
on_connect_done(void)
{
	uint32_t speed = rd(DSTS) & DSTS_CONNECTSPD;

	high_speed = speed == 0;
	acm.high_speed = high_speed;
	connected = true;
	usb_speed_v = (int)speed;
	(void)ep_config(0, 0, USBACM_EP0_MPS, DEPCFG_ACTION_MODIFY, 0);
	(void)ep_config(1, 0, USBACM_EP0_MPS, DEPCFG_ACTION_MODIFY, 0);
	if (say_resets > 0 || n_resets % 100 == 0) {
		say("usb: connected at %s speed (DSTS %08x)\n", speed == 0 ? "high" : speed == 4 ? "super (unexpected)" : "full", rd(DSTS));
	}
}

static void
events(void)
{
	uint32_t count = rd(GEVNTCOUNT) & GEVNTCOUNT_MASK;

	while (count >= 4) {
		uint32_t ev = *(volatile uint32_t *)(dma + EVT_OFF + ev_pos);

		ev_pos = (ev_pos + 4) % EVT_SIZE;
		count -= 4;
		n_events++;
		if (!(ev & 1)) {
			int p = (int)((ev >> 1) & 0x1f);
			uint32_t type = (ev >> 6) & 0xf, status = (ev >> 12) & 0xf;

			if (p < NPEPS && type == DEPEVT_XFERCOMPLETE) {
				pep[p].active = false;
				if (p <= 1) {
					ep0_complete(p);
				} else {
					bulk_complete(p);
				}
			} else if (p <= 1 && type == DEPEVT_XFERNOTREADY) {
				ep0_notready(p, status & 3);
			}
		} else if (((ev >> 1) & 0x7f) == 0) {
			switch ((ev >> 8) & 0xf) {
			case DEVT_DISCONNECT:
				say("usb: disconnected\n");
				connected = false;
				bulk_down();
				usbacm_reset(&acm);
				set_state("waiting");
				break;
			case DEVT_RESET:
				on_reset();
				break;
			case DEVT_CONNECT_DONE:
				on_connect_done();
				break;
			default:
				break;
			}
		}
		wr(GEVNTCOUNT, 4);
	}
}

/* Line console. */

static const char *
errno_name(int error)
{
	switch (error) {
	case ENOENT: return "ENOENT: no such name";
	case EPERM: return "EPERM: not marked CTLFLAG_KERN, so the kernel may not reach it (user space may)";
	case EISDIR: return "EISDIR: a node, not a value";
	case EINVAL: return "EINVAL: the value was refused";
	case EIO: return "EIO: the hardware did not take it";
	case ENODEV: return "ENODEV: no driver behind it";
	case ENOMEM: return "ENOMEM: longer than 256 bytes";
	default: return "error";
	}
}

/* sysctl NAME: read; sysctl NAME=N: an integer set, then read back */
static void
console_sysctl(char *arg)
{
	union { char s[256]; int32_t i; int64_t q; } v;
	size_t len = sizeof(v.s);
	bool text = true;
	char *equals = strchr(arg, '=');
	int error;

	if (equals != NULL) {
		const char *p = equals + 1;
		bool negative = *p == '-';
		int32_t value = 0;

		*equals = 0;
		p += negative ? 1 : 0;
		if (*p < '0' || *p > '9') {
			con("usb: sysctl %s=%s: an integer is wanted\n", arg, equals + 1);
			return;
		}
		while (*p >= '0' && *p <= '9' && value < 100000000) {
			value = value * 10 + (*p++ - '0');
		}
		value = negative ? -value : value;
		error = kernel_sysctlbyname(arg, NULL, NULL, &value, sizeof(value));
		con("usb: sysctl %s=%d: %s (%d)\n", arg, value, error == 0 ? "set" : errno_name(error), error);
		if (error != 0) {
			return;
		}
	}
	memset(&v, 0, sizeof(v));
	if ((error = kernel_sysctlbyname(arg, &v, &len, NULL, 0)) != 0) {
		con("usb: sysctl %s: %s (%d)\n", arg, errno_name(error), error);
		return;
	}
	for (size_t i = 0; i < len; i++) {
		if (v.s[i] == 0 && i + 1 == len) {
			break;
		}
		if (v.s[i] < 32 || v.s[i] > 126) {
			text = false;
		}
	}
	if (text && len > 0) {
		v.s[MIN(len, sizeof(v.s) - 1)] = 0;
		con("usb: %s = %s\n", arg, v.s);
	} else if (len == 4) {
		con("usb: %s = %d\n", arg, v.i);
	} else if (len == 8) {
		con("usb: %s = %lld\n", arg, v.q);
	} else {
		con("usb: %s: %zu bytes, not shown\n", arg, len);
	}
}

/*
 * swipe [l|r|u|d|lr|ud] [N] [MS] [GAP]: a finger drawn across the screen N times (default lr 6 250 1000: left, right,
 * left ..., each stroke 250 ms, a second apart), posted to the touch queue as the touch screen's driver posts its
 * events (hakios_touch_post(), one every 8 ms, the S6SY761's pace, with the governor's touch boost), so a run can be
 * repeated without a finger. Strokes keep 20% from the edges: from the bottom edge
 * one would go home. Left and right page the home screen; up and down scroll a list. On a thread of its own.
 */
#define SWIPE_STEP_MS   8
static struct {
	char pattern[3];
	uint32_t count, ms, gap;
} swipe_job;
static volatile uint32_t swipe_running;

static void
swipe_stroke(char dir, uint32_t ms)
{
	uint32_t x0 = 32768, y0 = 32768, x1 = 32768, y1 = 32768, steps = ms / SWIPE_STEP_MS;

	switch (dir) {
	case 'l': x0 = 52428; x1 = 13107; break;       /* 80% to 20% of the width */
	case 'r': x0 = 13107; x1 = 52428; break;
	case 'u': y0 = 45875; y1 = 19660; break;       /* 70% to 30% of the height */
	default: y0 = 19660; y1 = 45875; break;
	}
	steps = steps < 2 ? 2 : steps;
	hakios_exynos_cpufreq_touch();
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, x0, y0);
	for (uint32_t i = 1; i <= steps; i++) {
		IOSleep(SWIPE_STEP_MS);
		hakios_exynos_cpufreq_touch();
		hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, x0 + (uint32_t)(((int64_t)x1 - x0) * i / steps),
		    y0 + (uint32_t)(((int64_t)y1 - y0) * i / steps));
	}
	IOSleep(SWIPE_STEP_MS);
	hakios_touch_post(HAKIOS_TOUCH_UP, 0, x1, y1);
}

static void
swipe_thread(__unused void *arg, __unused wait_result_t wr)
{
	size_t kinds = strlen(swipe_job.pattern);
	uint64_t ns;

	for (uint32_t n = 0; n < swipe_job.count; n++) {
		if (n > 0) {
			IOSleep(swipe_job.gap);
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		con("usb: swipe %u of %u: %c at %llu.%03llu s\n", n + 1, swipe_job.count, swipe_job.pattern[n % kinds],
		    ns / NSEC_PER_SEC, (ns / NSEC_PER_MSEC) % 1000);
		swipe_stroke(swipe_job.pattern[n % kinds], swipe_job.ms);
	}
	con("usb: swipe: done\n");
	swipe_running = 0;
}

static uint32_t
console_number(const char **arg, uint32_t otherwise, uint32_t least, uint32_t most)
{
	uint32_t n = 0;
	bool any = false;

	while (**arg == ' ') {
		(*arg)++;
	}
	for (; **arg >= '0' && **arg <= '9' && n < 100000; (*arg)++) {
		n = n * 10 + (uint32_t)(**arg - '0');
		any = true;
	}
	return !any ? otherwise : n < least ? least : n > most ? most : n;
}

static void
console_swipe(const char *arg)
{
	thread_t thread;
	size_t k = 0;

	if (swipe_running) {
		con("usb: swipe: one is going on\n");
		return;
	}
	while (*arg == ' ') {
		arg++;
	}
	while (k < 2 && (*arg == 'l' || *arg == 'r' || *arg == 'u' || *arg == 'd')) {
		swipe_job.pattern[k++] = *arg++;
	}
	swipe_job.pattern[k] = 0;
	if (k == 0) {
		strlcpy(swipe_job.pattern, "lr", sizeof(swipe_job.pattern));
	}
	swipe_job.count = console_number(&arg, 6, 1, 1000);
	swipe_job.ms = console_number(&arg, 250, 16, 5000);
	swipe_job.gap = console_number(&arg, 1000, 0, 60000);
	con("usb: swipe %s: %u strokes of %u ms, %u ms apart\n", swipe_job.pattern, swipe_job.count, swipe_job.ms, swipe_job.gap);
	swipe_running = 1;
	if (kernel_thread_start(swipe_thread, NULL, &thread) != KERN_SUCCESS) {
		swipe_running = 0;
		con("usb: swipe: no thread\n");
		return;
	}
	thread_deallocate(thread);
}

/* bl: the panel's brightness; bl N: set, straight to the panel driver (not through sysctl), and the answer */
static void
console_bl(const char *arg)
{
	int level = 0, error;
	uint64_t t0, t1;

	if (*arg == 0) {
		console_sysctl((char *)(uintptr_t)"kern.hakios.brightness");
		return;
	}
	if (*arg < '0' || *arg > '9') {
		con("usb: bl %s: a level 0-255 is wanted\n", arg);
		return;
	}
	while (*arg >= '0' && *arg <= '9' && level < 1000) {
		level = level * 10 + (*arg++ - '0');
	}
	if (level > 255) {
		con("usb: bl %d: 0-255\n", level);
		return;
	}
	t0 = mach_absolute_time();
	error = hakios_panel_brightness(level);
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &t1);
	con("usb: bl %d: %s (%d) in %llu us%s\n", level, error == 0 ? "sent: the panel driver took it and the DSIM took "
	    "every packet" : errno_name(error), error, t1 / NSEC_PER_USEC, error == ENODEV ? " (panel=1 not given?)" : "");
}

/* what the link is doing, for the log; said before a restart too, so that a bad one leaves a trace */
static void
say_link(int (*out)(const char *, ...), const char *when)
{
	char st[120] = "";

	if (glue->phy_state != NULL) {
		glue->phy_state(st, sizeof(st));
	}
	out("usb: link %s: %s (%s); DCTL %08x DCFG %08x DSTS %08x DALEPENA %08x events %08x; ep0 state %d, OUT active %d (TRB %08x %08x), "
	    "IN active %d (TRB %08x %08x); bulk IN active %d OUT active %d, config %u; %llu resets, %llu requests, %u/%u resets since a "
	    "SETUP/a configuration; VBUS %d, stopped %d, %llu restarts\n", when, usb_state, st, rd(DCTL), rd(DCFG), rd(DSTS), rd(DALEPENA),
	    rd(GEVNTCOUNT), ep0_state, pep[0].active, trb_of(0)->size, trb_of(0)->ctrl, pep[1].active, trb_of(1)->size, trb_of(1)->ctrl,
	    pep[PEP_IN].active, pep[PEP_OUT].active, acm.config, n_resets, n_setups, resets_no_setup, resets_no_config, vbus_seen,
	    link_stopped, n_restarts);
}

/* A printf-style logger that returns int, for say_link(). */
static int
con_say(const char *format, ...)
{
	char line[600];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return 0;
}

/* usb, usb restart, usb drop MS, usb unplug MS, usb muic MS, usb watch on|off */
static void
console_usb(const char *arg)
{
	if (*arg == 0) {
		say_link(con_say, "now");
		hakios_max77705_usb_console("");
	} else if (strcmp(arg, "restart") == 0) {
		want_restart = true;            /* tick() does it, after this event */
	} else if (strncmp(arg, "drop ", 5) == 0) {
		const char *p = arg + 5;

		want_drop_ms = console_number(&p, 3000, 1, 60000);
	} else if (strcmp(arg, "watch off") == 0 || strcmp(arg, "watch on") == 0) {
		watch_storms = arg[7] == 'n';
		con("usb: the check for resets with no SETUP: %s\n", watch_storms ? "on" : "off");
	} else if (strncmp(arg, "unplug ", 7) == 0 || strncmp(arg, "muic ", 5) == 0 || strncmp(arg, "vbus ", 5) == 0) {
		hakios_max77705_usb_console(arg);
	} else {
		con("usb: usb [restart | drop MS | unplug MS | muic MS | vbus MS | watch on|off]\n");
	}
}

static void
console(char *line)
{
	char *arg = line;

	while (*arg != 0 && *arg != ' ') {
		arg++;
	}
	if (*arg == ' ') {
		*arg++ = 0;
		while (*arg == ' ') {
			arg++;
		}
	}
	con("usb> %s%s%s\n", line, *arg ? " " : "", arg);
	if (strcmp(line, "help") == 0) {
		con("usb: help, stat, uptime, sysctl NAME, sysctl NAME=N, bl (the panel's brightness), bl N (set it, 0-255), "
		    "log (the kernel's log again from its start), shot [N] (the panel's picture, binary: tools/usb-shot.py), "
		    "camshot [N] [tp] (the S9's front camera, cam_s9=6: tools/usb-shot.py --cam), "
		    "irshot [N] (the S9's iris camera, iris_s9=5: tools/usb-shot.py --iris), "
		    "ufs (the storage's ufs=3 summary), "
		    "abox [tone [S] | rec [S] | r BLOCK OFF [N] | w BLOCK OFF VALUE | set NAME VALUE] (the S9's speaker path, abox=N; "
		    "rec: S seconds of the codec's microphone, sent as a camshot's bytes), "
		    "codec [power | r REG [N] | w REG VALUE] (the S9's audio codec, cs47l93=N), "
		    "swipe [l|r|u|d|lr|ud] [N] [MS] [GAP] (a finger drawn across the screen N times, MS each, GAP apart; default "
		    "lr 6 250 1000), "
		    "sensors [on N | off N | fake X Y Z S | pins | retry ...] (the S9's sensor hub and its sensors' last samples, shub=N), "
		    "wifi (the radio's log kept, and its driver's state now), wifi scan (a scan of every channel by the station, and "
		    "the access points heard), bt (the S9's Bluetooth at bt=7: its state), bt scan (an inquiry and an LE scan: the devices "
		    "heard), wifi ip (en0's address and gateway), wifi ping [A.B.C.D] [SIZE] (four echo requests; the "
		    "gateway by default, 64 bytes), wifi dns NAME [A.B.C.D] (a DNS question; DHCP's server by default), wifi tcp HOST "
		    "[PORT [PATH]] (a TCP connection and an HTTP GET, the bytes back), wifi stat (en0's frames by size and kind, IP, UDP "
		    "and TCP counts), wifi names [off] (DNS questions and answers, TCP connections, as they come), touch X Y "
		    "[X2 Y2 [MS]] (a tap or a swipe, in thousandths of the screen), button lock|home|up|down (a press), usb [restart | "
		    "drop MS | unplug MS | muic MS | vbus MS | watch on|off] (the link and the cable), return (end the run)\n");
	} else if (strcmp(line, "stat") == 0) {
		con("usb: %s, %s speed, %llu resets, %llu requests (%llu stalled), %llu bytes sent, %llu received, %u lost, %llu events, "
		    "%llu shots, %llu restarts\n", usb_state, high_speed ? "high" : "full", n_resets, n_setups, n_stalls, n_in, n_out,
		    ring_dropped, n_events, n_shots, n_restarts);
	} else if (strcmp(line, "usb") == 0) {
		console_usb(arg);
	} else if (strcmp(line, "uptime") == 0) {
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		con("usb: up %llu.%03llu s\n", ns / NSEC_PER_SEC, (ns / NSEC_PER_MSEC) % 1000);
	} else if (strcmp(line, "sysctl") == 0 && *arg != 0) {
		console_sysctl(arg);
	} else if (strcmp(line, "bl") == 0) {
		console_bl(arg);
	} else if (strcmp(line, "swipe") == 0) {
		console_swipe(arg);
	} else if (strcmp(line, "wifi") == 0 && strcmp(arg, "scan") == 0) {
		hakios_sta_console_scan();
	} else if (strcmp(line, "wifi") == 0 && strcmp(arg, "ip") == 0) {
		hakios_net_console_ip();
	} else if (strcmp(line, "wifi") == 0 && strncmp(arg, "ping", 4) == 0 && (arg[4] == 0 || arg[4] == ' ')) {
		const char *to = arg + 4;

		while (*to == ' ') {
			to++;
		}
		hakios_net_console_ping(to);
	} else if (strcmp(line, "wifi") == 0 && strncmp(arg, "dns", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
		hakios_net_console_dns(arg + 3 + (arg[3] == ' '));
	} else if (strcmp(line, "wifi") == 0 && strncmp(arg, "tcp", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
		hakios_net_console_tcp(arg + 3 + (arg[3] == ' '));
	} else if (strcmp(line, "wifi") == 0 && strcmp(arg, "stat") == 0) {
		hakios_net_console_stat();
	} else if (strcmp(line, "wifi") == 0 && strncmp(arg, "names", 5) == 0 && (arg[5] == 0 || arg[5] == ' ')) {
		hakios_net_console_names(arg + 5 + (arg[5] == ' '));
	} else if (strcmp(line, "wifi") == 0) {
		hakios_wifi_report(false);
	} else if (strcmp(line, "touch") == 0) {
		hakios_input_console_touch(arg);
	} else if (strcmp(line, "button") == 0) {
		hakios_input_console_button(arg);
	} else if (strcmp(line, "log") == 0) {
		con("usb: the kernel's log from its start:\n");
		(void)loader_log_tap_set(tap, history);
		con("usb: (end of what the kernel keeps; the rest was sent as it came)\n");
	} else if (strcmp(line, "shot") == 0) {
		shot_begin(arg);
	} else if (strcmp(line, "camshot") == 0) {
		camshot_begin(arg);
	} else if (strcmp(line, "irshot") == 0) {
		irshot_begin(arg);
	} else if (strcmp(line, "abox") == 0) {
		hakios_abox_console(arg);
	} else if (strcmp(line, "codec") == 0) {
		hakios_cs47l93_console(arg);
	} else if (strcmp(line, "sensors") == 0) {
		hakios_ssp_console(arg);
	} else if (strcmp(line, "bt") == 0) {
		hakios_bt_console(arg);
	} else if (strcmp(line, "ufs") == 0) {
		hakios_ufs_summary("asked on the USB console", 1);
	} else if (strcmp(line, "return") == 0) {
		con("usb: ending the run: the loader will restart the phone\n");
		want_return = true;     /* done by tick() once this event is handled and the controller let go */
	} else {
		con("usb: %s: not a command (help)\n", line);
	}
}

/*
 * The console's "return": what loader_return_timer() does before it returns, then the return. Not on the poll's thread:
 * the save can wait for the storage, and the poll is what moves the log meanwhile.
 */
static void
end_the_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	hakios_var_save_on_return();
	hakios_adreno_stop();
	hakios_wifi_stop();
	hakios_adsp_stop();
	loader_return();
}

/* from the poll, once the console has asked: start the end of the run, or force it when it has not come */
static void
return_check(void)
{
	uint64_t now = mach_absolute_time(), ns;

	if (return_asked_at == 0) {
		return_asked_at = now;
		if (return_call != NULL) {
			thread_call_enter(return_call);
		} else {
			end_the_run(NULL, NULL);
		}
		return;
	}
	absolutetime_to_nanoseconds(now - return_asked_at, &ns);
	if (ns >= (uint64_t)RETURN_FORCE_SECONDS * NSEC_PER_SEC) {
		say("usb: the run has not ended %u s after \"return\" (the data volume's save or a driver's stop is waiting): "
		    "back to the loader without them\n", RETURN_FORCE_SECONDS);
		loader_return();
	}
}

/* Bring-up. */

static void
say_dwc3(const char *when)
{
	say("usb: DWC3 %s: GSNPSID %08x GCTL %08x GSTS %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x GHWPARAMS0/1/3 %08x %08x %08x\n", when,
	    rd(GSNPSID), rd(GCTL), rd(GSTS), rd(GUSB2PHYCFG), rd(GUSB3PIPECTL), rd(GHWPARAMS0), rd(GHWPARAMS1), rd(GHWPARAMS3));
	say("usb: DWC3 %s: DCFG %08x DCTL %08x (RUN_STOP %u) DSTS %08x (link state %u, halted %u) DEVTEN %08x event buffer %08x%08x size %08x count %08x\n",
	    when, rd(DCFG), rd(DCTL), (rd(DCTL) & DCTL_RUN_STOP) ? 1 : 0, rd(DSTS), DSTS_USBLNKST(rd(DSTS)),
	    (rd(DSTS) & DSTS_DEVCTRLHLT) ? 1 : 0, rd(DEVTEN), rd(GEVNTADRHI), rd(GEVNTADRLO), rd(GEVNTSIZ), rd(GEVNTCOUNT));
}

/* the controller stopped: pull-up off, then its events masked (gadget.c dwc3_gadget_run_stop(, 0), 1733-1800) */
static bool
stop_controller(void)
{
	wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);
	for (int i = 0; i < 100; i++) {
		uint32_t pending = rd(GEVNTCOUNT) & GEVNTCOUNT_MASK;

		if (rd(DSTS) & DSTS_DEVCTRLHLT) {
			break;
		}
		if (pending != 0) {
			wr(GEVNTCOUNT, pending);        /* the controller does not halt with events waiting to be taken */
			ev_pos = (ev_pos + pending) % EVT_SIZE;
		}
		delay(1000);
	}
	wr(GEVNTSIZ, GEVNTSIZ_INTMASK | rd(GEVNTSIZ));
	return (rd(DSTS) & DSTS_DEVCTRLHLT) != 0;
}

static int
dma_alloc(void)
{
	vm_offset_t va = 0;
	/* Any address: the 9810's DWC3 takes 64-bit TRB and event-buffer pointers, and RAM sits above 4 GB
	 * (0x880000000 seen on the S9). */

	if (kmem_alloc_contig(kernel_map, &va, round_page(DMA_SIZE), PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	FlushPoC_DcacheRegion(va, round_page(DMA_SIZE));
	dma_pa = kvtophys(va);
	dma = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)dma_pa, round_page(DMA_SIZE), VM_WIMG_WCOMB);
	if (dma == NULL) {
		return ENOMEM;
	}
	for (uint32_t i = 0; i < DMA_SIZE; i += 4) {
		*(volatile uint32_t *)(dma + i) = 0;
	}
	return 0;
}

/* step 3: the core reset and set up as a device, the pull-up still off */
static int
core_init(void)
{
	uint32_t id = rd(GSNPSID), v;
	int us;

	if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u) {
		say("usb: 3: GSNPSID %08x is not a DWC3's: stopping\n", id);
		return ENODEV;
	}
	say("usb: 3: device soft reset (DCTL CSFTRST)\n");
	wr(DCTL, DCTL_CSFTRST);
	for (us = 0; us < 500000 && (rd(DCTL) & DCTL_CSFTRST); us += 10) {
		delay(10);
	}
	if (rd(DCTL) & DCTL_CSFTRST) {
		say("usb: 3: the soft reset did not finish in 500 ms (DCTL %08x): the PHY's clock is not there? stopping\n", rd(DCTL));
		return ETIMEDOUT;
	}
	say("usb: 3: reset done in %d us\n", us);

	/* the 2.0 PHY interface: dwc3_phy_setup (core.c:637-765); the dt's snps,dis-u2-freeclk-exists-quirk (core.c:761) */
	v = rd(GUSB2PHYCFG) & ~(GUSB2_U2_FREECLK_EXISTS | GUSB2_ENBLSLPM | GUSB2_SUSPHY);
	if (susphy) {
		v |= GUSB2_SUSPHY;
	}
	wr(GUSB2PHYCFG, v);

	/* GCTL: device mode; SOFITPSYNC for the same quirk (core.c:921) */
	v = rd(GCTL) & ~(GCTL_SCALEDOWN_MASK | GCTL_PRTCAPDIR_MASK);
	wr(GCTL, v | GCTL_PRTCAP_DEVICE | GCTL_SOFITPSYNC);

	/* the bus: dwc3_core_config (core.c:140-186) */
	v = rd(GSBUSCFG0) | GSBUSCFG0_INCRBRSTEN | GSBUSCFG0_INCR16BRSTEN;
	if ((id & 0xffff0000u) == 0x33310000u || id >= 0x5533220au) {
		v |= GSBUSCFG0_REQINFO;
	}
	wr(GSBUSCFG0, v);
	wr(GSBUSCFG1, rd(GSBUSCFG1) | GSBUSCFG1_BREQLIMIT(3));
	wr(GUCTL, (rd(GUCTL) & ~GUCTL_REFCLKPER_MASK) | GUCTL_USBHSTINAUTORETRYEN | GUCTL_REFCLKPER(0x14));

	/* the event buffer (core.c dwc3_event_buffers_setup, 476-495) */
	ev_pos = 0;
	wr(GEVNTADRLO, (uint32_t)(dma_pa + EVT_OFF));
	wr(GEVNTADRHI, (uint32_t)((dma_pa + EVT_OFF) >> 32));
	wr(GEVNTSIZ, EVT_SIZE);
	wr(GEVNTCOUNT, 0);

	/* DCFG: the speed, address 0, no LPM (bcdUSB 2.00) */
	v = rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK | DCFG_LPM_CAP);
	wr(DCFG, v | (full_speed ? DCFG_FULLSPEED : DCFG_HIGHSPEED));
	wr(DEVTEN, DEVTEN_DISCONNEVTEN | DEVTEN_USBRSTEN | DEVTEN_CONNECTDONEEN | DEVTEN_ULSTCNGEN);

	/* endpoint 0 (gadget.c __dwc3_gadget_ep_enable -> dwc3_gadget_start_config, 533-562) */
	if (depcmd(0, CMD_DEPSTARTCFG, 0, 0, 0, NULL) != 0) {
		return EIO;
	}
	for (int p = 0; p < NPEPS; p++) {
		if (depcmd(p, CMD_SETTRANSFRESOURCE, DEPXFERCFG_NUM_XFER_RES(1), 0, 0, NULL) != 0) {
			return EIO;
		}
	}
	if (ep_config(0, 0, USBACM_EP0_MPS, DEPCFG_ACTION_INIT, 0) != 0 || ep_config(1, 0, USBACM_EP0_MPS, DEPCFG_ACTION_INIT, 0) != 0) {
		return EIO;
	}
	wr(DALEPENA, 3);
	usbacm_init(&acm);
	ep0_setup_start();
	if (ep0_state != EP0_SETUP) {
		return EIO;
	}
	say_dwc3("set up");
	return 0;
}

static int
run(void)
{
	uint32_t v = rd(DCTL) & ~(DCTL_KEEP_CONNECT | DCTL_U1U2_MASK);

	wr(DCTL, v | DCTL_RUN_STOP);
	for (int i = 0; i < 100; i++) {
		if (!(rd(DSTS) & DSTS_DEVCTRLHLT)) {
			return 0;
		}
		delay(1000);
	}
	say("usb: 4: still halted 100 ms after RUN_STOP (DSTS %08x)\n", rd(DSTS));
	return ETIMEDOUT;
}

static bool
bring_up(void)
{
	int e;

	say("usb: steps 1-%u (1 look, 2 PHY, 3 core, 4 enumerate, 5 log and console) on %s's glue\n", steps, glue->name);
	say("usb: 1: reading what the boot left\n");
	bool readable = glue->look(say, glue_flags);
	regs = ml_io_map((vm_offset_t)glue->dwc3_pa, glue->dwc3_size);
	if (regs == 0) {
		say("usb: could not map the DWC3 at %#llx; off\n", glue->dwc3_pa);
		return false;
	}
	if (readable) {
		say_dwc3("as the boot left it");
	}
	stage = 1;
	if (steps < 2) {
		return false;
	}

	if (readable && (rd(DCTL) & DCTL_RUN_STOP)) {
		say("usb: 2: the controller was left running (DCTL %08x): stopping it first\n", rd(DCTL));
		say("usb: 2: %s\n", stop_controller() ? "halted" : "NOT halted after 100 ms; going on");
	}
	say("usb: 2: the PHY (%s)\n", glue->name);
	if ((e = glue->phy_on(say, glue_flags)) != 0) {
		say("usb: 2: the PHY did not come up (%d); off\n", e);
		return false;
	}
	stage = 2;
	set_state("phy");
	if (steps < 3) {
		return false;
	}

	say("usb: 3: memory for the controller (32 KB, uncached)\n");
	if (dma_alloc() != 0) {
		say("usb: 3: no memory for the controller; off\n");
		return false;
	}
	say("usb: 3: at %#llx; resetting the core and setting it up as a device (%s speed), pull-up off\n", dma_pa,
	    full_speed ? "full" : "high");
	if ((e = core_init()) != 0) {
		say("usb: 3: failed (%d); off\n", e);
		return false;
	}
	stage = 3;
	set_state("core");
	if (steps < 4) {
		return false;
	}

	if (steps >= 5) {
		ring = kalloc_data(RING_SIZE, Z_WAITOK | Z_ZERO);
		if (ring == NULL) {
			say("usb: 5: no memory for the log's ring; going on without it (step 4)\n");
			steps = 4;
		} else {
			say("usb: 5: the kernel's log goes out on bulk IN from its first line; lines from the host to the console\n");
			(void)loader_log_tap_set(tap, history);
		}
	}
	say("usb: 4: pull-up on (DCTL RUN_STOP): a host should see %04x:%04x \"%s\"\n", USBACM_VID, USBACM_PID, "HakiOS kernel log and console");
	if (run() != 0) {
		return false;
	}
	stage = steps >= 5 ? 5 : 4;
	set_state("waiting");
	return true;
}

static int
say_nothing(__unused const char *format, ...)
{
	return 0;
}

/* Take the link down: controller stopped, transfers ended, a picture being sent dropped. */
static void
link_down(void)
{
	int phase = os_atomic_load(&shot.phase, acquire);

	for (int p = 0; p < NPEPS; p++) {
		if (p != 4) {
			end_xfer(p);
		}
	}
	(void)stop_controller();
	if (phase != SHOT_OFF && phase != SHOT_PREP) {
		os_atomic_store(&shot.phase, SHOT_OFF, release);
	}
	connected = false;
	usbacm_reset(&acm);
	memset(&cline, 0, sizeof(cline));
}

/*
 * The link up again as from the start of the run (the steps' 2, 3 and 4), in the memory it already has: what Android does
 * on every attach (dwc3/otg.c dwc3_otg_start_gadget: dwc3_phy_setup, dwc3_core_init, the pull-up). false: it stays down, for
 * the next try.
 */
static bool
link_restart(const char *why)
{
	uint64_t ns;
	bool ok = false;
	int e;

	n_restarts++;
	say_link(say, why);
	say("usb: restarting the link (%llu; %s)\n", n_restarts, why);
	link_down();
	if ((e = glue->phy_on(say_nothing, glue_flags)) != 0) {
		say("usb: the PHY did not come back (%d)\n", e);
		goto out;
	}
	for (uint32_t i = 0; i < EVT_SIZE + 0x1000u; i += 4) {
		*(volatile uint32_t *)(dma + i) = 0;    /* the event buffer and the TRBs */
	}
	memset(pep, 0, sizeof(pep));
	if ((e = core_init()) != 0) {
		say("usb: the core did not come back (%d)\n", e);
		goto out;
	}
	if (run() != 0) {
		goto out;
	}
	ok = true;
out:
	link_stopped = false;
	resets_no_setup = ok ? 0 : STORM_NO_SETUP;      /* a failed one is tried again by the check for storms */
	resets_no_config = 0;
	say_resets = 16;
	say_setups = 16;
	last_phy_say = 0;
	set_state(ok ? "waiting" : "stuck");
	nanoseconds_to_absolutetime((uint64_t)restart_wait * NSEC_PER_SEC, &ns);
	restart_after = mach_absolute_time() + ns;
	hold_log();
	last_heard = mach_absolute_time();
	restart_wait = restart_wait * 2 > RESTART_WAIT_MOST ? RESTART_WAIT_MOST : restart_wait * 2;
	return ok;
}

/* from hakios.max77705.c, every read of VBUS (a low only when it has stayed low): the poll does what follows from it */
void
hakios_usb_gadget_vbus(bool present)
{
	os_atomic_store(&vbus_heard_at, mach_absolute_time(), relaxed);
	os_atomic_store(&vbus_report, present ? 1 : 0, release);
}

/* From the cable and the link's state, decide whether to stop or restart the link. Runs in the poll. */
static void
link_watch(void)
{
	int vbus = os_atomic_load(&vbus_report, acquire);
	uint64_t now = mach_absolute_time(), ns;
	uint32_t sof = link_stopped ? last_sof : (rd(DSTS) >> 3) & 0x3fffu;     /* [16:3], SOFFN */

	if (vbus != VBUS_UNKNOWN && vbus != vbus_seen) {
		int was = vbus_seen;

		vbus_seen = vbus;
		if (vbus == 0 && was == 1) {
			say("usb: VBUS gone: the cable is out; the link down until it is back, the log kept\n");
			say_link(say, "at the unplug");
			link_down();
			if (glue->phy_off != NULL) {
				glue->phy_off();
			}
			link_stopped = true;
			set_state("unplugged");
			return;
		}
		if (vbus == 1 && was == 0) {
			say("usb: VBUS back: a cable in\n");
			quiet_restarts = 0;
			if (link_stopped || acm.config == 0) {
				restart_wait = RESTART_WAIT_FIRST;
				(void)link_restart("VBUS back");
				return;
			}
		}
	}
	if (want_restart) {
		want_restart = false;
		(void)link_restart("asked on the console");
		return;
	}
	if (want_drop_ms != 0) {
		uint32_t ms = want_drop_ms;

		want_drop_ms = 0;
		say("usb: the pull-up off for %u ms\n", ms);
		wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);
		IOSleep(ms);
		wr(DCTL, rd(DCTL) | DCTL_RUN_STOP);
		say("usb: the pull-up on again\n");
		return;
	}
	if (link_stopped) {
		absolutetime_to_nanoseconds(now - os_atomic_load(&vbus_heard_at, relaxed), &ns);
		if (ns > (uint64_t)VBUS_SILENT_SECONDS * NSEC_PER_SEC) {
			(void)link_restart("nothing heard of VBUS");
		}
		return;
	}
	if (watch_storms && (resets_no_setup >= STORM_NO_SETUP || resets_no_config >= STORM_NO_CONFIG) && now >= restart_after) {
		(void)link_restart(resets_no_setup >= STORM_NO_SETUP ? "bus resets, no SETUP" : "bus resets, no configuration");
		return;
	}
	if (sof != last_sof || last_heard == 0) {
		last_sof = sof;
		last_heard = now;
	}
	if (watch_storms && vbus_seen == 1 && quiet_restarts < QUIET_RESTARTS && now >= restart_after) {
		absolutetime_to_nanoseconds(now - last_heard, &ns);
		if (ns > (uint64_t)QUIET_SECONDS * NSEC_PER_SEC) {
			quiet_restarts++;
			hakios_max77705_usb_close();
			(void)link_restart("VBUS there, no host talking");
		}
	}
}

static void
tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (!os_atomic_cmpxchg(&busy, 0, 1, acquire)) {
		goto again;
	}
	if (!started) {
		started = true;
		if (!bring_up()) {
			os_atomic_store(&busy, 0, release);
			return;
		}
	}
	if (stage >= 4) {
		link_watch();
	}
	if (!link_stopped) {
		events();
		pump_in();
	}
	if (!connected && !link_stopped && glue->phy_state != NULL) {
		uint64_t now = mach_absolute_time(), ns;

		absolutetime_to_nanoseconds(now - last_phy_say, &ns);
		if (last_phy_say == 0 || ns > 10 * NSEC_PER_SEC) {
			char st[120];

			glue->phy_state(st, sizeof(st));
			say("usb: waiting for a host: %s; DSTS %08x\n", st, rd(DSTS));
			last_phy_say = now;
		}
	}
	os_atomic_store(&busy, 0, release);
	if (want_wifi_report) {
		want_wifi_report = false;
		hakios_wifi_report(true);
	}
	if (want_return) {
		return_check();
	}
again:
	clock_interval_to_deadline(connected ? poll_ms : 20, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

/*
 * On the way back to the loader (loader_return(), the other processors already stopped, the panic text appended): what
 * is left of the log sent while the host takes it, for up to usb_gadget_drain ms; then the pull-up off, the events
 * masked, the PHY powered down. Nothing of this kernel's memory is used by the controller after it.
 */
void
hakios_usb_gadget_quiesce(void)
{
	if (stage < 3 || regs == 0) {
		return;
	}
	for (int i = 0; i < 1000 && !os_atomic_cmpxchg(&busy, 0, 1, acquire); i++) {
		delay(100);     /* the poll is mid-way; after 100 ms it is taken to be gone with its processor */
	}
	hold_log_until = 0;
	if (stage >= 5 && acm.config != 0) {
		for (uint32_t ms = 0; ms < drain_ms; ms++) {
			events();
			pump_in();
			if (!pep[PEP_IN].active && os_atomic_load(&ring_head, acquire) == ring_tail) {
				break;
			}
			delay(1000);
		}
	}
	(void)loader_log_tap_set(NULL, NULL);
	bulk_down();
	(void)stop_controller();
	if (glue->phy_off != NULL) {
		glue->phy_off();
	}
	stage = 0;
}

__startup_func
static void
usb_gadget_init(void)
{
	char soc[32] = "exynos9810";
	uint32_t value = 0;

	if (!PE_parse_boot_argn("usb_gadget", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	steps = steps > 5 ? 5 : steps;
	(void)PE_parse_boot_argn("usb_gadget_soc", soc, sizeof(soc));
	if (PE_parse_boot_argn("usb_gadget_clk", &value, sizeof(value)) && value) {
		glue_flags |= USB_GLUE_CLK;
	}
	if (PE_parse_boot_argn("usb_gadget_ldo", &value, sizeof(value)) && value) {
		glue_flags |= USB_GLUE_LDO;
	}
	(void)PE_parse_boot_argn("usb_gadget_fs", &full_speed, sizeof(full_speed));
	(void)PE_parse_boot_argn("usb_gadget_susphy", &susphy, sizeof(susphy));
	(void)PE_parse_boot_argn("usb_gadget_crlf", &crlf, sizeof(crlf));
	if (PE_parse_boot_argn("usb_gadget_poll", &value, sizeof(value)) && value >= 1 && value <= 50) {
		poll_ms = value;
	}
	if (PE_parse_boot_argn("usb_gadget_drain", &value, sizeof(value)) && value <= 5000) {
		drain_ms = value;
	}
	if (strcmp(soc, "exynos9810") == 0) {
		glue = hakios_usb_glue_exynos9810();
	}
	if (glue == NULL) {
		say("usb: usb_gadget=%u, but no glue for \"%s\" in this kernel; off\n", steps, soc);
		return;
	}
	/*
	 * The poll uses the kernel-high group, not the group thread_call_allocate() gives ("high"). That group runs its calls
	 * one at a time (thread_call.c: TCG_NONE), and most drivers' periodic calls are in it, so a long one there would hold
	 * up the console and the log. kernel-high holds the IOKit timers that ask for it and little else. The end of the run
	 * goes to the "high" group, where the timed return (loader_return_after=) runs it; if it waits there, the poll can
	 * force the return (return_check).
	 */
	call = thread_call_allocate_with_priority(tick, NULL, THREAD_CALL_PRIORITY_KERNEL_HIGH);
	return_call = thread_call_allocate(end_the_run, NULL);
	thread_call_enter(call);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, usb_gadget_init);
