/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
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
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
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
#include "umi_frame_sample.h"
#include "umi_dpu_layer.h"
#include "hakios_blend.h"               /* blend(), over() and what an upper-only frame is composed over */

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
/* bsd/dev/umi_bootlog.c (sys/hakios.bootlog.h): the kernel's boot log on this frame, off for good at the first UI frame */
extern "C" void hakios_bootlog_stop(int clear);
/* the panel's scan (bsd/dev/hakios.display.c; sys/hakios.display.h, which iokit/ does not see) */
struct hakios_display_scan {
	int ok;
	uint32_t lines, shown, millihz;
	uint64_t line_ns, frame_ns;
};
extern "C" int hakios_display_scan_start(void);
extern "C" int hakios_display_session_prepare(void);
extern "C" bool umi_pm_screen_off(void);
extern "C" void umi_pm_frame_skipped(void);
extern "C" const struct hakios_display_scan * hakios_display_scan(void);
extern "C" int hakios_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns);
extern "C" uint64_t hakios_display_next_frame_ns(void);
extern "C" int hakios_display_command_mode(void);
extern "C" int hakios_display_wait_copy(uint64_t copy_ns, uint64_t limit_ns);
extern "C" void hakios_cpufreq_boost(uint32_t milliseconds);
/* fb_dpu=1 (bsd/dev/hakios.display.c): page flips between frames of the splash carve-out instead of a copy */
extern "C" unsigned hakios_display_dpu_start(uint64_t phys, uint32_t row_bytes, uint32_t height, vm_offset_t * virt, unsigned max);
extern "C" int hakios_display_dpu_flip(unsigned index, uint64_t limit_ns);
extern "C" int hakios_display_dpu_refresh(void);
extern "C" int hakios_display_dpu_poll(void);
extern "C" int hakios_display_dpu_shown(void);
extern "C" uint64_t hakios_display_dpu_frame(unsigned index);
extern "C" int hakios_display_dpu_leaving(unsigned index);
extern "C" void hakios_display_dpu_stride(unsigned index, uint32_t stride);
extern "C" unsigned hakios_display_dpu_write_busy(uint64_t phys);
/* fb_dpu=4, in place demotion: /var/db/hakios-display/inplace-demote-off, sampled with the display's other
 * switches before the first client (display-session; default on) */
extern "C" int hakios_display_inplace_demote(void);
/* fb_dpu=4: /var/db/hakios-display/compose-off, the same way (default on): a swap with layers is composed into the
 * cached staging buffer and copied into a free frame in one piece, then flipped to, instead of into the frame */
extern "C" int hakios_display_compose_copies(void);
/* how many times a copy found no idle window in the panel's transfer (the fbzero: line's nowin=) */
extern "C" uint32_t hakios_display_copy_nowin(void);
/* ... and how many windows were found impossible before any of them was waited for (nowin= no longer counts those) */
extern "C" uint32_t hakios_display_copy_impossible(void);
/* fb_dpu=4 (hakios.gpu.cpp, drivers/gpu/hakios.adreno.c) */
extern "C" bool HakiosGPUSurfaceAlias(MiniIOSurface * surface, uint64_t phys);
/* fb_dpu=4 (a2): the surface's fence, the copy of its frame back into its memory and the remap of its GPU mappings,
 * one operation under the GPU's lock, bounded in time (fence_ms, budget_ms). 0: the frame is still the surface's,
 * and *why says what ran out (0 none, 1 the fence, 2 the budget, 3 the remap). */
extern "C" bool HakiosGPUSurfaceDemote(MiniIOSurface * surface, const void * frame, uint64_t fence_ms,
    uint64_t budget_ms, unsigned * why);
extern "C" void hakios_gpu_to_memory(void * start, uint64_t size);
extern "C" void hakios_gpu_from_memory(void * start, uint64_t size);
extern "C" bool hakios_gpu_done(uint32_t fence);
/* fb_dpu=4: /var/db/hakios-display/write-kick-off, sampled with the display's other switches (default on) */
extern "C" int hakios_display_write_kick(void);
/* fb_dpu=4: /var/db/hakios-display/zero-recover-off, the same way (default on): zero copy is tried again after
 * zeroLeave() took it away (zeroRecover(), ZERO_REALIAS_DELAY_NS) */
extern "C" int hakios_display_zero_recover(void);
extern "C" uint32_t hakios_display_dpu_latched(void);

/* Successful CPU copies plus confirmed DPU latches, independent of fbstat's
 * resettable windows. Reading this counter never polls hardware or waits for TE. */
static SInt64 gCopiedPresents;
SYSCTL_DECL(_kern_hakios);
static int
sysctl_fb_presented(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint64_t count = (uint64_t)OSAddAtomic64(0, &gCopiedPresents) + hakios_display_dpu_latched();
	return SYSCTL_OUT(req, &count, sizeof(count));
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, fb_presented, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, sysctl_fb_presented, "QU", "completed panel copies and confirmed DPU latches");
extern "C" void hakios_display_dpu_stop(void);
extern "C" int hakios_display_dpu_fetch_test(uint64_t phys, vm_offset_t virt);
extern "C" unsigned hakios_display_dpu_scale_start(uint32_t w, uint32_t h, uint32_t stride, vm_offset_t * virt, unsigned max);

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
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
#define KICK_QUIET_NS           (12 * NSEC_PER_MSEC)    /* fb_dpu=4: a frame left alone this long is fetched again (see healWrites()) */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
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
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/* fb_dirty=1 reuses diff bounds for command-mode copies; it requires fb_diff=1. */
static TUNABLE(uint32_t, gDirty, "fb_dirty", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE              3                       /* triple-buffered producer queue; fb_queue may select fewer */
static TUNABLE(uint32_t, gQueue, "fb_queue", 3);
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
 * Boot-arg fb_dpu=1 (command-mode panel, fb_scan=1): no staging buffer and no copy into the panel's memory.
 * The frame is scaled straight into one of three frames of the splash carve-out that the panel is not
 * scanning, and the DPU's source pipes are flipped to it at the next TE (DISPLAY-SCANOUT-umi.md).
 * fb_dpu=2: the same, after a self-checking test at start (LM_0's MISR, no eyes needed): can the DPU fetch from
 * XNU's RAM below 4 GiB? Plus a read-only dump of apps_smmu's entries for the display.
 * fb_dpu=3: the DPU scales (VIG0 QSEED3, bilinear): frames in the carve-out are 1125x2436 and the CPU only copies
 * CoreAnimation's frame into one. Self-checked at start; on FAIL fb_dpu=1 goes on.
 * fb_dpu=4: as 3, and zero copy: CoreAnimation's frame surfaces are aliased onto carve-out frames (the GPU draws
 * straight into what the panel scans: HakiosGPUSurfaceAlias) and a swap is a flip. SwapEnd returns once the flip
 * is latched, so the surface CoreAnimation draws next is never the one scanned. Swaps that cannot be flipped
 * (other surfaces, more layers) are copied into the one frame kept unaliased.
 */
static TUNABLE(uint32_t, gDpu, "fb_dpu", 0);
#define DPU_FLIP_LIMIT_NS       (100 * NSEC_PER_MSEC)   /* a flip not latched by then: back to the CPU copy */
/*
 * fb_dpu=4, the last resort (the only frame free is the one the panel is fetching, so the composed frame has
 * to be copied into it during a DMA idle interval): one such interval comes round with every transfer, so one
 * refresh is all there is to wait for. The 100 ms above was waited out frame after frame when the interval did
 * not come, and none of those frames was shown (v29f: Maps' multi-layer frames, 15 of 15 lost every 2 s).
 */
#define DPU_INPLACE_LIMIT_NS    (20 * NSEC_PER_MSEC)
/*
 * fb_dpu=4, the in-place demotion (a2): a copied swap that has no frame free of aliases but the one the panel is
 * fetching gives one alias its frame back instead -- its picture copied back into its own memory, its GPU
 * mappings its own again -- so two frames are free of aliases, the frame is composed into one of them and flipped
 * to at the next TE: the true double buffering of the zero-copy path, instead of 10.1 MB written into the frame
 * the panel is fetching (which tore: v29i, INVESTIGATION-inplace-tearing.md).
 * DEMOTE_HOLD_NS: a frame given back stays free of aliases this long. Without the hold CoreAnimation's next
 * single-layer frame surfaces take the freed frames back at once, the copied swap after them demotes again, and
 * so on: 10.1 MB copied back every other frame.
 * DEMOTE_FENCE_MS / DEMOTE_BUDGET_MS: the fence, the copy back and the un-alias are one operation of the GPU's
 * (HakiosGPUSurfaceDemote() in hakios.gpu.cpp), the fence waited for this long, all of it this long -- it runs
 * with this driver's fLock held (show()), so it must never wait for a submission without a limit.
 */
#define DEMOTE_HOLD_NS          (200 * NSEC_PER_MSEC)
#define DEMOTE_FENCE_MS         6                       /* a demoted surface's GPU work is waited for this long at most */
#define DEMOTE_BUDGET_MS        12                      /* ... and the whole demotion, copy back included, this long: under fLock */
/*
 * fb_dpu=4, coming back from zero copy having left (zeroLeave()): CoreAnimation keeps presenting new full-screen
 * surfaces, so the alias is tried again on a later swap -- a boot that lost it (v29h: one ALIAS FAIL, an alias
 * whose check sampled the surface while the GPU was still drawing, turned zero copy off for every frame of the
 * session, backboardd's new surfaces included) must not cost the whole session its flips. Nothing is aliased
 * here: zeroSlot() does that with its own checks; this only lets it look again. A swap may wait this long for
 * the first re-alias, and a session gets ZERO_REALIAS_TRIES of them at most (each costs one 10.1 MB copy), so a
 * surface the GPU really will not leave cannot turn into a loop. Off: /var/db/hakios-display/zero-recover-off.
 */
#define ZERO_REALIAS_DELAY_NS   (700 * NSEC_PER_MSEC)   /* quiet this long after zero copy left before the next try */
#define ZERO_REALIAS_TRIES      8                       /* ... and at most this many tries in a display session */
#define COPY_RECENT             8                       /* the copy budget is the largest of this many copies */
/*
 * A swap with layers above the bottom one but no bottom layer is composed over the bottom layer of the last
 * swap that had one (hakios_blend.h, INVESTIGATION-alpha-black.md H1). That layer is not read where it lies:
 * its maker may be drawing into the same surface again the moment a wait for its fence returns, and the replay
 * tore (REVIEW-layer0-replay.md F1). It is copied into STAGED memory while the frame holds it and nothing
 * above it (stageBottom()), and the replay is a straight copy out of that. Nothing is waited for, so nothing
 * is waited for under this driver's fLock either (REVIEW F2): one memory-to-memory copy of the frame, in and
 * out, where the surface itself used to be scaled out of and then blended over.
 *
 * The memory: one display-sized buffer, IOBufferMemoryDescriptor (OSObject itself is limited to 16 KiB), taken
 * once when the display's size is known and let go with the service.
 */
#define STAGED_LIMIT            (64 * 1024 * 1024)      /* (the largest display a copy of is kept) */
/* Boot-arg fb_stats=1 (also on with metal_fps=1): every 2 s what the panel really got and what it cost here. */
static TUNABLE(uint32_t, gStats, "fb_stats", 0);
#define STATS_NS                (2 * NSEC_PER_SEC)
/* rows a thread takes at a time when a frame is scaled by several (fb_threads): slow cores just take fewer */
#define ROW_CHUNK               32

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static uint32_t gScanHz;

static inline uint32_t
refreshHz(void)
{
	if (gScanHz != 0) return gScanHz;
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
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	IOBufferMemoryDescriptor * fComposeMemory;
	uint8_t *       fComposeBase;           /* command-mode composition, away from display DMA */
	uint64_t        fCopyNs;
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
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
	unsigned        fSwapFirst, fSwapsWaiting;
	uint64_t        fNextTicket, fCompletedTicket;
	uint32_t        fShownID;               /* the last swap show() is done with (SwapWait: waitShown()) */
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
	uint64_t        fDrawNs;
	uint32_t        fStatScanWaits, fStatScanMissed;
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
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
	bool            fDirty;
	uint64_t        fDirtyPixels;
	uint32_t        fDirtyLeft, fDirtyTop, fDirtyRight, fDirtyBottom;
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
	/* where drawRows() writes: the panel's memory, the staging buffer, or (fb_dpu) the carve-out frame not scanned */
	uint8_t *       fTarget;
	bool            fTargetDevice;          /* fb_dpu: fTarget is the carve-out: rows are made in fRowOut[] and carveCopy()'d */
	uint32_t *      fRowOut[8];             /* ... one row per drawer (the presenter's [0], helper i's [i + 1]) */
	unsigned        fDpuBuffers;            /* fb_dpu: frames to flip between (0: off) */
	vm_offset_t     fDpuVirt[3];
	bool            fDpuScaled;             /* fb_dpu=3: the DPU scales; fPanel... and fRowBytes are the display frame */
	uint32_t        fDpuPanelWidth, fDpuPanelHeight, fDpuRowBytes;  /* ... and these the panel's, for a fallback */
	/* fb_dpu=4: the surfaces aliased onto carve-out frames (held), and the check that the GPU left their own memory */
	bool            fZero;
	MiniIOSurface * fBound[3];
	uint64_t        fBoundHash[3];
	uint32_t        fBoundChecks[3];
	uint32_t        fStZero;
	bool            fSlotBlack[3];          /* a copied frame that is all black (the bottom layer taken away) */
	int             zeroSlot(const uint8_t * swap, MiniIOSurface ** surfaces, int pending, int scanned);
	bool            zeroDemote(unsigned i, uint64_t fence_ms, uint64_t budget_ms, bool probe); /* (a2) one alias its frame back */
	/* fb_dpu=4: zero copy left (zeroLeave()) and is tried again later (ZERO_REALIAS_DELAY_NS, ZERO_REALIAS_TRIES) */
	uint64_t        fZeroLostAt;            /* (mach_absolute_time) when zero copy last left, 0: never (or still on) */
	uint32_t        fZeroOff;               /* how many times it left (fbzero: off=) */
	uint32_t        fRealias;               /* ... and how many times it came back (fbzero: realias=) */
	uint32_t        fRealiasTries;          /* tries made this session, all of them at most ZERO_REALIAS_TRIES */
	bool            zeroRecover(void);
	uint64_t        fDemotedAt;             /* (mach_absolute_time) when a frame was last given back, 0: never */
	uint32_t        fStrideSaid;            /* in-place targets whose leftover alias stride was put back (a bit each) */
	uint32_t        fCopyRecent[COPY_RECENT];   /* the last copies into a scanout frame, in nanoseconds */
	unsigned        fCopyNext;
	bool            fPathInplace;           /* the frame on the panel is being written into: one line an episode */
	uint32_t        fInplaceFrames;         /* ... how many frames of them */
	bool            fBottomGone;            /* a swap came without its bottom layer: one line an episode */
	/*
	 * The bottom picture: the frame the last swap that had one was drawn into, as it was before the layers
	 * above it were blended -- a copy of ours, not the surface, which its maker may draw into again at any
	 * moment and tear a replay out of (REVIEW-layer0-replay.md F1). A swap whose bottom layer is not given is
	 * composed over this, not over the frame's own last output (which already holds the layers above it:
	 * blending them in again takes the picture under them away every frame, INVESTIGATION-alpha-black.md H1,
	 * and hakios_blend.h). Let go when the client goes, with the display session, and when a fallback from
	 * fb_dpu changes the size of the frame it was taken at.
	 */
	IOBufferMemoryDescriptor * fStagedMemory;
	uint8_t *       fStaged;                /* the copy itself: fRowBytes rows of fPanelHeight */
	uint64_t        fStagedBytes;           /* ... the size it was made with, so a larger display is noticed */
	bool            fStagedValid;           /* it holds the bottom picture of a swap that had one */
	bool            fStagedDropped;         /* ... it did, and was let go: why a frame without one was cleared */
	uint32_t        fStAboveOnly;           /* upper-only frames, this 2 s window */
	uint32_t        fStAboveFrames, fAboveFirst;    /* ... of the episode going on, and its first swap */
	uint32_t        fAboveCleared;          /* ... and how many of those were cleared (no bottom picture) */
	uint32_t        fStClearNever, fStClearGone;    /* ... and why, this 2 s window: never staged, or staged and let go */
	bool            stagedRoom(void) const { return fStaged != NULL && (uint64_t) fRowBytes * fPanelHeight <= fStagedBytes; }
	bool            stageBottom(void);                     /* (with fLock: fTarget, holding layer 0 alone, -> fStaged) */
	bool            stageLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);   /* (with fLock) */
	bool            replayBottom(void);                    /* (with fLock: fStaged -> fTarget) */
	void            dropStaged(void);                      /* (with fLock) */
	void            forgetStaged(void);                     /* (takes fLock: the client and the session, below) */
	bool            fRefusedBound;          /* zeroSlot() had no frame left to alias: one line an episode */
	uint32_t        fStRefusedBound, fStNoBottom, fStFlips;     /* ... and what the 2 s lines count */
	uint32_t        fStLateWin;             /* in-place frames written with no window, this 2 s window */
	uint32_t        fStDemoteFence;         /* demotions that ran out of fence (round after the probe), this 2 s window */
	uint32_t        fStDemoteBusy;          /* ... whose GPU work was still in flight (the non-blocking probe) */
	uint32_t        fStDemoteBudget;        /* ... that ran out of the whole operation's budget */
	uint32_t        fStDemoteRemap;         /* ... whose GPU mappings could not be put back */
	uint32_t        fStComposed;            /* frames composed into fCopyStage and copied into a scanout frame, this window */
	IOBufferMemoryDescriptor * fAliasReadMemory, * fCopyStageMemory;
	uint8_t *       fAliasRead, * fCopyStage;
	const uint8_t * sourcePixels(MiniIOSurface *s);
	bool            fHardwareCompose;
	int             fLastHardwareBank;
	uint8_t         fLastHardwareSwap[SWAP_SIZE];
	MiniIOSurface * fLastHardwareSurfaces[4];
	void            hardwareRemember(const uint8_t *swap, MiniIOSurface **surfaces, int bank);
	void            hardwareReplay(void);
	int             hardwarePrepare(const uint8_t *swap, MiniIOSurface **surfaces, struct umi_dpu_layer *layers, unsigned *count, uint64_t *upload, uint64_t *waiting);
	uint32_t        fStLayers[5], fStHardware, fStInplace, fDemoteReason[3];
	uint64_t        fStUpload, fStUploadMax;
	uint32_t        fDemotions;
	uint32_t        fSlotStride[3];         /* each frame's row stride: an aliased surface's own, else fRowBytes */
	uint64_t        fStZeroWait, fStZeroWaitMax;
	uint32_t        fNotAliased[4];         /* full-display surfaces that could not be aliased: said once each */
	uint32_t        fStInplaceLate;         /* frames copied in place with no idle interval to copy in (said below) */
	bool            fZeroSlow;              /* ... one of them waiting still: said once per run of them */
	/*
	 * fb_dpu=4: a frame's picture is fetched by the panel when the frame is flipped to; in command mode the panel
	 * keeps its own copy of it. A GPU submission that WRITES the surface after that fetch (the submission reaching
	 * the kernel after SwapEnd did, or a fence of the surface's that was behind a submission still in the ring) does
	 * not change what is on the glass until a frame is fetched again -- and when CoreAnimation goes idle none is
	 * flipped, so the panel keeps a picture from before the write, or from the middle of one (the App Library going
	 * black while still). The GPU's submit path arms fWritten[f] (hakios.gpu.cpp: MiniIOMobileFramebufferSurfaceWritten)
	 * for the frame the surface is bound to; a flip of the frame clears the arm when the write is already done (the
	 * fetch carries it); healWrites() fetches the frame once more when an armed write is done and the frame has been
	 * left alone for KICK_QUIET_NS and is the one on the panel. Bounded (one fetch an arm), coalesced (the arm is the
	 * newest fence), and counted in the fbzero: line's kick=. Off with /var/db/hakios-display/write-kick-off.
	 */
	IOLock *        fWrittenLock;           /* around fWritten[] and fWrittenAt[] */
	uint32_t        fWritten[3];            /* the last submission that wrote the frame's surface (0: none, or done and fetched) */
	uint64_t        fWrittenAt[3];          /* ... and when it was armed (a submission may come in more than one piece) */
	uint32_t        fStKicks;               /* frames fetched again because of such a write (fbzero: kick=) */
	bool            fKickSaid;              /* the one line that says the first one was */
	void            written(MiniIOSurface * surface, uint32_t fence);   /* (the GPU's submit path: this lock only) */
	void            healWrites(void);                                   /* (the panel thread, and the GPU's idle thread) */
	bool            writesPending(void);                                /* ... and whether there is anything to look at */
	void            zeroLeave(const char * why, bool copyBack);
	uint64_t        ownHash(MiniIOSurface * s);
	/* nearest-neighbour scaling (fb_filter=0) as runs of consecutive source pixels: a memcpy each */
	uint32_t *      fRunDst;
	uint32_t *      fRunSrc;
	uint32_t *      fRunLen;
	uint32_t        fRuns;
	volatile SInt32 fHelperNext;            /* the next row to be taken (ROW_CHUNK at a time) */
	/* fb_stats (all mach_absolute_time units but the counts) */
	bool            fStats;
	bool            fThreadsSaid;           /* (the one line that says how many drawers a frame has: see statsLine()) */
	bool            fSampleValid;
	uint32_t        fSampleWidth, fSampleHeight;
	uint32_t        fSampleCount, fSampleChanged, fSampleBroad, fSampleCells, fSampleUnknown;
	uint64_t        fSampleTime, fSampleMax;
	uint64_t        fSwapIn[SWAP_QUEUE];    /* when each queued swap was handed over */
	uint64_t        fShowIn;                /* ... the one show() is putting on the panel */
	uint64_t        fStSince;
	volatile SInt32 fStIn, fStLost;
	uint32_t        fStShown, fStMissed, fStClears, fStLatched;
	uint64_t        fStDraw, fStDrawMax, fStWait, fStWaitMax, fStCopy, fStCopyMax, fStClear, fStClearMax, fStGpu, fStLat, fStLatMax;
	void            statsLine(uint64_t now);
	bool            holdsSource(void) const { return fComposeBase != NULL || fDpuBuffers != 0; }
	void            drawChunks(const struct RowJob * job, uint32_t * blendRow, uint32_t * rowOut);
	void            dpuFallback(const char * why, const uint8_t * latest);

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;    /* (the staged bottom picture is memory of ours: it goes with the service) */
	void publishModes(void);
	void publishRefresh(void);
	bool prepareDisplaySession(void);
	int fDisplaySessionState;       /* 0 before the first client, 1 ready, -1 failed */
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void completeSwap(void);
	void waitShown(uint32_t id);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	bool drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool over, const uint8_t *pixels = NULL);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow, uint32_t * rowOut);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
