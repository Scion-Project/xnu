/*
 * scion_splash.h: Scion's boot screen, the drawing that the loader (xnuloader/loader.c) and the kernel
 * (osfmk/console/scion_splash.c) share. Plain C that needs nothing but uint8_t/uint32_t/uint64_t, so that it
 * builds as the freestanding UEFI loader, in the kernel and on a host (tools/scion-splash-preview.c).
 *
 * What it draws (boot-argument splash=1; docs/boot-screen.md): the dark Scion background with a soft green glow, the
 * sprout (xnuloader/scion_splash_logo.h, the loader's) centred a little above the middle, under it a thin progress
 * line and, in the kernel's part, a few dim lines of what the kernel is saying. The loader draws the first two and
 * the line up to where it is; the kernel carries on from that framebuffer. Positions come from the screen's size
 * alone (scion_splash_layout), so both agree without telling each other.
 *
 * Colours are Scion's: the logo's #14181f and #0a0c10 for the ground, its leaf gradient #2fbf71 to #9be15d for the
 * line, its chip grey #5b6b82 for the track and the text. Nothing here is Apple's.
 *
 * Copyright (c) 2026 the Scion project. Licence: APSL 2.0 / MIT as the loader and the kernel it is part of
 * (LICENSE, LICENSE.APSL).
 */
#ifndef SCION_SPLASH_H
#define SCION_SPLASH_H

#define SPLASH_TEXT_LINES       4
#define SPLASH_LOGO_HW          1457    /* the sprout's height over its width, in thousandths: 612 / 420 (the loader checks it) */

struct scion_splash {
	volatile uint8_t *base;                 /* the framebuffer (the kernel's mapping of it, or the loader's address) */
	uint32_t width, height;                 /* pixels */
	uint32_t stride;                        /* bytes in a row */
	uint32_t rgb;                           /* 1: the pixel's bytes are red, green, blue (the GOP's format 0); 0: blue, green, red */
	uint32_t logo_x, logo_y, logo_w, logo_h;        /* where the sprout goes */
	uint32_t bar_x, bar_y, bar_w, bar_h;            /* the progress line */
	uint32_t text_y, text_scale, text_pitch;        /* the lines of text: first row, the font's scale, the rows between lines */
	uint32_t text_x0, text_x1;                      /* ... and the columns they stay within */
	uint32_t drawn;                         /* permille of the line filled so far, 0: not drawn (not even its track) */
};

/* A pixel as the framebuffer wants it. */
static inline uint32_t
scion_splash_pixel(const struct scion_splash *s, uint32_t r, uint32_t g, uint32_t b)
{
	return s->rgb ? (r | g << 8 | b << 16) : (b | g << 8 | r << 16);
}

/* logo_hw: the sprout's height over its width, in thousandths (SPLASH_LOGO_HW). */
static inline void
scion_splash_layout(struct scion_splash *s, volatile uint8_t *base, uint32_t width, uint32_t height, uint32_t stride,
    uint32_t rgb, uint32_t logo_hw)
{
	s->base = base;
	s->width = width;
	s->height = height;
	s->stride = stride;
	s->rgb = rgb;
	s->drawn = 0;
	/* the sprout: a third of the width, but never taller than a quarter of the height */
	s->logo_w = width * 34 / 100;
	if (s->logo_w * logo_hw / 1000 > height / 4) {
		s->logo_w = (height / 4) * 1000 / logo_hw;
	}
	s->logo_h = s->logo_w * logo_hw / 1000;
	s->logo_x = (width - s->logo_w) / 2;
	s->logo_y = height * 40 / 100 - s->logo_h / 2;
	s->bar_w = width * 30 / 100;
	s->bar_h = width / 240 > 4 ? width / 240 : 4;
	s->bar_x = (width - s->bar_w) / 2;
	s->bar_y = s->logo_y + s->logo_h + height * 45 / 1000;
	s->text_scale = (width + 360) / 720;
	s->text_pitch = 22 * s->text_scale;
	s->text_y = s->bar_y + s->bar_h + height * 30 / 1000;
	s->text_x0 = width * 7 / 100;
	s->text_x1 = width - s->text_x0;
}

static inline volatile uint32_t *
scion_splash_at(const struct scion_splash *s, uint32_t x, uint32_t y)
{
	return (volatile uint32_t *)(s->base + (uint64_t)y * s->stride + (uint64_t)x * 4);
}

