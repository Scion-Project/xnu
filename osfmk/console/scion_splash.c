/*
 * Scion's boot screen, the kernel's part: boot-argument splash=1 (the loader draws the sprout and starts the line;
 * docs/boot-screen.md).
 *
 * While it is on, the text the kernel sends to the screen (what -v would scroll over it) is not drawn as text: each
 * finished line goes to the few dim lines under the progress line, and the line moves with how many there have been.
 * -v, or the loader's volume-down check (hold Volume Down while the phone starts), turns it off for the run, and the
 * log scrolls as it always has. splash=0 or no splash argument: nothing here does anything.
 *
 * Progress is a guess, and says so: the loader's part is the first quarter; then the kernel's lines, `splash_lines`
 * of them (default 900) take it to 95 %; from the start of IOKit a timer lets it creep on to 99 % while the system
 * starts, and the first frame the display driver shows ends it (scion_splash_end). A boot that ends some other way
 * leaves the line where it was.
 *
 * The drawing is scion_splash.h's, the loader's own. The font is the console's (iso_font).
 */
#include <mach/mach_types.h>
#include <kern/kern_types.h>
#include <kern/thread_call.h>
#include <kern/locks.h>
#include <kern/simple_lock.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include <stdint.h>
#include <string.h>

#include "video_console.h"
#include "scion_splash.h"

#define LINE_MAX_CHARS  120
#define BASE_PERMILLE   250     /* the loader's share */
#define KERNEL_PERMILLE 950     /* where the kernel's lines end */
#define CREEP_PERMILLE  990     /* where the timer ends */
#define TICK_MS         250
#define LIMIT_TICKS     (240 * 1000 / TICK_MS)

extern struct vc_info vinfo;
extern unsigned char iso_font[256 * 16];

static struct scion_splash splash;
static boolean_t splash_on;             /* the kernel's text is taken */
static boolean_t splash_drawing;        /* ... and drawn */
static uint32_t splash_expected = 900;
static uint32_t splash_lines;
static uint32_t splash_permille = BASE_PERMILLE;
static char splash_text[SPLASH_TEXT_LINES][LINE_MAX_CHARS + 1];
static uint32_t splash_text_n;          /* lines held (to SPLASH_TEXT_LINES) */
static char splash_cur[LINE_MAX_CHARS + 1];
static uint32_t splash_cur_n;
static uint32_t splash_ticks;
static thread_call_t splash_call;
decl_simple_lock_data(static, splash_lock);

static void
splash_geometry(void)
{
	const uint32_t saved = splash.drawn;

	scion_splash_layout(&splash, (volatile uint8_t *)vinfo.v_baseaddr, vinfo.v_width, vinfo.v_height, vinfo.v_rowbytes,
	    0, SPLASH_LOGO_HW);
	splash.drawn = saved;
}

/* One glyph of the console font, in `scale` times its size; set pixels only, in colour v. */
static void
splash_glyph(uint32_t x, uint32_t y, unsigned char ch, uint32_t scale, uint32_t v)
{
	const unsigned char *g = iso_font + ch * 16;

	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t col = 0; col < 8; col++) {
			if (!(g[row] & (1u << col))) {
				continue;
			}
			for (uint32_t dy = 0; dy < scale; dy++) {
				for (uint32_t dx = 0; dx < scale; dx++) {
					*scion_splash_at(&splash, x + col * scale + dx, y + row * scale + dy) = v;
				}
			}
		}
	}
}

static void
splash_draw_text(void)
{
	/* the newest line brightest: 100, 60, 40, 25 % of the text colour #8a97aa over the ground (about #0c0f14) */
	static const uint32_t fade[SPLASH_TEXT_LINES] = { 256, 154, 102, 64 };
	const uint32_t glyph_w = 8 * splash.text_scale;

	scion_splash_ground(&splash, splash.text_x0, splash.text_y, splash.text_x1,
	    splash.text_y + SPLASH_TEXT_LINES * splash.text_pitch);
	for (uint32_t k = 0; k < splash_text_n; k++) {
		const char *t = splash_text[(splash_lines - 1 - k) % SPLASH_TEXT_LINES];
		uint32_t n = (uint32_t)strlen(t), room = (splash.text_x1 - splash.text_x0) / glyph_w;
		const uint32_t f = fade[k];
		const uint32_t v = scion_splash_pixel(&splash, 0x0c + (0x8a - 0x0c) * f / 256, 0x0f + (0x97 - 0x0f) * f / 256,
		    0x14 + (0xaa - 0x14) * f / 256);

		if (n > room) {
			n = room;
		}
		const uint32_t x0 = (splash.width - n * glyph_w) / 2, y = splash.text_y + k * splash.text_pitch;

		for (uint32_t i = 0; i < n; i++) {
			splash_glyph(x0 + i * glyph_w, y, (unsigned char)t[i], splash.text_scale, v);
		}
	}
}

static void
splash_draw_bar(void)
{
	scion_splash_bar(&splash, splash_permille);
}