static IOMobileFramebuffer * gFramebuffer;     /* (the one display: fb_dpu=4) */
/* Only the single display presenter accesses these. Keep scratch outside the 16 KiB OSObject limit. */
static uint32_t gFrameSample[UMI_SAMPLE_PIXELS], gFrameSampleNext[UMI_SAMPLE_PIXELS];

/* Defined below the framebuffer row helpers; keep every possible carve-out access on this path. */
static void carveCopy(void * to, const void * from, size_t n);
static void carveZero(void * to, size_t n);

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
 * CoreAnimation takes its frame clock from IOMFBDisplayRefresh.  The mode's
 * SyncRate alone leaves it on a built-in panel clock, so a due timestamp can
 * land on the same animation time after a late callback.  This panel has one
 * fixed interval: expose it both in 32.32-second units and in mach ticks.
 */
void
IOMobileFramebuffer::publishRefresh(void)
{
	OSDictionary * refresh = OSDictionary::withCapacity(4);
	const uint32_t rate = refreshHz();
	const uint64_t fixed = (1ULL << 32) / rate;
	uint64_t interval;

	if (refresh == NULL) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC / rate, &interval);
	setNumber(refresh, "displayMinRefreshInterval", fixed);
	setNumber(refresh, "displayMaxRefreshInterval", fixed);
	setNumber(refresh, "displayRefreshStep", fixed);
	setNumber(refresh, "displayRefreshStepMachTime", interval);
	setProperty("IOMFBDisplayRefresh", refresh);
	setProperty("refresh-rate", rate, 32);
	refresh->release();
	IOLog("MiniIOMobileFramebuffer: IOMFBDisplayRefresh: %u Hz, %llu ticks\n", rate, interval);
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

/* /var is mounted when userland first opens the service. Serialize that first
 * open before any client can enqueue frames or schedule vsync callbacks. */
bool
IOMobileFramebuffer::prepareDisplaySession(void)
{
	IOLockLock(fLock);
	if (fDisplaySessionState == 0) {
		/* (a display session beginning: the bottom picture staged is of the one before it) */
		dropStaged();
		fDisplaySessionState = hakios_display_session_prepare() == 0 ? 1 : -1;
		if (fDisplaySessionState == 1 && fDpuScaled) fHardwareCompose = hakios_display_compose_start() != 0;
		/* (the one line that says which of the two composition paths frames with layers take; compose-off, or no
		 * staging memory to compose into, is the old one: drawn straight into the frame) */
		bool composed = hakios_display_compose_copies() && fCopyStage != NULL;

		hakios_tl("fbcompose-path: frames with layers are %s: %s\n",
		    composed ? "composed into the staging buffer, copied into a free scanout frame, flipped to" :
		    "drawn straight into the scanout frame (a row's scratch, into Device memory)",
		    composed ? "compose-copies on" : !hakios_display_compose_copies() ?
		    "/var/db/hakios-display/compose-off" : "no staging memory (fb_dpu=4 off)");
		const struct hakios_display_scan * s = hakios_display_scan();
		if (fDisplaySessionState == 1 && s != NULL) {
			gScanHz = (s->millihz + 500) / 1000;
			publishModes();
		}
	}
	bool ready = fDisplaySessionState == 1;
	IOLockUnlock(fLock);
	return ready;
}

/*
 * CoreAnimation asks the framebuffer for the source limits before it starts
 * its compositor.  The real IOMFB publishes these as an OS dictionary.  Our
 * source path accepts one full-size BGRA surface, so advertise exactly that
 * bound; without the property WindowServer logs kIOMFBSourceLimitsKey
 * missing and treats the display as having unknown source capabilities.
 */
static void
publishSourceLimits(IOMobileFramebuffer * fb)
{
	OSDictionary * limits = OSDictionary::withCapacity(6);
	uint64_t pixels;

	if (limits == NULL) {
		return;
	}
	pixels = (uint64_t)fb->fWidth * fb->fHeight;
	setNumber(limits, "IOMFBMaxSrcPixels", pixels);
	setNumber(limits, "MaxSrcBufferWidth", fb->fWidth);
	setNumber(limits, "MaxSrcBufferHeight", fb->fHeight);
	setNumber(limits, "MaxSrcRectWidth", fb->fWidth);
	setNumber(limits, "MaxSrcRectTotal", pixels);
	setNumber(limits, "IOMFBMaxCompressedSizeInBytes", pixels * 4);
	fb->setProperty("IOMFBSourceLimits", limits);
	limits->release();
	IOLog("MiniIOMobileFramebuffer: source limits %ux%u (%llu pixels)\n", fb->fWidth, fb->fHeight, pixels);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;
	fComposeMemory = NULL;
	fComposeBase = NULL;
	fCopyNs = 4000000;
	fNextTicket = fCompletedTicket = 0;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	fWrittenLock = IOLockAlloc();           /* (NULL: the write-after-fetch heal is off, which costs nothing else) */
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	/* (as wide as the display too: with fb_dpu=3 the frames drawn here are the display's size) */
	fColumn = IONewZero(uint32_t, MAX(fPanelWidth, fWidth));
	fColumnWeight = IONewZero(uint16_t, MAX(fPanelWidth, fWidth));
	fRunDst = IONewZero(uint32_t, MAX(fPanelWidth, fWidth));
	fRunSrc = IONewZero(uint32_t, MAX(fPanelWidth, fWidth));
	fRunLen = IONewZero(uint32_t, MAX(fPanelWidth, fWidth));
	/* Keep a row scratch for the rare no-staging fallback too: fBase is the boot carve-out on UMI. */
	for (unsigned i = 0; i < fThreads && i < 8; i++) {
		fRowOut[i] = IONewZero(uint32_t, MAX(fPanelWidth, fWidth));
	}
	fTarget = fBase;
	{
		uint32_t metalFps = 0;
		fStats = gStats != 0 || (PE_parse_boot_argn("metal_fps", &metalFps, sizeof(metalFps)) && metalFps != 0);
	}
	fDiff = gDiff != 0;
	fDirty = gDirty != 0;
	fStInplaceLate = 0;
	fZeroSlow = false;
	fDiffLeft = fDiffTop = ~0u;
	fDirtyLeft = fDirtyTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

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
	/* the panel's scan, if it can be read: its rate is then the one published, unless fb_hz= is given */
	if (hakios_display_scan_start()) {
		uint32_t hz;
		const struct hakios_display_scan * scan = hakios_display_scan();

		if (!PE_parse_boot_argn("fb_hz", &hz, sizeof(hz)) && scan != NULL) {
			gScanHz = (scan->millihz + 500) / 1000;
		}
		IOLog("MiniIOMobileFramebuffer: frames follow the panel's scan, at %u a second\n", refreshHz());
		if (hakios_display_command_mode() && gDpu != 0) {
			fDpuBuffers = hakios_display_dpu_start(vinfo.v_physaddr, fRowBytes, fPanelHeight, fDpuVirt, 3);
			if (fDpuBuffers >= 2) {
				fDpuVirt[0] = (vm_offset_t) fBase;
				hakios_tl("display: fb_dpu: %u scanout frames; a frame is scaled straight into one the panel is not "
				    "scanning and the pipes are flipped to it at the next TE (no staging buffer, no copy)\n", fDpuBuffers);
			} else {
				fDpuBuffers = 0;
				hakios_tl("display: fb_dpu: not available; CPU copy path (staging buffer)\n");
			}
			if (fDpuBuffers != 0 && gDpu == 2) {
				/* 64 rows of the panel's stride, contiguous, below 4 GiB (the SSPP address registers are 32 bits) */
				IOBufferMemoryDescriptor * test = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
				    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) fRowBytes * 64, 0x00000000FFFFF000ULL);
				int result = hakios_display_dpu_fetch_test(test != NULL ? test->getPhysicalSegment(0, NULL) : 0,
				    test != NULL ? (vm_offset_t) test->getBytesNoCopy() : 0);
				if (result == -2) {
					hakios_tl("display: fb_dpu=2: the pipe may still fetch the test buffer: it is kept; flips off\n");
					hakios_display_dpu_stop();
					fDpuBuffers = 0;
				} else if (test != NULL) {
					test->release();
				}
			}
			if (fDpuBuffers != 0 && (gDpu == 3 || gDpu == 4)) {
				uint32_t stride = (fWidth * 4 + 255) & ~255u;   /* (CoreAnimation's surfaces' stride) */
				vm_offset_t virt[3];
				unsigned n = hakios_display_dpu_scale_start(fWidth, fHeight, stride, virt, 3);

				if (n >= 2) {
					fDpuPanelWidth = fPanelWidth;
					fDpuPanelHeight = fPanelHeight;
					fDpuRowBytes = fRowBytes;
					fPanelWidth = fWidth;
					fPanelHeight = fHeight;
					fRowBytes = stride;
					fDpuScaled = true;
					fDpuBuffers = n;
					fSlotStride[0] = fSlotStride[1] = fSlotStride[2] = stride;
					memcpy(fDpuVirt, virt, sizeof(fDpuVirt));
					hakios_tl("display: fb_dpu=3: frames are %ux%u in the carve-out, the DPU scales them to the panel\n",
					    fWidth, fHeight);
					if (gDpu == 4) {
						gFramebuffer = this;
						size_t bytes = (size_t)(hakios_display_dpu_frame(1) - hakios_display_dpu_frame(0));
						fAliasReadMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, PAGE_SIZE);
						fCopyStageMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, PAGE_SIZE);
						fZero = fAliasReadMemory != NULL && fCopyStageMemory != NULL;
						if (fZero) {
							fAliasRead = (uint8_t *)fAliasReadMemory->getBytesNoCopy();
							fCopyStage = (uint8_t *)fCopyStageMemory->getBytesNoCopy();
							/* Frame 0 is the black, latched scaling-test result; frame 1 is free.
							 * Measure a real Device-memory copy once, including at 90 Hz,
							 * instead of starting with a budget larger than its idle interval. */
							carveCopy(fCopyStage, (const void *)fDpuVirt[0], (size_t)fRowBytes * fPanelHeight);
							uint64_t copying = mach_absolute_time();
							carveCopy((void *)fDpuVirt[1], fCopyStage, (size_t)fRowBytes * fPanelHeight);
							__asm__ volatile("dsb sy" ::: "memory");
							absolutetime_to_nanoseconds(mach_absolute_time() - copying, &fCopyNs);
							/* (the window check's budget starts as this copy's time; see the in-place branch) */
							for (unsigned i = 0; i < COPY_RECENT; i++) {
								fCopyRecent[i] = fCopyNs > 0xffffffffu ? 0xffffffffu : (uint32_t) fCopyNs;
							}
						} else {
							hakios_tl("fbzero-init: enabled=0 reason=allocation\n");
						}
						hakios_tl("display: fb_dpu=4: zero copy enabled=%u: CoreAnimation's frame surfaces will be drawn "
						    "into %u of the %u carve-out frames\n", fZero, fDpuBuffers - 1, fDpuBuffers);
					}
				} else if (gDpu == 4) {
					hakios_tl("display: fb_dpu=4: needs the DPU scaling (fb_dpu=3) to pass; zero copy off\n");
				}
			}
		}
		if (hakios_display_command_mode() && fDpuBuffers == 0) {
			uint64_t bytes = (uint64_t)fRowBytes * fPanelHeight;
			if (bytes <= 64 * 1024 * 1024)
				fComposeMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
				    kIODirectionInOut, bytes, PAGE_SIZE);
			if (fComposeMemory != NULL) {
				fComposeBase = (uint8_t *)fComposeMemory->getBytesNoCopy();
				carveCopy(fComposeBase, fBase, bytes);
				hakios_tl("display: command-mode staging buffer: %llu bytes\n", bytes);
			} else hakios_tl("display: no staging buffer; command-mode presentation remains unsynchronized\n");
		}
	}
	/*
	 * The bottom picture a swap without one is composed over (stageBottom()): one frame's worth of memory of
	 * ours, taken here once the display's size is the one frames are drawn at, and let go with the service.
	 * Its cost is one memory-to-memory copy of the frame, where the replay used to read the surface itself.
	 */
	{
		uint64_t bytes = (uint64_t) fRowBytes * fPanelHeight;

		if (bytes <= STAGED_LIMIT) {
			fStagedMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, PAGE_SIZE);
		}
		if (fStagedMemory != NULL) {
			fStaged = (uint8_t *)fStagedMemory->getBytesNoCopy();
			fStagedBytes = bytes;
			hakios_tl("display: bottom picture staged in %llu bytes (%u a row); a swap without a bottom layer is composed over it\n",
			    fStagedBytes, fRowBytes);
		} else {
			hakios_tl("display: no memory for the staged bottom picture; a swap without a bottom layer is cleared to black\n");
		}
	}
	publishModes();
	publishSourceLimits(this);
	setName("disp0");
	registerService();
	return true;
}