/*
 * The ground of a rectangle of the screen: #14181f at the top to #0a0c10 at the bottom, and a green glow round where the
 * sprout is (the glow is (1 - d^2/R^2)^2 of 16 % of the leaf green, R 60 % of the width). The kernel repaints the part
 * under its lines of text with this, so that what it takes away is exactly what the loader put there.
 */
static inline void
scion_splash_ground(const struct scion_splash *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	const int32_t cx = (int32_t)(s->logo_x + s->logo_w / 2), cy = (int32_t)(s->logo_y + s->logo_h / 2);
	const int64_t rad = (int64_t)s->width * 60 / 100;
	const int64_t r2 = rad * rad, inv = ((int64_t)1 << 32) / r2;

	if (x1 > s->width) {
		x1 = s->width;
	}
	if (y1 > s->height) {
		y1 = s->height;
	}
	for (uint32_t y = y0; y < y1; y++) {
		const uint32_t r0 = 0x14 - (0x14 - 0x0a) * y / s->height, g0 = 0x18 - (0x18 - 0x0c) * y / s->height,
		    b0 = 0x1f - (0x1f - 0x10) * y / s->height;
		const int64_t dy = (int64_t)y - cy;
		volatile uint32_t *row = scion_splash_at(s, 0, y);

		for (uint32_t x = x0; x < x1; x++) {
			const int64_t dx = (int64_t)x - cx, d2 = dx * dx + dy * dy;
			uint32_t r = r0, g = g0, b = b0;

			if (d2 < r2) {
				const uint64_t t = (uint64_t)(r2 - d2) * (uint64_t)inv >> 24;   /* 0..256 */
				const uint64_t a = (t * t >> 8) * 40 >> 8;                      /* 0..40 of 256 */

				r = r0 + (uint32_t)(((int64_t)(0x2f - (int32_t)r0) * (int64_t)a) >> 8);
				g = g0 + (uint32_t)(((int64_t)(0xbf - (int32_t)g0) * (int64_t)a) >> 8);
				b = b0 + (uint32_t)(((int64_t)(0x71 - (int32_t)b0) * (int64_t)a) >> 8);
			}
			row[x] = scion_splash_pixel(s, r, g, b);
		}
	}
}

static inline void
scion_splash_background(struct scion_splash *s)
{
	scion_splash_ground(s, 0, 0, s->width, s->height);
	s->drawn = 0;
}

/* Is (x, y), relative to the line's own corner, inside the line's capsule (its ends are half circles)? */
static inline int
scion_splash_capsule(const struct scion_splash *s, uint32_t x, uint32_t y)
{
	const int64_t h = s->bar_h, r2x = h / 2 * 2;    /* all in half pixels: the cap's centre is r in from the end */
	const int64_t px = 2 * (int64_t)x + 1, py = 2 * (int64_t)y + 1 - h;
	int64_t dx = 0;

	if (px < r2x) {
		dx = r2x - px;
	} else if (px > 2 * (int64_t)s->bar_w - r2x) {
		dx = px - (2 * (int64_t)s->bar_w - r2x);
	}
	return dx * dx + py * py <= h * h;
}

/*
 * The progress line, filled up to `permille` (0..1000). The first call draws the track as well. It only ever
 * moves forward: a smaller value than the last is ignored. The fill runs from #2fbf71 to #9be15d along the line.
 */
static inline void
scion_splash_bar(struct scion_splash *s, uint32_t permille)
{
	const uint32_t track = scion_splash_pixel(s, 0x1c, 0x22, 0x2c);
	uint32_t from = s->drawn, to;

	if (permille > 1000) {
		permille = 1000;
	}
	to = s->bar_w * permille / 1000;
	if (s->drawn == 0) {
		for (uint32_t y = 0; y < s->bar_h; y++) {
			for (uint32_t x = 0; x < s->bar_w; x++) {
				if (scion_splash_capsule(s, x, y)) {
					*scion_splash_at(s, s->bar_x + x, s->bar_y + y) = track;
				}
			}
		}
		from = 0;
		s->drawn = 1;
	} else {
		from = s->bar_w * (s->drawn - 1) / 1000;
		if (permille + 1 <= s->drawn) {
			return;
		}
	}
	for (uint32_t x = from; x < to; x++) {
		const uint32_t k = x * 256 / s->bar_w;
		const uint32_t v = scion_splash_pixel(s, 0x2f + (0x9b - 0x2f) * k / 256, 0xbf + (0xe1 - 0xbf) * k / 256,
		    0x71 + (0x5d - 0x71) * k / 256);

		for (uint32_t y = 0; y < s->bar_h; y++) {
			if (scion_splash_capsule(s, x, y)) {
				*scion_splash_at(s, s->bar_x + x, s->bar_y + y) = v;
			}
		}
	}
	s->drawn = permille + 1;
}