/* "-v" among the boot arguments (a flag, which the catalogue of arguments with a value does not list)? */
static boolean_t
splash_verbose_asked(void)
{
	const char *line = PE_boot_args();

	for (const char *c = line; c != NULL && *c; c++) {
		if ((c == line || c[-1] == ' ') && c[0] == '-' && c[1] == 'v' && (c[2] == ' ' || c[2] == 0)) {
			return TRUE;
		}
	}
	return FALSE;
}

/* Called once the console exists (initialize_screen), and again when the framebuffer is remapped: takes the text over if asked to. */
void
scion_splash_init(void)
{
	static boolean_t decided;
	uint32_t v = 0;

	if (!decided) {
		decided = TRUE;

		if (PE_parse_boot_argn("splash", &v, sizeof(v)) && v == 1 && !splash_verbose_asked() &&
		    vinfo.v_depth == 32 && vinfo.v_baseaddr != 0) {
			if (PE_parse_boot_argn("splash_lines", &v, sizeof(v)) && v >= 50 && v <= 100000) {
				splash_expected = v;
			}
			simple_lock_init(&splash_lock, 0);
			splash_on = splash_drawing = TRUE;
		}
	}
	if (splash_on) {
		splash_geometry();
	}
}

/* The splash has the screen: the console must not clear it (gc_enable) or put its cursor on it. */
boolean_t
scion_splash_text_taken(void)
{
	return splash_on;
}

/* A character the kernel wrote to the console: TRUE if the splash took it (so it is not drawn as text). */
boolean_t
scion_splash_putc(char c)
{
	if (!splash_on) {
		return FALSE;
	}
	if (c == '\n') {
		const boolean_t was = ml_set_interrupts_enabled(FALSE);

		simple_lock(&splash_lock, LCK_GRP_NULL);
		splash_cur[splash_cur_n] = 0;
		if (splash_cur_n != 0) {
			memcpy(splash_text[splash_lines % SPLASH_TEXT_LINES], splash_cur, splash_cur_n + 1);
			splash_lines++;
			if (splash_text_n < SPLASH_TEXT_LINES) {
				splash_text_n++;
			}
			const uint32_t span = KERNEL_PERMILLE - BASE_PERMILLE, p = BASE_PERMILLE + (uint32_t)((uint64_t)span *
			    (splash_lines < splash_expected ? splash_lines : splash_expected) / splash_expected);

			if (p > splash_permille) {
				splash_permille = p;
			}
			if (splash_drawing) {
				splash_draw_text();
				splash_draw_bar();
			}
		}
		splash_cur_n = 0;
		simple_unlock(&splash_lock);
		ml_set_interrupts_enabled(was);
	} else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
		if (splash_cur_n < LINE_MAX_CHARS) {
			splash_cur[splash_cur_n++] = c;
		}
	} else if (c == '\t' && splash_cur_n < LINE_MAX_CHARS) {
		splash_cur[splash_cur_n++] = ' ';
	}
	return TRUE;
}

/* The display driver has put its first frame on the panel: the screen is the system's. Nothing is drawn after this. */
void
scion_splash_end(void)
{
	if (!splash_on || !splash_drawing) {
		return;
	}
	const boolean_t was = ml_set_interrupts_enabled(FALSE);

	kprintf("scion_splash: over (%u lines, line at %u of 1000, %u ticks)\n", splash_lines, splash_permille, splash_ticks);
	simple_lock(&splash_lock, LCK_GRP_NULL);
	splash_drawing = FALSE;
	simple_unlock(&splash_lock);
	ml_set_interrupts_enabled(was);
	if (splash_call != NULL) {
		thread_call_cancel(splash_call);
	}
}

static void
splash_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (!splash_drawing || ++splash_ticks > LIMIT_TICKS) {
		return;
	}
	if (splash_ticks == 1) {
		kprintf("scion_splash: the timer's first tick\n");
	}
	const boolean_t was = ml_set_interrupts_enabled(FALSE);

	simple_lock(&splash_lock, LCK_GRP_NULL);
	if (splash_drawing && splash_permille < CREEP_PERMILLE) {
		/* a slow creep that never reaches the end: about 0.1 % a tick, less as it gets there */
		splash_permille += splash_permille < 970 ? 2 : 1;
		splash_draw_bar();
	}
	simple_unlock(&splash_lock);
	ml_set_interrupts_enabled(was);
	clock_interval_to_deadline(TICK_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(splash_call, deadline);
}

/* IOKit is starting (PE_init_iokit's acquire of the screen): the timer can run now. */
void
scion_splash_late(void)
{
	uint64_t deadline;

	if (!splash_on || splash_call != NULL) {
		return;
	}
	splash_call = thread_call_allocate(splash_tick, NULL);
	kprintf("scion_splash: timer %s (%u lines so far)\n", splash_call != NULL ? "started" : "NOT allocated", splash_lines);
	if (splash_call != NULL) {
		clock_interval_to_deadline(TICK_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(splash_call, deadline);
	}
}
