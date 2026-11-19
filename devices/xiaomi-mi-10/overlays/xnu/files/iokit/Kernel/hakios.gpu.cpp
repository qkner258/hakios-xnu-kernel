/*
 * HakiosGPU: the phone's Adreno for programs to draw with.
 *
 * drivers/gpu/hakios.adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/hakios-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Buffers share one GPU address space; their last-use fences retain mappings through client exit.
 * Not here yet: interrupts (waiting polls), fences and buffers as file descriptors.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
extern "C" void loader_log_line(const char * line);

/* The five-second summary exceeds hakios_tl's 192-byte line buffer. Like
 * minisandbox_say(), send the complete line to the live ring and kernel log. */
static void gpuSummary(const char * format, ...) __printflike(1, 2);
static void
gpuSummary(const char * format, ...)
{
	char line[1024];
	va_list list;
	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	IOLog("%s", line);
}

extern "C" {
/* drivers/gpu/hakios.adreno.c ("the GPU kept on"; declared in bsd/sys/hakios.input.h, which is not seen from here) */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_remapped(void);
bool HakiosGPUSurfaceAlias(MiniIOSurface * surface, uint64_t phys);
unsigned hakios_display_dpu_write_busy(uint64_t phys);
void hakios_gpu_to_memory(void * start, uint64_t size);
void hakios_gpu_from_memory(void * start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
/* fb_dpu=4 (a2), MiniIOMobileFramebuffer.cpp: the surface's fence, the copy of its frame back into its own memory
 * and the remap of its GPU mappings, as one operation under gGPULock and bounded in time.
 * The copy is MiniIOMobileFramebuffer.cpp's carveCopy(): aligned accesses only, the carve-out is Device memory. */
void hakios_carve_copy(void * to, const void * from, size_t n);
bool HakiosGPUSurfaceDemote(MiniIOSurface * surface, const void * frame, uint64_t fence_ms, uint64_t budget_ms,
    unsigned * why);
extern uint32_t gpu_log;
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hakios_gpu_report(const char * why);
bool hakios_gpu_faulted(void);
bool hakios_gpu_recover(void);
void hakios_gpu_mapping(uint64_t address, uint64_t size, uint32_t surface, uint32_t fence, const char * event);
void hakios_gpu_stop(void);
void umi_gpu_screenoff_vote(bool off);
void hakios_cpufreq_boost(uint32_t milliseconds);
void umi_gpu_pm_update(bool off);
bool umi_pm_screen_off(void);

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
/* fb_dpu=4 (MiniIOMobileFramebuffer.cpp): a submission WROTE a framed surface -- the frame the panel shows may now
 * hold pixels its own copy does not; healWrites() fetches it once more when this fence is done (with a kill switch:
 * /var/db/hakios-display/write-kick-off). Both take only the framebuffer's own lock: safe under gGPULock. */
void MiniIOMobileFramebufferSurfaceWritten(MiniIOSurface * surface, uint32_t fence);
bool MiniIOMobileFramebufferWritesPending(void);
void MiniIOMobileFramebufferHealWrites(void);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kHakiosShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

#define kHakiosDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_BO_HAKIOS_UNCLEARED 0x00000100      /* hakios-metal's kBoUncleared: a kept one of the client's given again uncleared */
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_BO_WRITE     0x0002
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (hakios_gpu_...) */
static bool gGPUReported;
static uint64_t gGPULastSubmit;
static void gpuIdleThread(void *, wait_result_t);
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */
static bool gGPUFastCache;                      /* boot-arg gpu_fc=1: trust unchanged IOSurface seeds for GPU reads */

/*
 * fb_dpu=4 (MiniIOMobileFramebuffer.cpp): the display's frame surfaces drawn straight into frames of the splash
 * carve-out, which the display scans. Such a surface is "aliased": every GPU mapping of its pixels points at its
 * carve-out frame instead of its own memory (which the CPU still sees: stale from then on). The big surfaces'
 * mappings are kept track of for that; all of it with gGPULock held.
 */
#define GPU_SURFACE_MAPS        64
#define GPU_ALIASES             4
static struct {
	MiniIOSurface *         surface;
	uint64_t                address, size;
} gSurfaceMaps[GPU_SURFACE_MAPS];
static struct {
	MiniIOSurface *         surface;
	uint64_t                phys;
} gAliases[GPU_ALIASES];

static uint64_t
aliasOf(const MiniIOSurface * surface)
{
	for (unsigned i = 0; surface != NULL && i < GPU_ALIASES; i++) {
		if (gAliases[i].surface == surface) {
			return gAliases[i].phys;
		}
	}
	return 0;
}

static void
surfaceMapForget(uint64_t address)
{
	for (unsigned i = 0; i < GPU_SURFACE_MAPS; i++) {
		if (gSurfaceMaps[i].surface != NULL && gSurfaceMaps[i].address == address) {
			gSurfaceMaps[i].surface = NULL;
		}
	}
}

/* (the pages of one mapping onto `phys`, or back onto the surface's own memory when phys is 0) */
static bool
surfaceMapMove(MiniIOSurface * surface, uint64_t address, uint64_t size, uint64_t phys)
{
	bool mapped = true;

	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t page = phys != 0 ? phys + offset :
		    surface->memory->getPhysicalSegment(PAGE_SIZE + offset, &length, kIOMemoryMapperNone);

		mapped = page != 0 && hakios_gpu_map(address + offset, page, false);
	}
	hakios_gpu_remapped();
	return mapped;
}

static bool gpuWait(uint32_t fence, uint64_t milliseconds);

/*
 * The body of HakiosGPUSurfaceAlias() and HakiosGPUSurfaceDemote(), with gGPULock held: every mapping of the
 * surface onto `phys`, or (phys 0) back onto the surface's own memory, and the alias record kept to match.
 * A page that cannot be moved takes every page back to where the mappings were before the call, so that no
 * surface is ever left with some pages one memory and some another. (The alias direction used to roll back
 * only when phys != 0; the un-alias direction left the surface half mapped and zeroDemote() trusted the frame.)
 */
static bool
surfaceAliasLocked(MiniIOSurface * surface, uint64_t phys)
{
	bool done = true;
	int slot = -1;
	uint64_t previous = 0;

	for (unsigned i = 0; i < GPU_ALIASES; i++) {
		if (gAliases[i].surface == surface || (slot < 0 && phys != 0 && gAliases[i].surface == NULL)) {
			slot = (int) i;
		}
	}
	if (slot < 0 || (phys == 0 && gAliases[slot].surface != surface)) {
		return phys == 0;
	}
	if (gAliases[slot].surface == surface) {
		previous = gAliases[slot].phys;
	}
	gAliases[slot].surface = phys != 0 ? surface : NULL;
	gAliases[slot].phys = phys;
	for (unsigned i = 0; i < GPU_SURFACE_MAPS && hakios_gpu_ready(); i++) {
		if (gSurfaceMaps[i].surface == surface) {
			done = surfaceMapMove(surface, gSurfaceMaps[i].address, gSurfaceMaps[i].size, phys) && done;
		}
	}
	if (!done) {
		/* (whatever could not be moved goes back: previous 0 is the surface's own memory again) */
		bool back = true;

		gAliases[slot].surface = previous != 0 ? surface : NULL;
		gAliases[slot].phys = previous;
		for (unsigned i = 0; i < GPU_SURFACE_MAPS; i++) {
			if (gSurfaceMaps[i].surface == surface) {
				back = surfaceMapMove(surface, gSurfaceMaps[i].address, gSurfaceMaps[i].size, previous) && back;
			}
		}
		if (!back) {
			/*
			 * Not even the way back took: the surface's pages are split between two memories, and nothing
			 * the GPU draws into it can be trusted. Said once; the surface is never aliased or demoted again
			 * (zeroSlot(), zeroDemote()).
			 */
			surface->gpu_broken = true;
			hakios_tl("GPU-alias: surface %u: a mapping could not be put back; the surface is never aliased again\n",
			    surface->sid);
		}
	}
	return done;
}

/*
 * Alias `surface` onto the physically contiguous `phys` (round_page(allocSize) bytes), or (phys 0) end its alias.
 * Every existing GPU mapping of it is moved at once; later ones are made onto `phys` (surfaceBuffer()).
 */
bool
HakiosGPUSurfaceAlias(MiniIOSurface * surface, uint64_t phys)
{
	bool done;

	/* (an un-alias of a broken surface is still worth doing: it moves every page -- both memories' -- back onto
	 * the surface's own, which is the only consistent mapping there is) */
	if (gGPULock == NULL || surface == NULL || surface->client != NULL || (phys != 0 && surface->gpu_broken)) {
		return false;
	}
	IOLockLock(gGPULock);
	/* A remap changes the pages under every importer, so retire all uses of the surface first. */
	while (hakios_gpu_ready() && surface->drawn != 0 && !hakios_gpu_done(surface->drawn)) {
		uint32_t fence = surface->drawn;
		gpuWait(fence, GPU_PATIENCE_MS);
		if (!hakios_gpu_done(fence)) {
			IOLockUnlock(gGPULock);
			return false;
		}
	}
	done = surfaceAliasLocked(surface, phys);
	IOLockUnlock(gGPULock);
	return done;
}

/*
 * Wait, with gGPULock held (gpuWait() lets it go between looks), until nothing the surface's is still in flight,
 * within `milliseconds` of entering this function: 1 when it is quiet. A submission made while a wait is in
 * progress is waited for in the same budget, so the fence that answers is the last one the surface had. 0: not
 * quiet in time (a caller that must not block says so and lets the frame be).
 */
static bool
surfaceRetireLocked(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), budget, spent, left;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &budget);
	for (;;) {
		uint32_t fence = surface->drawn;

		if (!hakios_gpu_ready() || fence == 0 || hakios_gpu_done(fence)) {
			return true;
		}
		spent = mach_absolute_time() - began;
		if (spent >= budget) {
			return false;
		}
		absolutetime_to_nanoseconds(budget - spent, &left);
		left = MAX(1ull, left / NSEC_PER_MSEC);
		if (!gpuWait(fence, left)) {
			return false;
		}
		/* (the lock is back: if the surface was submitted again while waiting, the loop waits for that too) */
	}
}