#ifdef SPLASH_LOGO_W
/*
 * The sprout, from scion_splash_logo.h (include it first): unpacked once into a coverage plane and a palette-index
 * plane, then scaled to s->logo_w by s->logo_h with a bilinear sample (coverage-weighted for colour) and blended
 * over what is on the screen (scion_splash_background's ground).
 */
static uint8_t splash_logo_cov[SPLASH_LOGO_W * SPLASH_LOGO_H];
static uint8_t splash_logo_idx[SPLASH_LOGO_W * SPLASH_LOGO_H];

static inline void
scion_splash_logo(const struct scion_splash *s)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i + 2 < sizeof(splash_logo_runs) && n < SPLASH_LOGO_W * SPLASH_LOGO_H; i += 3) {
		for (uint32_t k = splash_logo_runs[i]; k != 0 && n < SPLASH_LOGO_W * SPLASH_LOGO_H; k--, n++) {
			splash_logo_cov[n] = splash_logo_runs[i + 1];
			splash_logo_idx[n] = splash_logo_runs[i + 2];
		}
	}
	for (uint32_t dy = 0; dy < s->logo_h; dy++) {
		const int64_t sy = (int64_t)(2 * dy + 1) * SPLASH_LOGO_H * 128 / s->logo_h - 128;   /* 1/256 of a source pixel */
		int32_t y0 = (int32_t)(sy >> 8), fy = (int32_t)(sy & 255);

		for (uint32_t dx = 0; dx < s->logo_w; dx++) {
			const int64_t sx = (int64_t)(2 * dx + 1) * SPLASH_LOGO_W * 128 / s->logo_w - 128;
			int32_t x0 = (int32_t)(sx >> 8), fx = (int32_t)(sx & 255);
			uint32_t cov = 0, wr = 0, wg = 0, wb = 0;

			for (int32_t j = 0; j < 2; j++) {
				for (int32_t i = 0; i < 2; i++) {
					const int32_t xx = x0 + i, yy = y0 + j;
					const uint32_t wgt = (uint32_t)((i ? fx : 256 - fx) * (j ? fy : 256 - fy));     /* of 65536 */

					if (xx < 0 || yy < 0 || xx >= SPLASH_LOGO_W || yy >= SPLASH_LOGO_H) {
						continue;
					}
					const uint32_t c = splash_logo_cov[yy * SPLASH_LOGO_W + xx];
					const uint8_t *p = splash_logo_palette[splash_logo_idx[yy * SPLASH_LOGO_W + xx]];
					const uint32_t wc = (wgt >> 4) * c >> 8;        /* of 256 * 255 / 16 */

					cov += wc;
					wr += wc * p[0], wg += wc * p[1], wb += wc * p[2];
				}
			}
			if (cov == 0) {
				continue;
			}
			volatile uint32_t *d = scion_splash_at(s, s->logo_x + dx, s->logo_y + dy);
			const uint32_t cur = *d;
			const uint32_t br = s->rgb ? (cur & 255) : (cur >> 16 & 255), bg = cur >> 8 & 255, bb = s->rgb ? (cur >> 16 & 255) : (cur & 255);
			uint32_t a = cov * 16 / 255;                    /* 0..256 */
			if (a > 256) {
				a = 256;
			}
			const uint32_t r = wr / cov, g = wg / cov, b = wb / cov;

			*d = scion_splash_pixel(s, (br * (256 - a) + r * a) >> 8, (bg * (256 - a) + g * a) >> 8, (bb * (256 - a) + b * a) >> 8);
		}
	}
}
#endif /* SPLASH_LOGO_W */

#endif /* SCION_SPLASH_H */
