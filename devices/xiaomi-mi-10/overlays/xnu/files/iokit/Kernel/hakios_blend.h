/*
 * The framebuffer's pixel arithmetic, and the rules a frame whose bottom layer is not given is composed by,
 * kept out of MiniIOMobileFramebuffer.cpp so that a host test can build and check them
 * (devices/xiaomi-mi-10/tools/test-display-blend.py; work/v29i/INVESTIGATION-alpha-black.md §1, §4, §6).
 *
 * Pure arithmetic on 32-bit pixels -- BGRA in memory, so a word is A<<24 | R<<16 | G<<8 | B -- and pure
 * decisions on the numbers a swap carries. No IOKit, no machine words, no locks: the kernel file includes
 * this, and nothing here reaches back into it.
 */
#ifndef _IOKIT_HAKIOS_BLEND_H
#define _IOKIT_HAKIOS_BLEND_H

#include <stdint.h>

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
 * How much of `d` an alpha of `s` keeps, of 256: 256 when s is wholly transparent, 0 when it is opaque (the + >> 7
 * rounds 255/256 to 1 so that a transparent s keeps all of d and an opaque one keeps none).
 */
static inline uint32_t
hakios_keep(uint32_t s)
{
	uint32_t k = 255 - (s >> 24);

	return k + (k >> 7);
}

/*
 * `s` over `d` with the fraction of `d` kept already worked out (`k`, from hakios_keep()): two of the four channels
 * at a time in one word, as blend() has them. Two pixels of a row share a k, which is what lets over_pair() do
 * both of them in one step.
 */
static inline uint32_t
over_keep(uint32_t s, uint32_t d, uint32_t k)
{
	return ((s & 0xff00ff) + ((d & 0xff00ff) * k >> 8 & 0xff00ff)) |
	       ((s >> 8 & 0xff00ff) + ((d >> 8 & 0xff00ff) * k >> 8 & 0xff00ff)) << 8;
}

/*
 * `s` over `d`, premultiplied as CoreAnimation's surfaces are: d is kept by 255 - (s's alpha) of 255 (in 256ths).
 * The alpha channel is composited the same way (s's alpha, plus d's kept by the same fraction).
 */
static inline uint32_t
over(uint32_t s, uint32_t d)
{
	return over_keep(s, d, hakios_keep(s));
}

/*
 * Two pixels of one row over two of another, both with the same k: the arithmetic of over_keep(), for both pixels,
 * in 64-bit words -- the two pixels' channels packed into 16-bit lanes (8 bits of headroom for a byte times 256,
 * so no lane can carry into the next), so that one 64-bit multiply is the work of the four 32-bit ones, and the
 * additions and shifts afterwards are the ones over_keep() does, bit for bit. The two pixels come back packed:
 * the first at the bottom 32 bits, the second at the top.
 */
static inline uint64_t
over_pair(uint32_t s0, uint32_t s1, uint32_t d0, uint32_t d1, uint32_t k)
{
	uint64_t sr = (s0 & 0xff00ff) | ((uint64_t) (s1 & 0xff00ff) << 32);
	uint64_t dr = (d0 & 0xff00ff) | ((uint64_t) (d1 & 0xff00ff) << 32);
	uint64_t sg = (s0 >> 8 & 0xff00ff) | ((uint64_t) (s1 >> 8 & 0xff00ff) << 32);
	uint64_t dg = (d0 >> 8 & 0xff00ff) | ((uint64_t) (d1 >> 8 & 0xff00ff) << 32);
	uint64_t br = (dr * k >> 8) & 0x00ff00ff00ff00ffULL;
	uint64_t bg = (dg * k >> 8) & 0x00ff00ff00ff00ffULL;
	uint32_t r0 = (uint32_t) sr + (uint32_t) br, g0 = (uint32_t) sg + (uint32_t) bg;
	uint32_t r1 = (uint32_t) (sr >> 32) + (uint32_t) (br >> 32);
	uint32_t g1 = (uint32_t) (sg >> 32) + (uint32_t) (bg >> 32);

	return (uint64_t) (r0 | g0 << 8) | ((uint64_t) (r1 | g1 << 8) << 32);
}

/*
 * `n` pixels of a layer blended over `n` pixels of the frame, as over() would do them one at a time -- the whole
 * row of a layer that is the panel's width, which on a full screen is 2.7 million pixels of the frame's time.
 * Two cases are decided for the whole row first, both of them common in CoreAnimation's surfaces: every pixel
 * transparent (nothing of the row is there, the frame stands, and none of it is read) and every pixel opaque (the
 * row is the frame, exactly -- over() keeps no fraction of d at all). The rest goes two pixels at a time when
 * their alphas agree (over_pair()), one when they do not, and the two alphas that need no arithmetic -- 0 and 255
 * -- are taken as such, a pixel under nothing being the one it is under (that one only where a transparent pixel
 * of the row carries no colour, which is what premultiplied means; `none_rgb` is 0 then, and the general path
 * keeps over()'s answer for a surface that is not premultiplied).
 */