/*
 * fb_dpu=4 (a2), MiniIOMobileFramebuffer.cpp: the surface's picture copied from its carve-out frame back into its
 * own memory, then every GPU mapping of it put back onto that memory -- as one operation against the GPU's
 * submissions, under gGPULock from the fence that is waited for to the remap: the submit path sets
 * surface->drawn under this lock (submit()), so no submission naming the surface can start between the two, the
 * copy cannot read the frame while the GPU writes it, and the remap cannot cut a submission in two.
 * `frame` is the carve-out frame, Device memory to this processor (hakios_carve_copy()).
 * `fence_ms` bounds the wait for the surface's GPU work, `budget_ms` the whole call: when either runs out, or a
 * mapping cannot be put back, 0 is returned and the frame is still the surface's (the caller must not reuse it);
 * a copy already made is then harmless, as a surface's own memory is unused while it is aliased.
 * `fence_ms` 0 asks without waiting: 1 comes back only if the GPU is already done with the surface.
 * `why` says what stopped it (the fbzero: line counts them apart, MiniIOMobileFramebuffer.cpp): 0 done, 1 the
 * surface's GPU work was not finished (or it is not a surface this may touch), 2 the whole operation's budget ran
 * out, 3 a GPU mapping could not be put back.
 */
bool
HakiosGPUSurfaceDemote(MiniIOSurface * surface, const void * frame, uint64_t fence_ms, uint64_t budget_ms,
    unsigned * why)
{
	uint64_t began = mach_absolute_time(), budget, bytes;
	bool done;

	if (why != NULL) {
		*why = 1;
	}
	if (gGPULock == NULL || surface == NULL || surface->client != NULL || frame == NULL) {
		return false;
	}
	nanoseconds_to_absolutetime(budget_ms * NSEC_PER_MSEC, &budget);
	IOLockLock(gGPULock);
	if (!surfaceRetireLocked(surface, fence_ms)) {
		IOLockUnlock(gGPULock);
		return false;                           /* (still being drawn into: the frame is not ours to take) */
	}
	if (mach_absolute_time() - began >= budget) {
		if (why != NULL) {
			*why = 2;
		}
		IOLockUnlock(gGPULock);
		return false;                           /* (the fence used the budget up: no copy under it either) */
	}
	bytes = (uint64_t) surface->info.bytesPerRow * surface->info.height;
	/* the picture the GPU put into the frame, into the surface's own memory (Device memory: the copy is aligned) */
	hakios_carve_copy(surface->pixels(), frame, (size_t) bytes);
	__asm__ volatile("dsb sy" ::: "memory");
	hakios_gpu_to_memory(surface->pixels(), bytes);
	if (mach_absolute_time() - began >= budget) {
		if (why != NULL) {
			*why = 2;
		}
		IOLockUnlock(gGPULock);
		return false;                           /* (no remap past the budget: the frame stays the surface's) */
	}
	done = surfaceAliasLocked(surface, 0);
	IOLockUnlock(gGPULock);
	if (done) {
		if (why != NULL) {
			*why = 0;
		}
	} else if (why != NULL) {
		*why = 3;
	}
	return done;
}

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint64_t fastSkipped;                   /* bytes not cleaned by gpu_fc=1 (seed unchanged, read-only use) */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
	uint64_t submitWall, submitMax, aliasWait, submitLock, ringTime, copyinTime;
	uint32_t scanWaits, pendingWaits, scanTimeouts, writeShownLate;
} gGPUStat;

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (hakios_gpu_faulted() || mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	if ((milliseconds >= 1000 || hakios_gpu_faulted()) && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("no answer");
		if (hakios_gpu_recover()) gGPUReported = false;
	}
	return false;
}

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	bool done = true;

	if (gGPULock != NULL) {
		IOLockLock(gGPULock);
		uint32_t fence = surface->drawn;
		bool ready = hakios_gpu_ready();
		bool stable = fence == 0 || !ready;
		done = stable;
		/* Waiting drops gGPULock; if the surface was submitted again, wait for its fence too. */
		for (unsigned retry = 0; ready && fence != 0 && retry < 4; retry++) {
			done = gpuWait(fence, milliseconds);
			if (!done) break;
			uint32_t latest = surface->drawn;
			if (latest == fence) {
				stable = true;
				break;
			}
			fence = latest;
			if (fence == 0) {
				stable = true;
				break;
			}
		}
		if (done && ready && !stable) done = false;
		if (done && ready && stable && surface->gpu_written) {
			hakios_gpu_from_memory(surface->kpixels, surface->info.allocSize);
			surface->gpu_written = false;
		}
		IOLockUnlock(gGPULock);
	}
	return done;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hakios_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left, end;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	/* the seconds are the caller's: a number too big to be nanoseconds is a time far ahead (or far behind), not a wrapped one */
	if (__builtin_mul_overflow(when->seconds, (int64_t) NSEC_PER_SEC, &end) || __builtin_add_overflow(end, when->nanoseconds, &end)) {
		return when->seconds > 0 && when->nanoseconds >= 0 ? GPU_PATIENCE_MS : 0;
	}
	if (end <= 0) {
		return 0;                       /* (the clock's reading is never negative: this is past) */
	}
	left = end - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HakiosGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOService);

