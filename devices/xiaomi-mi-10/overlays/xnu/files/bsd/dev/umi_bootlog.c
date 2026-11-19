/*
 * The kernel's boot log on the panel.
 *
 * The console (osfmk/console/serial_console.c: console_write()) and the loader's live log
 * (osfmk/arm64/loader_return.c: loader_log_line(), which every hakios_tl() line and the drivers'
 * milestones also go through) are given whole lines into a static ring here. That is all the
 * caller's context ever does: append to the ring and return. console_write() runs on any CPU, can
 * be reached with interrupts off, and is often called under a kernel mutex (the modem's bb_lock
 * around IMS_LOG/BB_LOG), so nothing here waits, allocates or touches the framebuffer.
 *
 * The drawing is done by one dedicated kernel thread (bootlog_thread), started at the end of
 * kernel startup, woken ten times a second at most. It paints with the kernel's own 8x16 iso_font
 * into the frame the panel is scanning -- vinfo.v_baseaddr until hakios.display.c's first
 * hakios_bootlog_resume(), the frame it names afterwards -- append only, and restarts at the
 * top when the screen is full -- never a scroll, which on this mapping is 10 MB of read and write
 * per newline (vc_scroll_down(), osfmk/console/video_console.c). It re-checks the stop, pause and
 * target flags before every text row, and it exits for good once the session is stopped.
 *
 * The display driver owns the frames while it sets up fb_dpu, runs its MISR self-tests or re-lays
 * the frames it scans: hakios.display.c brackets each of those with hakios_bootlog_pause() /
 * hakios_bootlog_resume(), and pause waits (bounded) for a row in flight, which is made
 * non-preemptible so that wait is bounded by the row itself. Without that, text written into the
 * GOP frame while the driver fills it white and measures LM_0's MISR changes the signature and the
 * scaling self-test fails (2026-11-12): the panel then scanned another frame and the log was
 * invisible for the rest of the boot. resume() names the frame the panel actually scans -- its
 * kernel virtual address (the pipes are programmed with physical addresses, which are not kernel
 * addresses on this port) and its physical address, which is what identifies it -- and the painter
 * repaints the last screenful there; before the first resume() it draws into vinfo.v_baseaddr, the
 * GOP frame, as it always did.
 *
 * The first UI frame ends it: IOMobileFramebuffer::show() calls hakios_bootlog_stop() at the frame
 * that sets fGraphics, and from then on nothing is drawn -- the session is not touched. A panic
 * ends it the same way (hakios_bootlog_panic(), from the panic wrapper in osfmk/kern/debug.c), so
 * no panic or debugger path ever draws. That is also why there is no boot argument: default on,
 * always, and out of the way once the screen belongs to the UI. `-v` keeps the stock verbose
 * console: when the loader says text mode (v_display == 0) this painter stays off, so the two never
 * draw over each other.
 *
 * The frame is Device memory to this processor whatever the kernel's mapping says (the
 * hypervisor's stage 2): DC ZVA (bzero) and unaligned or vector accesses take an alignment fault
 * there (the 2026-11-10 panic). Every store below is a naturally aligned volatile 8- or 4-byte
 * store, the same rule carveZero()/carveCopy() in iokit/Kernel/MiniIOMobileFramebuffer.cpp keep.
 */
#include <console/video_console.h>
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/debug.h>
#include <kern/sched_prim.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.bootlog.h>
#include <sys/systm.h>

#define BOOTLOG_CHAR_W          8       /* iso_font */
#define BOOTLOG_CHAR_H          16
#define BOOTLOG_COLS            135     /* 1080 / 8: the umi panel the loader passes (width is read at runtime) */
#define BOOTLOG_TEXT            (2 * BOOTLOG_COLS + 1)  /* a console line, up to what wraps to two rows */
#define BOOTLOG_RING            128     /* the lines kept; the painter catches up from them */
#define BOOTLOG_PER_TICK        32      /* lines one painting pass may draw: bounds its work */
#define BOOTLOG_PERIOD_NS       (NSEC_PER_SEC / 10)     /* at most 10 paintings a second */
#define BOOTLOG_STOP_SPINS      500000  /* a stop's wait for one row in flight (a row is ~69 KB) */
#define BOOTLOG_STRIDE_MAX      (64 * 1024)     /* bytes a row may stride: a 4K-wide BGRX row is 15360 */
#define BOOTLOG_FRAME_MAX       (64 * 1024 * 1024)      /* the frame one pass may paint into (umi: 10.1 MB) */

