/*
 * The kernel's boot log on the panel (bsd/dev/umi_bootlog.c): on from the first line the console or
 * the loader's live log is given, off for good at the first UI frame. No boot argument.
 */
#ifndef _SYS_HAKIOS_BOOTLOG_H_
#define _SYS_HAKIOS_BOOTLOG_H_

#include <stdbool.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

/*
 * A console line, or part of one, as console_write() and loader_log_line() have it. Bounded copy
 * into a static ring; safe from any context (it never draws and never allocates), and after
 * hakios_bootlog_stop() it is one relaxed load. len < 0: to the NUL.
 */
void hakios_bootlog_feed(const char *text, int len);

/*
 * The first UI frame (IOMobileFramebuffer::show): no further drawing, for good. The painter gets a
 * short, bounded chance to finish the one text row in flight; clear then blanks the frame the log
 * was drawn into once, because the UI is about to be drawn into it (a clear is skipped if that row
 * did not finish, and the summary says so). One summary line, once, emitted by the painter thread --
 * not here: stop() runs with the framebuffer's lock held (hakios_tl() masks interrupts and spins for
 * the loader log's lock).
 */
void hakios_bootlog_stop(int clear);

/*
 * The display driver is about to own the frames: it is setting up fb_dpu, running its MISR
 * self-tests, or re-laying the frames it scans. The painter draws nothing until
 * hakios_bootlog_resume() (a text row in flight gets the same bounded wait stop() gives it), so its
 * text can never land in a test frame and change what MISR measures. Pauses nest (a count), a pause
 * after stop() does nothing, and returns whether the row in flight finished in time: false means one
 * may still be writing for a moment -- say so once, not per call.
 */
bool hakios_bootlog_pause(void);

/*
 * The display driver is done. `base` is the kernel virtual address of the frame the panel scans
 * (width x height pixels, `stride` bytes a row, 32 bits a pixel) and `phys` its physical address;
 * a frame is identified by phys and geometry, so the same memory at another virtual address only
 * retargets (frame 0 is mapped twice: vinfo.v_baseaddr and the display driver's carve-out). The
 * painter draws into that frame from now on -- not into vinfo.v_baseaddr, which may no longer be the
 * frame on the panel -- and when the frame differs from the one it was drawing into, it repaints the
 * last screenful of lines from its ring first (whole rows, the same rate limit, the stop/pause check
 * before every row). Passing the same frame again is a no-op, a nested resume leaves the painter
 * paused until the outermost one, and a frame this painter cannot use makes it draw nothing.
 */
void hakios_bootlog_resume(uint64_t base, uint64_t phys, uint32_t width, uint32_t height, uint32_t stride);

/* A panic has begun: stop permanently, so no panic or debugger path ever draws. Nothing is printed. */
void hakios_bootlog_panic(void);

__END_DECLS

#endif /* _SYS_HAKIOS_BOOTLOG_H_ */