bool
HakiosGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setName("HakiosGPU");
	IOLog("HakiosGPU: zeroed allocations: fresh pages and completed buffers before pool reuse\n");
	registerService();
	return true;
}

void
HakiosGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosGPU * gpu;
	uint32_t cached = 0;
	uint32_t fastCache = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPUFastCache = PE_parse_boot_argn("gpu_fc", &fastCache, sizeof(fastCache)) && fastCache != 0;
	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosGPU: the service did not start\n");
	}
	gpu->release();
	thread_t idleThread;
	if (kernel_thread_start(gpuIdleThread, NULL, &idleThread) == KERN_SUCCESS) {
		thread_deallocate(idleThread);
		IOLog("GPU idle: 305 MHz after 100 ms without submissions; restore before submit\n");
	} else {
		IOLog("GPU idle: could not start policy thread\n");
	}
}

/* The run is ending (hakios.adreno.c): nobody is in the middle of anything when the GPU goes. */
void
HakiosGPUStop(void)
{
	IOLockLock(gGPULock);
	hakios_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* Both idle and screen policy serialize with submit, recovery and teardown.
 * A completed fence alone is not inactivity: retain the OPP between frames. */
void
umi_gpu_pm_update(bool off)
{
	if (gGPULock == NULL) return;
	IOLockLock(gGPULock);
	uint64_t hold;
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &hold);
	if (!gGPUReported && hakios_gpu_ready() && !hakios_gpu_faulted() &&
	    hakios_gpu_done(hakios_gpu_last())) {
		umi_gpu_screenoff_vote(off || mach_absolute_time() - gGPULastSubmit >= hold);
	}
	IOLockUnlock(gGPULock);
}