/* osfmk/console/iso_font.c, compiled into video_console.c; the stock console's font, so the two
 * look the same. Bit 0 of a glyph row is the leftmost pixel.
 */
extern unsigned char iso_font[256 * BOOTLOG_CHAR_H];
/* osfmk/console/video_console.c: the frame buffer the loader left, as the console knows it. */
extern struct vc_info vinfo;
/* osfmk/arm/locks_arm.c; its declaration (osfmk/arm/cpu_data.h) is behind MACH_KERNEL_PRIVATE, which
 * bsd/ is not built with. */
extern int get_preemption_level(void);
/* osfmk/kern/thread.c; kern/thread.h keeps this prototype behind MACH_KERNEL_PRIVATE too. */
extern kern_return_t thread_terminate(thread_t);
/* osfmk/arm64/loader_return.c: the loader's live log (USB), which is where the drivers' milestones and
 * this painter's own summary line are read. Declared here as hakios.input.c declares it. */
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

struct bootlog_entry {
	uint64_t seq;                   /* (0 while being rewritten) entry n of the ring holds line seq - 1 */
	char     text[BOOTLOG_TEXT];
};

/* The frame, sampled once for a whole pass: base, width, height and stride from one vinfo (or from
 * the display driver's hakios_bootlog_resume()); gen is the target generation the pass was built on. */
struct bootlog_geom {
	uint8_t *base;
	uint32_t stride;                /* bytes */
	uint32_t height;                /* pixels */
	uint32_t cols;                  /* characters a row holds */
	uint32_t rows;                  /* text rows */
	uint32_t gen;                   /* bootlog_target_gen this geometry belongs to (0: vinfo) */
};

static struct bootlog_entry bootlog_ring[BOOTLOG_RING];

/* the line being assembled, under bootlog_feed_lock */
static char     bootlog_cur[BOOTLOG_TEXT];
static uint32_t bootlog_cur_len;
static uint32_t bootlog_feed_lock;

static uint32_t bootlog_total;          /* lines committed: the sequence number of the next one */
static uint32_t bootlog_drawn;          /* the next line the painter owes (only it writes this) */
static uint32_t bootlog_row_lock;       /* one text row at a time; stop()/pause() take it to wait for a row */
static uint32_t bootlog_stopped;        /* the first UI frame (or a panic): for good */
static uint32_t bootlog_panic;          /* a panic began: no more drawing */
static uint32_t bootlog_pause_depth;    /* pause() without resume(): the display driver owns the frames */
static uint32_t bootlog_stop_due;       /* stop() is done (or -v): the painter emits the one summary line */
static bool     bootlog_stop_quiesced;  /* stop() found the row in flight finished */
static uint32_t bootlog_row;            /* the text row the next line goes on (the painter only) */
static uint64_t bootlog_draws;          /* painting passes */
static uint64_t bootlog_draw_ns;        /* their total time, for the summary */
static bool     bootlog_titled;         /* the one header line has been painted */

/* The frame the panel scans, as hakios_bootlog_resume() last named it. bootlog_target_set is false
 * until the display driver has set fb_dpu up; then the painter uses these instead of vinfo (the GOP
 * frame may not be the one scanned any more). base is the kernel virtual address the painter stores
 * through; phys is that frame's physical address, which is what identifies it (frame 0 is mapped at
 * two virtual addresses: vinfo.v_baseaddr and the display driver's carve-out). geometry and gen are
 * written under bootlog_row_lock (pause() quiesced the row in flight first, and bootlog_row_begin()
 * re-checks gen). */
static uint32_t bootlog_target_gen;     /* bumped whenever that frame changes */
static bool     bootlog_target_set;
static uint64_t bootlog_target_base;
static uint64_t bootlog_target_phys;
static uint32_t bootlog_target_width, bootlog_target_height, bootlog_target_stride;

/* A repaint of the last screenful is due (resume() saw a different frame): the painter consumes the
 * request at the start of its next pass, so no painter-owned state (drawn, row, titled) is written
 * from another thread. */