/*
 * (the staged bottom picture is memory of this service's own: it goes with it. The service is not one that ends
 * -- nothing else here is ever freed either -- but the buffer is 11 MB, and a driver that keeps it for good
 * without a place to let it go is REVIEW-layer0-replay.md F7.)
 */
void
IOMobileFramebuffer::free(void)
{
	OSSafeReleaseNULL(fStagedMemory);
	fStaged = NULL;
	fStagedBytes = 0;
	fStagedValid = false;
	IOService::free();
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
	const uint8_t *pixels = sourcePixels(s);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (pixels + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

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

/* (blend() and over(), the pixel arithmetic of every layer drawn here, are in hakios_blend.h with the rule an
 * upper-only frame is composed by: a host test builds and checks them, devices/xiaomi-mi-10/tools/test-display-blend.py) */

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

/*
 * The splash carve-out (fb_dpu: fDpuVirt[], and the GOP frame fBase) is Device memory to this processor -- the
 * hypervisor's stage 2 says so, whatever the kernel's own mapping asks for: DC ZVA (bzero) and unaligned or vector
 * accesses there take an alignment fault (the 2026-11-10 panic in __bzero). With fb_dpu every CPU access to it goes
 * through these: naturally aligned 8-byte (else 4-byte, else byte) loads and stores, volatile so that the compiler
 * neither vectorizes nor merges them. Pixels are 4-byte aligned; rows and frames are 64-byte aligned.
 */
static void __attribute__((noinline))
carveCopy(void * to, const void * from, size_t n)
{
	uint8_t * d = (uint8_t *) to;
	const uint8_t * s = (const uint8_t *) from;

	if ((((uintptr_t) d | (uintptr_t) s) & 7) == 0) {
		volatile uint64_t * d8 = (volatile uint64_t *) d;
		const volatile uint64_t * s8 = (const volatile uint64_t *) s;
		size_t words = n / 8;

		for (size_t i = 0; i < words; i++) {
			d8[i] = s8[i];
		}
		d += words * 8;
		s += words * 8;
		n -= words * 8;
	}
	if ((((uintptr_t) d | (uintptr_t) s) & 3) == 0) {
		volatile uint32_t * d4 = (volatile uint32_t *) d;
		const volatile uint32_t * s4 = (const volatile uint32_t *) s;
		size_t words = n / 4;

		for (size_t i = 0; i < words; i++) {
			d4[i] = s4[i];
		}
		d += words * 4;
		s += words * 4;
		n -= words * 4;
	}
	for (size_t i = 0; i < n; i++) {
		((volatile uint8_t *) d)[i] = ((const volatile uint8_t *) s)[i];
	}
}

/* (hakios.gpu.cpp's HakiosGPUSurfaceDemote() copies a frame back with the GPU's lock held: the same access rules) */
extern "C" void
hakios_carve_copy(void * to, const void * from, size_t n)
{
	carveCopy(to, from, n);
}

static void __attribute__((noinline))
carveZero(void * to, size_t n)
{
	uint8_t * d = (uint8_t *) to;

	if (((uintptr_t) d & 7) == 0) {
		volatile uint64_t * d8 = (volatile uint64_t *) d;
		size_t words = n / 8;

		for (size_t i = 0; i < words; i++) {
			d8[i] = 0;
		}
		d += words * 8;
		n -= words * 8;
	}
	if (((uintptr_t) d & 3) == 0) {
		volatile uint32_t * d4 = (volatile uint32_t *) d;
		size_t words = n / 4;

		for (size_t i = 0; i < words; i++) {
			d4[i] = 0;
		}
		d += words * 4;
		n -= words * 4;
	}
	for (size_t i = 0; i < n; i++) {
		((volatile uint8_t *) d)[i] = 0;
	}
}

struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         ry0, ry1;       /* the rows of the rectangle that are on the panel: all that is looked at, whatever its size */
	bool            filter, unscaled;
	bool            over;           /* a layer above the bottom one: drawn over what is there (over()), not copied */
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow, uint32_t * rowOut)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		uint32_t * row = (uint32_t *) (fTarget + (size_t) y * fRowBytes);

		if ((fTargetDevice || fTarget == fBase) && rowOut == NULL) {
			/* Never let a low-memory fallback issue ordinary stores into the carve-out. */
			continue;
		}
		to = (fTargetDevice || fTarget == fBase) && rowOut != NULL ? rowOut : row;
		if (to != row && (fDiff || job->over)) {
			/* fDiff compares the old pixels, a layer over them is drawn onto them; rowOut is scratch, while row may be Device memory. */
			carveCopy(to + cx0, row + cx0, (size_t) (cx1 - cx0) * 4);
		}
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			if (to != row) {
				carveCopy(row + cx0, to + cx0, (size_t) (cx1 - cx0) * 4);
			}
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
					if (fDirty) {
						fDirtyPixels++;
						fDirtyLeft = (uint32_t) x < fDirtyLeft ? (uint32_t) x : fDirtyLeft;
						fDirtyRight = (uint32_t) x > fDirtyRight ? (uint32_t) x : fDirtyRight;
						fDirtyTop = (uint32_t) y < fDirtyTop ? (uint32_t) y : fDirtyTop;
						fDirtyBottom = (uint32_t) y > fDirtyBottom ? (uint32_t) y : fDirtyBottom;
					}
				}
			}
		}
		if (job->over && !filter && sx == 0 && sw == px1 - px0) {
			/*
			 * The layer's columns are the panel's, one for one (no scaling, or a scaling that keeps every
			 * column): the row is blended in one pass, two pixels at a time where it can be, with a whole row
			 * of transparent or opaque pixels decided before any of it is touched (hakios_over_row()). Maps'
			 * map and controls are such layers, 2.7 million pixels of the frame otherwise.
			 */
			hakios_over_row(to + cx0, from + (cx0 - px0), cx1 - cx0);
		} else if (job->over) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t c = fColumn[x], w = filter ? fColumnWeight[x] : 0, a = from[c];
				uint32_t s = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
				uint32_t alpha = s >> 24;

				if (alpha == 0 && (s & 0x00ffffffu) == 0) {
					continue;               /* (over() of such a pixel is the one under it, bit for bit) */
				}
				to[x] = alpha == 255 ? s : over(s, to[x]);
			}
		} else if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else if (fRuns != 0) {
			/* (runs: worked out in drawLayer(); 1125 -> 1080 is 45 copies of 24 pixels a row) */
			for (uint32_t r = 0; r < fRuns; r++) {
				memcpy(to + fRunDst[r], from + fRunSrc[r], (size_t) fRunLen[r] * 4);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
		if (to != row) {
			carveCopy(row + cx0, to + cx0, (size_t) (cx1 - cx0) * 4);
		}
	}
}

/* Rows ROW_CHUNK at a time, for as long as there are any: every drawer of a frame (fb_threads) runs this. */
void
IOMobileFramebuffer::drawChunks(const RowJob * job, uint32_t * blendRow, uint32_t * rowOut)
{
	for (;;) {
		int32_t y0 = OSAddAtomic(ROW_CHUNK, &fHelperNext);

		if (y0 >= job->ry1) {
			return;
		}
		drawRows(job, y0, y0 + ROW_CHUNK < job->ry1 ? y0 + ROW_CHUNK : job->ry1, blendRow, rowOut);
	}
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
	/* (only the next frame's work: the presenter counts it among fHelpersBusy from then on, never before --
	 * REVIEW-maps-compose-perf.md B1) */
	done = fb->fHelperGeneration;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* rows a chunk at a time, as the presenter does: a helper on a slow (A55) core takes fewer */
		fb->drawChunks(job, fb->fHelperBlend[index], index + 1 < 8 ? fb->fRowOut[index + 1] : NULL);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0). False: nothing of it was drawn (its
 * rectangle, or the surface it names, is not one to read: the frame keeps what it had).
 */