static void
gpuIdleThread(void *arg __unused, wait_result_t wr __unused)
{
	for (;;) {
		/*
		 * (fb_dpu=4: a frame the GPU wrote after the panel fetched it is fetched once more when that write is
		 * done; a frame waiting for it is looked at every few milliseconds, otherwise every 100. Bounded: there
		 * is one look an arm, and an arm is only made by a write to a framed surface.)
		 */
		IOSleep(MiniIOMobileFramebufferWritesPending() ? 8 : 100);
		MiniIOMobileFramebufferHealWrites();
		umi_gpu_pm_update(umi_pm_screen_off());
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
class HakiosGPUUserClient;
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
	HakiosGPUUserClient *           owner;          /* whose kept buffers it joins once done (NULL: that client is gone) */
	struct gpu_left *               next;
};
/* Retired mappings outlive their user client; any client can reap them after the fence. */
static struct gpu_left * gLeft;

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A kept buffer is cleared when it is taken again, out of the lock and before it has a
 * handle (unless it was asked for MSM_BO_HAKIOS_UNCLEARED, an attachment): the next allocation
 * must not expose its old contents. At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0: none kept; all kept buffers are cleared */

class HakiosGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hakios_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
HakiosGPUUserClient::start(IOService * provider)
{
	if (!hakios_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

/* Remove the handle now; retain both the mapping and the IOSurface until its last use retires. */
void
HakiosGPUUserClient::freeBuffer(struct gpu_buffer * slot)
{
	struct gpu_buffer saved = *slot;
	struct gpu_buffer * b = &saved;
	slot->memory = NULL;
	slot->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;

	bool busy = hakios_gpu_ready() && b->fence != 0 && !hakios_gpu_done(b->fence);
	struct gpu_left * left = busy ? IONew(struct gpu_left, 1) : NULL;
	if (busy && left == NULL) {
		gpuWait(b->fence, GPU_PATIENCE_MS);
		busy = !hakios_gpu_done(b->fence);
	}
	if (busy) {
		hakios_gpu_mapping(b->address, b->size, b->surface ? b->surface->sid : 0, b->fence, "defer");
		if (left != NULL) {
			left->memory = b->memory;
			left->surface = b->surface;
			left->address = b->address;
			left->size = b->size;
			left->fence = b->fence;
			left->flags = b->flags;
			left->owner = this;
			left->next = gLeft;
			gLeft = left;
		} else {
			/* Failed reset and no bookkeeping memory: keep BOTH references, including the alias owner. */
			IOLog("HakiosGPU: retaining busy mapping %#llx after allocation/recovery failure\n", b->address);
		}
		return;
	}
	if (b->surface == NULL && hakios_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
		return;
	}
	if (hakios_gpu_ready()) {
		hakios_gpu_mapping(b->address, b->size, b->surface ? b->surface->sid : 0, b->fence, "unmap");
		surfaceMapForget(b->address);
		hakios_gpu_unmap(b->address, b->size);
		hakios_gpu_address_free(b->address, b->size);
	}
	b->memory->release();
	if (b->surface != NULL) MiniIOSurfaceDrop(b->surface);
}

/* Global: a process exit must not lose the queue or hand its buffers to another client's cache. */
void
HakiosGPUUserClient::reap(void)
{
	for (struct gpu_left ** link = &gLeft; *link != NULL;) {
		struct gpu_left * left = *link;
		if ((hakios_gpu_ready() && !hakios_gpu_done(left->fence)) ||
		    (left->surface == NULL && left->owner != NULL && left->owner != this)) {
			link = &left->next;
			continue;
		}
		/* A frame's texture is given back while the GPU still draws with it: kept for the next frame's, as one given back done. */
		if (left->owner == this && left->surface == NULL && hakios_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			*link = left->next;
			IODelete(left, struct gpu_left, 1);
			continue;
		}
		if (hakios_gpu_ready()) {
			hakios_gpu_mapping(left->address, left->size, left->surface ? left->surface->sid : 0, left->fence, "retired-unmap");
			surfaceMapForget(left->address);
			hakios_gpu_unmap(left->address, left->size);
			hakios_gpu_address_free(left->address, left->size);
		}
		left->memory->release();
		if (left->surface != NULL) MiniIOSurfaceDrop(left->surface);
		*link = left->next;
		IODelete(left, struct gpu_left, 1);
	}
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (hakios_gpu_ready()) {
			gpuWait(hakios_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (struct gpu_left * left = gLeft; left != NULL; left = left->next) {
			if (left->owner == this) {
				left->owner = NULL;     /* (still busy: whoever reaps it next frees it) */
			}
		}
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HakiosGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HakiosGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
HakiosGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hakios_gpu_address(size)) == 0) {
		IOLog("HakiosGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hakios_gpu_mapped();
	if (!mapped) {
		hakios_gpu_unmap(b->address, size);
		hakios_gpu_address_free(b->address, size);
		IOLog("HakiosGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	hakios_gpu_mapping(b->address, size, 0, 0, "buffer-map");
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
HakiosGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hakios_gpu_ready()) {
		hakios_gpu_mapping(k->address, k->size, 0, 0, "cache-unmap");
		hakios_gpu_unmap(k->address, k->size);
		hakios_gpu_address_free(k->address, k->size);
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
HakiosGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	/* (up to half the budget: Maps makes a 4x MSAA depth/stencil of the screen, 56 MB, afresh every frame) */
	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 2 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	/* (not cleared here, under the lock: gemNew clears it when it is taken, before it has a handle) */
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HakiosGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	/*
	 * Taken out of the pool under the lock, cleared without it (a 56 MB one takes milliseconds every
	 * client's submit would wait for), and only then given a handle: until then nothing names it.
	 */
	if (found != NULL) {
		struct gpu_kept taken = *found;

		found->memory = NULL;
		fKeptBytes -= size;
		IOLockUnlock(gGPULock);
		if (!(taken.flags & MSM_BO_HAKIOS_UNCLEARED)) {     /* (its own old contents: an attachment's start undefined) */
			bzero(taken.kernel, (size_t) size);
		}
		hakios_gpu_to_memory(gGPUCached ? taken.kernel : NULL, gGPUCached ? size : 0);
		IOLockLock(gGPULock);
		for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
			uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

			if (fBuffers[h].memory == NULL) {
				b = &fBuffers[h];
				bzero(b, sizeof(*b));
				b->memory = taken.memory;
				b->kernel = taken.kernel;
				b->address = taken.address;
				b->size = taken.size;
				b->flags = taken.flags;
				fNextBuffer = h + 1;
				fBufferCount++;
				fBufferBytes += size;
				request->handle = h + 1;
				gGPUStat.given_again++;
				IOLockUnlock(gGPULock);
				return kIOReturnSuccess;
			}
		}
		fKeptBytes += size;             /* (dropKept takes it off the count again) */
		dropKept(&taken);
		IOLockUnlock(gGPULock);
		IOLog("HakiosGPU: no handles left for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	IOLockUnlock(gGPULock);
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HakiosGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	if (gGPUCached) {
		hakios_gpu_to_memory(memory->getBytesNoCopy(), size);
	}

	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kHakiosDescribe), no buffer is made.
 */
IOReturn
HakiosGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		/* (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows the GPU takes) */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else {
		uint64_t alias = aliasOf(s);
		IOMemoryDescriptor * carve = alias != 0 ? IOMemoryDescriptor::withPhysicalAddress(alias, size, kIODirectionInOut) : NULL;

		if ((alias == 0 || carve != NULL) &&
		    (b = newBuffer(carve != NULL ? carve : s->memory, carve != NULL ? 0 : PAGE_SIZE, size, 0, &request->handle)) != NULL) {
			b->memory = memory;
			b->kernel = s->kbase + PAGE_SIZE;
			b->surface = s;
			hakios_gpu_mapping(b->address, b->size, s->sid, 0, "surface-map");
			gGPUStat.surfaces++;
			for (unsigned i = 0; size >= (4u << 20) && i < GPU_SURFACE_MAPS; i++) {
				if (gSurfaceMaps[i].surface == NULL) {
					gSurfaceMaps[i].surface = s;
					gSurfaceMaps[i].address = b->address;
					gSurfaceMaps[i].size = size;
					break;
				}
			}
		}
		OSSafeReleaseNULL(carve);
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	uint64_t entry = mach_absolute_time(), copyDone = 0, aliasTime = 0, lockTime = 0, ringTime = 0, mark = 0, submitWall;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("HakiosGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	hakios_cpufreq_boost(32);
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	copyDone = mach_absolute_time();
	/*
	 * Submit wait v29: only GPU writes conflict with the DPU's reads. Check every
	 * alias under gGPULock, and keep it through ring submission/fence publication
	 * once safe, so alias remapping cannot slip between the check and submission.
	 * Sleep outside it; re-read BOs and aliases after every sleep (no stale refs).
	 */
	{
		uint64_t started = mach_absolute_time(), elapsed;
		unsigned reasons = 0, busy;
		bool timedOut = false;

		for (;;) {
			mark = mach_absolute_time();
			IOLockLock(gGPULock);
			lockTime += mach_absolute_time() - mark;
			busy = 0;
			for (uint32_t i = 0; i < request->nr_bos; i++) {
				if (!(bos[i].flags & MSM_SUBMIT_BO_WRITE)) continue;
				struct gpu_buffer * b = buffer(bos[i].handle);
				uint64_t phys = b != NULL ? aliasOf(b->surface) : 0;
				if (phys != 0) busy |= hakios_display_dpu_write_busy(phys);
			}
			reasons |= busy;
			if (busy == 0) break;
			absolutetime_to_nanoseconds(mach_absolute_time() - started, &elapsed);
			if (elapsed >= 50 * NSEC_PER_MSEC) {
				timedOut = true;
				break;
			}
			IOLockUnlock(gGPULock);
			delay_for_interval(250, NSEC_PER_USEC);
		}
		if (reasons != 0) {
			aliasTime = mach_absolute_time() - started - lockTime;
			gGPUStat.scanWaits += (reasons & 1) != 0;
			gGPUStat.pendingWaits += (reasons & 2) != 0;
			gGPUStat.aliasWait += aliasTime;
		}
		if (timedOut && reasons != 1) {
			gGPUStat.scanTimeouts++;
			if (gGPUStat.scanTimeouts == 1) {
				hakios_tl("GPU-submit: scanout write wait timed out; submission refused (reasons %u)\n", reasons);
			}
			ret = kIOReturnTimeout;
			IOLockUnlock(gGPULock);
			goto out;
		}
		if (timedOut) {
			/* (only the shown frame was in the way and its successor had not latched within the limit: let
			 * the submission go and count it. Refusing it loses the whole frame -- CoreAnimation's black. The
			 * frame on the panel is drawn into while still fetched: the next flip fetches the successor, so
			 * at most that one frame is torn. Line once per boot; the count is in GPU-waits:) */
			gGPUStat.writeShownLate++;
			if (gGPUStat.writeShownLate == 1) {
				hakios_tl("GPU-submit: shown-frame write wait timed out; submission let through (reasons %u)\n",
				    reasons);
			}
		}
	}
	if (gGPUReported || !hakios_gpu_ready()) {
		ret = kIOReturnNotReady;
		IOLockUnlock(gGPULock);
		goto out;
	}
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL || bos[i].presumed != b->address) {
			IOLog("HakiosGPU: refused stale BO: handle %u presumed %#llx actual %#llx\n",
			    bos[i].handle, bos[i].presumed, b ? b->address : 0ull);
			IOLockUnlock(gGPULock);
			goto out;
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (aliasOf(b->surface) != 0) {
				continue;                       /* (fb_dpu=4: its GPU memory is the carve-out frame, not cached) */
			}
			/* The normal path is deliberately conservative: a client may have written pixels without moving
			 * IOSurface's seed, so an already-mapped surface is pushed again.  gpu_fc=1 is an A/B switch for
			 * CoreAnimation's read-only texture traffic: when the seed is unchanged and this submit does not
			 * write the surface, no CPU line could have become newer since the last push.  Writes remain on the
			 * conservative path because a load/store render target can read old pixels before overwriting them. */
			const bool fastRead = gGPUFastCache && b->pushed != 0 && seed == b->seed &&
			    !(bos[i].flags & MSM_SUBMIT_BO_WRITE);
			if (fastRead || (seed == b->seed && !b->seen && began - b->pushed < again &&
			    !(bos[i].flags & MSM_SUBMIT_BO_WRITE))) {
				if (fastRead) gGPUStat.fastSkipped += b->size;
				continue;
			}
			b->seed = seed;
			b->pushed = began;
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		hakios_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	hakios_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("HakiosGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	mark = mach_absolute_time();
	umi_gpu_screenoff_vote(false);
	gGPULastSubmit = mach_absolute_time();
	request->fence = hakios_gpu_submit(addresses, words, count);
	ringTime = mach_absolute_time() - mark;
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hakios_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * Keep read fences too: they retain BO mappings and order later surface access.
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
				if ((bos[i].flags & MSM_SUBMIT_BO_WRITE) && aliasOf(b->surface) == 0) {
					b->surface->gpu_written = true;
				}
				/* (fb_dpu=4: an aliased surface's frame is what the panel fetched; a write of it that comes after
				 * that fetch is fetched again once this fence is done -- MiniIOMobileFramebuffer.cpp) */
				if ((bos[i].flags & MSM_SUBMIT_BO_WRITE) && aliasOf(b->surface) != 0) {
					MiniIOMobileFramebufferSurfaceWritten(b->surface, request->fence);
				}
			}
		}
	}
	reap();
	if (request->fence == 0 && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("the ring is full");
		if (hakios_gpu_recover()) gGPUReported = false;
	}
	if (fSubmits++ < 4) {
		/* (addresses and words are only set up to `count`: with no command buffer among the commands, the first would be stack garbage) */
		IOLog("HakiosGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    count != 0 ? addresses[0] : 0ull, count != 0 ? words[0] : 0u);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	submitWall = mach_absolute_time() - entry;
	gGPUStat.submitWall += submitWall;
	gGPUStat.submitMax = MAX(gGPUStat.submitMax, submitWall);
	gGPUStat.submitLock += lockTime;
	gGPUStat.ringTime += ringTime;
	gGPUStat.copyinTime += copyDone - entry;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = entry;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hakios_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		gpuSummary("HakiosGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		gpuSummary("HakiosGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission (%llu KB skipped by gpu_fc=%d); the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    gGPUStat.fastSkipped >> 10, gGPUFastCache ? 1 : 0, timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		uint64_t wallNs, maxNs, aliasNs, lockNs, ringNs, copyNs;
		absolutetime_to_nanoseconds(gGPUStat.submitWall, &wallNs);
		absolutetime_to_nanoseconds(gGPUStat.submitMax, &maxNs);
		absolutetime_to_nanoseconds(gGPUStat.aliasWait, &aliasNs);
		absolutetime_to_nanoseconds(gGPUStat.submitLock, &lockNs);
		absolutetime_to_nanoseconds(gGPUStat.ringTime, &ringNs);
		absolutetime_to_nanoseconds(gGPUStat.copyinTime, &copyNs);
		gpuSummary("GPU-submit: %u calls wall %llu max %llu us; offscreen %llu lock %llu copyin %llu clean %llu ring %llu us total; "
		    "buffers %u made %llu us each, %u kept given again\n",
		    gGPUStat.submissions, wallNs / 1000, maxNs / 1000, aliasNs / 1000, lockNs / 1000, copyNs / 1000, flushing / 1000, ringNs / 1000,
		    gGPUStat.made, gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again);
		uint64_t busyTenths = worked * 1000000 / nanos;
		hakios_tl("GPU-busy: %llu.%llu%%; the GPU worked %llu us / %llu ms, %u timed submissions\n",
		    busyTenths / 10, busyTenths % 10, worked, nanos / NSEC_PER_MSEC, timed);
		hakios_tl("GPU-waits: scan %u pending %u timeout %u; scanout %llu ms; fence %u, %llu ms total per window; write-shown late=%u\n",
		    gGPUStat.scanWaits, gGPUStat.pendingWaits, gGPUStat.scanTimeouts, aliasNs / NSEC_PER_MSEC,
		    gGPUStat.waits, waiting / NSEC_PER_MSEC, gGPUStat.writeShownLate);
		bzero(&gGPUStat, sizeof(gGPUStat));
		gGPUStat.since = mach_absolute_time();
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
HakiosGPUUserClient::show(const struct hakios_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	/* rowBytes is the caller's own number: without a limit it asks for terabytes of wired memory (no more than a buffer may be) */
	if (size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hakios_surface surface;
		struct hakios_show show;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_gpu_ready() && selector != kHakiosShow) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = hakios_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(hakios_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, hakios_gpu_last());
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		/* a fence that was never issued is never done: refuse it, so that waiting for it cannot make gpuWait() declare the GPU dead for every client */
		if ((int32_t)(r.waitFence.fence - hakios_gpu_last()) > 0) {
			IOLockUnlock(gGPULock);
			ret = kIOReturnBadArgument;
			break;
		}
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
