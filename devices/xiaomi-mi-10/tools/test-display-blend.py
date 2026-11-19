#!/usr/bin/env python3
"""Host test of the framebuffer's pixel math, of what a frame without its bottom layer is composed from, and of
where the picture that is replayed came from and goes.

Compiles the production header (iokit/Kernel/hakios_blend.h, which MiniIOMobileFramebuffer.cpp includes; every
decision is a pure function there) against asserts, checks the kernel file's use of them and its release points
by reading its text, and never accesses a phone. See work/v29i/INVESTIGATION-alpha-black.md §1.2 (the
compounding), §4 (the fix) and §6 (this test), and REVIEW-layer0-replay.md (F1: the replay is a copy of the
frame, not the surface; F2: nothing is waited for; F3: a partial layer 0 does not drop the bottom picture).
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
dev = root / 'devices/xiaomi-mi-10'
header = dev / 'overlays/xnu/files/iokit/Kernel/hakios_blend.h'
fb = (dev / 'overlays/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()


def function(source, name):
    """The body of `name(...)`, from the line the definition starts on to its closing brace."""
    start = re.search(r'^' + re.escape(name) + r'\(', source, re.M).start()
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]


def after(source, marker, chars):
    """The text within `chars` of `marker`."""
    at = source.index(marker)
    return source[at:at + chars]


def c_literal(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


# --- the kernel must use the header's rules, and nothing of the surface it was given up on (F1, F2) ----------
assert '#include "hakios_blend.h"' in fb, 'MiniIOMobileFramebuffer.cpp must include the tested header'
assert 'hakios_upper_only_base(' in fb, 'show() must ask the tested rule what a bottom-less frame starts from'
assert 'hakios_covers_display(' in fb, 'show() must ask the tested rule what covers the display'
assert 'hakios_stage_bottom(' in fb, 'show() must ask the tested rule whether a frame is kept as the bottom'
assert re.search(r'^\s*(static\s+inline\s+)?uint32_t\s+(blend|over)\s*\(', fb, re.M) is None, \
    'blend()/over() must not be defined a second time in MiniIOMobileFramebuffer.cpp'
for gone in ('fLastBottom', 'keepBottom', 'dropBottom', 'forgetBottom', 'REPLAY_FENCE_MS'):
    assert gone not in fb, ('%s must be gone: the replay reads a copy taken before the GPU could draw into the '
                            'surface again, and waits for nothing (REVIEW F1, F2)' % gone)
# (the one fence wait left is the swap's own, at the top of show(), and it is not in the bottom layer's path)
assert fb.count('MiniIOSurfaceDrawn(') == 1, 'only the swap itself is waited for; the replay is not'

# The frame is started from the staged copy before any layer above is blended over it, and taken again before
# them too (a replay after the layers above would compound like the bug this fixes).
replay_call = fb.index('(void) replayBottom();')
stage_call = fb.index('(void) stageBottom();')
layer0_draw = fb.index('layerDrawn = drawLayer(s, src, dst, layer != 0);')
assert replay_call < layer0_draw, 'the staged bottom must be drawn into the frame before the layers above'
assert fb.index('recorded = s;') < stage_call < layer0_draw + 4000, \
    'the frame must be staged where layer 0 is drawn into it, before the layers above are blended'
assert 'stageBottom()' in after(fb, 'if (layer == 0) {\n\t\t\t\trecorded = s;', 2000), \
    'staging must be in the layer-0 block, where the frame holds layer 0 alone'
assert fb.index('bottom layer back') > stage_call, \
    'the episode ends only once the layer that ends it could be staged (F5)'

# F8: a frame with layers above and no bottom layer is cleared even before the first UI frame.
assert '} else if (others != 0 || (fGraphics && (set & 1))) {' in fb, \
    'a bottom-less frame with layers above must be cleared before fGraphics too'
# F9: cleared frames are counted where they are cleared, and the window's counters only while there is a window.
assert fb.index('if (clearing) {') < fb.index('fAboveCleared++;'), 'count the clear where it happened (F9)'

# --- the release points, and that the copy is a copy (F1, F3, F7) ---------------------------------------------
assert 'dropStaged();' in function(fb, 'IOMobileFramebuffer::prepareDisplaySession'), \
    'a display session beginning lets the last session\'s bottom picture go'
assert 'forgetStaged();' in function(fb, 'IOMobileFramebufferUserClient::free'), \
    'a client going lets the bottom picture go'
assert 'forgetStaged();' in after(fb, 'if (umi_pm_screen_off())', 300), \
    'the screen going off lets the bottom picture go'
assert 'dropStaged();' in function(fb, 'IOMobileFramebuffer::dpuFallback'), \
    'a fallback that changes the size of the frame lets the copy taken at the other size go'
assert 'fStagedMemory' in function(fb, 'IOMobileFramebuffer::free'), \
    'the staged memory is released with the service (F7)'

take = function(fb, 'IOMobileFramebuffer::stageBottom')
replay = function(fb, 'IOMobileFramebuffer::replayBottom')
draw_into_staged = function(fb, 'IOMobileFramebuffer::stageLayer')
drop = function(fb, 'IOMobileFramebuffer::dropStaged')
assert 'fStagedValid = true;' in take and 'carveCopy(' in take, \
    'stageBottom must copy the frame into the staged memory and say it is there'
assert 'fStagedValid = true;' in draw_into_staged and 'drawLayer(' in draw_into_staged and 'fTarget = fStaged;' in draw_into_staged, \
    'stageLayer must draw the layer into the staged memory and say it is there'
assert 'fStagedValid && ' in replay or '!fStagedValid' in replay, 'the replay needs a staged copy to make one'
assert 'memcpy(' in replay or 'carveCopy(' in replay, 'the replay is a copy out of the staged memory'
assert 'fStagedValid = false;' in drop, 'dropping the staged copy says so'

# No per-frame log: the counters of an episode are counted, not printed; the 2 s line is printed once, in stats.
episode = after(fb, '\t\t\t\tif (others != 0) {', 1400)
assert 'hakios_tl' not in episode, 'an upper-only frame must not log (deduped, periodic only)'
assert re.findall(r'fb-bottom:', fb) and 'fb-bottom:' in function(fb, 'IOMobileFramebuffer::statsLine'), \
    'the bottom picture\'s counters belong in the periodic line'

# --- the log line's format, printed with the worst numbers a client can send (F10) ----------------------------
m = re.search(r'hakios_tl\("(fbcompose: no bottom layer[^"]*)"\s*\n\s*"([^"]*)"', fb)
assert m, 'the episode line\'s format must be there to be measured'
line_format = (m.group(1) + m.group(2)).encode().decode('unicode_escape')
assert 'dst %d,%d %dx%d' in line_format and 'src %d' not in line_format, \
    'the line prints the upper layer\'s destination, the field that says whether it covers the display'
limit = 99999                                                   # (HAKIOS_PRINT_LIMIT, in the header)
worst = line_format % (
    3, 'frame cleared to black (the staged bottom was let go)', limit, limit, limit, 'B', 'G', 'R',
    'A', limit, -limit, -limit, limit, limit)
assert len(worst) < 192, 'the episode line is %d bytes; hakios_tl\'s is 192' % len(worst)
# (and the periodic line, which is printed every 2 s with five counters of any size)
counters = re.search(r'(fb-bottom: [^"]*\\n)', fb).group(1).encode().decode('unicode_escape')
assert len(counters % (1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)) < 192, 'the fb-bottom: line is too long'

# --- the composed frames: into the cached staging buffer, one copy into a free frame, then a flip ---------------
# (INVESTIGATION-maps-compose-perf.md §6: 38 ms of over() into Device memory and 60 ms of a copy window that could
# never come become one 2 ms carveCopy() into a frame nothing is scanning, and a flip.)
draw_rows = function(fb, 'IOMobileFramebuffer::drawRows')
assert 'hakios_over_row(' in draw_rows, \
    'drawRows() must blend a layer whose columns are the panel\'s a row at a time (hakios_over_row())'
assert 'sx == 0 && sw == px1 - px0' in draw_rows, \
    'the row blend is for the columns that map one for one; the rest keeps the per-pixel path'
show = function(fb, 'IOMobileFramebuffer::show')
assert 'hakios_display_compose_copies()' in show, 'the composition path needs its switch (compose-off)'
assert 'fCopyStage' in show and 'fTargetDevice = !inPlace && !stageCopy;' in show, \
    'a swap with layers is composed into the staging memory, not row by row into the frame'
assert 'carveCopy((void *) fDpuVirt[dpuTarget], fCopyStage' in show, \
    '... and the composed frame goes into the scanout frame in one copy'
assert 'inPlace || stageCopy ? fCopyStage' in show, \
    'the in-place path (and the fallback to it) stays: it composes into the same staging buffer'
assert 'hakios_display_inplace_demote()' in show and 'zeroDemote(' in show, \
    'the (a2) demotion is what makes a frame free for a swap with layers'

# The periodic lines say what the paths did, and stay inside hakios_tl's 192-byte line.
def tl_format(source, marker):
    """The format of the hakios_tl() call whose format starts `marker`, its adjacent literals joined."""
    at = source.rindex('hakios_tl(', 0, source.index(marker))
    body = source[at:source.index(');', at)]
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', body)).encode().decode('unicode_escape')

fbzero = tl_format(fb, 'fbzero: enabled=')
assert 'demote_abort' not in fbzero and 'fence=%u' in fbzero and 'busy=%u' in fbzero and \
    'budget=%u' in fbzero and 'remap=%u' in fbzero, \
    'the demotion failures are counted by cause (fence, busy, budget, remap), not as one number'
assert len(fbzero % ((1,) + (99999,) * 11)) < 192, 'the fbzero: line is %d bytes' % len(fbzero % ((1,) + (99999,) * 11))
latency = tl_format(fb, 'fb-latency: ')
assert 'drawers %u/%u' in latency, 'the fb-latency: line carries the drawers (the fbstat: line is truncated)'
assert len(latency.replace('%llu', '%u') % ((999999,) + (999999999,) * 5 + (3, 8, 8))) < 192, \
    'the fb-latency: line is too long'
threads = tl_format(fb, 'display: fb_threads:')
assert 'helper(s) started' in threads and len(threads) < 192, \
    'the one line that says how many drawers a frame has is short'

program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "''' + str(header) + r'''"

/* A pixel of a BGRA surface: A<<24 | R<<16 | G<<8 | B. */
static unsigned ch(uint32_t p, unsigned i) { return (p >> (8 * i)) & 0xffu; }
static bool one(unsigned a, unsigned b) { return a + 1 >= b && b + 1 >= a; }

/* 50% premultiplied black: CoreAnimation's dimming/shadow layer, the one that went opaque (INV §1.2). */
#define HALF_BLACK 0x80000000u
#define OPAQUE_GREY 0xFF3421F0u         /* A=255 R=0x34 G=0x21 B=0xF0 */
#define CLEAR 0x00000000u
#define OPAQUE_BLACK 0xFF000000u
#define NO_BOTTOM 0u                    /* (what a frame holds that nothing has drawn into) */

/*
 * show() as it composes one pixel: the frame starts from its own bottom layer when that covers the display,
 * else from the staged bottom picture, else from black; every layer above the bottom is blended over it once;
 * and the frame is staged for the next swap without a bottom layer when hakios_stage_bottom() says it may be.
 * The staged copy is a copy: what the surface is given afterwards does not reach it (F1).
 */
struct staged { bool there; uint32_t pixel; };

static uint32_t compose(struct staged * s, bool bottom_given, bool covers, uint32_t bottom,
    const uint32_t * above, unsigned n)
{
    uint32_t d;
    bool from_staged = false;

    if (!bottom_given) {
        d = hakios_upper_only_base(s->there) == HAKIOS_BASE_BOTTOM ? s->pixel : CLEAR;
    } else if (covers || !s->there) {
        d = bottom;                                             /* layer 0: straight into the frame */
    } else {
        d = s->pixel;                                           /* started from the staged bottom picture */
        from_staged = true;
    }
    for (unsigned i = 0; i < n; i++) d = over(above[i], d);
    if (bottom_given && hakios_stage_bottom(covers, from_staged)) {
        /* (the frame with layer 0 alone in it: for a layer that covers the display it is that layer, for a
         * partial one it is that layer's part of this pixel -- which this pixel is in) */
        s->there = true;
        s->pixel = bottom;
    }
    return d;
}

int main(void)
{
    /* --- premultiplied over(): the four invariants over() has to keep --- */
    const uint32_t pixels[] = { CLEAR, OPAQUE_GREY, 0x80C04020u, OPAQUE_BLACK, 0x00000001u };
    for (uint32_t p : pixels) {
        assert(over(CLEAR, p) == p);                    /* fully transparent: the destination, bit for bit */
        assert(over(OPAQUE_BLACK, p) == OPAQUE_BLACK);  /* opaque: the source, whatever is under it */
        assert(over(0xFFABCDEFu, p) == 0xFFABCDEFu);
    }
    /* 50% premultiplied black keeps half of R, G and B (256ths: 127/256), and its alpha composites too */
    uint32_t half = over(HALF_BLACK, OPAQUE_GREY);
    for (unsigned i = 0; i < 3; i++) assert(one(ch(half, i), ch(OPAQUE_GREY, i) / 2));
    assert(one(ch(half, 3), ch(HALF_BLACK, 3) + ch(OPAQUE_GREY, 3) / 2));
    /* channels do not bleed into each other: white under it is 127/256 of white in every channel, no carry */
    half = over(HALF_BLACK, 0xFFFFFFFFu);
    for (unsigned i = 0; i < 3; i++) assert(ch(half, i) == 0x7Eu || ch(half, i) == 0x7Fu);
    assert(ch(half, 3) == 0xFEu);                       /* (half of white's alpha, kept by the same 127/256) */
    assert(ch(over(CLEAR, OPAQUE_GREY), 1) == 0x21u && ch(over(CLEAR, OPAQUE_GREY), 2) == 0x34u);
    /* blend(): the weights are 0 and 256 at the ends */
    assert(blend(OPAQUE_GREY, OPAQUE_BLACK, 0) == OPAQUE_GREY);
    assert(blend(OPAQUE_GREY, OPAQUE_BLACK, 256) == OPAQUE_BLACK);
    assert(blend(OPAQUE_GREY, OPAQUE_BLACK, 128) == blend(OPAQUE_BLACK, OPAQUE_GREY, 128));
    puts("PASS: premultiplied over() -- transparent and opaque ends, half-alpha, alpha composited, channels apart");

    /* --- what a bottom-less frame starts from, and that it does not compound --- */
    const uint32_t upper[] = { HALF_BLACK };
    const uint32_t two[] = { HALF_BLACK, 0x40000000u };     /* (a dimming layer and a shadow over it) */
    const uint32_t transparent[] = { CLEAR };               /* the parts CoreAnimation does not cover */
    struct staged s = { false, NO_BOTTOM };

    /* [B+U] [U] [U]: the first stages the bottom, the two after it are over that same picture, not over the
     * frame before (which already holds the layer above) */
    uint32_t first = compose(&s, true, true, OPAQUE_GREY, upper, 1);
    assert(first == over(HALF_BLACK, OPAQUE_GREY));
    assert(s.there && s.pixel == OPAQUE_GREY);
    for (int frame = 0; frame < 2; frame++) {
        assert(compose(&s, false, false, NO_BOTTOM, upper, 1) == first);
        assert(compose(&s, false, false, NO_BOTTOM, two, 2) == over(two[1], over(two[0], OPAQUE_GREY)));
    }
    /* F1: the surface is drawn into again between them; the replay is the copy taken before that */
    uint32_t kept = s.pixel;
    s.pixel = kept;                                     /* (the copy stays what it was: nothing writes to it) */
    assert(compose(&s, false, false, NO_BOTTOM, upper, 1) == over(HALF_BLACK, kept));
    /* [B+U] again with a new map: the new map under the layers above, and not the frame before */
    const uint32_t new_bottom = 0xFF00FF00u;
    uint32_t fourth = compose(&s, true, true, new_bottom, upper, 1);
    assert(fourth == over(HALF_BLACK, new_bottom));
    assert(fourth != first);
    assert(s.pixel == new_bottom);                      /* (the copy is the new map now) */
    /* where the layers above are transparent the bottom layer still shows: no trails of the frame before */
    assert(compose(&s, false, false, NO_BOTTOM, transparent, 1) == new_bottom);
    /* with no bottom picture staged at all the frame is cleared, not left as it is (it would compound) */
    s.there = false;
    assert(hakios_upper_only_base(false) == HAKIOS_BASE_CLEAR);
    assert(compose(&s, false, false, NO_BOTTOM, upper, 1) == over(HALF_BLACK, CLEAR));
    assert(compose(&s, false, false, NO_BOTTOM, upper, 1) != first);

    /* F3: a layer 0 that does not cover the display keeps the bottom picture (it does not drop it), and the
     * frame it is drawn into starts from it -- so the upper-only frame after it still has a bottom */
    s.there = true;
    s.pixel = OPAQUE_GREY;
    uint32_t partial = compose(&s, true, false, new_bottom, upper, 1);
    assert(partial == over(HALF_BLACK, OPAQUE_GREY));   /* (that map's part of the picture, over the staged one) */
    assert(s.there && s.pixel == new_bottom);           /* (the staged picture follows the partial layer) */
    assert(compose(&s, false, false, NO_BOTTOM, upper, 1) == over(HALF_BLACK, new_bottom));
    /* ... but a partial layer 0 with nothing staged is not a bottom picture: the frame is not kept */
    s.there = false;
    compose(&s, true, false, new_bottom, upper, 1);
    assert(!s.there);

    /* The old composition -- every frame over the frame before it -- to show this test can see the bug:
     * eight frames of a 50% black layer leave 1/256 of the picture under it (INV §1.2: (1 - alpha)^n). */
    uint32_t old = over(HALF_BLACK, OPAQUE_GREY);
    for (int frame = 0; frame < 8; frame++) old = over(HALF_BLACK, old);
    assert(old != first);
    for (unsigned i = 0; i < 3; i++) assert(ch(old, i) * 256 < ch(OPAQUE_GREY, i));
    puts("PASS: [B+U][U][U][B+U] over the staged copy, idempotent; a partial layer keeps it; nothing staged clears");

    /* --- the decisions, as the kernel asks them --- */
    const int32_t full[4] = { 0, 0, 1080, 2400 }, partial_rect[4] = { 0, 0, 1080, 1200 };
    const int32_t sized[4] = { 0, 0, 0, 0 }, offset[4] = { 1, 0, 1080, 2400 }, over_rect[4] = { -8, -8, 1096, 2416 };
    assert(hakios_covers_display(full, 1080, 2400));            /* the display, exactly */
    assert(!hakios_covers_display(partial_rect, 1080, 2400));   /* half of it: no base on its own */
    assert(hakios_covers_display(sized, 1080, 2400));           /* no size given: drawLayer() takes the whole display */
    assert(!hakios_covers_display(offset, 1080, 2400));         /* one pixel in: not from the edge */
    assert(hakios_covers_display(over_rect, 1080, 2400));       /* a layer larger than the display covers it */
    assert(hakios_upper_only_base(true) == HAKIOS_BASE_BOTTOM);
    assert(hakios_stage_bottom(true, false) && hakios_stage_bottom(true, true) && hakios_stage_bottom(false, true));
    assert(!hakios_stage_bottom(false, false));                 /* (a partial layer over no bottom picture) */
    /* A rect of a client's, all 32 bits of each, printed clamped: hakios_tl's line is 192 bytes (F10) */
    assert(hakios_print_num(0) == 0 && hakios_print_num(1080) == 1080 && hakios_print_num(0xFFFFFFFFu) == HAKIOS_PRINT_LIMIT);
    assert(hakios_print_num(0x7fffffff) == HAKIOS_PRINT_LIMIT && hakios_print_num(-0x7fffffff - 1) == -HAKIOS_PRINT_LIMIT);
    puts("PASS: what covers the display, what is staged, what a bottom-less frame starts from; rects clamped");

    /* --- the row blend drawRows() uses for a layer that is the panel's width, against over() one pixel at a time --- */
    /* Every length there is, and alphas both premultiplied by construction and not (a client's surface is not
     * checked): over_pair() and hakios_over_row() must be over()'s answer bit for bit on any pixels at all -- they
     * are the same arithmetic, with one 64-bit multiply of four channels standing in for two pixels' of two each. */
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    #define RAND() (seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17, (uint32_t) seed)
    uint32_t src[257], dst[257], want[257], got[257];
    for (unsigned n = 0; n <= 256; n++) {
        for (unsigned trial = 0; trial < 200; trial++) {
            uint32_t alpha = (uint32_t) (trial % 256);
            for (unsigned i = 0; i < n; i++) {
                uint32_t colour = RAND() & 0x00ffffffu;
                src[i] = trial % 3 == 0 ? colour | (colour >> 8 & 0xff00u) << 8 :   /* (not premultiplied at all) */
                    trial % 3 == 1 ? colour | alpha << 24 :                        /* every pixel one alpha */
                    RAND();                                                        /* anything at all */
                dst[i] = RAND();
                want[i] = over(src[i], dst[i]);
            }
            memcpy(got, dst, sizeof(got));
            hakios_over_row(got, src, (int32_t) n);
            for (unsigned i = 0; i < n; i++) {
                assert(got[i] == want[i]);
            }
        }
    }
    /* two pixels of one alpha are over_pair()'s case */
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t s0 = RAND(), s1 = (RAND() & 0x00ffffffu) | (s0 & 0xff000000u), d0 = RAND(), d1 = RAND();
        uint32_t k = hakios_keep(s0);
        uint64_t both = over_pair(s0, s1, d0, d1, k);
        assert((uint32_t) both == over(s0, d0));
        assert((uint32_t) (both >> 32) == over(s1, d1));
        assert(over_keep(s0, d0, k) == over(s0, d0));
    }
    /* a row of transparent premultiplied pixels (no colour under an alpha of 0) is left alone, a row of opaque
     * ones is copied: neither reads the frame. A transparent pixel that does carry colour is over()'s: s + d. */
    for (unsigned i = 0; i < 256; i++) {
        src[i] = 0;
        dst[i] = want[i] = RAND();
    }
    hakios_over_row(dst, src, 256);
    for (unsigned i = 0; i < 256; i++) assert(dst[i] == want[i]);
    for (unsigned i = 0; i < 256; i++) {
        src[i] = RAND() & 0x00ffffffu;
        want[i] = over(src[i], dst[i] = RAND());
    }
    hakios_over_row(dst, src, 256);
    for (unsigned i = 0; i < 256; i++) assert(dst[i] == want[i]);
    for (unsigned i = 0; i < 256; i++) {
        src[i] = RAND() | 0xff000000u;
        dst[i] = RAND();
    }
    hakios_over_row(dst, src, 256);
    for (unsigned i = 0; i < 256; i++) assert(dst[i] == src[i]);
    puts("PASS: over_pair() and hakios_over_row() are over() bit for bit -- every length, every alpha, any pixels, rows of none and of all");
}
'''

with tempfile.TemporaryDirectory(prefix='umi-blend-') as tmp:
    source = Path(tmp) / 'blend.cpp'
    binary = Path(tmp) / 'blend'
    source.write_text(program)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-iquote', str(header.parent),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

print('PASS: the kernel uses these rules, stages where layer 0 is drawn, and lets the copy go at every exit')