static uint32_t bootlog_repaint_due;
static uint32_t bootlog_repaint_base;   /* bootlog_total when it was asked for */
/* the painter's own state of that repaint, in progress */
static bool     bootlog_repainting;
static uint32_t bootlog_repaint_upto;

static void
bootlog_cpu_relax(void)
{
	__asm__ volatile ("yield" ::: "memory");
}

/*
 * Wait (bounded) for the text row in flight to finish, then leave its latch free. The caller has set
 * bootlog_stopped or bootlog_pause_depth (release) first, so no new row can start once the row in
 * flight has ended: bootlog_row_begin() takes this same latch and re-checks both. The row itself is
 * non-preemptible (bootlog_row_begin()), so the spin covers it. False: the wait ran out (one row is
 * ~69 KB of stores; the next pass is then only one row behind).
 */
static bool
bootlog_row_wait(void)
{
	for (uint32_t tries = 0; tries < BOOTLOG_STOP_SPINS; tries++) {
		if (os_atomic_cmpxchg(&bootlog_row_lock, 0, 1, acquire)) {
			os_atomic_store(&bootlog_row_lock, 0, release);
			return true;
		}
		bootlog_cpu_relax();
	}
	return false;
}

/* One frame's geometry, validated: 4 bytes a pixel, 32 bytes a glyph cell (8 pixels a character),
 * and a whole frame this painter can bound. */
static bool
bootlog_geom_from(struct bootlog_geom *g, uint64_t base, uint32_t width, uint32_t height, uint32_t stride,
    uint32_t gen)
{
	if (base == 0 || width < BOOTLOG_CHAR_W || height < BOOTLOG_CHAR_H) {
		return false;
	}
	if (width > stride / 4 || stride > BOOTLOG_STRIDE_MAX) {
		return false;   /* the row must hold 4 bytes of every pixel, and be a screen's stride */
	}
	if ((uint64_t)height * stride > BOOTLOG_FRAME_MAX) {
		return false;   /* not a frame this painter knows how to bound */
	}

	g->base = (uint8_t *)(uintptr_t)base;
	g->stride = stride;
	g->height = height;
	g->cols = width / BOOTLOG_CHAR_W;
	if (g->cols > BOOTLOG_COLS) {
		g->cols = BOOTLOG_COLS;
	}
	g->rows = height / BOOTLOG_CHAR_H;
	g->gen = gen;
	return g->cols != 0 && g->rows != 0;
}

/*
 * The frame this pass paints into, or false. Before the display driver's first resume() that is
 * vinfo (the GOP frame); afterwards it is the frame resume() named (a carve-out frame the panel
 * scans). vinfo is four separate fields to us (its writer, initialize_screen(), replaces the whole
 * struct in one store and is not reachable under a lock here), so it is read twice and the pass is
 * skipped if the two reads disagree -- a new base paired with an old stride would paint outside the
 * frame. The resume() target is written under bootlog_row_lock with bootlog_target_gen bumped, so a
 * pass built on an older target aborts at its next row (bootlog_row_begin()).
 */
static bool
bootlog_geom_get(struct bootlog_geom *g)
{
	unsigned long base1, base2;
	uint32_t width1, width2, height1, height2;
	uint32_t stride1, stride2, depth1, depth2;
	uint32_t gen1, gen2;

	if (os_atomic_load(&bootlog_target_set, acquire)) {
		gen1 = os_atomic_load(&bootlog_target_gen, acquire);
		base1 = (unsigned long)bootlog_target_base;
		width1 = bootlog_target_width;
		height1 = bootlog_target_height;
		stride1 = bootlog_target_stride;
		__asm__ volatile ("" ::: "memory");
		gen2 = os_atomic_load(&bootlog_target_gen, acquire);
		if (gen1 != gen2) {
			return false;   /* changed under us: the next pass reads the new one */
		}
		return bootlog_geom_from(g, base1, width1, height1, stride1, gen1);
	}

	base1 = vinfo.v_baseaddr;
	width1 = vinfo.v_width;
	height1 = vinfo.v_height;
	stride1 = vinfo.v_rowbytes;
	depth1 = vinfo.v_depth;
	__asm__ volatile ("" ::: "memory");
	base2 = vinfo.v_baseaddr;
	width2 = vinfo.v_width;
	height2 = vinfo.v_height;
	stride2 = vinfo.v_rowbytes;
	depth2 = vinfo.v_depth;

	if (base1 != base2 || width1 != width2 || height1 != height2 || stride1 != stride2 ||
	    depth1 != depth2) {
		return false;   /* replaced under us: the next pass reads the new one */
	}
	if (depth1 != 32) {
		return false;
	}
	return bootlog_geom_from(g, base1, width1, height1, stride1, 0);
}