bool
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool over, const uint8_t *pixels)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

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
		return false;
	}
	/* the rows and columns named must be in the surface's memory, whatever stride and size it was made with */
	if (!surfaceHolds(s, (uint32_t) (sy + sh), (uint32_t) (sx + sw))) {
		return false;
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
		return false;
	}
	px0 = (int32_t) qx0;
	py0 = (int32_t) qy0;
	px1 = (int32_t) qx1;
	py1 = (int32_t) qy1;
	if (px1 <= px0 || py1 <= py0) {
		return false;
	}
	/* nothing of it on the panel: nothing to draw (and the columns below are only worked out for ones that are) */
	if (px1 <= 0 || px0 >= (int32_t) fPanelWidth || py1 <= 0 || py0 >= (int32_t) fPanelHeight) {
		return false;
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
	fRuns = 0;
	if (!filter) {
		for (int32_t x = cx0; x < cx1; x++) {
			if (fRuns != 0 && fRunDst[fRuns - 1] + fRunLen[fRuns - 1] == (uint32_t) x &&
			    fRunSrc[fRuns - 1] + fRunLen[fRuns - 1] == fColumn[x]) {
				fRunLen[fRuns - 1]++;
			} else {
				fRunDst[fRuns] = (uint32_t) x;
				fRunSrc[fRuns] = fColumn[x];
				fRunLen[fRuns] = 1;
				fRuns++;
			}
		}
		/* short runs (an enlargement) are faster pixel by pixel */
		if ((uint64_t) fRuns * 8 > (uint64_t) (cx1 - cx0)) {
			fRuns = 0;
		}
	}
	job.pixels = pixels != NULL ? pixels : sourcePixels(s);
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
	job.unscaled = !over && px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.over = over;
	/*
	 * Shared out between the presenter and the helpers: the rows are claimed a chunk at a time, so a helper is
	 * simply one more drawer. Waiting for every one of fb_threads - 1 to have started (fHelpersStarted + 1 <
	 * fThreads) made a frame drawn before the last of them was up -- and, if one of them never was, every frame --
	 * one thread's work: the "35 ms for one screen of over()" of Maps' composed frames. One helper up is enough to
	 * share, and the busy count stays the number of helper threads there are (each wakes on the generation and
	 * says when it is done, whether it found rows to take or not).
	 */
	IOLockLock(fHelperLock);
	unsigned helpers = fHelpersStarted;
	IOLockUnlock(fHelperLock);
	if (fThreads <= 1 || fDiff || helpers == 0) {
		drawRows(&job, job.ry0, job.ry1, fBlendRow, fRowOut[0]);
		return true;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelperNext = job.ry0;
	/* the helpers that are up: one that never started is never waited for (B1); each of these wakes on the
	 * generation below and says when it is done */
	fHelpersBusy = fHelpersStarted;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawChunks(&job, fBlendRow, fRowOut[0]);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
	return true;
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
	}
	OSIncrementAtomic(&fStIn);
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			OSIncrementAtomic(&fStLost);
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	fSwapIn[slot] = now;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	uint64_t ticket = ++fNextTicket;
	IOLockWakeup(fSwapLock, fSwap, true);
	/*
	 * fb_dpu=4: a swap of one aliased surface is a flip; the CPU reads none of its pixels, so CoreAnimation goes on
	 * at once instead of waiting here for its GPU work and the flip (v10c: 3.2 ms a frame, and the next frame's
	 * submission waited 7.1 ms more for the latch -- half of its render thread's frame time). It is held back by
	 * SwapWait instead, which it calls for this swap before it commits its next frame (waitShown()), and its GPU work
	 * into the surface still on the panel waits for the latch (MiniIOMobileFramebufferOffscreen()).
	 */
	bool flipOnly = fZero && fRecBase == NULL && fSwapSurfaces[slot][0] != NULL;
	for (unsigned layer = 1; layer < SWAP_LAYERS; layer++) {
		flipOnly = flipOnly && swapWord(swap, SWAP_SURFACE(layer)) == 0;
	}
	if (flipOnly) {
		flipOnly = fBound[0] == fSwapSurfaces[slot][0] || fBound[1] == fSwapSurfaces[slot][0] || fBound[2] == fSwapSurfaces[slot][0];
	}
	/* A source surface must not be handed back to CoreAnimation while the CPU
	 * is still scaling it into the command-mode staging buffer. Once staged,
	 * the panel wait no longer needs the source and can overlap the next render.
	 */
	while (holdsSource() && !flipOnly && fCompletedTicket < ticket) {
		if (IOLockSleepDeadline(fSwapLock, &fCompletedTicket, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fCompletedTicket < ticket) {
			hakios_tl("display: staged swap completion timed out\n");
			break;
		}
	}
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::completeSwap(void)
{
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	fCompletedTicket++;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockWakeup(fSwapLock, &fCompletedTicket, false);
	IOLockUnlock(fSwapLock);
}

/*
 * fb_dpu=4, SwapWait: until swap `id` is flipped to (show() done with it). CoreAnimation calls it for its last swap
 * before it commits the GPU work of the next frame, which goes into the surface of the swap before: with SwapEnd
 * returning at once (present()), that surface could still be queued or pending for the panel (v11b: 2266 frames,
 * shown out of order). Once this flip is programmed, that surface is the one shown with a flip away from it pending,
 * and its submissions wait for the latch (MiniIOMobileFramebufferOffscreen()).
 */
void
IOMobileFramebuffer::waitShown(uint32_t id)
{
	uint64_t deadline;

	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	IOLockLock(fSwapLock);
	while (fZero && (int32_t) (id - fShownID) > 0 && (int32_t) (id - fSwapID) <= 0) {
		if (IOLockSleepDeadline(fSwapLock, &fShownID, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
			break;
		}
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
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		fShowIn = fSwapIn[fSwapFirst];
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		IOLockLock(fSwapLock);
		fShownID = swapWord(swap, SWAP_ID);
		IOLockWakeup(fSwapLock, &fShownID, false);
		IOLockUnlock(fSwapLock);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		uint64_t clearing = mach_absolute_time();
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
		clearing = mach_absolute_time() - clearing;
		fStClear += clearing;
		fStClearMax = clearing > fStClearMax ? clearing : fStClearMax;
		fStClears++;
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report, stagedReleased = false;
	const bool holds = holdsSource();       /* the source is ours until it is scaled (staged copy, fb_dpu) */
	int dpuTarget = -1;
	bool inPlace = false, hardware = false, hardwareUsed = false;
	bool stageCopy = false;                 /* the layers were composed into fCopyStage: one copy into the frame, then a flip */
	bool inPlaceStride = false;             /* in place, into a frame whose alias stride was just put back */
	int hwBank = -1;
	struct umi_dpu_layer hwLayers[4];
	unsigned hwCount = 0;
	uint64_t upload = 0, hwWaiting = 0;
	bool zero = false;                      /* fb_dpu=4: this swap is a flip to its own surface's frame */
	uint64_t drawStart, cpuComposition = 0;
	bool gpuReady = true;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			gpuReady = false;

		}
	}
	if (!gpuReady) {
		hakios_tl("fbcompose-failure: reason=fence swap=%u\n", swapWord(swap, SWAP_ID));
		OSIncrementAtomic(&fStLost);
		completeSwap();
		return;
	}
	/* Drain producer fences before retiring a skipped swap: clients may
	 * reuse its surfaces as soon as completion is visible. */
	if (umi_pm_screen_off()) {
		/* (the display is ending for now: nothing is kept of the last picture it showed) */
		forgetStaged();
		umi_pm_frame_skipped();
		/* Retire queue credits and let the presenter release its surfaces. */
		completeSwap();
		return;
	}
	fStatWaiting += mach_absolute_time() - began;
	fStGpu += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (hakios_display_scan() != NULL && !hakios_display_command_mode()) {
		/*
		 * Where the copy may start (fb_scan): every band of rows (fThreads of them, drawn side by side, each top
		 * to bottom in about fDrawNs) has to stay behind the scan in this refresh -- written after the scan has
		 * passed it, so it is first shown in the next -- and to be done before the next refresh's scan gets to
		 * it. The last band starts below the scan and must not catch it up; the first must be done before the
		 * scan comes round to its last row. That is a window of lines for the scan to be in when the copy starts:
		 *   from  max(shown (n-1)/n, shown - D/line) and   up to  lines + shown/n - D/line.
		 * Waited for, a frame and a half at most: a frame is never held longer than that for it.
		 */
		const struct hakios_display_scan * scan = hakios_display_scan();
		uint64_t d = fDrawNs != 0 ? fDrawNs : scan->frame_ns / 3, dl = d / scan->line_ns;
		uint32_t n = fThreads != 0 ? fThreads : 1, S = scan->shown, T = scan->lines;
		uint64_t lo = (uint64_t) S * (n - 1) / n, hi = (uint64_t) T + S / n;

		lo = S > dl && S - dl > lo ? S - dl : lo;
		lo += 16;
		hi = hi > dl + 16 ? hi - dl - 16 : 0;
		if (hi > T) {
			hi = T;
		}
		if (lo >= hi) {
			lo = S;                         /* (a copy too slow for any window: at least start in the blanking) */
			hi = T;
		}
		fStatScanWaits++;
		if (!hakios_display_wait_lines((uint32_t) lo, (uint32_t) hi, scan->frame_ns * 3 / 2)) {
			fStatScanMissed++;
		}
		fBeat = began = mach_absolute_time();
	} else if (!hakios_display_command_mode() && gOnTheBeat) {
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
	/* Staged sources stay owned until the CPU has consumed every layer. */
	if (!holds) completeSwap();
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
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	if (fDirty) {
		fDirtyPixels = 0;
		fDirtyLeft = fDirtyTop = ~0u;
		fDirtyRight = fDirtyBottom = 0;
	}
	fTarget = fComposeBase != NULL ? fComposeBase : fBase;
	fTargetDevice = false;
	/*
	 * Whether this swap's own bottom layer is drawn over the whole of the display: then the frame is that layer
	 * and nothing has to be carried into it. If not -- or if the swap has no bottom layer at all, which the
	 * layer loop composes over the staged bottom picture -- the frame is started from the staged bottom picture
	 * rather than from the frame before it, which already holds the layers above (hakios_blend.h).
	 */
	uint32_t sidBottom = swapWord(swap, SWAP_SURFACE(0));
	int32_t d0[4];
	bool whole, fromStaged = false;

	memcpy(d0, swap + SWAP_DEST(0), sizeof(d0));
	whole = sidBottom != 0 && surfaces[0] != NULL && surfaces[0]->info.bytesPerElement == 4 &&
	    surfaces[0]->info.planeCount == 0 && hakios_covers_display(d0, (int32_t) fWidth, (int32_t) fHeight);
	if (fHardwareCompose && gpuReady && fGraphics && fRecBase == NULL && !fDiff && !fDirty) {
		hwBank = hardwarePrepare(swap, surfaces, hwLayers, &hwCount, &upload, &hwWaiting);
		fStWait += hwWaiting; fStWaitMax = MAX(fStWaitMax, hwWaiting);
		hardware = hwBank >= 0;
	}
	if (fDpuBuffers != 0 && !hardware) {
		/* a frame neither scanned nor waiting to be: with three there always is one */
		int pending = hakios_display_dpu_poll(), scanned = hakios_display_dpu_shown();

		if (!fZero) {
			/* (fb_dpu=4: zero copy left earlier -- an alias check that failed, or a mapping that would not go
			 * back: let zeroSlot() look again on this swap, once a delay and a try budget allow it) */
			(void) zeroRecover();
		}
		if (fZero && (dpuTarget = zeroSlot(swap, surfaces, pending, scanned)) >= 0) {
			zero = true;
		}
		for (unsigned i = 0; i < fDpuBuffers && dpuTarget < 0; i++) {
			if ((int) i != pending && (int) i != scanned && !(fZero && fBound[i] != NULL)) {
				dpuTarget = (int) i;
			}
		}
		bool black = swapWord(swap, SWAP_SURFACE(0)) == 0 && (set & 1) && fGraphics && swapWord(swap, SWAP_SURFACE(1)) == 0 &&
		    swapWord(swap, SWAP_SURFACE(2)) == 0 && swapWord(swap, SWAP_SURFACE(3)) == 0;
		if (dpuTarget < 0 && fZero && black && scanned >= 0 && scanned < 3 && fSlotBlack[scanned]) {
			dpuTarget = scanned;            /* (black again: what is shown already is; nothing to draw or flip) */
			zero = true;
		}
		/* Keep both aliases. The sole copied slot can be pending or scanned.
		 * Retire its pending flip, then compose privately and update it only
		 * during DMA idle, rather than evicting a GPU render target.
		 * Composing into a frame that is neither shown nor on its way there and flipping to it needs no DMA
		 * idle interval; copying into the frame on the panel does, and that interval is not always there, so
		 * a frame free and not shown is taken first and the copy is what is left when there is no other.
		 * (Before v29f the lowest free frame was taken even when it was the shown one, and a run of such
		 * frames could lose every one of itself: see the in-place branch below.) */
		if (dpuTarget < 0 && fZero) {
			if (pending >= 0 && hakios_display_dpu_flip((unsigned)pending, DPU_INPLACE_LIMIT_NS)) {
				scanned = hakios_display_dpu_shown();
				pending = -1;
			}
			for (unsigned pass = 0; pass < 2 && dpuTarget < 0; pass++) {
				for (unsigned i = 0; i < fDpuBuffers && dpuTarget < 0; i++) {
					if (fBound[i] == NULL && (int)i != pending && (pass != 0 || (int)i != scanned)) {
						dpuTarget = (int)i;
						inPlace = (int)i == scanned;
					}
				}
			}
			/*
			 * (a2) The only frame free of aliases is the one the panel is fetching (or there is none): an alias
			 * that is not on the panel gives its frame back instead -- its picture copied back into its own
			 * memory, its GPU mappings its own again, both of the last two steps checked -- and this frame is
			 * composed into that one and flipped to at the next TE. Two frames stay free of aliases for a
			 * moment (DEMOTE_HOLD_NS, zeroSlot()), so the next copied swap does not demote again.
			 * /var/db/hakios-display/inplace-demote-off turns this off: the in-place branch below is then the
			 * last resort, as it is when nothing can be given back.
			 */
			if ((dpuTarget < 0 || inPlace) && hakios_display_inplace_demote()) {
				uint64_t demoteAt = mach_absolute_time(), demoteBudget;

				/* (all of the demotions of one swap together stay inside DEMOTE_BUDGET_MS: each attempt
				 * gets what is left of it, so a frame that cannot be given back cannot stretch fLock)
				 * Two rounds: first a surface whose GPU work is already done (no fence waited for at all, the
				 * frame is given back in the time of the copy back), then the bounded fence of the older
				 * scheme. The demotion used to be the second round only, 6 ms of fence against a surface
				 * CoreAnimation is still drawing into, which every composed frame of Maps' failed
				 * (demote_abort, ~2 a frame) and left it with no frame but the one the panel fetches. */
				nanoseconds_to_absolutetime(DEMOTE_BUDGET_MS * NSEC_PER_MSEC, &demoteBudget);
				for (unsigned round = 0; round < 2 && (dpuTarget < 0 || inPlace); round++) {
					for (unsigned i = 0; i < fDpuBuffers; i++) {
						uint64_t left, left_ms, fence_ms;

						if ((int) i == scanned || (int) i == pending) {
							continue;
						}
						left = mach_absolute_time() - demoteAt;
						if (left >= demoteBudget) {
							break;
						}
						absolutetime_to_nanoseconds(demoteBudget - left, &left_ms);
						left_ms /= NSEC_PER_MSEC;
						if (left_ms < 2) {
							break;          /* (too little left for a fence and the copy) */
						}
						fence_ms = round == 0 ? 0 : MIN(DEMOTE_FENCE_MS, left_ms);
						if (zeroDemote(i, fence_ms, left_ms, round == 0)) {
							dpuTarget = (int) i;
							inPlace = false;
							break;
						}
					}
				}
			}
		}
		/* (fb_dpu=4: only a frame no surface is aliased onto may be composed into, so the flip that frees the
		 * one scanned before is no use when what is shown is an alias: that frame stays the GPU's) */
		if (dpuTarget < 0 && pending >= 0 &&
		    (!fZero || (scanned >= 0 && scanned < (int) fDpuBuffers && fBound[scanned] == NULL)) &&
		    hakios_display_dpu_flip((unsigned) pending, DPU_FLIP_LIMIT_NS)) {
			dpuTarget = scanned;            /* (two frames only: the one scanned before is free once the flip is latched) */
		}
		if (dpuTarget >= 0 && !zero) {
			if (!inPlace) fSlotBlack[dpuTarget] = black;
			if (fDpuScaled && fSlotStride[dpuTarget] != fRowBytes) {
				/* A frame an alias gave back keeps that surface's stride (zeroSlot() sets it on the frame it
				 * aliases onto); this frame is composed at its own. For an in-place target that is not a
				 * cosmetic difference: the copy writes fRowBytes rows into a frame the pipe may be fetching
				 * with the surface's stride (a diagonal skew, black bands). Said once for each frame. */
				if (inPlace && !(fStrideSaid & (1u << dpuTarget))) {
					fStrideSaid |= 1u << dpuTarget;
					hakios_tl("fbpath: in-place into frame %d with stride %u, the frame is composed at %u\n",
					    dpuTarget, fSlotStride[dpuTarget], fRowBytes);
				}
				inPlaceStride = inPlace;
				fSlotStride[dpuTarget] = fRowBytes;
				hakios_display_dpu_stride((unsigned) dpuTarget, fRowBytes);
			}
		}
		if (dpuTarget < 0) {
			dpuFallback("no scanout frame is free", NULL);
		} else if (!zero) {
			/*
			 * The layers are composed into the cached staging buffer, not pixel by pixel into the frame the
			 * panel fetches through the display's DMA: one carveCopy() of the whole frame into a scanout frame
			 * nothing is showing follows the layer loop, and the pipes are flipped to it. Drawing into the
			 * carve-out with a row's scratch and copying every row out cost two Device-memory copies a row;
			 * against a frame that is not the one on the panel there is no copy window to find either (the
			 * frame is only scanned after the flip). /var/db/hakios-display/compose-off, or no staging memory
			 * (fb_dpu=4 off), keeps the old way: rows straight into the frame.
			 */
			stageCopy = !inPlace && hakios_display_compose_copies() && fCopyStage != NULL;
			fTarget = inPlace || stageCopy ? fCopyStage : (uint8_t *) fDpuVirt[dpuTarget];
			fTargetDevice = !inPlace && !stageCopy;
			hakios_cpufreq_boost(24);       /* (the scaling is now all there is between the GPU and the flip) */
			scanned = hakios_display_dpu_shown();
			uint64_t preserving = mach_absolute_time();
			/* (a swap whose bottom layer does not cover the display starts from the staged bottom picture, never
			 * from what is shown: see below, where the frame is not one of fb_dpu's) */
			if (!whole && sidBottom != 0) {
				fromStaged = replayBottom();
			}
			if (!whole && sidBottom != 0 && !fromStaged && scanned >= 4) hardwareReplay();
			if (!whole && sidBottom != 0 && !fromStaged && scanned >= 0 && (unsigned) scanned < fDpuBuffers) {
				uint32_t from = fDpuScaled ? fSlotStride[scanned] : fRowBytes;

				if (from == fRowBytes) {
					carveCopy(fTarget, (const void *) fDpuVirt[scanned], (size_t) fRowBytes * fPanelHeight);
				} else {
					for (uint32_t y = 0; y < fPanelHeight; y++) {
						carveCopy(fTarget + (size_t) y * fRowBytes, (const void *) (fDpuVirt[scanned] + (size_t) y * from),
						    (size_t) fPanelWidth * 4);
					}
				}
			}
			cpuComposition += mach_absolute_time() - preserving;
		}
	}
	/*
	 * In-place frames come in runs, and a run is the picture written into the frame the panel is fetching
	 * (INVESTIGATION-inplace-tearing.md): one line when such a run starts -- with why this swap had no other
	 * frame to go to, which no other line said -- and one when it ends. Nothing per frame.
	 */
	if (inPlace != fPathInplace) {
		if (inPlace) {
			unsigned aliased = 0, swapLayers = 0;

			fPathInplace = true;
			fInplaceFrames = 0;
			for (unsigned i = 0; i < fDpuBuffers; i++) {
				aliased += fBound[i] != NULL;
			}
			for (unsigned l = 0; l < SWAP_LAYERS; l++) {
				swapLayers += surfaces[l] != NULL;
			}
			hakios_tl("fbpath: in-place from swap %u: %u layer(s), %u aliased frame(s), the free frame %d is the shown one\n",
			    swapWord(swap, SWAP_ID), swapLayers, aliased, dpuTarget);
		} else {
			fPathInplace = false;
			hakios_tl("fbpath: in-place ended after %u frames (swap %u)\n", fInplaceFrames, swapWord(swap, SWAP_ID));
		}
	}
	/*
	 * A frame whose bottom layer does not cover the display is started from the staged bottom picture, not from
	 * what the frame holds (the frame before, layers above and all: blending the layers above into that takes
	 * the picture under them away, hakios_blend.h). fb_dpu's frames were given it above; on the CPU copy path
	 * the frame is fComposeBase, still holding the frame before -- the one case that path had no answer for.
	 * A swap without a bottom layer at all is composed over it by the layer loop below, which draws it first.
	 */
	if (fDpuBuffers == 0 && !hardware && !zero && !whole && sidBottom != 0) {
		uint64_t preserving = mach_absolute_time();

		fromStaged = replayBottom();
		cpuComposition += mach_absolute_time() - preserving;
	}
	drawStart = mach_absolute_time();
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
			/*
			 * The bottom layer taken away. A swap that still has layers above it is composed over the staged
			 * bottom picture -- the last swap's layer 0 as it was drawn into a frame of ours, copied then, so
			 * that nothing is read out of a surface its maker may be drawing into again (REVIEW-layer0-replay.md
			 * F1) and nothing is waited for here (F2): that copy is drawn into the frame straight, and the
			 * layers above are then blended over it once. Not over the frame's own last output -- which already
			 * holds those layers -- because that takes (1 - alpha) of the picture under them away every frame:
			 * eight frames of CoreAnimation's half-transparent black dimming are opaque black, and wherever the
			 * layers above are transparent the frame before stays as a trail (Maps; INVESTIGATION-alpha-black.md
			 * H1, hakios_blend.h). v29i kept that picture; before it the frame was cleared, which blacked the map.
			 * With no bottom picture staged the frame is cleared, as before v29i, and counted with the reason
			 * (`fb-bottom:`, every 2 s). A swap with no layers at all is the black one and is cleared the same
			 * way (a frame flipped to that fSlotBlack[] says is black already is not touched again). One line an
			 * episode, like the ones above and below.
			 */
			if (layer == 0) {
				unsigned others = 0;
				int base = hakios_upper_only_base(fStagedValid);
				bool clearing = false;

				for (unsigned l = 1; l < SWAP_LAYERS; l++) {
					others += surfaces[l] != NULL;
				}
				if (!fBottomGone) {
					fBottomGone = true;
					fStNoBottom++;
					fStAboveFrames = 0;
					fAboveCleared = 0;
					fAboveFirst = swapWord(swap, SWAP_ID);
					if (others == 0) {
						hakios_tl("fbcompose: no bottom layer (0 other layer(s)): frame cleared\n");
					} else {
						/* (the first layer above: what it is and where it goes -- one line an episode, and the
						 * one that says whether that layer covers the panel and has an alpha to lose; the rects
						 * are the client's numbers, printed clamped: hakios_tl's line is 192 bytes) */
						const char * how = base == HAKIOS_BASE_BOTTOM ? "bottom layer replayed" :
						    fStaged == NULL ? "frame cleared to black (no staging memory)" :
						    fStagedDropped ? "frame cleared to black (the staged bottom was let go)" :
						    "frame cleared to black (nothing staged yet)";
						for (unsigned l = 1; l < SWAP_LAYERS; l++) {
							MiniIOSurface * u = surfaces[l];
							int32_t udst[4];

							if (u == NULL) {
								continue;
							}
							memcpy(udst, swap + SWAP_DEST(l), sizeof(udst));
							hakios_tl("fbcompose: no bottom layer (%u other layer(s)): %s; upper %d %dx%d '%c%c%c%c' "
							    "stride %d dst %d,%d %dx%d\n", others, how,
							    hakios_print_num(swapWord(swap, SWAP_SURFACE(l))), hakios_print_num(u->info.width),
							    hakios_print_num(u->info.height),
							    (int) (u->info.pixelFormat >> 24 & 0xff), (int) (u->info.pixelFormat >> 16 & 0xff),
							    (int) (u->info.pixelFormat >> 8 & 0xff), (int) (u->info.pixelFormat & 0xff),
							    hakios_print_num(u->info.bytesPerRow), hakios_print_num(udst[0]), hakios_print_num(udst[1]),
							    hakios_print_num(udst[2]), hakios_print_num(udst[3]));
							break;
						}
					}
				}
				/*
				 * (zero and hardware cannot be set here: zeroSlot() and hardwarePrepare() both refuse a swap whose
				 * layer 0 is not given. The guard stays, because what is below it is false for them.)
				 */
				if (!zero && !hardware) {
					if (base == HAKIOS_BASE_BOTTOM) {
						/* (the bottom picture back, before the layers above: each of them is blended over it once) */
						(void) replayBottom();
					} else if (others != 0 || (fGraphics && (set & 1))) {
						/* (nothing to replay, or the all-black swap: the frame is cleared, as it always was. With
						 * layers above it is cleared even before the first UI frame: they would be blended over
						 * whatever the frame held) */
						for (uint32_t y = 0; y < fPanelHeight; y++) {
							if (fTargetDevice || fTarget == fBase) {
								carveZero(fTarget + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
							} else {
								bzero(fTarget + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
							}
						}
						clearing = true;
					}
				}
				if (others != 0) {
					fStAboveFrames++;
					if (fStats) {
						fStAboveOnly++;
					}
					/* (counted where it happened, not where it was decided, and the window's counters only while
					 * there is a window to report them in: F9 of REVIEW-layer0-replay.md) */
					if (clearing) {
						fAboveCleared++;
						if (fStats) {
							if (fStagedDropped) {
								fStClearGone++;
							} else {
								fStClearNever++;
							}
						}
					}
				}
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
				/* the first picture: the kernel's text console (and the boot log) must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
				/* the boot log stops for good; when the frame it wrote into is the one drawn into, it is blanked first
				 * (a layer that does not cover the display starts from what is shown: no stale text under the UI) */
				hakios_bootlog_stop(fDpuBuffers == 0 && !hardware);
			}
			bool layerDrawn = false;

			if (!zero && !hardware) {
				/* (Maps: its map in layer 0, CoreAnimation's controls over it in 1) */
				layerDrawn = drawLayer(s, src, dst, layer != 0);
			}
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
				/*
				 * The frame holds this layer and nothing above it yet: that is the bottom picture the swaps
				 * that come without one are composed over, and it is copied out here -- before the layers
				 * above are blended -- because the surface itself is no copy: its maker may draw the next map
				 * frame into it at any moment (REVIEW-layer0-replay.md F1, hakios_blend.h). A layer that does
				 * not cover the display is kept only when the frame was started from the staged picture (the
				 * frame is then that picture with this layer's part of it brought up to date, F3). On the
				 * zero-copy and hardware paths the layer is not drawn into the frame at all: drawn into the
				 * staged copy instead, when it covers the display.
				 */
				/*
				 * Only a swap that has layers above this one is staged: swaps without a bottom layer come after
				 * those (Maps). A lone layer 0 -- every app frame, and on the zero-copy path the copy would be a
				 * whole CPU drawLayer() per frame, the very cost zero-copy exists to avoid -- only makes the
				 * staged picture stale, so it is let go (an episode after it starts from black, not from an old
				 * map).
				 */
				if ((set & ~1u) == 0) {
					fStagedValid = false;
				} else if (layerDrawn) {
					if (hakios_stage_bottom(hakios_covers_display(dst, (int32_t) fWidth, (int32_t) fHeight), fromStaged)) {
						(void) stageBottom();
					}
				} else if (hakios_stage_bottom(hakios_covers_display(dst, (int32_t) fWidth, (int32_t) fHeight), false)) {
					(void) stageLayer(s, src, dst);
				}
				/* (the episode of swaps without a bottom layer, ended -- only now that this one could be staged:
				 * one that could not leaves it going; F5 of REVIEW-layer0-replay.md) */
				if (fBottomGone) {
					fBottomGone = false;
					if (fStAboveFrames != 0) {
						hakios_tl("fbcompose: bottom layer back after %u upper-only frame(s) (swap %u..%u), "
						    "%u of them cleared\n", fStAboveFrames, fAboveFirst, swapWord(swap, SWAP_ID), fAboveCleared);
					}
				}
			}
		}
	}
	{
		uint64_t scaled = cpuComposition += mach_absolute_time() - drawStart;

		fStDraw += scaled;
		fStDrawMax = scaled > fStDrawMax ? scaled : fStDrawMax;
	}
	if (stageCopy) {
		/*
		 * The composed frame, in one copy: fCopyStage is ordinary cached memory (memcpy, no alignment rules)
		 * and the scanout frame is Device memory to this CPU (carveCopy, aligned accesses only). The frame is
		 * neither shown nor pending -- that is what made it available -- so nothing has to be waited for, and
		 * the flip below puts it on the panel at the next TE.
		 */
		uint64_t copying = mach_absolute_time();

		carveCopy((void *) fDpuVirt[dpuTarget], fCopyStage, (size_t) fRowBytes * fPanelHeight);
		__asm__ volatile("dsb sy" ::: "memory");
		fStCopy += mach_absolute_time() - copying;
		fStCopyMax = MAX(fStCopyMax, mach_absolute_time() - copying);
		fStComposed++;
	}
	unsigned changedSamples = 0;
	bool sampleComparable = false;
	if (fStats && dpuTarget >= 0 && gpuReady && !inPlace) {
		uint64_t sampling = mach_absolute_time();
		uint32_t stride = fDpuScaled ? fSlotStride[dpuTarget] : fRowBytes;
		umi_frame_sample((const volatile uint8_t *)fDpuVirt[dpuTarget], fPanelWidth, fPanelHeight, stride, gFrameSampleNext);
		sampleComparable = fSampleValid && fSampleWidth == fPanelWidth && fSampleHeight == fPanelHeight;
		if (sampleComparable) changedSamples = umi_frame_sample_changes(gFrameSample, gFrameSampleNext);
		sampling = mach_absolute_time() - sampling;
		fSampleTime += sampling;
		fSampleMax = MAX(fSampleMax, sampling);
	}
	if (hardware) {
		if (hakios_display_compose((unsigned)hwBank, hwLayers, hwCount)) {
			hardwareRemember(swap, surfaces, hwBank);
			hardwareUsed = true;
			fStHardware++;
			fStShown++;
			fSampleUnknown++;
			fSampleValid = false; /* a source hash is not the hardware-composed output */
			shown = mach_absolute_time();
		} else {
			hakios_tl("fbcompose-failure: reason=commit\n");
			fHardwareCompose = false;
			fStMissed++;
		}
		completeSwap(); /* immutable bank owns all pixels before producer reuse */
		stagedReleased = true;
	} else if (inPlace) {
		/* Last resort: no frame is free (not aliased, not the one the panel is fetching -- the demotion above
		 * could not make one), so the frame composed into fCopyStage has to be written into the one the panel
		 * is fetching, while the panel's transfer is not reading it. That interval comes and goes with each
		 * transfer, and it can stay shut for good -- the transfers are started by commits, and a frame copied
		 * in place commits nothing (v29f: after a few of them the interval was never seen again, every frame
		 * waited out the whole limit and none was shown, which left Maps at 6-8 fps with a still map).
		 * So: the panel's frame is committed again to start a transfer and the wait tried once more, twice; if
		 * the interval is shut still the frame is written into the panel's frame anyway -- the panel fetches it
		 * with its next transfer, so one frame may be seen torn rather than not at all. (v29i: it was committed
		 * and waited for once; a second wait halves the frames that had to be written blind.)
		 * A frame is not dropped here, and fb_dpu is not given up: demoting would take the zero copy away from
		 * the single-layer frames that do reach the panel at 50/s. */
		if (fRecBase == NULL) { completeSwap(); stagedReleased = true; }
		fInplaceFrames++;
		uint64_t waiting = mach_absolute_time();
		bool window = hakios_display_wait_copy(fCopyNs, DPU_INPLACE_LIMIT_NS);
		if (!window && hakios_display_dpu_refresh()) {
			window = hakios_display_wait_copy(fCopyNs, DPU_INPLACE_LIMIT_NS);
			if (!window && hakios_display_dpu_refresh()) {
				window = hakios_display_wait_copy(fCopyNs, DPU_INPLACE_LIMIT_NS);
			}
		}
		uint64_t copying = mach_absolute_time();
		carveCopy((void *)fDpuVirt[dpuTarget], fCopyStage, (size_t)fRowBytes * fPanelHeight);
		__asm__ volatile("dsb sy" ::: "memory");
		uint64_t elapsed = mach_absolute_time() - copying, ns;
		absolutetime_to_nanoseconds(elapsed, &ns);
		/* The window check's budget is the largest of the last few copies. A decaying maximum (MAX(ns,
		 * fCopyNs * 7/8), to v29h) could come to be under what a copy really takes and have every window check
		 * refuse it: "no room left after the transfer", 195 times in one run, and frames written blind.
		 * The largest of the last COPY_RECENT copies follows a slow copy in one, and forgets a budget learnt
		 * too large in COPY_RECENT of them. */
		fCopyRecent[fCopyNext++ % COPY_RECENT] = ns > 0xffffffffu ? 0xffffffffu : (uint32_t) ns;
		fCopyNs = 0;
		for (unsigned i = 0; i < COPY_RECENT; i++) {
			fCopyNs = MAX(fCopyNs, (uint64_t) fCopyRecent[i]);
		}
		fStCopy += elapsed; fStCopyMax = MAX(fStCopyMax, elapsed);
		fStInplace++; fStShown++;
		fSlotBlack[dpuTarget] = swapWord(swap, SWAP_SURFACE(0)) == 0;
		OSIncrementAtomic64(&gCopiedPresents);
		hardwareRemember(NULL, NULL, -1);
		shown = copying;
		fSampleValid = false;
		/* (a flip pending at the selection may have latched while the window was waited for: the frame copied
		 * into is not the one on the panel any more, and only a flip puts it there) */
		if (hakios_display_dpu_shown() != dpuTarget && hakios_display_dpu_poll() < 0) {
			if (hakios_display_dpu_flip((unsigned) dpuTarget, DPU_INPLACE_LIMIT_NS)) {
				fStFlips++;
			}
		}
		if (inPlaceStride && hakios_display_dpu_shown() == dpuTarget && hakios_display_dpu_poll() < 0) {
			/* (the frame is the one on the panel, and it was being fetched with the stride an alias left on
			 * it: fetch it again, with the stride it was just written at) */
			(void) hakios_display_dpu_refresh();
		}
		if (!window) {
			fStInplaceLate++;
			fStLateWin++;
			/* (a commit after the copy too: with no transfer running nothing would fetch what was just written) */
			(void) hakios_display_dpu_refresh();
			if (!fZeroSlow) {
				fZeroSlow = true;
				hakios_tl("fbzero-slow: frame %u copied into scanout frame %d with no idle interval (%u so far); "
				    "the panel's frame is committed again each time\n", swapWord(swap, SWAP_ID), dpuTarget,
				    fStInplaceLate);
			}
		} else if (fZeroSlow) {
			fZeroSlow = false;
		}
		elapsed = mach_absolute_time() - waiting;
		fStWait += elapsed; fStWaitMax = MAX(fStWaitMax, elapsed);
	} else if (dpuTarget >= 0) {
		/* fb_dpu: every source pixel is consumed; the frame goes to the panel by a flip at the next TE */
		if (fRecBase == NULL && !zero) {
			completeSwap();
			stagedReleased = true;
		}
		uint64_t flipping = mach_absolute_time();

		/*
		 * fb_dpu=4: the surface itself is flipped to, and the swap completes as soon as the flip is programmed.
		 * CoreAnimation may then draw into the surface still on the panel until the next TE: its GPU submissions
		 * naming it wait for the latch (MiniIOMobileFramebufferOffscreen(), hakios.gpu.cpp), not this SwapEnd.
		 */
		if (hakios_display_dpu_flip((unsigned) dpuTarget, DPU_FLIP_LIMIT_NS)) {
			fStFlips++;
			/*
			 * This fetch of the frame carries every write of the GPU's that is done by now (MiniIOSurfaceDrawn()
			 * waited for the surface's fence before the flip, so a write submitted before SwapEnd is done): the
			 * arm such a write left is cleared. One still in flight stays armed, and healWrites() fetches the
			 * frame once more when it ends -- the picture this flip fetched is from before it.
			 */
			if (fWrittenLock != NULL) {
				IOLockLock(fWrittenLock);
				if (fWritten[dpuTarget] != 0 && hakios_gpu_done(fWritten[dpuTarget])) {
					fWritten[dpuTarget] = 0;
				}
				IOLockUnlock(fWrittenLock);
			}
			hardwareRemember(NULL, NULL, -1);
			if (fStats && gpuReady) {
				if (sampleComparable) {
					fSampleCount++;
					fSampleChanged += changedSamples != 0;
					fSampleBroad += changedSamples * 100 >= UMI_SAMPLE_PIXELS;
					fSampleCells += changedSamples;
				}
				memcpy(gFrameSample, gFrameSampleNext, sizeof(gFrameSample)); /* both arrays are ordinary RAM */
				fSampleValid = true;
				fSampleWidth = fPanelWidth; fSampleHeight = fPanelHeight;
			} else if (fStats) {
				fSampleUnknown++;
				fSampleValid = false;
			}
			if (gTouchLat) {
				hakios_tl("tl: F swap %u slot %d in %llu ready %llu flip-return %llu changed %d\n",
				    swapWord(swap, SWAP_ID), dpuTarget, micros(fShowIn), micros(ready), micros(mach_absolute_time()),
				    sampleComparable ? (int)changedSamples : -1);
			}
			if (zero) {
				fStZero++;
				fStZeroWait += mach_absolute_time() - flipping;
				fStZeroWaitMax = mach_absolute_time() - flipping > fStZeroWaitMax ? mach_absolute_time() - flipping : fStZeroWaitMax;
				if (fRecBase == NULL) {
					completeSwap();
					stagedReleased = true;
				}
			}
			shown = mach_absolute_time();
			flipping = shown - flipping;
			fStWait += flipping;
			fStWaitMax = flipping > fStWaitMax ? flipping : fStWaitMax;
			fStShown++;
			if (fSwapsShown < 5) {
				hakios_tl("display: fb_dpu: frame %u flipped to scanout frame %d (%llu us waiting for the flip before)\n",
				    fSwapsShown, dpuTarget, micros(flipping));
			}
		} else {
			fStMissed++;
			dpuFallback("a flip was not latched within 100 ms", fTarget);
		}
	} else if (fComposeBase != NULL) {
		/* Only this presenter writes fComposeBase. All source pixels have now
		 * been consumed, so CoreAnimation may render its next frame while we
		 * wait for TE and copy this private buffer to the panel. Recordings
		 * still sample source pixels below, so keep their old ownership span.
		 */
		if (fRecBase == NULL) {
			completeSwap();
			stagedReleased = true;
		}
		const struct hakios_display_scan * scan = hakios_display_scan();
		fStatScanWaits++;
		if (scan != NULL) {
			/* A frame deadline is a short, explicit foreground boost before the TE wait. */
			hakios_cpufreq_boost(24);
		}
		uint64_t te_wait = mach_absolute_time();
		bool window = scan != NULL && hakios_display_wait_copy(fCopyNs, scan->frame_ns * 3 / 2);

		te_wait = mach_absolute_time() - te_wait;
		fStWait += te_wait;
		fStWaitMax = te_wait > fStWaitMax ? te_wait : fStWaitMax;
		if (window) {
			uint64_t copy_start = mach_absolute_time(), copy_ns;
			shown = copy_start;
			if (fDirty && fDirtyPixels != 0 && fDirtyLeft < fPanelWidth && fDirtyTop < fPanelHeight &&
			    fDirtyRight >= fDirtyLeft && fDirtyBottom >= fDirtyTop &&
			    (uint64_t)(fDirtyRight - fDirtyLeft + 1) * (fDirtyBottom - fDirtyTop + 1) * 4 <
			    (uint64_t)fPanelWidth * fPanelHeight * 3) {
				uint32_t right = MIN(fDirtyRight + 1, fPanelWidth);
				uint32_t bottom = MIN(fDirtyBottom + 1, fPanelHeight);
				for (uint32_t y = fDirtyTop; y < bottom; y++) {
					carveCopy(fBase + (size_t)y * fRowBytes + (size_t)fDirtyLeft * 4,
					    fComposeBase + (size_t)y * fRowBytes + (size_t)fDirtyLeft * 4,
					    (size_t)(right - fDirtyLeft) * 4);
				}
			} else {
				carveCopy(fBase, fComposeBase, (size_t)fRowBytes * fPanelHeight);
			}
			__asm__ volatile("dsb oshst" ::: "memory");
			fStCopy += mach_absolute_time() - copy_start;
			fStCopyMax = mach_absolute_time() - copy_start > fStCopyMax ? mach_absolute_time() - copy_start : fStCopyMax;
			fStShown++;
			OSIncrementAtomic64(&gCopiedPresents);
			absolutetime_to_nanoseconds(mach_absolute_time() - copy_start, &copy_ns);
			fCopyNs = copy_ns > fCopyNs ? copy_ns : (fCopyNs * 31 + copy_ns) / 32;
			if (fSwapsShown < 5)
				hakios_tl("display: staged frame %u copied in %llu us\n", fSwapsShown, copy_ns / 1000);
		} else {
			/* Keep the previous complete picture if this frame misses its window. */
			fStatScanMissed++;
			fStMissed++;
			fCopyNs = fCopyNs > 4000000 ? fCopyNs * 7 / 8 : fCopyNs;
			if (fSwapsShown < 5) hakios_tl("display: staged frame %u missed its idle window\n", fSwapsShown);
		}
	}
	drawn = mach_absolute_time();
	if (dpuTarget < 0 && fComposeBase == NULL && !hardware) {
		fStShown++;                     /* (straight into the panel's memory) */
		OSIncrementAtomic64(&gCopiedPresents);
	}
	if (fShowIn != 0 && shown > fShowIn) {
		fStLat += shown - fShowIn;
		fStLatMax = shown - fShowIn > fStLatMax ? shown - fShowIn : fStLatMax;
	}
	unsigned layerCount = 0;
	for (unsigned l = 0; l < SWAP_LAYERS; l++) layerCount += surfaces[l] != NULL;
	fStLayers[layerCount]++;
	fStUpload += upload; fStUploadMax = MAX(fStUploadMax, upload);
	if (gTouchLat) hakios_tl("fbcompose-swap: id=%u layers=%u cpu_us=%llu upload_us=%llu hw=%u inplace=%u\n",
	    swapWord(swap, SWAP_ID), layerCount, micros(cpuComposition), micros(upload), hardwareUsed, inPlace);
	/* (a write of the GPU's that landed on a frame after its picture was fetched: fetched again, once it is done) */
	healWrites();
	statsLine(drawn);
	absolutetime_to_nanoseconds(drawn - began, &nanos);
	fDrawNs = fDrawNs == 0 ? nanos : (fDrawNs * 7 + nanos) / 8;
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
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
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fStatSwapsLate = 0;
		if (fStatScanWaits != 0) {
			IOLog("MiniIOMobileFramebuffer: the scan (fb_scan): %u frames started where it let them land whole, %u did not "
			    "get there in time; %llu us to put a frame into the panel's memory\n", fStatScanWaits - fStatScanMissed,
			    fStatScanMissed, fDrawNs / NSEC_PER_USEC);
			fStatScanWaits = fStatScanMissed = 0;
		}
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
		fStatHeld = fStatLost = fStatOnBeat = 0;
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
	if (holds && !stagedReleased) completeSwap();
}

/* fb_dpu gave up: the pipes back on the GOP frame (with the latest picture in it), frames copied from now on */
void
IOMobileFramebuffer::dpuFallback(const char * why, const uint8_t * latest)
{
	uint64_t bytes = (uint64_t) fRowBytes * fPanelHeight;

	hakios_tl("display: fb_dpu: %s; back to the CPU copy path\n", why);
	if (fZero) {
		zeroLeave("the CPU copy path takes over", true);
	}
	if (fDpuScaled) {
		/* (the frames were the display's size: the panel's geometry back, the GOP frame refilled by the next frame) */
		fPanelWidth = fDpuPanelWidth;
		fPanelHeight = fDpuPanelHeight;
		fRowBytes = fDpuRowBytes;
		bytes = (uint64_t) fRowBytes * fPanelHeight;
		fDpuScaled = false;
		latest = NULL;
		/* (the staged bottom picture was taken at the other size and stride: no bottom until one is staged again) */
		dropStaged();
	}
	if (latest != NULL && latest != fBase) {
		carveCopy(fBase, latest, (size_t) bytes);
		__asm__ volatile("dsb oshst" ::: "memory");
	}
	hakios_display_dpu_stop();
	if (fComposeMemory == NULL) {
		fComposeMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, PAGE_SIZE);
		if (fComposeMemory != NULL) {
			fComposeBase = (uint8_t *) fComposeMemory->getBytesNoCopy();
			carveCopy(fComposeBase, fBase, (size_t) bytes);
		}
	}
	fDpuBuffers = 0;
	fTargetDevice = false;
	fTarget = fComposeBase != NULL ? fComposeBase : fBase;
}


/* Read the current backing store without moving any GPU mapping. Device
 * memory is copied into ordinary RAM before row workers use vector loads. */
const uint8_t *
IOMobileFramebuffer::sourcePixels(MiniIOSurface *s)
{
	for (unsigned i = 0; i < fDpuBuffers; i++) {
		if (fBound[i] == s) {
			carveCopy(fAliasRead, (const void *)fDpuVirt[i], (size_t)s->info.bytesPerRow * s->info.height);
			return fAliasRead;
		}
	}
	return s->pixels();
}

int
IOMobileFramebuffer::hardwarePrepare(const uint8_t *swap, MiniIOSurface **surfaces,
    struct umi_dpu_layer *layers, unsigned *count, uint64_t *upload, uint64_t *waiting)
{
	*count = 0;
	if (surfaces[0] == NULL) return -1;
	for (unsigned i = 0; i < SWAP_LAYERS; i++) {
		MiniIOSurface *s = surfaces[i];
		if (s == NULL) continue;
		if (s->info.bytesPerElement != 4 || s->info.planeCount ||
		    (s->info.pixelFormat != FOURCC('B','G','R','A') && s->info.pixelFormat != FOURCC('R','G','B','A')) ||
		    (uint64_t)s->info.offset + (uint64_t)s->info.bytesPerRow * s->info.height > s->info.allocSize) return -1;
		struct umi_dpu_layer *l = layers + (*count)++;
		l->width = s->info.width; l->height = s->info.height; l->stride = s->info.bytesPerRow;
		l->rgba = s->info.pixelFormat == FOURCC('R','G','B','A');
		memcpy(l->src, swap + SWAP_SOURCE(i), sizeof(l->src));
		memcpy(l->dst, swap + SWAP_DEST(i), sizeof(l->dst));
		if (l->src[2] <= 0 || l->src[3] <= 0) {
			l->src[0] = l->src[1] = 0; l->src[2] = l->width; l->src[3] = l->height;
		}
		if (l->dst[2] <= 0 || l->dst[3] <= 0) {
			l->dst[0] = l->dst[1] = 0; l->dst[2] = fWidth; l->dst[3] = fHeight;
		}
		int64_t x1 = ((int64_t)l->dst[0] + l->dst[2]) * fDpuPanelWidth / fWidth;
		int64_t y1 = ((int64_t)l->dst[1] + l->dst[3]) * fDpuPanelHeight / fHeight;
		l->dst[0] = (int32_t)((int64_t)l->dst[0] * fDpuPanelWidth / fWidth);
		l->dst[1] = (int32_t)((int64_t)l->dst[1] * fDpuPanelHeight / fHeight);
		l->dst[2] = (int32_t)(x1 - l->dst[0]); l->dst[3] = (int32_t)(y1 - l->dst[1]);
	}
	if (!hakios_display_compose_validate(layers, *count)) return -1;
	uint64_t waitStart = mach_absolute_time();
	int bank = hakios_display_compose_begin(DPU_FLIP_LIMIT_NS);
	*waiting = mach_absolute_time() - waitStart;
	if (bank < 0) {
		hakios_tl("fbcompose-failure: reason=bank-or-smmu\n");
		fHardwareCompose = false;
		return -1;
	}
	uint64_t start = mach_absolute_time();
	unsigned n = 0;
	for (unsigned i = 0; i < SWAP_LAYERS; i++) {
		MiniIOSurface *s = surfaces[i];
		if (s == NULL) continue;
		void *to = (void *)hakios_display_compose_source((unsigned)bank, n++);
		size_t bytes = (size_t)s->info.bytesPerRow * s->info.height;
		bool aliased = false;
		for (unsigned k = 0; k < fDpuBuffers; k++) {
			if (fBound[k] == s) { carveCopy(to, (const void *)fDpuVirt[k], bytes); aliased = true; break; }
		}
		if (!aliased) memcpy(to, s->pixels(), bytes);
		hakios_gpu_to_memory(to, bytes); /* display SMMU uses normal NC mappings */
	}
	*upload = mach_absolute_time() - start;
	return bank;
}

void
IOMobileFramebuffer::hardwareRemember(const uint8_t *swap, MiniIOSurface **surfaces, int bank)
{
	for (unsigned i = 0; i < SWAP_LAYERS; i++) {
		if (fLastHardwareSurfaces[i] != NULL) MiniIOSurfaceDrop(fLastHardwareSurfaces[i]);
		fLastHardwareSurfaces[i] = surfaces != NULL && surfaces[i] != NULL ? MiniIOSurfaceCopy(surfaces[i]->sid) : NULL;
	}
	fLastHardwareBank = bank;
	if (swap != NULL) memcpy(fLastHardwareSwap, swap, SWAP_SIZE);
}

/* A later partial CPU swap must preserve the actual previous composed image,
 * not an unrelated old carve-out frame. Replay immutable sources only then. */
void
IOMobileFramebuffer::hardwareReplay(void)
{
	unsigned n = 0;
	for (unsigned i = 0; i < SWAP_LAYERS; i++) {
		MiniIOSurface *s = fLastHardwareSurfaces[i];
		if (s == NULL) continue;
		int32_t src[4], dst[4];
		memcpy(src, fLastHardwareSwap + SWAP_SOURCE(i), sizeof(src));
		memcpy(dst, fLastHardwareSwap + SWAP_DEST(i), sizeof(dst));
		drawLayer(s, src, dst, i != 0, (const uint8_t *)hakios_display_compose_source(fLastHardwareBank, n++));
	}
}

/*
 * The bottom picture, kept: the frame as it is now, with the swap's bottom layer drawn into it and nothing above
 * it yet. The swaps that come with layers above and no bottom layer of theirs are composed over this, not over
 * the frame's own last output (hakios_blend.h; INVESTIGATION-alpha-black.md H1). A copy, not the surface: its
 * maker may draw the next frame into that at any moment, and reading it then tore the replay (REVIEW-layer0-
 * replay.md F1). With fLock held (show()).
 */
bool
IOMobileFramebuffer::stageBottom(void)
{
	size_t n = (size_t) fRowBytes * fPanelHeight;

	if (!stagedRoom()) {
		return false;
	}
	if (fTargetDevice || fTarget == fBase) {
		carveCopy(fStaged, fTarget, n);         /* (the frame is the carve-out's: Device memory to this CPU) */
	} else {
		memcpy(fStaged, fTarget, n);
	}
	fStagedValid = true;
	fStagedDropped = false;
	return true;
}

/*
 * The bottom layer drawn into the staged copy itself, for the paths that do not draw it into the frame (the
 * zero-copy and hardware ones): the same scaling and blending, into memory of ours. Only a layer that covers the
 * display: nothing is carried into the rest of the copy.
 */
bool
IOMobileFramebuffer::stageLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	uint8_t * target = fTarget;
	bool device = fTargetDevice;
	bool drawn;

	if (!stagedRoom()) {
		return false;
	}
	fTarget = fStaged;
	fTargetDevice = false;
	drawn = drawLayer(s, src, dst, false);
	fTarget = target;
	fTargetDevice = device;
	if (drawn) {
		fStagedValid = true;
		fStagedDropped = false;
	}
	return drawn;
}