static inline void
hakios_over_row(uint32_t *to, const uint32_t *from, int32_t n)
{
	uint32_t any = 0, all = 0xff000000u, none_rgb = 0;
	int32_t i;

	if (n <= 0) {
		return;
	}
	for (i = 0; i < n; i++) {
		uint32_t p = from[i], a = p & 0xff000000u;

		any |= a;
		all &= a;
		none_rgb |= a == 0 ? (p & 0x00ffffffu) : 0;
	}
	if (any == 0 && none_rgb == 0) {
		return;                                 /* nothing of it is there: the frame is kept, untouched */
	}
	if (all == 0xff000000u) {
		for (i = 0; i < n; i++) {
			to[i] = from[i];                /* over() of an opaque pixel is that pixel */
		}
		return;
	}
	for (i = 0; i + 1 < n; i += 2) {
		uint32_t s0 = from[i], s1 = from[i + 1], a0 = s0 >> 24, a1 = s1 >> 24;

		if (a0 == 0 && none_rgb == 0) {
			if (a1 != 0) {
				to[i + 1] = a1 == 255 ? s1 : over(s1, to[i + 1]);
			}
		} else if (a1 == 0 && none_rgb == 0) {
			to[i] = a0 == 255 ? s0 : over(s0, to[i]);
		} else if (a0 == 255) {
			to[i] = s0;
			to[i + 1] = a1 == 255 ? s1 : over(s1, to[i + 1]);
		} else if (a1 == 255) {
			to[i] = over(s0, to[i]);
			to[i + 1] = s1;
		} else if (a0 == a1) {
			uint64_t both = over_pair(s0, s1, to[i], to[i + 1], hakios_keep(s0));

			to[i] = (uint32_t) both;
			to[i + 1] = (uint32_t) (both >> 32);
		} else {
			to[i] = over(s0, to[i]);
			to[i + 1] = over(s1, to[i + 1]);
		}
	}
	if (i < n) {
		to[i] = over(from[i], to[i]);
	}
}

/*
 * A layer's rectangle on the display -- a swap's destination, in display coordinates -- covers the whole of it,
 * or it is given no size at all, which drawLayer() takes as the whole display. The same test decides what a
 * frame is drawn over and what may be kept as the bottom picture the next swap is composed over.
 */
static inline bool
hakios_covers_display(const int32_t * dst, int32_t width, int32_t height)
{
	return dst[2] <= 0 || dst[3] <= 0 ||
	    (dst[0] <= 0 && dst[1] <= 0 && (int64_t) dst[0] + dst[2] >= width && (int64_t) dst[1] + dst[3] >= height);
}

/*
 * What a frame whose bottom layer is not given is composed from.
 *
 * Such a swap (Maps: its map surface is not given while a card is dragged up, and CoreAnimation's own layers --
 * a half-transparent black dimming, its shadows -- are) used to be composed over the frame before it, which
 * already holds the layers above: blending them in again takes (1 - alpha) of the picture under them away every
 * frame -- eight frames of a 50% black layer are opaque black, and wherever the layers above are transparent
 * the frame before stays in the picture as a trail (INVESTIGATION-alpha-black.md H1) -- and then over the
 * bottom layer of the last swap that had one, read where it lies, which tore while the GPU drew the next map
 * frame into the same surface (REVIEW-layer0-replay.md F1). It is composed over a copy of that bottom layer
 * now, taken while the frame held it and nothing above it (stageBottom()), or over black when no such copy
 * was taken. Never over the frame's own last output.
 *
 * staged: a copy of the bottom picture is there to be copied into the frame.
 */
#define HAKIOS_BASE_CLEAR       0       /* nothing to replay: the frame is cleared to black (as it was before v29i) */
#define HAKIOS_BASE_BOTTOM      1       /* the staged bottom picture, straight copied into the frame first */

static inline int
hakios_upper_only_base(bool staged)
{
	return staged ? HAKIOS_BASE_BOTTOM : HAKIOS_BASE_CLEAR;
}

/*
 * Whether the frame just given its bottom layer is to be kept as the bottom picture.
 *
 * covers: that layer was drawn over the whole display, so the frame is that layer and nothing else; from_staged:
 * the frame was started from the bottom picture (it was given this swap's bottom layer over part of the
 * display only), so it is still a bottom picture -- that one, with this layer's part of it brought up to date.
 * Neither: the frame holds the layer before it too, which is no bottom to replay.
 */
static inline bool
hakios_stage_bottom(bool covers, bool from_staged)
{
	return covers || from_staged;
}

/*
 * A number of a swap's, as hakios_tl() prints it: the rectangles and the surfaces' own sizes are the client's,
 * all 32 bits of each, and a line is 192 bytes. Clamped here, so that the line keeps the fields after this one
 * (a rectangle of a billion is no rectangle of a display either); takes signed and unsigned both.
 */
#define HAKIOS_PRINT_LIMIT      99999

static inline int32_t
hakios_print_num(int64_t v)
{
	return (int32_t) (v > HAKIOS_PRINT_LIMIT ? HAKIOS_PRINT_LIMIT :
	    v < -HAKIOS_PRINT_LIMIT ? -HAKIOS_PRINT_LIMIT : v);
}

#endif /* _IOKIT_HAKIOS_BLEND_H */