/*
 * May the painter draw at all? The drawing thread satisfies these by construction; they also keep
 * the first passes (before the loader's graphics mode and STARTUP_SUB_EARLY_BOOT are established)
 * and any debugger stop from painting.
 */
static bool
bootlog_can_draw(void)
{
	if (__improbable(os_atomic_load(&bootlog_stopped, acquire) != 0) ||
	    __improbable(os_atomic_load(&bootlog_panic, acquire) != 0)) {
		return false;
	}
	if (__improbable(os_atomic_load(&bootlog_pause_depth, acquire) != 0)) {
		return false;   /* the display driver owns the frames (fb_dpu setup, MISR tests, relayout) */
	}
	if (__improbable(startup_phase < STARTUP_SUB_EARLY_BOOT)) {
		return false;   /* preemption is not what get_preemption_level() reads yet */
	}
	if (__improbable(PE_state.video.v_display == 0)) {
		return false;   /* -v: the stock verbose console owns the screen */
	}
	if (__improbable(kernel_debugger_entry_count != 0)) {
		return false;
	}
	if (__improbable(get_preemption_level() != 0)) {
		return false;
	}
	if (__improbable(!ml_get_interrupts_enabled())) {
		return false;
	}
	return true;
}

/* The whole frame to black: aligned volatile stores only, never bzero() (Device memory). */
static void
bootlog_clear(const struct bootlog_geom *g)
{
	for (uint32_t y = 0; y < g->height; y++) {
		uint8_t *row = g->base + (uint64_t)y * g->stride;
		uint32_t left = g->stride;

		if ((((uintptr_t)row) & 7) == 0) {
			volatile uint64_t *p = (volatile uint64_t *)row;
			uint32_t words = left / 8;

			for (uint32_t i = 0; i < words; i++) {
				p[i] = 0;
			}
			row += words * 8;
			left -= words * 8;
		}
		if ((((uintptr_t)row) & 3) == 0) {
			volatile uint32_t *p = (volatile uint32_t *)row;
			uint32_t words = left / 4;

			for (uint32_t i = 0; i < words; i++) {
				p[i] = 0;
			}
			row += words * 4;
			left -= words * 4;
		}
		for (uint32_t i = 0; i < left; i++) {
			((volatile uint8_t *)row)[i] = 0;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * One text row: `len` characters of `text` on the framebuffer row `row`, the rest of the row
 * black. A glyph cell is 8 pixels = 32 bytes; white 0xFFFFFFFF, black 0, BGRX.
 */
static void
bootlog_paint_row(const struct bootlog_geom *geom, uint32_t row, const char *text, uint32_t len)
{
	uint32_t cols = geom->cols;
	uint32_t y = row * BOOTLOG_CHAR_H;

	if (len > cols) {
		len = cols;
	}
	for (uint32_t g = 0; g < BOOTLOG_CHAR_H; g++) {
		uint8_t *rowp = geom->base + (uint64_t)(y + g) * geom->stride;

		if ((((uintptr_t)rowp) & 7) == 0) {
			volatile uint64_t *p = (volatile uint64_t *)rowp;

			for (uint32_t c = 0; c < cols; c++) {
				uint32_t bits = c < len ? iso_font[(unsigned char)text[c] * BOOTLOG_CHAR_H + g] : 0;

				for (uint32_t k = 0; k < 4; k++) {
					uint64_t word = 0;

					if (bits & (1u << (2 * k))) {
						word |= 0xFFFFFFFFu;
					}
					if (bits & (1u << (2 * k + 1))) {
						word |= (uint64_t)0xFFFFFFFFu << 32;
					}
					p[c * 4 + k] = word;    /* two pixels in one aligned store */
				}
			}
		} else if ((((uintptr_t)rowp) & 3) == 0) {
			volatile uint32_t *p = (volatile uint32_t *)rowp;

			for (uint32_t c = 0; c < cols; c++) {
				uint32_t bits = c < len ? iso_font[(unsigned char)text[c] * BOOTLOG_CHAR_H + g] : 0;

				for (uint32_t k = 0; k < 8; k++) {
					p[c * 8 + k] = (bits & (1u << k)) ? 0xFFFFFFFFu : 0;
				}
			}
		} else {
			volatile uint8_t *p = rowp;

			for (uint32_t c = 0; c < cols; c++) {
				uint32_t bits = c < len ? iso_font[(unsigned char)text[c] * BOOTLOG_CHAR_H + g] : 0;

				for (uint32_t k = 0; k < 8; k++) {
					uint32_t pixel = (bits & (1u << k)) ? 0xFFFFFFFFu : 0;

					p[(c * 8 + k) * 4 + 0] = (uint8_t)pixel;
					p[(c * 8 + k) * 4 + 1] = (uint8_t)(pixel >> 8);
					p[(c * 8 + k) * 4 + 2] = (uint8_t)(pixel >> 16);
					p[(c * 8 + k) * 4 + 3] = (uint8_t)(pixel >> 24);
				}
			}
		}
	}
}

/*
 * One text row at a time; stop() and pause() take this same latch to wait for the row in flight.
 * Only the painter thread calls these. False means stopped, paused, or the frame the pass was built
 * for is not the one the painter draws into any more: draw no more this pass.
 */
static bool
bootlog_row_begin(const struct bootlog_geom *g)
{
	/*
	 * One row is at most BOOTLOG_COLS glyphs x 16 pixel lines of aligned stores (~69 KB) and nothing
	 * in bootlog_paint_row() can block, so with preemption off from before the latch is taken it
	 * finishes well inside the spin bootlog_row_wait() allows: a pause() that gets the latch knows no
	 * store follows it, and no preemption between the latch and the flag checks can outlast that spin.
	 */
	disable_preemption();
	if (!os_atomic_cmpxchg(&bootlog_row_lock, 0, 1, acquire)) {
		enable_preemption();
		return false;
	}
	if (__improbable(os_atomic_load(&bootlog_stopped, acquire) != 0) ||
	    __improbable(os_atomic_load(&bootlog_panic, acquire) != 0) ||
	    __improbable(os_atomic_load(&bootlog_pause_depth, acquire) != 0) ||
	    __improbable(g->gen != os_atomic_load(&bootlog_target_gen, acquire))) {
		os_atomic_store(&bootlog_row_lock, 0, release);
		enable_preemption();
		return false;
	}
	return true;
}

static void
bootlog_row_end(void)
{
	/* (the latch goes first: enable_preemption() may schedule this thread away) */
	os_atomic_store(&bootlog_row_lock, 0, release);
	enable_preemption();
}

/* A line, wrapped over as many text rows as it needs (two for a console line); false: stopped. */
static bool
bootlog_paint(const struct bootlog_geom *g, const char *text, uint32_t len)
{
	do {
		uint32_t n = len > g->cols ? g->cols : len;

		if (bootlog_row >= g->rows) {
			/* (the screen is full: from the top. paint_row writes whole rows, so no clear.) */
			bootlog_row = 0;
		}
		if (!bootlog_row_begin(g)) {
			return false;
		}
		bootlog_paint_row(g, bootlog_row, text, n);
		bootlog_row++;
		bootlog_row_end();
		text += n;
		len -= n;
	} while (len > 0);
	return true;
}

/* One painting pass, at most BOOTLOG_PER_TICK lines; only bootlog_thread calls it. */
static void
bootlog_draw_pass(void)
{
	static const char title[] = "HakiOS boot log - the first UI frame ends it";
	struct bootlog_geom g;
	char text[BOOTLOG_TEXT];
	uint32_t total, drawn, stop_at;
	uint64_t began, elapsed = 0;

	if (!bootlog_can_draw()) {
		return;
	}
	if (!bootlog_geom_get(&g)) {
		return;
	}
	began = mach_absolute_time();
	if (os_atomic_xchg(&bootlog_repaint_due, 0, acquire) != 0) {
		if (__improbable(g.gen != os_atomic_load(&bootlog_target_gen, acquire))) {
			/* resume() retargeted while this pass read the frame: the request is for that newer
			 * frame, so it is left for the pass that draws there (the row latch aborts this one). */
			os_atomic_store(&bootlog_repaint_due, 1, release);
		} else {
			/*
			 * resume() named a different frame: it is blank or holds a test pattern, so the last
			 * screenful is painted again from the ring, from the top, whole rows -- then the log
			 * continues where it was. The ring (128 lines) is the most that is repainted; newer lines
			 * are drawn as they always are, and the per-pass limit still bounds this pass's work.
			 */
			uint32_t base = os_atomic_load(&bootlog_repaint_base, acquire);

			bootlog_row = 0;
			bootlog_titled = false;
			bootlog_drawn = base > BOOTLOG_RING ? base - BOOTLOG_RING : 0;
			bootlog_repainting = true;
			bootlog_repaint_upto = base;
		}
	}
	if (!bootlog_titled) {
		if (!bootlog_paint(&g, title, sizeof(title) - 1)) {
			goto out;
		}
		bootlog_titled = true;
	}
	total = os_atomic_load(&bootlog_total, acquire);
	drawn = bootlog_drawn;
	if (total - drawn > BOOTLOG_RING) {
		drawn = total - BOOTLOG_RING;   /* (the oldest were overwritten before they could be seen) */
	}
	stop_at = total;
	if (total - drawn > BOOTLOG_PER_TICK) {
		if (!bootlog_repainting) {
			drawn = total - BOOTLOG_PER_TICK;   /* (a backlog this old is not worth drawing) */
		} else {
			stop_at = drawn + BOOTLOG_PER_TICK; /* (the repaint keeps its place, a pass at a time) */
		}
	}
	while (drawn != stop_at) {
		struct bootlog_entry *e = &bootlog_ring[drawn % BOOTLOG_RING];
		uint32_t i;

		if (os_atomic_load(&e->seq, acquire) != (uint64_t)drawn + 1) {
			break;          /* being rewritten by the producer: the next pass retries */
		}
		for (i = 0; i < (uint32_t)(BOOTLOG_TEXT - 1) && e->text[i] != 0; i++) {
			text[i] = e->text[i];
		}
		text[i] = 0;
		if (os_atomic_load(&e->seq, acquire) != (uint64_t)drawn + 1) {
			drawn++;        /* overwritten while it was copied: a torn line is dropped, not shown */
			continue;
		}
		if (!bootlog_paint(&g, text, i)) {
			break;
		}
		drawn++;
	}
	bootlog_drawn = drawn;
	if (bootlog_repainting && drawn >= bootlog_repaint_upto) {
		bootlog_repainting = false;     /* (the last screenful is back; new lines append from here) */
	}
out:
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &elapsed);
	bootlog_draw_ns += elapsed;
	bootlog_draws++;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* The painter: ten passes a second while the boot lasts, then it exits for good. */
static void
bootlog_thread(void *arg, wait_result_t wresult)
{
	(void)arg;
	(void)wresult;
	thread_set_thread_name(current_thread(), "bootlog");
	for (;;) {
		assert_wait_timeout((event_t)(uintptr_t)&bootlog_stopped, THREAD_UNINT,
		    (uint32_t)(BOOTLOG_PERIOD_NS / NSEC_PER_MSEC), NSEC_PER_MSEC);
		(void)thread_block(THREAD_CONTINUE_NULL);
		if (os_atomic_load(&bootlog_panic, acquire) != 0) {
			break;          /* (the panic path has its own output: no summary) */
		}
		if (os_atomic_load(&bootlog_stopped, acquire) != 0) {
			if (os_atomic_load(&bootlog_stop_due, acquire) != 0) {
				break;
			}
			continue;       /* (stop() is still clearing the frame: wait for its next tick) */
		}
		bootlog_draw_pass();
	}
	/*
	 * The summary line is emitted here, not in hakios_bootlog_stop(): show() calls stop() with the
	 * framebuffer's fLock held, and hakios_tl() masks interrupts and spins for the loader log's lock,
	 * which has no business under a display lock. stop() publishes bootlog_stop_due (a stop, not a
	 * panic; `-v` keeps the stock verbose console's own output).
	 */
	if (os_atomic_load(&bootlog_stop_due, acquire) != 0 && PE_state.video.v_display != 0) {
		hakios_tl("bootlog: %u lines, %u draws, %u ms%s\n", os_atomic_load(&bootlog_total, acquire),
		    (uint32_t)bootlog_draws, (uint32_t)(bootlog_draw_ns / NSEC_PER_MSEC),
		    bootlog_stop_quiesced ? "" : ", row still in flight");
	}
	thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * Start the painter. The ring has been filling since the first console line, long before this
 * runs; whatever was given while there was no thread is drawn in its first pass. EARLY_BOOT's last
 * rank is where the umi USB driver starts its threads too: kernel_thread_start works there, and
 * the boot log's own gate (STARTUP_SUB_EARLY_BOOT) holds it back until startup is done.
 */
static void
bootlog_start(void)
{
	thread_t thread;

	if (kernel_thread_start(bootlog_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bootlog_start);

/* Is this one of the userspace lines worth not painting (a crash reporter's own noise)? */
static bool
bootlog_filtered(const char *text, uint32_t len)
{
	static const char noisy[] = "_PanicOnCrash";

	for (uint32_t i = 0; i + sizeof(noisy) - 1 <= len; i++) {
		if (memcmp(text + i, noisy, sizeof(noisy) - 1) == 0) {
			return true;
		}
	}
	return false;
}

/* A whole line, under bootlog_feed_lock. */
static void
bootlog_commit(const char *text, uint32_t len)
{
	struct bootlog_entry *e;
	uint32_t index;

	if (len > BOOTLOG_TEXT - 1) {
		len = BOOTLOG_TEXT - 1;
	}
	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
		len--;          /* (the console's padding, not worth a pixel) */
	}
	if (bootlog_filtered(text, len)) {
		return;
	}
	index = bootlog_total % BOOTLOG_RING;
	e = &bootlog_ring[index];
	os_atomic_store(&e->seq, 0, relaxed);
	for (uint32_t i = 0; i < len; i++) {
		e->text[i] = text[i];
	}
	e->text[len] = 0;
	os_atomic_store(&e->seq, (uint64_t)bootlog_total + 1, release);
	bootlog_total++;
}

/*
 * The display driver owns the frames: no row may start (bootlog_row_begin() checks
 * bootlog_pause_depth) and the one in flight is waited out, so not one store lands in a frame the
 * driver is filling with a test pattern or has just re-laid. Called by hakios.display.c around its
 * fb_dpu setup, its MISR self-tests and its relayout; safe after stop() (does nothing then).
 * Returns whether the row in flight finished in time -- false means it may still be writing for a
 * moment, which a caller that is about to measure a frame must say once. Pauses nest: the frames are
 * the driver's until the outermost resume().
 */
bool
hakios_bootlog_pause(void)
{
	if (os_atomic_load(&bootlog_stopped, acquire) != 0) {
		return true;    /* nothing draws: already quiescent */
	}
	os_atomic_inc(&bootlog_pause_depth, release);
	return bootlog_row_wait();
}

/*
 * The frame the panel scans now: `base` is its kernel virtual address (what the painter stores
 * through) and `phys` its physical address (what identifies it), width x height pixels, stride bytes
 * a row. The painter draws there from now on; a different frame than the one it was drawing into
 * gets the last screenful painted again (the painter, at the start of its next pass). The same frame
 * at another virtual address only retargets; a nested resume leaves the painter paused.
 */
/* One resume() undoes one pause(); an unpaired resume never takes the depth below zero. */
static void
bootlog_pause_unwind(void)
{
	uint32_t d = os_atomic_load(&bootlog_pause_depth, relaxed);

	while (d != 0 && !os_atomic_cmpxchgv(&bootlog_pause_depth, d, d - 1, &d, release)) {
	}
}

void
hakios_bootlog_resume(uint64_t base, uint64_t phys, uint32_t width, uint32_t height, uint32_t stride)
{
	struct bootlog_geom g;
	bool same;

	if (__improbable(os_atomic_load(&bootlog_stopped, acquire) != 0)) {
		return;         /* the UI owns the screen: nothing draws again */
	}
	if (!bootlog_geom_from(&g, base, width, height, stride, 0)) {
		/* not a frame this painter can bound: it draws nothing rather than somewhere unknown, and
		 * stays paused (depth kept) -- the boot log goes dark for the rest of boot rather than
		 * risk a frame the driver has moved away from (not reachable while dpu_ready holds) */
		return;
	}
	(void)bootlog_row_wait();       /* (no row starts meanwhile: still paused) */

	same = bootlog_target_set
	    ? phys == bootlog_target_phys && width == bootlog_target_width &&
	      height == bootlog_target_height && stride == bootlog_target_stride
	    : phys == vinfo.v_physaddr && width == vinfo.v_width &&
	      height == vinfo.v_height && stride == vinfo.v_rowbytes;
	bootlog_target_base = base;
	bootlog_target_phys = phys;
	bootlog_target_width = width;
	bootlog_target_height = height;
	bootlog_target_stride = stride;
	os_atomic_store(&bootlog_target_set, true, release);
	os_atomic_inc_orig(&bootlog_target_gen, release);
	if (!same) {
		/* (the base is stored before the flag; the painter's next pass paints the last screenful
		 * into the new frame and continues from there) */
		os_atomic_store(&bootlog_repaint_base, os_atomic_load(&bootlog_total, acquire), relaxed);
		os_atomic_store(&bootlog_repaint_due, 1, release);
	}
	/* (pause() and resume() are paired; an unpaired resume leaves the painter paused, not drawing) */
	bootlog_pause_unwind();
}

void
hakios_bootlog_feed(const char *text, int len)
{
	if (__improbable(text == NULL)) {
		return;
	}
	/* Nothing reads the ring after the first UI frame: every later kprintf pays one relaxed load. */
	if (__improbable(os_atomic_load(&bootlog_stopped, relaxed) != 0)) {
		return;
	}
	if (len < 0) {
		/* (bounded: a caller's line that is not terminated is cut, not chased) */
		for (len = 0; len < 4096 && text[len] != 0; len++) {
		}
	}
	if (len == 0) {
		return;
	}
	if (!os_atomic_cmpxchg(&bootlog_feed_lock, 0, 1, acquire)) {
		return;         /* another processor is assembling a line: this chunk is dropped */
	}
	for (int i = 0; i < len; i++) {
		char c = text[i];

		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			bootlog_commit(bootlog_cur, bootlog_cur_len);
			bootlog_cur_len = 0;
			continue;
		}
		if (bootlog_cur_len >= BOOTLOG_TEXT - 1) {
			/* a line longer than the ring entry: the rest is still the next line's */
			bootlog_commit(bootlog_cur, bootlog_cur_len);
			bootlog_cur_len = 0;
		}
		bootlog_cur[bootlog_cur_len++] = c;
	}
	os_atomic_store(&bootlog_feed_lock, 0, release);
}

void
hakios_bootlog_stop(int clear)
{
	struct bootlog_geom g;
	bool quiesced = false;

	if (!os_atomic_cmpxchg(&bootlog_stopped, 0, 1, release)) {
		return;         /* said once */
	}
	if (PE_state.video.v_display != 0) {
		/* Wait for the row in flight, if any (one row is ~69 KB of stores). Bounded: this runs with the
		 * framebuffer's fLock held, and a failed wait means the next pass is only one row behind. */
		quiesced = bootlog_row_wait();
		if (clear && quiesced && bootlog_geom_get(&g)) {
			/* The UI is about to be drawn into this frame: start it from black. Only when no row can
			 * still be writing into it (a clear racing a row would paint text over the UI layer). */
			bootlog_clear(&g);
		}
	}
	/*
	 * The one summary line goes out from the painter thread, which is not under this call's
	 * framebuffer lock (hakios_tl() masks interrupts and spins for the loader log's lock). `-v` keeps
	 * the stock verbose console: the thread sees v_display and emits nothing.
	 */
	bootlog_stop_quiesced = quiesced;
	os_atomic_store(&bootlog_stop_due, 1, release);
}

void
hakios_bootlog_panic(void)
{
	/* Permanent: the painter will not draw another row, and it exits. Nothing is printed here. */
	os_atomic_store(&bootlog_panic, 1, release);
	os_atomic_store(&bootlog_stopped, 1, release);
}