/* The bottom picture, drawn into the frame: a straight copy, nothing waited for and no surface read (F1, F2). */
bool
IOMobileFramebuffer::replayBottom(void)
{
	size_t n = (size_t) fRowBytes * fPanelHeight;

	if (!fStagedValid || !stagedRoom()) {
		return false;
	}
	if (fTargetDevice || fTarget == fBase) {
		carveCopy(fTarget, fStaged, n);
	} else {
		memcpy(fTarget, fStaged, n);
	}
	return true;
}

/* The client goes, the screen goes off, a fallback changes the size of the frame: with fLock held. */
void
IOMobileFramebuffer::dropStaged(void)
{
	if (fStagedValid) {
		fStagedDropped = true;          /* (why a frame without a bottom layer was cleared: the 2 s line says) */
	}
	fStagedValid = false;
}

/* The same, from outside the presenter (a user client going, the screen going off): fLock, which show() holds
 * for as long as it uses the staged picture. */
void
IOMobileFramebuffer::forgetStaged(void)
{
	if (fLock == NULL) {
		return;
	}
	IOLockLock(fLock);
	dropStaged();
	IOLockUnlock(fLock);
}

/* fb_dpu=4: a sample of a surface's own pixels (every 64th row, out of the cache first): unchanged while aliased */
uint64_t
IOMobileFramebuffer::ownHash(MiniIOSurface * s)
{
	uint64_t hash = 0xcbf29ce484222325ULL;

	for (uint32_t y = 0; y < s->info.height; y += 64) {
		const uint64_t * row = (const uint64_t *) (s->pixels() + (size_t) y * s->info.bytesPerRow);

		hakios_gpu_from_memory((void *) row, (uint64_t) s->info.width * 4);
		for (uint32_t i = 0; i < s->info.width / 2; i++) {
			hash = (hash ^ row[i]) * 0x100000001b3ULL;
		}
	}
	return hash;
}

/*
 * fb_dpu=4: the carve-out frame of the swap's surface, aliasing it now if it is a frame surface of CoreAnimation's
 * (the display's size and stride, one layer over all of it) and a frame is free (one always stays unaliased, for
 * swaps that are copied); -1: copy this swap.
 */
int
IOMobileFramebuffer::zeroSlot(const uint8_t * swap, MiniIOSurface ** surfaces, int pending, int scanned)
{
	MiniIOSurface * s = surfaces[0];
	int32_t src[4], dst[4];
	unsigned bound = 0;
	int k = -1;

	/* aliases of surfaces nobody but this driver holds any more: ended */
	for (unsigned i = 0; i < fDpuBuffers; i++) {
		if (fBound[i] != NULL && fBound[i]->holders <= 1 && (int) i != scanned && (int) i != pending) {
			hakios_tl("display: fb_dpu=4: surface %u is gone; frame %u free again\n", fBound[i]->sid, i);
			(void) HakiosGPUSurfaceAlias(fBound[i], 0);
			MiniIOSurfaceDrop(fBound[i]);
			fBound[i] = NULL;
			/* (the frame keeps that surface's stride unless it is put back: a copied frame is composed at its own) */
			if (fDpuScaled && fSlotStride[i] != fRowBytes) {
				fSlotStride[i] = fRowBytes;
				hakios_display_dpu_stride(i, fRowBytes);
			}
			fSlotBlack[i] = false;
		}
		bound += fBound[i] != NULL;
	}
	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
	if (s == NULL || s->info.width != fWidth || s->info.height != fHeight) {
		return -1;                              /* (not a frame of the display's: nothing to say) */
	}
	if (!fGraphics) {
		return -1;                              /* (the first picture: the console is stopped while it is drawn) */
	}
	const char * why = NULL;
	if (swapWord(swap, SWAP_SURFACE(1)) != 0 || swapWord(swap, SWAP_SURFACE(2)) != 0 || swapWord(swap, SWAP_SURFACE(3)) != 0) {
		why = "more than one layer";
	} else if (s->info.bytesPerElement != 4 || s->info.planeCount != 0 || s->info.pixelFormat != FOURCC('B', 'G', 'R', 'A')) {
		why = "not one plane of BGRA";
	} else if (s->info.bytesPerRow < fWidth * 4 || (s->info.bytesPerRow & 63) != 0 || s->info.bytesPerRow > 0xffff) {
		why = "a row stride the pipe cannot fetch (64-byte multiple of at least the width)";
	} else if (s->info.offset != 0) {
		why = "pixels not at the start of its memory";
	} else if (s->client != NULL) {
		why = "made around client memory";
	} else if (hakios_display_dpu_frame(1) - hakios_display_dpu_frame(0) < round_page_64(s->info.allocSize) ||
	    (uint64_t) s->info.bytesPerRow * s->info.height > s->info.allocSize) {
		why = "bigger than a carve-out frame";
	} else if (!((src[2] <= 0 || src[3] <= 0) || (src[0] == 0 && src[1] == 0 && (uint32_t) src[2] == fWidth &&
	    (uint32_t) src[3] == fHeight)) || !((dst[2] <= 0 || dst[3] <= 0) || (dst[0] <= 0 && dst[1] <= 0 &&
	    (int64_t) dst[0] + dst[2] >= fWidth && (int64_t) dst[1] + dst[3] >= fHeight))) {
		why = "not shown whole over the whole display";
	}
	if (why != NULL) {
		for (unsigned i = 0; i < 4; i++) {
			if (fNotAliased[i] == s->sid) {
				break;
			}
			if (fNotAliased[i] == 0) {
				fNotAliased[i] = s->sid;
				hakios_tl("display: fb_dpu=4: surface %u not aliased (%s): '%c%c%c%c' %u bytes/element, %u planes, %u bytes a row, "
				    "offset %u, %u bytes, %s, layers %u %u %u, from %d,%d %dx%d to %d,%d %dx%d\n", s->sid, why,
				    (int) (s->info.pixelFormat >> 24 & 0xff), (int) (s->info.pixelFormat >> 16 & 0xff),
				    (int) (s->info.pixelFormat >> 8 & 0xff), (int) (s->info.pixelFormat & 0xff), s->info.bytesPerElement,
				    s->info.planeCount, s->info.bytesPerRow, s->info.offset, s->info.allocSize,
				    s->client != NULL ? "around client memory" : "own memory", swapWord(swap, SWAP_SURFACE(1)),
				    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), src[0], src[1], src[2], src[3],
				    dst[0], dst[1], dst[2], dst[3]);
				break;
			}
		}
		return -1;
	}
	for (unsigned i = 0; i < fDpuBuffers; i++) {
		if (fBound[i] == s) {
			k = (int) i;
		}
	}
	if (k >= 0) {
		/* the self-check: the GPU draws into the frame, not into the surface's own memory any more */
		if (fBoundChecks[k] < 8) {
			if (ownHash(s) != fBoundHash[k]) {
				hakios_tl("display: fb_dpu=4: ALIAS FAIL: surface %u's own memory changed after it was aliased onto frame %d: "
				    "the GPU still draws there\n", s->sid, k);
				zeroLeave("an alias did not take", false);
				return -1;
			}
			if (++fBoundChecks[k] == 8) {
				hakios_tl("display: fb_dpu=4: ALIAS PASS: surface %u drawn 8 times into frame %d, its own memory untouched\n",
				    s->sid, k);
			}
		}
		return k;
	}
	/*
	 * (a2) A frame given back for a copied swap (zeroDemote()) stays free of aliases for a moment. CoreAnimation
	 * makes a new frame surface for every frame, and the one it makes now would be aliased onto the frame just
	 * given back at once, the copied swap after it would find only the scanned frame free and demote again (10.1
	 * MB copied back every other frame). Said nowhere: fbzero: demotions= counts them.
	 */
	if (fDemotedAt != 0 && hakios_display_inplace_demote()) {
		uint64_t held;

		absolutetime_to_nanoseconds(mach_absolute_time() - fDemotedAt, &held);
		if (held < DEMOTE_HOLD_NS) {
			return -1;
		}
	}
	if (bound + 1 >= fDpuBuffers) {
		/* (no frame left to alias onto: said once per run of them -- this was silent, and in one run 1,351
		 * swaps with a single layer were copied for it, which no line accounted for) */
		fStRefusedBound++;
		if (!fRefusedBound) {
			fRefusedBound = true;
			hakios_tl("fbpath: no frame left to alias surface %u (%u of %u bound): copied\n", s->sid, bound, fDpuBuffers);
		}
		return -1;
	}
	for (unsigned i = 0; i < fDpuBuffers && k < 0; i++) {
		if (fBound[i] == NULL && (int) i != scanned && (int) i != pending) {
			k = (int) i;
		}
	}
	if (k < 0) {
		return -1;
	}
	/* the surface's picture into its frame (the GPU is done with it: show() waited), then its GPU mappings moved there */
	carveCopy((void *) fDpuVirt[k], s->pixels(), (size_t) s->info.bytesPerRow * s->info.height);
	__asm__ volatile("dsb sy" ::: "memory");
	if (MiniIOSurfaceCopy(s->sid) != s) {
		return -1;
	}
	if (!HakiosGPUSurfaceAlias(s, hakios_display_dpu_frame((unsigned) k))) {
		MiniIOSurfaceDrop(s);
		zeroLeave("a surface's GPU mappings could not be moved", false);
		return -1;
	}
	/*
	 * The hash of the surface's own memory is taken AFTER the alias: HakiosGPUSurfaceAlias() retires every
	 * submission the surface still has (surface->drawn, under the GPU's lock) before it moves the mappings, so
	 * the own memory is final here. Sampled before it, a draw still in flight at boot landed afterwards and the
	 * self-check below failed an alias that had taken: v29h, one ALIAS FAIL, and zero copy was off for the rest
	 * of the session (fbzero-demote: total=2 reason=2, then enabled=0 on every line).
	 */
	fBoundHash[k] = ownHash(s);
	fBoundChecks[k] = 0;
	fBound[k] = s;
	fSlotBlack[k] = false;
	fSlotStride[k] = s->info.bytesPerRow;
	hakios_display_dpu_stride((unsigned) k, s->info.bytesPerRow);
	fRefusedBound = false;                          /* (a frame was free again: the next refusal is a new episode) */
	hakios_tl("display: fb_dpu=4: surface %u aliased onto frame %d at %#llx (%u bytes a row): drawn there by the GPU from now on\n",
	    s->sid, k, hakios_display_dpu_frame((unsigned) k), s->info.bytesPerRow);
	return k;
}

/*
 * fb_dpu=4: zero copy left (zeroLeave()) -- try it again on a later swap. zeroLeave() sets fZero = false and
 * nothing else ever set it back, so one alias check that failed at boot (v29h, an alias sampled before the GPU's
 * work was retired) cost every frame of the session its flips: 14,236 of 14,238 frames copied, and the new
 * surfaces of a backboardd restart too. Here, once ZERO_REALIAS_DELAY_NS have been quiet (the failure is behind,
 * and one swap at boot was not a verdict on the session), a try budget is left (ZERO_REALIAS_TRIES) and the switch
 * allows it (/var/db/hakios-display/zero-recover-off), fZero is set again and zeroSlot() aliases this swap's
 * surface if it may -- with all of zeroSlot()'s own checks: a frame no surface is bound to, the panel not fetching
 * it, the surface not smaller or larger than a frame, one plane of BGRA, shown whole. Nothing is aliased or
 * remapped here, so no locking changes: this runs in show(), under fLock, where zeroSlot() runs. If the alias
 * fails its check again zeroLeave() takes zero copy away again and the next try waits the same delay, so a
 * surface the GPU really will not leave cannot loop (at most ZERO_REALIAS_TRIES copies of a frame).
 * 1 when zero copy is back on (this swap may be a flip); 0 when it stays off.
 */
bool
IOMobileFramebuffer::zeroRecover(void)
{
	uint64_t after;

	if (fZero || fDpuBuffers < 3 || !fDpuScaled || fZeroLostAt == 0 || fRealiasTries >= ZERO_REALIAS_TRIES ||
	    !hakios_display_zero_recover()) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fZeroLostAt, &after);
	if (after < ZERO_REALIAS_DELAY_NS) {
		return false;
	}
	for (unsigned i = 0; i < 3; i++) {
		if (fBound[i] != NULL) {
			return false;                   /* (zeroLeave() dropped them all: a frame still bound is not ours to reuse) */
		}
	}
	fRealiasTries++;
	fRealias++;
	fZero = true;
	/*
	 * The state zeroLeave() leaves behind: no alias, but the frames may keep an aliased surface's stride (the copy
	 * path puts its own back on the frame it composes into), and a demotion's hold must not keep this swap from
	 * aliasing either.
	 */
	fDemotedAt = 0;
	fRefusedBound = false;
	for (unsigned i = 0; i < 3; i++) {
		fBoundHash[i] = 0;
		fBoundChecks[i] = 0;
		fSlotBlack[i] = false;
	}
	hakios_tl("fbzero-recover: zero copy back on, try %u of %u\n", fRealiasTries, ZERO_REALIAS_TRIES);
	return true;
}

/*
 * fb_dpu=4 (a2): one aliased surface, not on the panel, its own again -- the picture the GPU drew into its frame
 * copied back into the surface's own memory, all its GPU mappings back onto that memory -- and its frame free for
 * a copied swap, which is then composed into it and flipped to at the next TE. Two frames free of aliases, the
 * true double buffering the zero-copy path has, instead of 10.1 MB written into the frame the panel is fetching
 * (which tore: v29i, INVESTIGATION-inplace-tearing.md §4.2).
 * 1 when the frame is free. The GPU must be done with the surface (its fence is waited for, DEMOTE_FENCE_MS at
 * most; after that the in-place branch takes the frame), the pipe must not fetch the frame, and a surface somebody
 * else still holds is not touched (zeroSlot() ends the ones nobody holds). The same surface may be aliased again by
 * zeroSlot() once DEMOTE_HOLD_NS is past; with CoreAnimation's two frame surfaces that is what keeps it from
 * demoting one after another.
 *
 * `fence_ms` is what the GPU's work on the surface is waited for (`probe`: it is not waited for at all -- the frame
 * is given back only if that work is already done -- and a surface still being drawn into is counted apart, as
 * busy, and left for the round after), `budget_ms` bounds the whole operation.
 */
bool
IOMobileFramebuffer::zeroDemote(unsigned i, uint64_t fence_ms, uint64_t budget_ms, bool probe)
{
	MiniIOSurface * s;
	uint32_t sid;
	unsigned why = 0;

	if (i >= fDpuBuffers || budget_ms == 0) {
		return false;                           /* (the bounds check before the read of fBound[]) */
	}
	s = fBound[i];
	if (s == NULL || s->holders <= 1 || s->gpu_broken) {
		return false;
	}
	if ((int) i == hakios_display_dpu_shown() || (int) i == hakios_display_dpu_poll()) {
		return false;                           /* (the panel fetches it, or is about to) */
	}
	if (hakios_display_dpu_write_busy(hakios_display_dpu_frame(i)) != 0) {
		return false;
	}
	sid = s->sid;
	/*
	 * The fence, the copy back and the PTE remap are one operation, with the GPU's own lock held throughout
	 * (hakios.gpu.cpp): the submit path sets surface->drawn under that lock, so no submission of this surface can
	 * start between the fence that is waited for and the remap -- the copy cannot read a frame the GPU is writing,
	 * and the remap cannot cut a submission in two. Bounded: fence_ms for the fence, the budget the caller
	 * hands over (at most DEMOTE_BUDGET_MS for all of a swap's demotions) for all of it -- a demotion holds fLock
	 * that long at most, and the frame stays the surface's when it does not fit.
	 */
	if (!HakiosGPUSurfaceDemote(s, (const void *) fDpuVirt[i], MIN(fence_ms, budget_ms), budget_ms, &why)) {
		/* (which of the three it was: the fence, the budget the whole operation has, or a mapping that would not
		 * go back. A probe that stopped at the fence means CoreAnimation is drawing into the surface now, which
		 * is what the round after it is for -- counted apart, because it is not a demotion that failed) */
		if (probe && why == 1) {
			fStDemoteBusy++;
		} else if (why == 2) {
			fStDemoteBudget++;
		} else if (why == 3) {
			fStDemoteRemap++;
		} else {
			fStDemoteFence++;
		}
		return false;                           /* (the frame is still the surface's: the in-place branch takes it) */
	}
	fBound[i] = NULL;
	MiniIOSurfaceDrop(s);
	fSlotBlack[i] = false;
	if (fDpuScaled) {                               /* (this frame is composed at its own stride from now on) */
		fSlotStride[i] = fRowBytes;
		hakios_display_dpu_stride(i, fRowBytes);
	}
	fDemotions++;
	fDemotedAt = mach_absolute_time();
	hakios_tl("display: fb_dpu=4: surface %u gives frame %u back for a copied swap (%u so far)\n", sid, i, fDemotions);
	return true;
}

/* fb_dpu=4 off: every aliased surface its own again (with the frame's picture copied back when it is the newer) */
void
IOMobileFramebuffer::zeroLeave(const char * why, bool copyBack)
{
	unsigned reason = !copyBack ? (strcmp(why, "an alias did not take") == 0 ? 1 : 0) : 2;
	fDemoteReason[reason]++;
	for (unsigned i = 0; i < 3; i++) fDemotions += fBound[i] != NULL;
	/* (zero copy left: zeroRecover() tries it again after ZERO_REALIAS_DELAY_NS, if the switch and the try budget
	 * allow it -- unless fb_dpu itself is going away, which zeroRecover() cannot tell from here; dpuFallback()
	 * clears fDpuBuffers right after this and zeroRecover() asks for three frames) */
	if (fZero) {
		fZeroLostAt = mach_absolute_time();
		fZeroOff++;
	}
	hakios_tl("fbzero-demote: total=%u reason=%u\n", fDemotions, reason + 1);
	hakios_tl("display: fb_dpu=4: zero copy off: %s; frames are copied (fb_dpu=3)\n", why);
	for (unsigned i = 0; i < 3; i++) {
		MiniIOSurface * s = fBound[i];

		if (s == NULL) {
			continue;
		}
		if (copyBack) {
			carveCopy(s->pixels(), (const void *) fDpuVirt[i], (size_t) s->info.bytesPerRow * s->info.height);
			hakios_gpu_to_memory(s->pixels(), (uint64_t) s->info.bytesPerRow * s->info.height);
		}
		(void) HakiosGPUSurfaceAlias(s, 0);
		MiniIOSurfaceDrop(s);
		fBound[i] = NULL;
	}
	fZero = false;
}

/*
 * fb_dpu=4, from the GPU's submissions (hakios.gpu.cpp): a submission naming an aliased surface whose frame is on the
 * panel with a flip away from it pending waits, at most `ms`, until the DPU will not fetch that frame again (this
 * refresh's transfer done, or the flip latched: hakios_display_dpu_leaving()): CoreAnimation draws into the surface it
 * swapped before as soon as its SwapEnd returns, which is before the TE that takes the new frame.
 */
extern "C" void
MiniIOMobileFramebufferOffscreen(MiniIOSurface * surface, uint32_t ms)
{
	IOMobileFramebuffer * fb = gFramebuffer;
	uint64_t start = mach_absolute_time(), waited;
	int index = -1;

	if (fb == NULL || !fb->fZero) {
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		if (fb->fBound[i] == surface) {
			index = (int) i;
		}
	}
	/* (that the flip away is programmed by then is SwapWait's: IOMobileFramebuffer::waitShown()) */
	while (index >= 0 && hakios_display_dpu_leaving((unsigned) index)) {
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &waited);
		if (waited >= (uint64_t) ms * NSEC_PER_MSEC) {
			break;
		}
		delay_for_interval(250, NSEC_PER_USEC);
	}
}

/*
 * fb_dpu=4, from the GPU's submissions (hakios.gpu.cpp: submit(), for every buffer of a submission that is WRITTEN
 * and is an IOSurface): this frame's surface was written. The picture the panel has of that frame was fetched when
 * the frame was flipped to, and in command mode it is the panel's own copy: a write that comes after that fetch
 * changes the frame's memory and nothing on the glass until the frame is fetched again. A frame that is flipped to
 * again gets that by itself, and a write that is done by then is carried by that fetch -- show() clears the arm of
 * a frame it flips to when the fence it holds is done. What is left is the write found here and no flip after it:
 * CoreAnimation has gone idle on a picture from before the write (or from the middle of it), which stays on the
 * panel until something moves. healWrites() fetches that frame once more. Nothing here sleeps; the caller holds
 * the GPU's lock, and this takes only its own (the order is the GPU's lock, then this one).
 */
void
IOMobileFramebuffer::written(MiniIOSurface * surface, uint32_t fence)
{
	if (fWrittenLock == NULL || !fZero || surface == NULL || fence == 0) {
		return;
	}
	IOLockLock(fWrittenLock);
	for (unsigned i = 0; i < 3; i++) {
		if (fBound[i] != surface) {
			continue;
		}
		/* (the newest: a frame's picture is one, whatever the number of submissions that make it) */
		if (fWritten[i] == 0 || (int32_t) (fence - fWritten[i]) > 0) {
			fWritten[i] = fence;
			fWrittenAt[i] = mach_absolute_time();
		}
		break;
	}
	IOLockUnlock(fWrittenLock);
}

extern "C" void
MiniIOMobileFramebufferSurfaceWritten(MiniIOSurface * surface, uint32_t fence)
{
	IOMobileFramebuffer * fb = gFramebuffer;

	if (fb != NULL) {
		fb->written(surface, fence);
	}
}

/* Whether a frame on the panel has a write armed -- the only kind healWrites() can act on: the GPU's idle thread
 * looks every few milliseconds while one has, and every 100 otherwise (one is left only by the fault this heals). */
bool
IOMobileFramebuffer::writesPending(void)
{
	bool pending = false;
	int shown;

	if (fWrittenLock == NULL) {
		return false;
	}
	shown = hakios_display_dpu_shown();
	IOLockLock(fWrittenLock);
	for (unsigned i = 0; i < 3; i++) {
		pending = pending || (fWritten[i] != 0 && (int) i == shown);
	}
	IOLockUnlock(fWrittenLock);
	return pending;
}

extern "C" bool
MiniIOMobileFramebufferWritesPending(void)
{
	IOMobileFramebuffer * fb = gFramebuffer;

	return fb != NULL && fb->writesPending();
}

/*
 * fb_dpu=4: fetch the frame the panel is showing once more when a write of the GPU's landed on it after its picture
 * was fetched, so the panel's copy is the frame's content as it is now. Armed by written(), decided here:
 *   - the frame must be the one on the panel; one that is not is left for the flip that will fetch it, which clears
 *     the arm when the write is done by then (show()), and healed here if that flip came before the write ended,
 *   - the write's fence must be done, and the frame left alone for KICK_QUIET_NS: a frame is written by more than
 *     one submission, and fetching it between two of them would show half of the frame's work, which is what this
 *     is here to avoid,
 *   - a flip must not be outstanding (hakios_display_dpu_refresh() keeps the shown frame, changes nothing shown).
 * One fetch an arm; both the arm and the check are cheap (three words) and there is no line per frame.
 */
void
IOMobileFramebuffer::healWrites(void)
{
	uint32_t healFence[3] = { 0, 0, 0 };
	unsigned want = 0;
	uint64_t now, quiet;
	int shown;

	if (fWrittenLock == NULL || !fZero || !hakios_display_write_kick()) {
		return;
	}
	nanoseconds_to_absolutetime(KICK_QUIET_NS, &quiet);
	now = mach_absolute_time();
	shown = hakios_display_dpu_shown();
	IOLockLock(fWrittenLock);
	for (unsigned i = 0; i < 3; i++) {
		if (fWritten[i] == 0) {
			continue;
		}
		if ((int) i != shown) {
			continue;                       /* (not on the glass: kept, the frame it is may still be flipped to here) */
		}
		if (now - fWrittenAt[i] < quiet || !hakios_gpu_done(fWritten[i])) {
			continue;                       /* (still being written, or written again since: looked at again) */
		}
		healFence[want++] = fWritten[i];
	}
	IOLockUnlock(fWrittenLock);
	if (want == 0) {
		return;
	}
	/* (the frame on the panel is the one that was looked at: a flip latched in between fetched it itself) */
	if (hakios_display_dpu_shown() != shown) {
		return;
	}
	if (!hakios_display_dpu_refresh()) {
		return;                                 /* (a flip outstanding, or the display not up: again next time) */
	}
	bool first;
	IOLockLock(fWrittenLock);
	for (unsigned n = 0; n < want; n++) {
		for (unsigned i = 0; i < 3; i++) {
			if (fWritten[i] == healFence[n] && (int) i == shown) {
				fWritten[i] = 0;
				fStKicks++;
				break;
			}
		}
	}
	first = !fKickSaid;
	fKickSaid = true;
	IOLockUnlock(fWrittenLock);
	/* (the panel's picture is not the one the last flip's sample was of any more: the next flip is not compared to it) */
	fSampleValid = false;
	if (first) {
		hakios_tl("fbzero-kick: a GPU write landed on the frame on the panel after its picture was fetched: fetched again\n");
	}
}

extern "C" void
MiniIOMobileFramebufferHealWrites(void)
{
	IOMobileFramebuffer * fb = gFramebuffer;

	if (fb != NULL) {
		fb->healWrites();
	}
}

/* fb_stats=1: a line every 2 s of frames. "to the panel": copied in its window, or (fb_dpu) flipped to; "latched": flips the DPU took */
void
IOMobileFramebuffer::statsLine(uint64_t now)
{
	uint64_t ns;

	if (!fStats) {
		return;
	}
	if (fStSince == 0) {
		fStSince = now;
		fStLatched = hakios_display_dpu_latched();
		return;
	}
	absolutetime_to_nanoseconds(now - fStSince, &ns);
	if (ns < STATS_NS) {
		return;
	}
	uint32_t in = (uint32_t) fStIn, lost = (uint32_t) fStLost, latched = hakios_display_dpu_latched() - fStLatched;
	uint32_t shown = fStShown != 0 ? fStShown : 1, clears = fStClears != 0 ? fStClears : 1;
	uint64_t ms = ns / NSEC_PER_MSEC;

	OSAddAtomic(-(SInt32) in, &fStIn);
	OSAddAtomic(-(SInt32) lost, &fStLost);
	hakios_tl("fbstat: %llu ms %s: %u swaps in (%llu.%llu/s), %u to the panel (%llu.%llu/s), latched %u, %u not in time, %u dropped; "
	    "scale %llu us avg %llu max; %s %llu us avg %llu max; copy %llu us avg %llu max; surface clear %llu us avg %llu max; "
	    "GPU wait %llu us avg; swap to panel %llu us avg %llu max; %u threads%s; zero-copy %u (flip wait %llu us avg %llu max)\n", ms,
	    fZero ? "dpu-zero" : fDpuScaled ? "dpu-scale" : fDpuBuffers != 0 ? "dpu-flip" : fComposeBase != NULL ? "staged-copy" : "direct",
	    in, (uint64_t) in * 10000 / ms / 10, (uint64_t) in * 10000 / ms % 10,
	    fStShown, (uint64_t) fStShown * 10000 / ms / 10, (uint64_t) fStShown * 10000 / ms % 10, latched, fStMissed, lost,
	    micros(fStDraw) / shown, micros(fStDrawMax), fDpuBuffers != 0 ? "flip wait" : "TE wait",
	    micros(fStWait) / shown, micros(fStWaitMax), micros(fStCopy) / shown, micros(fStCopyMax),
	    micros(fStClear) / clears, micros(fStClearMax), micros(fStGpu) / shown, micros(fStLat) / shown, micros(fStLatMax),
	    fThreads, fRuns != 0 ? ", nearest by runs" : "", fStZero, fStZero != 0 ? micros(fStZeroWait) / fStZero : 0,
	    micros(fStZeroWaitMax));
	hakios_tl("fbcontent: %llu ms %u sampled flips, %u changed, %u same, %u broad(>=1%%); cells %u/2048 avg; sample %llu us total %llu max; unknown %u\n",
	    ms, fSampleCount, fSampleChanged, fSampleCount - fSampleChanged, fSampleBroad,
	    fSampleCount ? fSampleCells / fSampleCount : 0, micros(fSampleTime), micros(fSampleMax), fSampleUnknown);
	/* The old fbstat exceeds hakios_tl's 192-byte line. Keep these timings on a separate short line. */
	hakios_tl("fb-latency: %u frames GPU-wait %llu us avg; swap-to-flip %llu avg %llu max; flip-wait %llu avg %llu max; "
	    "source queue %u; drawers %u/%u\n", fStShown, micros(fStGpu) / shown, micros(fStLat) / shown, micros(fStLatMax),
	    micros(fStWait) / shown, micros(fStWaitMax), swapQueue(), fHelpersStarted + 1, fThreads);
	/* Whether the rows of a frame really are shared out: the number in fbstat is past that line's 192 bytes, and
	 * this is the answer (once: it is a property of the boot, not of the frames). */
	if (!fThreadsSaid) {
		fThreadsSaid = true;
		hakios_tl("display: fb_threads: %u threads, %u helper(s) started: a frame's rows are %s\n", fThreads,
		    fHelpersStarted, fThreads <= 1 || fHelpersStarted == 0 ? "drawn by one" : "claimed by all of them");
	}
	uint32_t swaps = fStLayers[0] + fStLayers[1] + fStLayers[2] + fStLayers[3] + fStLayers[4];
	uint32_t composed = swaps != 0 ? swaps : 1;
	/* "nobottom" counts the episodes (one line each, above), "aboveonly" the frames in them: how fast (1 - alpha)^n
	 * would have taken the picture under the layers above away (INVESTIGATION-alpha-black.md). */
	hakios_tl("fbcompose: swaps=%u layers0=%u layers1=%u layers2=%u layers3=%u layers4=%u hw=%u inplace=%u refused=%u nobottom=%u "
	    "aboveonly=%u cpu_us=%llu/%llu upload_us=%llu/%llu\n",
	    swaps, fStLayers[0], fStLayers[1], fStLayers[2], fStLayers[3], fStLayers[4], fStHardware, fStInplace,
	    fStRefusedBound, fStNoBottom, fStAboveOnly, micros(fStDraw) / composed, micros(fStDrawMax), micros(fStUpload) / composed,
	    micros(fStUploadMax));
	/* What the frames without a bottom layer were composed over, and why any of them was cleared instead: a line
	 * of its own, because fbcompose: above is at hakios_tl's 192 bytes already. "staged" says whether there is a
	 * bottom picture to replay now (0: the next such frame is cleared), "never"/"gone" why one was not (no
	 * bottom picture staged since the session began, or one was and was let go). No frame wait, no reason to read
	 * a surface again (REVIEW-layer0-replay.md F1, F2). */
	hakios_tl("fb-bottom: staged=%u aboveonly=%u cleared=%u never=%u gone=%u\n",
	    fStagedValid, fStAboveOnly, fStClearNever + fStClearGone, fStClearNever, fStClearGone);
	/* fb_dpu=4: "slow" and "late" count the frames copied into the panel's frame with no idle interval to copy in
	 * (the run-totals; each of them logged once as fbzero-slow) -- a frame that the panel takes with its next
	 * transfer. "fence"/"busy"/"budget"/"remap" count the demotions (a2) that could not be made this window, by
	 * what stopped each: the fence of a surface CoreAnimation was still drawing into (busy: the non-blocking
	 * round that leaves those alone; fence: the bounded wait of the round after), the operation's budget, or a
	 * GPU mapping that would not go back -- each of those frames went the in-place way. "nowin" counts the copy
	 * window checks that found none, in place and staged (hakios.display.c), "impossible" the ones that could
	 * never come (not waited for at all any more). "flips" the flips programmed since the last line, so a window
	 * with "inplace=" frames and no flips shows. "copied" is the frames composed into the staging buffer and
	 * copied into a scanout frame in one piece (the compose path, compose-off for the old one). "refused" and
	 * "nobottom" are in the fbcompose: line above. "kick" is the run total of frames fetched again because the
	 * GPU wrote them after their picture was fetched (healWrites(), first one said as fbzero-kick:). "off" is how
	 * many times zero copy left (zeroLeave(); the reason is the one fbzero-demote: line), "realias" how many of
	 * those turns zeroRecover() got it back -- enabled=1 with realias>0 is a session that lost it once and went on
	 * flipping; enabled=0 with off>0 and realias<off is one still off (within ZERO_REALIAS_DELAY_NS of a failure,
	 * or out of tries). Both are run totals, not this window's. */
	hakios_tl("fbzero: enabled=%u demotions=%u fence=%u busy=%u budget=%u remap=%u copied=%u slow=%u late=%u nowin=%u "
	    "impossible=%u flips=%u kick=%u off=%u realias=%u\n", fZero, fDemotions, fStDemoteFence, fStDemoteBusy, fStDemoteBudget,
	    fStDemoteRemap, fStComposed, fStInplaceLate, fStLateWin, hakios_display_copy_nowin(), hakios_display_copy_impossible(),
	    fStFlips, fStKicks, fZeroOff, fRealias);
	bzero(fStLayers, sizeof(fStLayers));
	fStHardware = fStInplace = 0;
	fStUpload = fStUploadMax = 0;
	fSampleCount = fSampleChanged = fSampleBroad = fSampleCells = fSampleUnknown = 0;
	fSampleTime = fSampleMax = 0;
	fStSince = now;
	fStLatched += latched;
	fStShown = fStMissed = fStClears = fStZero = 0;
	fStRefusedBound = fStNoBottom = fStFlips = fStLateWin = fStAboveOnly = 0;
	fStDemoteFence = fStDemoteBusy = fStDemoteBudget = fStDemoteRemap = fStComposed = 0;
	fStClearNever = fStClearGone = 0;
	fStZeroWait = fStZeroWaitMax = 0;
	fStDraw = fStDrawMax = fStWait = fStWaitMax = fStCopy = fStCopyMax = fStClear = fStClearMax = fStGpu = fStLat = fStLatMax = 0;
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
	if (!fOwner->prepareDisplaySession()) return false;
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
	/* (its application is going: the bottom picture it swapped last is let go with it, not kept wired) */
	if (fOwner != NULL) {
		fOwner->forgetStaged();
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

	/* Avoid scan MMIO and 60/120 Hz callbacks while dark. Resume <=250 ms. */
	if (umi_pm_screen_off()) {
		nanoseconds_to_absolutetime(250 * NSEC_PER_MSEC, &period);
		fVSyncTime = now + period;
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	if (hakios_display_scan() != NULL) {
		/* fb_scan: when the panel's next frame begins, by its scan -- locked to it, never drifting */
		uint64_t ahead;

		nanoseconds_to_absolutetime(hakios_display_next_frame_ns(), &ahead);
		fVSyncTime = now + (ahead != 0 ? ahead : period);
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}
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
	} else if (umi_pm_screen_off()) {
		armVSync();
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
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
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
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
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

	case kFBMethodSwapWait:                         /* in: the swap: until it is flipped to (fb_dpu=4; else at once) */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		if (args->scalarInputCount > 0) {
			fOwner->waitShown((uint32_t) in[0]);
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
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
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
public:
	IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * OpenGL ES for applications, behind boot-arg metal=1 and a switch file, never by default. libGFXShared reads
 * IOGLESMetalBundleName off this service and loads /System/Library/Extensions/<name>.bundle as the GL driver of
 * every EAGLContext; AppleMetalGLRenderer draws GL with the Metal device above (HakiosMetal, which turns GL's shader
 * IR into AIR: HakiosMetalCA.m hm_gles_air). Without the property an application's EAGLContext is nil, which GL
 * applications survive; with it and a device that cannot link GL's programs they would not. So the property is not
 * set here: the window server's HakiosMetal asks for it (IORegistryEntrySetCFProperty) when
 * /var/db/hakios-metal-cache/gles-on exists, and this accepts that one key with that one value, once a boot, with
 * metal=1. Anything else is refused.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSString * name = dict != NULL ? OSDynamicCast(OSString, dict->getObject("IOGLESMetalBundleName")) : NULL;
	uint32_t metal = 0;

	if (!PE_parse_boot_argn("metal", &metal, sizeof(metal)) || metal == 0) {
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() != 1 || name == NULL || !name->isEqualTo("AppleMetalGLRenderer")) {
		return kIOReturnBadArgument;
	}
	if (getProperty("IOGLESMetalBundleName") == NULL) {
		setProperty("IOGLESMetalBundleName", "AppleMetalGLRenderer");
		IOLog("MiniIOMobileFramebuffer: gles: the accelerator names AppleMetalGLRenderer as its OpenGL ES driver (asked by pid %d)\n",
		    proc_selfpid());
	}
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

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
		carveCopy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
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
