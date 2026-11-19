/*
 * Where the panel's scan is, on the POCO X5 Pro (redwood): for iokit/Kernel/MiniIOMobileFramebuffer.cpp, which
 * puts every frame into the memory the panel is sent from and, without this, can only guess when the panel
 * reads it -- which is why the picture tore and why frames came at a pace of their own, beside the panel's.
 *
 * The panel is a DSI video panel, sent the framebuffer over and over by the display controller's timing engine
 * at the rate of its timing registers, which nobody touches after the firmware: 2432 lines a frame of which
 * 2400 are shown, 119.93 frames a second (seen: hakios.adreno.c, display_probe). The interface the panel hangs
 * on (INTF 1, dt: mdss_dsi0's mdp_intf_base) counts the frames and, within a frame, the line being sent; this
 * reads those two counters. To read them at all two bus clocks have to run, which the firmware leaves off: the
 * one of the display's clock controller (in GCC) and, in that controller, the one of the display block's
 * registers. They are turned on here, as Linux keeps them on and as display_probe did on this phone; that is all
 * that is ever written. The timing itself is only read.
 *
 * Boot-arg fb_scan=1 (on the phone; QEMU has no such block): on. Then the framebuffer driver
 *   - starts copying a frame into the panel's memory only where the scan lets the copy land whole in one
 *     refresh (hakios_display_window(): see MiniIOMobileFramebuffer.cpp, show), so that no refresh shows
 *     part of one frame and part of the next -- no tearing;
 *   - runs at the panel's own rate (120 Hz) and tells CoreAnimation that rate, and its refresh notifications
 *     come when the panel's frames begin, not on a clock of their own that drifts against them.
 *
 * Registers: Linux drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c (INTF), drivers/clk/qcom/dispcc-sc7280.c.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <console/video_console.h>
#include <sys/errno.h>
#include <sys/hakios.display.h>
#include <sys/hakios.bootlog.h>
#include <sys/umi_screenoff.h>
#include <sys/hakios.board.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include "../../iokit/Kernel/umi_dpu_layer.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* Preserve early clock/timing diagnostics in quiet boots and live loader logs. */
extern void hakios_tl(const char *, ...) __printflike(1, 2);
#define printf hakios_tl

#define GCC_PHYS                hakios_board_addr("gcc", 0x00100000UL)
#define GCC_DISP_AHB_CBCR       (umi ? 0x0b00c : 0x27004)
#define DISPCC_PHYS             hakios_board_addr("dispcc", 0x0af00000UL)    /* dt: clock-controller@af00000 */
#define DISPCC_MDSS_CORE_GDSCR  (umi ? 0x3000 : 0x1004)
#define DISPCC_MDSS_AHB_CBCR    (umi ? 0x2080 : 0x1050)
#define INTF1_PHYS              hakios_board_addr("intf", umi ? 0x0ae6b800UL : 0x0ae36000UL)
#define DSI0_PHYS               hakios_board_addr("dsi0", 0x0ae94000UL)
#define DSI_CTRL                0x0004
#define DSI_STATUS              0x0008
#define DSI_COMMAND_MODE_DMA_CTRL 0x003c
#define DSI_DMA_CMD_LENGTH      0x004c
#define DSI_TRIG_CTRL           0x0084
#define DSI_CMD_MODE_DMA_SW_TRIGGER 0x0090
#define DSI_INT_CTRL            0x0110
#define DSI_TEST_PATTERN_GEN_CTRL 0x015c
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x017c
#define DSI_TPG_DMA_FIFO_RESET  0x01ec
#define DSI_CTRL_ENABLE         (1u << 0)
#define DSI_CTRL_CMD_MODE_EN    (1u << 2)
#define DSI_STATUS_CMD_BUSY     ((1u << 1) | (1u << 2))
#define DSI_INT_CMD_DMA_DONE    (1u << 0)
#define DSI_INT_CMD_DMA_DONE_MASK (1u << 24)
#define UMI_BL_MIN_LEVEL        16u
#define UMI_BL_MAX_LEVEL        2047u
#define UMI_BL_DEFAULT_PERMILLE 600
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_HSYNC_CTL          0x008           /* clocks a line << 16 | of which the pulse */
#define INTF_VSYNC_PERIOD       0x00c           /* clocks a frame */
#define INTF_DISPLAY_V_START    0x01c
#define INTF_DISPLAY_V_END      0x024
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0

static vm_offset_t intf, gcc_map, dispcc_map, dsi0;
static struct hakios_display_scan scan;
static bool umi;
static bool command_mode;
static uint32_t te_initial, te_idle_start;

int hakios_display_command_mode(void) { return scan.ok && command_mode; }


static inline uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/*
 * Whether the display block's registers can be read right now: its power switch on and the two bus clocks
 * running. On these SoCs a read of a block whose clock is off does not fail, it hangs the bus -- the whole
 * phone, the loader's core with it. The clock controllers themselves always answer (GCC; dispcc once GCC's
 * clock for it runs, which is checked first), so this is asked before every read of the scan's counters,
 * and once the answer is no, the scan is given up for good: frames go back to the driver's own clock.
 */
static int
regs_alive(void)
{
	uint32_t v = rd(gcc_map, GCC_DISP_AHB_CBCR & 0xfffu);
	if (v == UINT32_MAX || !(v & CBCR_ENABLE) || (v & CBCR_OFF)) return 0;
	v = rd(dispcc_map, DISPCC_MDSS_CORE_GDSCR);
	if (v == UINT32_MAX || !(v & GDSC_POWER_ON)) return 0;
	v = rd(dispcc_map, DISPCC_MDSS_AHB_CBCR);
	return v != UINT32_MAX && (v & CBCR_ENABLE) && !(v & CBCR_OFF);
}

/* one bus clock on (and left on); 1 if it runs */
static int
clock_on(vm_offset_t base, uint32_t cbcr, const char *name)
{
	uint32_t v = rd(base, cbcr);
	if (v == UINT32_MAX) return 0;

	if (!(v & CBCR_ENABLE) || (v & CBCR_OFF)) {
		wr(base, cbcr, v | CBCR_ENABLE);
		delay_for_interval(10, NSEC_PER_MSEC);
		v = rd(base, cbcr);
	}
	if (!(v & CBCR_ENABLE) || (v & CBCR_OFF)) {
		printf("display: the %s clock does not take (%#x): no scan to go by\n", name, v);
		return 0;
	}
	return 1;
}

uint32_t
hakios_display_line(uint32_t *frame)
{
	uint32_t f, line;

	if (intf == 0) {
		return 0;
	}
	if (!regs_alive()) {
		printf("display: the display block's clock or power went off: the scan is no longer read\n");
		intf = 0;
		scan.ok = 0;
		return 0;
	}
	if (command_mode) {
		uint32_t raw = rd(intf, 0x298) & 0xffff;
		if (frame != NULL) *frame = 0;
		line = raw >= te_initial ? raw - te_initial : 0;
		return line < scan.lines ? line : scan.lines - 1;
	}
	/* the two counters together: a frame that begins between the two reads is read again */
	for (unsigned retry = 0; retry < 8; retry++) {
		f = rd(intf, INTF_FRAME_COUNT);
		line = rd(intf, INTF_LINE_COUNT);
		if (rd(intf, INTF_FRAME_COUNT) == f) {
			if (frame != NULL) *frame = f;
			return line;
		}
	}
	/* A broken counter must not trap the presenter in an unbounded retry. */
	intf = 0;
	scan.ok = 0;
	if (frame != NULL) {
		*frame = f;
	}
	return line;
}

const struct hakios_display_scan *
hakios_display_scan(void)
{
	return scan.ok ? &scan : NULL;
}

/* How many times a copy found no idle window in the panel's transfer (the fbzero: line's nowin=, and the
 * "display: no copy window" lines above). Incremented from the framebuffer's presenter and the staged path, so
 * atomically: the two are different threads (the backlight's wait_copy() is too). */
static uint32_t display_copy_nowin;
/* ... and how many of those could never have found one, whatever moment they were asked at: not waited for. */
static uint32_t display_copy_impossible;

uint32_t
hakios_display_copy_nowin(void)
{
	return os_atomic_load(&display_copy_nowin, relaxed);
}

uint32_t
hakios_display_copy_impossible(void)
{
	return os_atomic_load(&display_copy_impossible, relaxed);
}

/* The command-mode write pointer is the display DMA's progress through the
 * framebuffer. Copy only after it finishes, with room before the next TE.
 * Rendering/scaling must already be complete in a separate staging buffer.
 */
int
hakios_display_wait_copy(uint64_t copy_ns, uint64_t limit_ns)
{
	uint64_t start = mach_absolute_time(), elapsed;
	/* Which of the conditions below a timeout ran out on, said once per run of the same one (a copy window
	 * that comes clears it): 1 the phase wrapped during the sample, 2 the panel's transfer is not done,
	 * 3 not past the transfer, 4 the frame is over, 5 no room left after the transfer, 6 no window can ever
	 * come (below, said before any of it is waited for). */
	static unsigned said;

	if (!hakios_display_command_mode()) return 0;
	uint64_t budget = copy_ns + copy_ns / 4 + 500000;
	/*
	 * A window that cannot come is not waited for. The copy may start only with the panel's transfer done --
	 * written >= shown, which in command mode is from line te_idle_start on -- and with `budget` of the frame
	 * still to go, so the earliest moment that can take it leaves frame_ns - te_idle_start * line_ns of the
	 * frame. When that is not more than the copy's budget, no moment of any frame ever has it: Maps' 10.2 ms
	 * copies against the 8 ms the transfer leaves were three 20 ms timeouts a frame, 60 ms of waiting for
	 * nothing (INVESTIGATION-maps-compose-perf §1.4, §5). The windows that could come are still waited for, and
	 * still counted by nowin= when they do not. (The per-pixel loop below is the fallback path's now: a frame
	 * with layers is composed into the staging buffer and copied into a frame nothing is scanning.)
	 */
	uint64_t earliest = (uint64_t) te_idle_start * scan.line_ns;

	if (scan.frame_ns == 0 || budget >= scan.frame_ns || earliest >= scan.frame_ns ||
	    scan.frame_ns - earliest <= budget) {
		os_atomic_inc(&display_copy_impossible, relaxed);
		if (said != 6) {
			said = 6;
			printf("display: no copy window can ever come: %llu us of a %llu us frame, %llu us left after the "
			    "transfer; not waited for\n", (unsigned long long)(budget / 1000),
			    (unsigned long long)(scan.frame_ns / 1000),
			    (unsigned long long)((scan.frame_ns > earliest ? scan.frame_ns - earliest : 0) / 1000));
		}
		return 0;
	}
	for (;;) {
		uint32_t before = hakios_display_line(NULL);
		if (!scan.ok) return 0;
		uint32_t written = rd(intf, 0x2b0) & 0xffff;
		uint32_t phase = hakios_display_line(NULL);
		if (!scan.ok) return 0;
		uint64_t used = (uint64_t)phase * scan.line_ns;
		/* A completed DMA stays idle until the next TE. Restricting admission
		 * to the first 128 lines of that idle interval made a frame arriving
		 * slightly later wait an entire refresh, even with ample copy time.
		 * Use the measured copy budget instead. Read phase around the DMA
		 * status so a TE wrap cannot pair an old completion with a new frame.
		 */
		if (phase >= before && phase >= te_idle_start && written >= scan.shown &&
		    used < scan.frame_ns && budget < scan.frame_ns - used) {
			said = 0;
			return 1;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= limit_ns) {
			unsigned reason = phase < before ? 1 : written < scan.shown ? 2 : phase < te_idle_start ? 3 :
			    used >= scan.frame_ns ? 4 : 5;

			os_atomic_inc(&display_copy_nowin, relaxed);
			if (reason != said) {
				said = reason;
				printf("display: no copy window (%s): phase %u of %u (idle from %u), %u of %u lines written, "
				    "%llu us of the frame left, the copy wants %llu us\n", reason == 1 ?
				    "the phase wrapped during the sample" : reason == 2 ? "the panel's transfer is not done" :
				    reason == 3 ? "not past the transfer" : reason == 4 ? "the frame is over" :
				    "no room left after the transfer", phase, scan.lines, te_idle_start, written, scan.shown,
				    (unsigned long long)((scan.frame_ns - (used < scan.frame_ns ? used : scan.frame_ns)) / 1000),
				    (unsigned long long)(budget / 1000));
			}
			return 0;
		}
		/* A 50 us poll spins in XNU. Sleep through the known active interval
		 * instead of occupying a rendering core for several milliseconds.
		 * Recheck the registers after waking; only the final 200 us is polled.
		 */
		uint64_t ahead = 0, nap = 50000;
		if (phase < te_idle_start) {
			ahead = (uint64_t)(te_idle_start - phase) * scan.line_ns;
		} else if (used >= scan.frame_ns || budget >= scan.frame_ns - used) {
			ahead = (used < scan.frame_ns ? scan.frame_ns - used : 0) +
			    (uint64_t)te_idle_start * scan.line_ns;
		}
		if (ahead > 400000) nap = ahead - 200000;
		if (nap > limit_ns - elapsed) nap = limit_ns - elapsed;
		delay_for_interval((uint32_t)((nap + 999) / 1000), NSEC_PER_USEC);
	}
}

static uint32_t
umi_permille_to_level(int permille)
{
	if (permille <= 0) {
		return UMI_BL_MIN_LEVEL;
	}
	if (permille >= 1000) {
		return UMI_BL_MAX_LEVEL;
	}
	return UMI_BL_MIN_LEVEL + ((UMI_BL_MAX_LEVEL - UMI_BL_MIN_LEVEL) * (uint32_t)permille + 500u) / 1000u;
}

static void
umi_dsi_backlight_packet(uint32_t level, uint32_t *word0, uint32_t *word1)
{
	if (level > UMI_BL_MAX_LEVEL) {
		level = UMI_BL_MAX_LEVEL;
	}
	*word0 = 0xc0390003u;
	*word1 = 0xff000051u | (((level >> 8) & 0xffu) << 8) | ((level & 0xffu) << 16);
}

static int
umi_dsi_send_backlight(uint32_t level)
{
	uint32_t word0, word1, old_dma_ctrl, old_trig;
	int done = 0;

	if (!umi || dsi0 == 0 || !scan.ok || !regs_alive()) {
		return 0;
	}
	if ((rd(dsi0, DSI_CTRL) & (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) !=
	    (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) {
		return 0;
	}
	if (hakios_display_command_mode()) {
		(void)hakios_display_wait_copy(500000, 12000000);
	}
	for (unsigned i = 0; i < 40; i++) {
		if (!regs_alive()) {
			return 0;
		}
		if ((rd(dsi0, DSI_STATUS) & DSI_STATUS_CMD_BUSY) == 0) {
			break;
		}
		if (i == 39) {
			return 0;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}
	umi_dsi_backlight_packet(level, &word0, &word1);
	old_dma_ctrl = rd(dsi0, DSI_COMMAND_MODE_DMA_CTRL);
	old_trig = rd(dsi0, DSI_TRIG_CTRL);
	wr(dsi0, DSI_INT_CTRL, (rd(dsi0, DSI_INT_CTRL) & ~DSI_INT_CMD_DMA_DONE_MASK) | DSI_INT_CMD_DMA_DONE);
	if ((old_trig & 0xfu) != 0x4u) {
		wr(dsi0, DSI_TRIG_CTRL, (old_trig & ~0xfu) | 0x4u);
	}
	wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, 0x00030006u);
	wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, word0);
	wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, word1);
	wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, (old_dma_ctrl & ~0xc0000000u) | 0x14000000u);
	wr(dsi0, DSI_DMA_CMD_LENGTH, 8u);
	wr(dsi0, DSI_CMD_MODE_DMA_SW_TRIGGER, 1u);
	for (unsigned i = 0; i < 100; i++) {
		if (rd(dsi0, DSI_INT_CTRL) & DSI_INT_CMD_DMA_DONE) {
			wr(dsi0, DSI_INT_CTRL,
			    (rd(dsi0, DSI_INT_CTRL) & ~DSI_INT_CMD_DMA_DONE_MASK) | DSI_INT_CMD_DMA_DONE);
			done = 1;
			break;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}
	wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, 0u);
	wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 1u);
	delay_for_interval(2, NSEC_PER_USEC);
	wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 0u);
	wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, old_dma_ctrl);
	if ((old_trig & 0xfu) != 0x4u) {
		wr(dsi0, DSI_TRIG_CTRL, old_trig);
	}
	return done;
}

#include "umi_backlight.c"

static LCK_GRP_DECLARE(brightness_lck_grp, "hakios-brightness");
static LCK_MTX_DECLARE(brightness_mtx, &brightness_lck_grp);
static int brightness_permille = UMI_BL_DEFAULT_PERMILLE;
static uint32_t brightness_last_level;
static unsigned brightness_logged;

SYSCTL_DECL(_kern_hakios);

static int
sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = brightness_permille, error;
	uint32_t level;
	if (bl_mode == 1) {
		lck_mtx_lock(&bl_lock);
		value = (int)bl_base;
		lck_mtx_unlock(&bl_lock);
	}

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value < 0) value = 0;
	if (value > 1000) value = 1000;
	if (bl_mode == 1) {
		return bl_request(1, (unsigned)value, 120);
	}
	lck_mtx_lock(&brightness_mtx);
	brightness_permille = value;
	level = umi_permille_to_level(value);
	if (level != brightness_last_level) {
		int ok = umi_dsi_send_backlight(level);
		if (ok) {
			brightness_last_level = level;
		}
		if (brightness_logged < 8 || !ok) {
			brightness_logged++;
			printf("display: UMI backlight %d permille -> DCS 0x51 level %u (%s)\n",
			    value, level, ok ? "ok" : "skipped");
		}
	}
	umi_pm_display(value == 0, "backlight");
	lck_mtx_unlock(&brightness_mtx);
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    NULL, 0, sysctl_brightness, "I", "display brightness in permille (0..1000)");

static int
command_scan_start(void)
{
	struct hakios_display_scan measured = scan;
	uint32_t initial;
	uint32_t config = rd(intf, 0x288), refresh = rd(intf, 0x2b4);
	initial = rd(intf, 0x294) & 0xffff;
	if (!umi || !(rd(intf, 0x284) & 1) || (config & 0x180000) != 0x180000 ||
	    refresh != 0x80000001 || initial != 2340) return 0;
	uint64_t start = mach_absolute_time(), previous_time = start, first_wrap = 0, last_wrap = 0;
	uint64_t line_time = 0, line_count = 0, elapsed;
	uint32_t previous = rd(intf, 0x298) & 0xffff, busy_last = 0;
	unsigned wraps = 0, busy_samples = 0, idle_samples = 0;
	while (wraps < 4) {
		delay_for_interval(50, NSEC_PER_USEC);
		uint32_t raw = rd(intf, 0x298) & 0xffff;
		uint32_t written = rd(intf, 0x2b0) & 0xffff;
		uint64_t now = mach_absolute_time(), interval;
		absolutetime_to_nanoseconds(now - start, &elapsed);
		if (elapsed > 150000000 || raw < initial) return 0;
		/* Do not pair the previous frame's read phase with a new DMA transfer. */
		if ((rd(intf, 0x298) & 0xffff) < raw) continue;
		if (raw < previous) {
			if (wraps++ == 0) first_wrap = now;
			last_wrap = now;
		} else if (raw > previous) {
			absolutetime_to_nanoseconds(now - previous_time, &interval);
			line_time += interval;
			line_count += raw - previous;
		}
		uint32_t phase = raw - initial;
		if (written > 0 && written < initial) {
			if (phase > busy_last) busy_last = phase;
			busy_samples++;
		} else if (written >= initial) idle_samples++;
		previous = raw;
		previous_time = now;
	}
	if (!line_count || !busy_samples || !idle_samples) return 0;
	absolutetime_to_nanoseconds(last_wrap - first_wrap, &elapsed);
	measured.frame_ns = elapsed / (wraps - 1);
	measured.line_ns = line_time / line_count;
	if (!measured.line_ns || measured.frame_ns < NSEC_PER_SEC / 120 ||
	    measured.frame_ns > NSEC_PER_SEC / 30) return 0;
	measured.lines = (uint32_t)(measured.frame_ns / measured.line_ns);
	measured.shown = initial;
	uint32_t idle_start = busy_last + 32; /* margin after the last observed active DMA line */
	/* The TE phase counter counts synthetic vsync ticks, not image rows.
	 * At 90 Hz the unchanged UEFI tick clock gives ~1570 ticks for 2340
	 * image rows. DMA completion is checked separately via WR_PTR. */
	if (!measured.lines || measured.lines > 8192 || idle_start >= measured.lines) return 0;
	measured.millihz = (uint32_t)(1000000000000ULL / measured.frame_ns);
	command_mode = true;
	measured.ok = 1;
	te_initial = initial;
	te_idle_start = idle_start;
	scan = measured;
	printf("display: UMI command TE: %u.%03u Hz, %u ns/line, %u phase lines; DMA idle after %u; staged presentation enabled\n",
	    measured.millihz / 1000, measured.millihz % 1000, (unsigned)measured.line_ns, measured.lines, te_idle_start);
	return 1;
}

/*
 * Wait until the scan is in [first, last) of a frame (lines, 0 = the first line shown), at most `limit_ns`;
 * the wait is a sleep for most of the way and a short spin for the end. 1 when it got there.
 */
int
hakios_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns)
{
	uint64_t start = mach_absolute_time(), now, waited;

	if (!scan.ok || first >= last || last > scan.lines) {
		return 0;
	}
	for (;;) {
		uint32_t line = hakios_display_line(NULL);
		uint64_t ahead_ns;

		if (!scan.ok) return 0;

		if (line >= first && line < last) {
			return 1;
		}
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - start, &waited);
		if (waited >= limit_ns) {
			return 0;
		}
		/* lines to go to `first`, as time: sleep all but the last 200 us of it */
		ahead_ns = (uint64_t)((first + scan.lines - line) % scan.lines) * scan.line_ns;
		if (ahead_ns > 400000) {
			uint64_t nap = ahead_ns - 200000;

			if (waited + nap > limit_ns) {
				nap = limit_ns - waited;
			}
			delay_for_interval((uint32_t)(nap / 1000), NSEC_PER_USEC);
		}
	}
}

/* nanoseconds until the next frame begins (its first line shown), from where the scan is now */
uint64_t
hakios_display_next_frame_ns(void)
{
	uint32_t line;

	if (!scan.ok) {
		return 0;
	}
	line = hakios_display_line(NULL);
	return (uint64_t)(scan.lines - line) * scan.line_ns;
}

int
hakios_display_scan_start(void)
{
	uint32_t on = 0, hsync, period, v0, v1, f0, f1, l0, l1;
	uint64_t t0, t1, ns, gone;
	vm_offset_t gcc, dispcc;

	umi_pm_init();
	PE_parse_boot_argn("bl", &bl_mode, sizeof(bl_mode));

	if (scan.ok) {
		return 1;
	}
	if (!PE_parse_boot_argn("fb_scan", &on, sizeof(on)) || on != 1) {
		return 0;
	}
	DTEntry chosen;
	const void *board;
	unsigned board_size;
	umi = SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "hakios-board", &board, &board_size) == kSuccess &&
	    board_size == 4 && memcmp(board, "umi", 4) == 0;
	gcc = gcc_map = ml_io_map(GCC_PHYS + (GCC_DISP_AHB_CBCR & ~0xfffu), 0x1000);
	if (!gcc) return 0;
	if (!clock_on(gcc, GCC_DISP_AHB_CBCR & 0xfffu, "display clock controller's bus")) {
		return 0;
	}
	dispcc = dispcc_map = ml_io_map(DISPCC_PHYS, umi ? 0x4000 : 0x2000);
	if (!dispcc) return 0;
	if (!(rd(dispcc, DISPCC_MDSS_CORE_GDSCR) & GDSC_POWER_ON)) {
		printf("display: the display block has no power (the firmware did not leave the panel on?): no scan to go by\n");
		return 0;
	}
	if (!clock_on(dispcc, DISPCC_MDSS_AHB_CBCR, "display registers' bus")) {
		return 0;
	}
	intf = ml_io_map(INTF1_PHYS, 0x1000);
	if (!intf) return 0;
	if (umi) {
		dsi0 = ml_io_map(DSI0_PHYS, 0x1000);
	}
	printf("display: %s scan registers at %#lx, engine=%#x; GCC AHB=%#x DISPCC AHB=%#x GDSC=%#x\n",
	    umi ? "UMI/Kona" : "Yupik", (unsigned long)INTF1_PHYS,
	    rd(intf, INTF_TIMING_ENGINE_EN), rd(gcc, GCC_DISP_AHB_CBCR & 0xfffu),
	    rd(dispcc, DISPCC_MDSS_AHB_CBCR), rd(dispcc, DISPCC_MDSS_CORE_GDSCR));
	if (!(rd(intf, INTF_TIMING_ENGINE_EN) & 1)) {
		printf("display: command mode: TE enable=%#x config=%#x height=%#x initial=%#x start=%#x auto=%#x\n",
		    rd(intf, 0x284), rd(intf, 0x288), rd(intf, 0x28c), rd(intf, 0x294), rd(intf, 0x2a0), rd(intf, 0x2b4));
		if (command_scan_start()) {
			bl_start();
			return 1;
		}
		printf("display: command TE calibration failed; scan sync deferred\n");
		intf = 0;
		return 0;
	}
	hsync = rd(intf, INTF_HSYNC_CTL) >> 16;
	period = rd(intf, INTF_VSYNC_PERIOD);
	v0 = rd(intf, INTF_DISPLAY_V_START);
	v1 = rd(intf, INTF_DISPLAY_V_END);
	if (hsync == 0 || period < hsync || v1 <= v0) {
		printf("display: the timing engine's registers make no sense (line %u, frame %u clocks): no scan to go by\n", hsync, period);
		intf = 0;
		return 0;
	}
	scan.lines = period / hsync;
	scan.shown = (v1 - v0 + 1) / hsync;
	/* the rate the panel is really sent at: the counters 100 ms apart */
	l0 = hakios_display_line(&f0);
	if (!intf) return 0;
	t0 = mach_absolute_time();
	delay_for_interval(100, NSEC_PER_MSEC);
	l1 = hakios_display_line(&f1);
	if (!intf) return 0;
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	gone = (uint64_t)(f1 - f0) * scan.lines + l1 - l0;
	if (f1 == f0 || gone == 0 || ns == 0 || scan.lines < scan.shown || scan.shown == 0 ||
	    scan.lines > 65535 || scan.shown > 8192 || l0 >= scan.lines || l1 >= scan.lines) {
		printf("display: the scan counters do not move (frame %u -> %u, line %u -> %u): no scan to go by\n", f0, f1, l0, l1);
		intf = 0;
		return 0;
	}
	scan.line_ns = ns / gone;
	scan.frame_ns = scan.line_ns * scan.lines;
	if (scan.line_ns == 0 || scan.frame_ns < NSEC_PER_SEC / 240 ||
	    scan.frame_ns > NSEC_PER_SEC / 20) {
		printf("display: implausible scan period %llu ns; scan sync deferred\n", scan.frame_ns);
		intf = 0;
		return 0;
	}
	scan.millihz = (uint32_t)(1000000000000ULL / scan.frame_ns);
	scan.ok = 1;
	printf("display: the panel's scan: %u lines a frame (%u shown), %u.%03u frames a second, %llu ns a line; "
	    "frames follow it (fb_scan=1)\n", scan.lines, scan.shown, scan.millihz / 1000, scan.millihz % 1000, scan.line_ns);
	return 1;
}

/*
 * fb_dpu: page flips instead of a copy into the panel's memory (DISPLAY-SCANOUT-umi.md).
 *
 * UEFI leaves the DPU in command mode with INTF autorefresh: at every TE the source pipe it set up (VIG0 on
 * LM_0, DSC, CTL_0 on the phone) fetches the GOP buffer at 0x9C000000 and sends it to the panel. That buffer
 * is the start of the splash carve-out (cont_splash_region, 0x9C000000 + 0x2300000), which holds three
 * 1080x2340 frames, or three 1125x2436 ones. The pipes are found by their SRCn_ADDR pointing into the GOP
 * frame; a flip writes the same offsets into another frame of the carve-out and flushes the pipe, its mixer
 * and the CTL. The SSPP registers are double-buffered; the flush is latched by the next autorefresh frame
 * start, after which the CTL_FLUSH bits read 0 and the previous frame is free again. The carve-out is what
 * UEFI's SMMU setup already lets the display fetch, so no SMMU state is touched.
 *
 *   fb_dpu=1  flips; the CPU scales into the frame not scanned.
 *   fb_dpu=2  as 1, after a self-checking fetch test: is XNU RAM below 4 GiB fetchable by the DPU? (LM_0's
 *             MISR -- a CRC of every frame the mixer puts out -- of a pattern there must equal that of the same
 *             pattern in the carve-out); and a read-only dump of apps_smmu's entries for the display's stream.
 *   fb_dpu=3  as 1, but VIG0's QSEED3 scales 1125x2436 to the panel (bilinear): the carve-out is re-laid as
 *             three 1125x2436 frames, the CPU only copies. Switched on only when the MISR of a scaled white
 *             frame equals that of an unscaled white one (and differs from black); else fb_dpu=1 goes on.
 *
 * Registers: Linux drivers/gpu/drm/msm/disp/dpu1 (dpu_hw_sspp.c, dpu_hw_ctl.c, dpu_hw_util.c dpu_hw_setup_scaler3,
 * dpu_plane.c _dpu_plane_setup_scaler3/_pixel_ext, dpu_hw_lm.c MISR, catalog/dpu_6_0_sm8250.h).
 */
/*
 * Mapped in the kernel map, as the boot framebuffer is (ml_io_map_unmappable), not ml_io_map()'s 32 MB permanent I/O
 * window: 36 MB of frames would not fit there next to PCIe, GPU and audio, and a full window panics (KMA_NOFAIL).
 */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern uint64_t kvtophys(vm_offset_t va);
#define DPU_WIMG_IO             0x7             /* VM_WIMG_IO (arm/memory_types.h) */
#define DPU_WIMG_WCOMB          0x6             /* VM_WIMG_WCOMB */

#define MDP_PHYS                0x0ae01000UL    /* MDSS + 0x1000: the catalog's block offsets are from here */
#define MDP_SIZE                0x4a000UL       /* through LM_5 */
#define DPU_CTL(n)              (0x1000u + 0x200u * (n))
#define DPU_LM(n)               (0x44000u + 0x1000u * (n))
#define CTL_LAYER(lm)           ((lm) == 5 ? 0x024u : 4u * (lm))
#define CTL_LAYER_EXT(lm)       (0x40u + 4u * (lm))
#define CTL_LAYER_EXT2(lm)      (0x70u + 4u * (lm))
#define CTL_LAYER_EXT3(lm)      (0xa0u + 4u * (lm))
#define CTL_FLUSH               0x018u
#define CTL_DSC_ACTIVE          0x0e8u
#define CTL_MERGE_3D_ACTIVE     0x0e4u
#define CTL_INTF_ACTIVE         0x0f4u
#define CTL_FETCH_PIPE_ACTIVE   0x0fcu
#define CTL_MIXER_BORDER_OUT    (1u << 24)
#define CTL_FLUSH_CTL           (1u << 17)
#define LM_OUT_SIZE             0x004u
#define LM_MISR_CTRL            0x310u
#define LM_MISR_SIGNATURE       0x314u
#define MISR_CTRL_ENABLE        (1u << 8)
#define MISR_CTRL_STATUS        (1u << 9)
#define MISR_CTRL_STATUS_CLEAR  (1u << 10)
#define MISR_CTRL_FREE_RUN      (1u << 31)
#define SSPP_SRC_SIZE           0x000u
#define SSPP_SRC_XY             0x008u
#define SSPP_OUT_SIZE           0x00cu
#define SSPP_OUT_XY             0x010u
#define SSPP_SRC0_ADDR          0x014u
#define SSPP_SRC_YSTRIDE0       0x024u
#define SSPP_SRC_FORMAT         0x030u
#define SSPP_SRC_OP_MODE        0x038u
#define SSPP_PE_C0_LR           0x100u          /* SW pixel extension: LR, TB, REQ_PIXELS for C0, C1C2 (+0x10), C3 (+0x20) */
#define SSPP_MULTIRECT_OPMODE   0x170u
#define SSPP_OP_PE_OVERRIDE     (1u << 31)
#define VIG_SCALER              0xa00u          /* QSEED3LITE (scaler version 3.0) */
#define QSEED3_OP_MODE          0x04u
#define QSEED3_PHASE_STEP_Y_H   0x10u
#define QSEED3_PHASE_STEP_Y_V   0x14u
#define QSEED3_PHASE_STEP_UV_H  0x18u
#define QSEED3_PHASE_STEP_UV_V  0x1cu
#define QSEED3_PRELOAD          0x20u
#define QSEED3_SRC_SIZE_Y_RGB_A 0x40u
#define QSEED3_SRC_SIZE_UV      0x44u
#define QSEED3_DST_SIZE         0x48u
#define QSEED3_PHASE_INIT_Y_H   0x90u           /* .. _Y_V, _UV_H, _UV_V */
#define UMI_SPLASH_PHYS         0x9c000000ULL
#define UMI_SPLASH_SIZE         0x2300000ULL
#define DPU_MAX_BUFFERS         3
#define DPU_MAX_SRCS            16
#define DPU_NOT_OURS            DPU_MAX_BUFFERS /* a pending/shown "frame" that is a test buffer */
#define APPS_SMMU_PHYS          0x15000000UL
#define DISPLAY_SID             0x820u          /* live DT: smmu_sde_unsec iommus = <&apps_smmu 0x820 0x402> */

static const uint32_t sspp_off[8] = { 0x4000, 0x6000, 0x8000, 0xa000, 0x24000, 0x26000, 0x28000, 0x2a000 };
static const uint32_t sspp_flush[8] = { 1u << 0, 1u << 1, 1u << 2, 1u << 18, 1u << 11, 1u << 12, 1u << 24, 1u << 25 };
static const char *const sspp_name[8] = { "VIG0", "VIG1", "VIG2", "VIG3", "DMA0", "DMA1", "DMA2", "DMA3" };
static const uint32_t lm_flush[6] = { 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 20 };

/* the geometry of the one pipe (fb_dpu=2/3 change it and put UEFI's back) */
struct dpu_geo {
	uint32_t src_size, src_xy, out_size, out_xy, ystride0, op;
	uint32_t pe[9];
	uint32_t qseed[16];             /* op, steps x4, preload, src y/uv, dst, init x4 (fb_dpu=3) */
};

static vm_offset_t mdp, carve;
static uint32_t dpu_ctl, dpu_flush, dpu_pipe = UINT32_MAX, dpu_lm;
static bool dpu_vig;
static struct { uint32_t reg, offset; } dpu_src[DPU_MAX_SRCS];
static unsigned dpu_srcs, dpu_count;
static uint64_t dpu_phys[DPU_MAX_BUFFERS], dpu_frame_bytes, dpu_base;
static int dpu_shown, dpu_pending = -1;
static bool dpu_ready, dpu_scaled;
static uint32_t dpu_latched;
static uint64_t dpu_programmed_at;
static int dpu_latency_log = -1;
static struct dpu_geo dpu_uefi;
static uint32_t dpu_stride[DPU_MAX_BUFFERS];    /* fb_dpu=3/4: each frame's row stride (0: the pipe's as it is) */

/* The boot log's frame (bsd/dev/umi_bootlog.c): the geometry of the frames the flips use, so that
 * after a pause the painter can be told which frame the panel scans. UEFI's frame is what the caller
 * passed to hakios_display_dpu_start(); fb_dpu=3's frames are what hakios_display_dpu_scale_start()
 * was given. Both are set once and read by dpu_bootlog_resume(). */
static uint64_t dpu_gop_phys;
static uint32_t dpu_gop_w, dpu_gop_h, dpu_gop_stride;
static uint32_t dpu_scale_w, dpu_scale_h, dpu_scale_stride;
/* osfmk/console/video_console.c: the frame buffer the loader left, as the console knows it. The boot
 * log draws into vinfo.v_baseaddr until our first resume(), and back there when no DPU of ours took
 * the panel (the pipes are given physical addresses; this is the kernel mapping of that frame). */
extern struct vc_info vinfo;
/* One line, once: a MISR test paused the boot log while a text row was still being written. */
static bool dpu_bootlog_pause_warned;
static LCK_GRP_DECLARE(dpu_submit_lck_grp, "hakios-dpu-submit");
static LCK_MTX_DECLARE(dpu_submit_mtx, &dpu_submit_lck_grp);

static void compose_single(void);
static void compose_restore(void);
static bool compose_active, compose_failed;

/*
 * The kernel virtual address of a frame of the carve-out: dpu_start_locked() mapped the whole
 * carve-out at `carve`, and the physical addresses the pipes are given are offsets into it. The GOP
 * frame's own mapping (vinfo.v_baseaddr) covers only that frame, so it cannot address frames 1/2.
 */
static uint64_t
dpu_frame_va(uint64_t phys)
{
	if (carve == 0 || phys < dpu_base || phys - dpu_base >= UMI_SPLASH_SIZE) {
		return 0;
	}
	return (uint64_t)(carve + (vm_offset_t)(phys - dpu_base));
}

/*
 * Tell the boot log which frame the panel scans, after a pause around frame work (dpu setup, an MISR
 * self-test, a relayout). The base passed is the kernel virtual address of the frame (the painter
 * stores through it); its physical address goes along because that is what identifies the frame.
 * A programmed flip that has not latched yet is the frame that will be scanned at the next TE and is
 * not scanned now: the painter may draw into it at once. Otherwise the shown frame; with no flips,
 * UEFI's own frame (what the caller gave hakios_display_dpu_start()).
 */
static void
dpu_bootlog_resume(void)
{
	int shown = dpu_pending >= 0 ? dpu_pending : dpu_shown;

	if (dpu_ready && shown >= 0 && (unsigned)shown < dpu_count && dpu_phys[shown] != 0) {
		uint64_t phys = dpu_phys[shown];
		uint64_t va = dpu_frame_va(phys);

		if (va == 0) {
			return;         /* (no mapping names that frame: the painter draws nothing, not elsewhere) */
		}
		if (dpu_scaled) {
			uint32_t stride = dpu_stride[shown] != 0 ? dpu_stride[shown] : dpu_scale_stride;

			hakios_bootlog_resume(va, phys, dpu_scale_w, dpu_scale_h, stride);
		} else {
			hakios_bootlog_resume(va, phys, dpu_gop_w, dpu_gop_h, dpu_gop_stride);
		}
		return;
	}
	/*
	 * No DPU of ours on the panel: UEFI's own frame, through the console's own mapping of it (with no
	 * flip yet, or after a stop put the pipes back on the frame hakios_display_dpu_start() was given).
	 */
	hakios_bootlog_resume((uint64_t)vinfo.v_baseaddr,
	    dpu_gop_phys != 0 ? dpu_gop_phys : vinfo.v_physaddr,
	    vinfo.v_width, vinfo.v_height, vinfo.v_rowbytes);
}

/*
 * A MISR test is about to fill the frames and measure LM_0: pause the boot log and say, once, if its
 * row was still in flight (the measurement could then be off; it is a few microseconds, so it should
 * not happen now that a row is non-preemptible).
 */
static void
dpu_bootlog_pause_for_test(void)
{
	if (!hakios_bootlog_pause() && !dpu_bootlog_pause_warned) {
		dpu_bootlog_pause_warned = true;
		printf("display: fb_dpu: the boot log had a text row in flight when a MISR test paused it\n");
	}
}

static void
dpu_program(uint64_t base)
{
	for (unsigned i = 0; i < dpu_srcs; i++) {
		wr(mdp, dpu_src[i].reg, (uint32_t)(base + dpu_src[i].offset));
	}
	wr(mdp, dpu_ctl + CTL_FLUSH, dpu_flush);
}

static const uint32_t qseed_regs[10] = { QSEED3_OP_MODE, QSEED3_PHASE_STEP_Y_H, QSEED3_PHASE_STEP_Y_V, QSEED3_PHASE_STEP_UV_H,
	                                 QSEED3_PHASE_STEP_UV_V, QSEED3_PRELOAD, QSEED3_SRC_SIZE_Y_RGB_A, QSEED3_SRC_SIZE_UV,
	                                 QSEED3_DST_SIZE, QSEED3_PHASE_INIT_Y_H };

static void
dpu_geo_read_pipe(struct dpu_geo *g, uint32_t p)
{

	g->src_size = rd(mdp, p + SSPP_SRC_SIZE);
	g->src_xy = rd(mdp, p + SSPP_SRC_XY);
	g->out_size = rd(mdp, p + SSPP_OUT_SIZE);
	g->out_xy = rd(mdp, p + SSPP_OUT_XY);
	g->ystride0 = rd(mdp, p + SSPP_SRC_YSTRIDE0);
	g->op = rd(mdp, p + SSPP_SRC_OP_MODE);
	for (unsigned i = 0; i < 9; i++) g->pe[i] = rd(mdp, p + SSPP_PE_C0_LR + 0x10 * (i / 3) + 4 * (i % 3));
	for (unsigned i = 0; i < 16; i++) {
		g->qseed[i] = dpu_vig && i < 13 ? rd(mdp, p + VIG_SCALER + (i < 9 ? qseed_regs[i] : QSEED3_PHASE_INIT_Y_H + 4 * (i - 9))) : 0;
	}
}

static void
dpu_geo_read(struct dpu_geo *g)
{
	dpu_geo_read_pipe(g, dpu_pipe);
}

/* written into the shadow registers: the next flush (dpu_program) makes them current with the address */
static void
dpu_geo_write_pipe(const struct dpu_geo *g, uint32_t p)
{

	if (dpu_vig) {
		for (unsigned i = 1; i < 13; i++) {
			wr(mdp, p + VIG_SCALER + (i < 9 ? qseed_regs[i] : QSEED3_PHASE_INIT_Y_H + 4 * (i - 9)), g->qseed[i]);
		}
		wr(mdp, p + VIG_SCALER + QSEED3_OP_MODE, g->qseed[0]);
	}
	for (unsigned i = 0; i < 9; i++) wr(mdp, p + SSPP_PE_C0_LR + 0x10 * (i / 3) + 4 * (i % 3), g->pe[i]);
	wr(mdp, p + SSPP_SRC_SIZE, g->src_size);
	wr(mdp, p + SSPP_SRC_XY, g->src_xy);
	wr(mdp, p + SSPP_OUT_SIZE, g->out_size);
	wr(mdp, p + SSPP_OUT_XY, g->out_xy);
	wr(mdp, p + SSPP_SRC_YSTRIDE0, g->ystride0);
	wr(mdp, p + SSPP_SRC_OP_MODE, g->op);
}

static void
dpu_geo_write(const struct dpu_geo *g)
{
	dpu_geo_write_pipe(g, dpu_pipe);
}

/* the pending flip latched (CTL_FLUSH bits cleared at an autorefresh frame start)? */
static int
dpu_check_locked(void)
{
	if (dpu_pending < 0) return 1;
	if (!regs_alive()) {
		printf("display: fb_dpu: the display block's clock or power went off; flips stop\n");
		dpu_ready = false;
		return 0;
	}
	if (rd(mdp, dpu_ctl + CTL_FLUSH) & dpu_flush) return 0;
	dpu_shown = dpu_pending;
	dpu_pending = -1;
	dpu_latched++;
	if (dpu_latency_log == 1 && dpu_programmed_at != 0) {
		uint64_t programmed, observed;
		absolutetime_to_nanoseconds(dpu_programmed_at, &programmed);
		absolutetime_to_nanoseconds(mach_absolute_time(), &observed);
		printf("tl: L slot %d programmed %llu observed %llu\n", dpu_shown, programmed / 1000, observed / 1000);
		dpu_programmed_at = 0;
	}
	return 1;
}

static int
dpu_check(void)
{
	lck_mtx_lock(&dpu_submit_mtx);
	int done = dpu_check_locked();
	lck_mtx_unlock(&dpu_submit_mtx);
	return done;
}

static int
dpu_wait(uint64_t limit_ns)
{
	uint64_t start = mach_absolute_time(), elapsed;

	for (;;) {
		if (dpu_check()) return 1;
		if (!dpu_ready) return 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= limit_ns) return 0;
		/* the latch comes with the next TE: sleep to just before it, then poll */
		uint64_t nap = 250000;
		if (scan.ok && command_mode) {
			uint32_t phase = hakios_display_line(NULL);
			uint64_t ahead = (uint64_t)(scan.lines - phase) * scan.line_ns;
			if (ahead > 600000) nap = ahead - 300000;
		}
		if (nap > limit_ns - elapsed) nap = limit_ns - elapsed;
		delay_for_interval((uint32_t)((nap + 999) / 1000), NSEC_PER_USEC);
	}
}

/*
 * Show `base` (any fetchable address; geometry as currently in the shadow registers), and once it is on the panel,
 * the CRC LM_0 computes over the frames it puts out (MISR). 1 with *sig when two frames gave the same signature.
 */
static int
dpu_signature(uint64_t base, int as, uint32_t *sig)
{
	uint32_t lm = DPU_LM(dpu_lm), c, s1, s2;

	if (!dpu_wait(100000000)) return 0;
	dpu_program(base);
	dpu_pending = as;
	if (!dpu_wait(100000000)) {
		printf("display: fb_dpu: MISR: flip to %#llx not latched (CTL_FLUSH=%#x)\n", base, rd(mdp, dpu_ctl + CTL_FLUSH));
		return 0;
	}
	delay_for_interval(40, NSEC_PER_MSEC);          /* the latched frame and one more out */
	wr(mdp, lm + LM_MISR_CTRL, MISR_CTRL_STATUS_CLEAR);
	wr(mdp, lm + LM_MISR_CTRL, 1u | MISR_CTRL_ENABLE | MISR_CTRL_FREE_RUN);
	delay_for_interval(60, NSEC_PER_MSEC);
	c = rd(mdp, lm + LM_MISR_CTRL);
	s1 = rd(mdp, lm + LM_MISR_SIGNATURE);
	delay_for_interval(35, NSEC_PER_MSEC);
	s2 = rd(mdp, lm + LM_MISR_SIGNATURE);
	wr(mdp, lm + LM_MISR_CTRL, 0);
	*sig = s1;
	if (!(c & MISR_CTRL_STATUS) || s1 != s2) {
		printf("display: fb_dpu: MISR at %#llx: ctrl %#x signatures %#x/%#x (not settled)\n", base, c, s1, s2);
		return 0;
	}
	return 1;
}

/* 0 black, 1 white, 2 eight vertical colour bars; through a write-combined or cached mapping */
static void
dpu_fill(vm_offset_t virt, uint32_t w, uint32_t h, uint32_t stride, int kind)
{
	static const uint32_t bars[8] = { 0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000 };

	/* (the carve-out is Device memory to the CPU, through the hypervisor's stage 2: aligned 4-byte stores, never
	 * vectorized or merged -- see carveCopy() in MiniIOMobileFramebuffer.cpp) */
	for (uint32_t y = 0; y < h; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(virt + (uint64_t)y * stride);
		for (uint32_t x = 0; x < w; x++) row[x] = kind == 0 ? 0xff000000u : kind == 1 ? 0xffffffffu : bars[x * 8 / w];
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

int
hakios_display_dpu_poll(void)
{
	if (dpu_ready) (void)dpu_check();
	return dpu_pending;
}

int hakios_display_dpu_shown(void) { return dpu_shown; }
uint64_t hakios_display_dpu_frame(unsigned index) { return dpu_ready && index < dpu_count ? dpu_phys[index] : 0; }

/* fb_dpu=4: the row stride frame `index` is fetched with from its next flip on (an aliased surface's own) */
void
hakios_display_dpu_stride(unsigned index, uint32_t stride)
{
	if (index < DPU_MAX_BUFFERS && stride <= 0xffff) dpu_stride[index] = stride;
}

/*
 * fb_dpu=4, for the GPU's submissions (any thread; nothing changed here): is frame `index` the one on the panel with a
 * flip away from it not yet latched, and may the DPU still fetch it? CoreAnimation may already draw into it then; its
 * submission waits for this.
 * Command mode: the panel keeps the picture in its own memory and a frame is fetched only by the transfer an
 * autorefresh TE starts. Once this refresh's transfer is done (the test hakios_display_wait_copy() writes into the
 * scanned GOP frame by), the pending flip is taken by the next TE and this frame is never fetched again: free about
 * 8 ms before the latch, which is what a frame that took longer than the TE before it had to wait for (v15: 4-13 ms
 * of every frame of backboardd's, GPU-submit "offscreen"). (The flush is read first: a TE after it latches the flip.)
 */
int
hakios_display_dpu_leaving(unsigned index)
{
	int shown = dpu_shown, pending = dpu_pending;

	if (!dpu_ready || (int)index != shown || pending < 0 || pending == (int)index || !regs_alive()) return 0;
	if (!(rd(mdp, dpu_ctl + CTL_FLUSH) & dpu_flush)) return 0;
	if (hakios_display_command_mode()) {
		uint32_t before = hakios_display_line(NULL);

		if (!scan.ok) return 1;                 /* (the scan given up: intf is 0) */
		uint32_t written = rd(intf, 0x2b0) & 0xffff, phase = hakios_display_line(NULL);

		if (scan.ok && phase >= before && phase >= te_idle_start && written >= scan.shown) return 0;
	}
	return 1;
}
uint32_t hakios_display_dpu_latched(void) { return dpu_latched; }

/* GPU write hazards: 1 = current fetch, 2 = incoming/current pending frame.
 * Serialize the software slot snapshot with flip programming and latch observation.
 * Only an outgoing frame with a programmed successor may use the latch shortcut.
 * A stopped clock or missing successor never proves that a shown frame is reusable.
 * Command mode with INTF autorefresh fetches the shown frame at every TE, so "this refresh's transfer is done"
 * frees nothing: the panel may take that frame once more, and a GPU drawing into it meanwhile lands on the glass
 * (v29t, INVESTIGATION-applibrary-black-3.md: CoreAnimation's clear-then-draw of the surface it just presented
 * made App Library flash black). Only a latched successor proves the frame is not fetched again; the older test is
 * back with /var/db/hakios-display/write-shown-off. */
unsigned
hakios_display_dpu_write_busy(uint64_t phys)
{
	unsigned reason = 0;
	lck_mtx_lock(&dpu_submit_mtx);
	/* (the caller's wait loop does not poll, so dpu_shown/dpu_pending would be as old as the last flip; the
	 * latch is taken here, under the same lock, before the slot is classified) */
	(void)dpu_check_locked();
	for (unsigned i = 0; i < dpu_count; i++) {
		if (dpu_phys[i] != phys) continue;
		if ((int)i == dpu_pending) {
			reason = 2;
		} else if ((int)i == dpu_shown) {
			reason = 1;
			/* Flush first: a TE after this read takes the successor, not this frame. */
			if (dpu_ready && dpu_pending >= 0 && regs_alive() &&
			    !(rd(mdp, dpu_ctl + CTL_FLUSH) & dpu_flush)) {
				reason = 0;
			} else if (!hakios_display_write_shown() && dpu_ready && dpu_pending >= 0 &&
			    regs_alive() && hakios_display_command_mode()) {
				uint32_t before = hakios_display_line(NULL);
				if (scan.ok) {
					uint32_t written = rd(intf, 0x2b0) & 0xffff, phase = hakios_display_line(NULL);
					if (scan.ok && phase >= before && phase >= te_idle_start && written >= scan.shown) reason = 0;
				}
			}
		}
		break;
	}
	lck_mtx_unlock(&dpu_submit_mtx);
	return reason;
}

/* Show buffer `index` from the next TE on: waits (at most limit_ns) for a flip still pending. */
int
hakios_display_dpu_flip(unsigned index, uint64_t limit_ns)
{
	if (dpu_latency_log < 0) {
		uint32_t on = 0;
		dpu_latency_log = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	if (!dpu_ready || index >= dpu_count || !dpu_wait(limit_ns)) return 0;
	if ((int)index == dpu_shown && !compose_active) return 1;
	lck_mtx_lock(&dpu_submit_mtx);
	compose_single();
	/* (double-buffered like the address: latched with it by the same flush) */
	if (dpu_scaled && dpu_stride[index] != 0) wr(mdp, dpu_pipe + SSPP_SRC_YSTRIDE0, dpu_stride[index]);
	dpu_program(dpu_phys[index]);
	dpu_programmed_at = mach_absolute_time();
	dpu_pending = (int)index;
	lck_mtx_unlock(&dpu_submit_mtx);
	if (compose_failed) compose_restore();
	return 1;
}

/*
 * fb_dpu=4 (any thread): have the panel fetch the frame it is showing once more. In command mode a transfer is
 * started by a commit, and a flip is one; a frame copied into the panel's own frame in place commits nothing, so
 * a run of those leaves the panel without a transfer and the DMA idle interval that copy waits for never comes
 * (v29f: Maps' multi-layer frames waited out the whole limit, one after another, and none reached the panel).
 * Committing the frame that is on the panel again starts the transfers and changes nothing that is shown.
 * 1 when it was committed; 0 when a flip was outstanding (programming then would be taken for that flip's latch
 * by dpu_check_locked, and the frame it goes to would be believed shown) or the display is not up.
 */
int
hakios_display_dpu_refresh(void)
{
	if (!dpu_ready || !regs_alive()) return 0;
	lck_mtx_lock(&dpu_submit_mtx);
	if (dpu_pending >= 0 || dpu_shown < 0 || dpu_shown >= (int)dpu_count) {
		lck_mtx_unlock(&dpu_submit_mtx);
		return 0;
	}
	compose_single();
	if (dpu_scaled && dpu_stride[dpu_shown] != 0) wr(mdp, dpu_pipe + SSPP_SRC_YSTRIDE0, dpu_stride[dpu_shown]);
	dpu_program(dpu_phys[dpu_shown]);
	lck_mtx_unlock(&dpu_submit_mtx);
	if (compose_failed) compose_restore();
	return 1;
}

/*
 * Back to UEFI's geometry and the GOP frame (the loader's and the panic console's): on fallback and on the way
 * to the loader (hakios_ddr_quiesce). No wait.
 */
static void
dpu_restore_locked(void)
{
	if (!dpu_ready || !regs_alive()) return;
	lck_mtx_lock(&dpu_submit_mtx);
	compose_single();
	if (dpu_scaled) {
		dpu_geo_write(&dpu_uefi);
		dpu_scaled = false;             /* (from here on the pipes fetch UEFI's geometry, and say so) */
	}
	dpu_program(dpu_base);
	dpu_pending = 0;
	lck_mtx_unlock(&dpu_submit_mtx);
	compose_restore();
}

/* The pipes are re-pointed at another frame: the boot log must not be drawing anywhere meanwhile. */
void
hakios_display_dpu_restore(void)
{
	if (!dpu_ready) return;         /* nothing of ours is scanned: nothing to bracket */
	hakios_bootlog_pause();
	dpu_restore_locked();
	dpu_bootlog_resume();
}

static void
dpu_stop_locked(void)
{
	bool scaled;

	if (!dpu_ready) return;
	scaled = dpu_scaled;
	dpu_restore_locked();
	(void)dpu_wait(100000000);
	printf("display: fb_dpu: pipes back on the GOP frame%s (CTL_FLUSH=%#x); flips off\n",
	    scaled ? " and UEFI's geometry" : "", regs_alive() ? rd(mdp, dpu_ctl + CTL_FLUSH) : 0xdeadu);
	dpu_ready = false;
}

void
hakios_display_dpu_stop(void)
{
	if (!dpu_ready) return;
	hakios_bootlog_pause();
	dpu_stop_locked();
	dpu_bootlog_resume();
}

/* read-only: apps_smmu's stream entries for the display (SID 0x820) and the context bank they use */
static void
dpu_smmu_probe(void)
{
	vm_offset_t s = ml_io_map_unmappable(APPS_SMMU_PHYS, 0x1000, DPU_WIMG_IO);
	uint32_t idr0, idr1, nsmr, ncb, pgshift, numpage, found = 0;

	if (!s) return;
	printf("display: fb_dpu=2: reading apps_smmu at %#lx (read-only)\n", APPS_SMMU_PHYS);
	idr0 = rd(s, 0x20);
	idr1 = rd(s, 0x24);
	nsmr = idr0 & 0xff;
	ncb = idr1 & 0xff;
	pgshift = (idr1 & (1u << 31)) ? 16 : 12;
	numpage = 1u << (((idr1 >> 28) & 7) + 1);
	printf("display: fb_dpu=2: apps_smmu sCR0 %#x idr0 %#x idr1 %#x: %u stream match registers, %u context banks, %u pages of %#x\n",
	    rd(s, 0), idr0, idr1, nsmr, ncb, numpage, 1u << pgshift);
	uint64_t size = (uint64_t)numpage << pgshift << 1;
	if (size > 0x2000000 || nsmr == 0) return;
	s = ml_io_map_unmappable(APPS_SMMU_PHYS, (vm_size_t)size, DPU_WIMG_IO);
	if (!s) return;
	for (uint32_t i = 0; i < nsmr; i++) {
		uint32_t smr = rd(s, 0x800 + 4 * i), id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (!(smr & (1u << 31)) || ((id ^ DISPLAY_SID) & ~mask & 0x7fff)) continue;
		uint32_t s2cr = rd(s, 0xc00 + 4 * i), type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		printf("display: fb_dpu=2: apps_smmu SMR%u %#x (id %#x mask %#x) S2CR %#x: %s, context bank %u\n", i, smr, id, mask,
		    s2cr, type == 0 ? "translated" : type == 1 ? "BYPASS" : "fault", cb);
		if (type == 0 && cb < ncb) {
			vm_offset_t c = s + ((vm_offset_t)numpage << pgshift) + ((vm_offset_t)cb << pgshift);
			printf("display: fb_dpu=2: apps_smmu CB%u: CBAR %#x SCTLR %#x TCR %#x TTBR0 %#x%08x MAIR0 %#x FSR %#x FAR %#x%08x\n", cb,
			    rd(s, (1u << pgshift) + 4 * cb), rd(c, 0x0), rd(c, 0x30), rd(c, 0x24), rd(c, 0x20), rd(c, 0x38), rd(c, 0x58),
			    rd(c, 0x64), rd(c, 0x60));
		}
		if (++found == 8) break;
	}
	if (!found) printf("display: fb_dpu=2: apps_smmu: no valid stream match entry covers SID %#x\n", DISPLAY_SID);
}

/*
 * fb_dpu=2: can the DPU fetch `phys` (a contiguous, cached buffer of 64 rows of the panel's stride, below 4 GiB, in
 * XNU's RAM; `virt` its kernel address)? The pipe shows the top 64 rows only (the rest of the mixer is border
 * black) of: bars in the carve-out (R), black in the carve-out (B), bars at `phys` (T). PASS: T == R != B.
 * Returns 1 PASS, 0 FAIL, -1 no answer; the pipe is back on UEFI's geometry and the shown frame either way,
 * except -2: that did not latch (the pipe may still fetch `phys`: the caller keeps the buffer).
 */
static int
dpu_fetch_test_locked(uint64_t phys, vm_offset_t virt)
{
	const uint32_t rows = 64, w = dpu_uefi.src_size & 0xffff, stride = dpu_uefi.ystride0 & 0xffff;
	struct dpu_geo g = dpu_uefi;
	uint32_t r = 0, b = 0, t = 0;
	int okr, okb, okt, back = dpu_shown, result;

	dpu_smmu_probe();
	if (!dpu_ready || dpu_srcs != 1 || dpu_src[0].offset != 0 || dpu_count < 3 || dpu_scaled) {
		printf("display: fb_dpu=2: FETCH TEST SKIPPED: needs one pipe at the frame's start and three frames\n");
		return -1;
	}
	if (phys == 0 || phys + (uint64_t)rows * stride > 0x100000000ULL) {
		printf("display: fb_dpu=2: FETCH TEST SKIPPED: no contiguous buffer below 4 GiB (at %#llx)\n", phys);
		return -1;
	}
	g.src_size = g.out_size = rows << 16 | w;
	for (unsigned c = 0; c < 3; c++) g.pe[3 * c + 2] = rows << 16 | w;
	/* the carve-out frames not scanned now */
	unsigned fr = (unsigned)(dpu_shown + 1) % 3, fb = (unsigned)(dpu_shown + 2) % 3;
	dpu_fill(carve + (fr * (dpu_phys[1] - dpu_phys[0])), w, rows, stride, 2);
	dpu_fill(carve + (fb * (dpu_phys[1] - dpu_phys[0])), w, rows, stride, 0);
	dpu_fill(virt, w, rows, stride, 2);
	flush_dcache64((addr64_t)virt, rows * stride, 0);
	dpu_geo_write(&g);
	okr = dpu_signature(dpu_phys[fr], (int)fr, &r);
	okb = dpu_signature(dpu_phys[fb], (int)fb, &b);
	okt = dpu_signature(phys, DPU_NOT_OURS, &t);
	dpu_geo_write(&dpu_uefi);
	dpu_program(dpu_phys[back]);
	dpu_pending = back;
	if (!dpu_wait(100000000)) {
		printf("display: fb_dpu=2: FETCH TEST: the way back did not latch; flips off\n");
		return -2;
	}
	if (!okr || !okb || r == b) {
		printf("display: fb_dpu=2: FETCH TEST NO ANSWER: MISR bars %#x (%d) black %#x (%d) does not tell them apart\n", r, okr, b, okb);
		result = -1;
	} else if (okt && t == r) {
		printf("display: fb_dpu=2: FETCH TEST PASS: XNU RAM at %#llx is fetched by the DPU (MISR %#x = carve-out %#x, black %#x)\n",
		    phys, t, r, b);
		result = 1;
	} else {
		printf("display: fb_dpu=2: FETCH TEST FAIL: XNU RAM at %#llx is not fetched as written (MISR %#x%s, carve-out %#x, black %#x)\n",
		    phys, t, okt ? "" : " unsettled", r, b);
		result = 0;
	}
	return result;
}

/*
 * The fetch test fills frames and reads LM_0's MISR: the boot log draws nothing for its duration (its
 * text in a test frame would change the signature), then goes back to the frame on the panel.
 */
int
hakios_display_dpu_fetch_test(uint64_t phys, vm_offset_t virt)
{
	int result;

	dpu_bootlog_pause_for_test();
	result = dpu_fetch_test_locked(phys, virt);
	dpu_bootlog_resume();
	return result;
}

/*
 * fb_dpu=3: VIG0's QSEED3 scales a w x h frame (stride bytes a row) to the panel; the carve-out becomes three such
 * frames. Self-checked: MISR of white at w x h scaled == MISR of white at the panel's size, != black. On PASS the
 * frames are virt[0..n-1] and n is returned (frame 0, black, is shown); on FAIL 0 is returned and fb_dpu=1 goes on
 * with UEFI's geometry and its own frames.
 */
static unsigned
dpu_scale_start_locked(uint32_t w, uint32_t h, uint32_t stride, vm_offset_t *virt, unsigned max)
{
	uint32_t pw = dpu_uefi.out_size & 0xffff, ph = dpu_uefi.out_size >> 16, ps = dpu_uefi.ystride0 & 0xffff;
	uint64_t frame = ((uint64_t)stride * h + 0xffffULL) & ~0xffffULL, old = dpu_phys[1] - dpu_phys[0];
	uint32_t white = 0, black = 0, scaled = 0;
	struct dpu_geo g = dpu_uefi;
	int okw, okb, oks;

	if (!dpu_ready || !dpu_vig || dpu_srcs != 1 || dpu_src[0].offset != 0 || dpu_count < 3 || dpu_scaled ||
	    (dpu_uefi.src_size != dpu_uefi.out_size) || frame * 3 > UMI_SPLASH_SIZE || stride < w * 4 || stride > 0xffff ||
	    w > 4096 || h > 4096) {
		printf("display: fb_dpu=3: DPU SCALING NOT TRIED: needs one unscaled VIG pipe at the frame's start, three frames, "
		    "and %ux%u (stride %u) in thirds of the carve-out\n", w, h, stride);
		return 0;
	}
	/* white and black at the panel's size in the old frames 1 and 2 (frame 0 is shown: fb_dpu=1 has not flipped yet) */
	if (dpu_shown != 0 || dpu_pending >= 0) return 0;
	dpu_fill(carve + old, pw, ph, ps, 1);
	dpu_fill(carve + 2 * old, pw, ph, ps, 0);
	okw = dpu_signature(dpu_phys[1], 1, &white);
	okb = dpu_signature(dpu_phys[2], 2, &black);
	/* white at w x h in the new frame 0 (overlaps old frames 0 and 1, neither shown now), scaled */
	dpu_fill(carve, w, h, stride, 1);
	g.src_size = h << 16 | w;
	g.src_xy = 0;
	g.ystride0 = stride;
	for (unsigned c = 0; c < 3; c++) {
		g.pe[3 * c] = g.pe[3 * c + 1] = 0;
		g.pe[3 * c + 2] = h << 16 | w;          /* _dpu_plane_setup_pixel_ext: the whole source, no overfetch */
	}
	g.op |= SSPP_OP_PE_OVERRIDE;
	/* dpu_hw_setup_scaler3 with _dpu_plane_setup_scaler3's RGB defaults: bilinear, no LUT, QSEED4 preloads */
	uint32_t step_x = (uint32_t)(((uint64_t)w << 21) / pw), step_y = (uint32_t)(((uint64_t)h << 21) / ph);
	g.qseed[0] = 1u | 3u << 16 | 1u << 31 | 1u << 14 | 1u << 10 | 1u << 29;  /* enable, BIL, blend, !DX, alpha on, alpha BIL */
	g.qseed[1] = g.qseed[3] = step_x;
	g.qseed[2] = g.qseed[4] = step_y;
	g.qseed[5] = 4u | 2u << 8 | 4u << 16 | 2u << 24;
	g.qseed[6] = g.qseed[7] = h << 16 | w;
	g.qseed[8] = ph << 16 | pw;
	g.qseed[9] = g.qseed[10] = g.qseed[11] = g.qseed[12] = 0;
	dpu_geo_write(&g);
	dpu_scaled = true;                      /* (from here on a restore puts UEFI's geometry back) */
	oks = dpu_signature(dpu_base, 0, &scaled);
	if (okw && okb && oks && white != black && scaled == white) {
		dpu_count = (unsigned)(UMI_SPLASH_SIZE / frame);
		if (dpu_count > max) dpu_count = max;
		if (dpu_count > DPU_MAX_BUFFERS) dpu_count = DPU_MAX_BUFFERS;
		for (unsigned i = 0; i < dpu_count; i++) {
			dpu_phys[i] = dpu_base + i * frame;
			virt[i] = carve + (vm_offset_t)(i * frame);
		}
		dpu_frame_bytes = (uint64_t)stride * h;
		for (unsigned i = 0; i < DPU_MAX_BUFFERS; i++) dpu_stride[i] = stride;
		dpu_scale_w = w;
		dpu_scale_h = h;
		dpu_scale_stride = stride;
		dpu_fill(carve, w, h, stride, 0);
		printf("display: fb_dpu=3: DPU SCALING PASS: QSEED3 %ux%u -> %ux%u (MISR scaled white %#x = white %#x, black %#x); "
		    "%u frames of %ux%u, stride %u, at %#llx + %#llx*i: the CPU only copies\n", w, h, pw, ph, scaled, white, black,
		    dpu_count, w, h, stride, dpu_base, frame);
		return dpu_count;
	}
	printf("display: fb_dpu=3: DPU SCALING FAIL: MISR scaled white %#x (%d), white %#x (%d), black %#x (%d); "
	    "UEFI's geometry back, fb_dpu=1 goes on\n", scaled, oks, white, okw, black, okb);
	dpu_geo_write(&dpu_uefi);
	dpu_scaled = false;
	dpu_program(dpu_phys[2]);               /* the black one */
	dpu_pending = 2;
	if (!dpu_wait(100000000)) {
		printf("display: fb_dpu=3: the way back did not latch; flips off\n");
		dpu_stop_locked();
	}
	return 0;
}

/*
 * The scaling test fills the frames white and black and reads LM_0's MISR: the boot log draws nothing
 * while it runs. On PASS the panel scans frame 0 of the new, scaled layout; on FAIL frame 2 of UEFI's
 * (the black test frame) -- resume() names whichever it is, and the painter repaints there.
 */
unsigned
hakios_display_dpu_scale_start(uint32_t w, uint32_t h, uint32_t stride, vm_offset_t *virt, unsigned max)
{
	unsigned n;

	dpu_bootlog_pause_for_test();
	n = dpu_scale_start_locked(w, h, stride, virt, max);
	dpu_bootlog_resume();
	return n;
}

/*
 * Find the pipes that scan the GOP frame and map the whole carve-out (write-combined, as the GOP frame is).
 * Returns the number of buffers (buffer 0 is the GOP frame itself, which the caller has mapped too), their kernel
 * addresses in virt[1..]; 0 when the DPU is not as expected (then nothing was written).
 */
static unsigned
dpu_start_locked(uint64_t phys, uint32_t row_bytes, uint32_t height, vm_offset_t *virt, unsigned max)
{
	uint32_t pipes = 0, lms = 0;
	int ctl = -1;
	bool first_lm = true;

	if (!hakios_display_command_mode() || !regs_alive()) {
		printf("display: fb_dpu: needs the command-mode scan (fb_scan=1); CPU path kept\n");
		return 0;
	}
	dpu_frame_bytes = (uint64_t)row_bytes * height;
	if (phys != UMI_SPLASH_PHYS || dpu_frame_bytes == 0 || dpu_frame_bytes > UMI_SPLASH_SIZE) {
		printf("display: fb_dpu: GOP frame %#llx (%llu bytes) is not the UMI splash carve-out; CPU path kept\n",
		    phys, dpu_frame_bytes);
		return 0;
	}
	mdp = ml_io_map_unmappable(MDP_PHYS, MDP_SIZE, DPU_WIMG_IO);
	if (!mdp) return 0;
	for (int n = 0; n < 6; n++) {
		uint32_t intf_active = rd(mdp, DPU_CTL(n) + CTL_INTF_ACTIVE);
		printf("display: fb_dpu: CTL_%d intf=%#x fetch=%#x dsc=%#x merge3d=%#x flush=%#x layer0..2=%#x/%#x/%#x ext0..1=%#x/%#x\n",
		    n, intf_active, rd(mdp, DPU_CTL(n) + CTL_FETCH_PIPE_ACTIVE), rd(mdp, DPU_CTL(n) + CTL_DSC_ACTIVE),
		    rd(mdp, DPU_CTL(n) + CTL_MERGE_3D_ACTIVE), rd(mdp, DPU_CTL(n) + CTL_FLUSH),
		    rd(mdp, DPU_CTL(n) + CTL_LAYER(0)), rd(mdp, DPU_CTL(n) + CTL_LAYER(1)), rd(mdp, DPU_CTL(n) + CTL_LAYER(2)),
		    rd(mdp, DPU_CTL(n) + CTL_LAYER_EXT(0)), rd(mdp, DPU_CTL(n) + CTL_LAYER_EXT(1)));
		if (ctl < 0 && (intf_active & (1u << 1))) ctl = n;      /* INTF_1: DSI0, the panel */
	}
	if (ctl < 0) {
		printf("display: fb_dpu: no CTL drives INTF_1; CPU path kept\n");
		return 0;
	}
	dpu_ctl = DPU_CTL(ctl);
	for (unsigned lm = 0; lm < 6; lm++) {
		uint32_t staged = (rd(mdp, dpu_ctl + CTL_LAYER(lm)) & ~CTL_MIXER_BORDER_OUT) | rd(mdp, dpu_ctl + CTL_LAYER_EXT(lm)) |
		    rd(mdp, dpu_ctl + CTL_LAYER_EXT2(lm)) | rd(mdp, dpu_ctl + CTL_LAYER_EXT3(lm));
		if (staged) {
			lms |= lm_flush[lm];
			if (first_lm) dpu_lm = lm;
			first_lm = false;
			printf("display: fb_dpu: LM_%u staged on CTL_%d, out size %#x\n", lm, ctl, rd(mdp, DPU_LM(lm) + LM_OUT_SIZE));
		}
	}
	dpu_srcs = 0;
	unsigned npipes = 0;
	for (unsigned p = 0; p < 8; p++) {
		uint32_t base = sspp_off[p];
		bool found = false;

		for (unsigned k = 0; k < 4; k++) {
			uint32_t addr = rd(mdp, base + SSPP_SRC0_ADDR + 4 * k);
			if (addr < phys || addr >= phys + dpu_frame_bytes) continue;
			if (dpu_srcs == DPU_MAX_SRCS) return 0;
			dpu_src[dpu_srcs].reg = base + SSPP_SRC0_ADDR + 4 * k;
			dpu_src[dpu_srcs].offset = (uint32_t)(addr - phys);
			dpu_srcs++;
			found = true;
		}
		if (!found) continue;
		pipes |= sspp_flush[p];
		npipes++;
		dpu_pipe = base;
		dpu_vig = p < 4;
		printf("display: fb_dpu: SSPP %s scans the GOP frame: src size %#x xy %#x out size %#x xy %#x addr0..3 %#x %#x %#x %#x "
		    "ystride0 %#x format %#x op %#x multirect %#x%s%#x pe %#x/%#x/%#x\n", sspp_name[p], rd(mdp, base + SSPP_SRC_SIZE),
		    rd(mdp, base + SSPP_SRC_XY), rd(mdp, base + SSPP_OUT_SIZE), rd(mdp, base + SSPP_OUT_XY),
		    rd(mdp, base + SSPP_SRC0_ADDR), rd(mdp, base + SSPP_SRC0_ADDR + 4), rd(mdp, base + SSPP_SRC0_ADDR + 8),
		    rd(mdp, base + SSPP_SRC0_ADDR + 12), rd(mdp, base + SSPP_SRC_YSTRIDE0), rd(mdp, base + SSPP_SRC_FORMAT),
		    rd(mdp, base + SSPP_SRC_OP_MODE), rd(mdp, base + SSPP_MULTIRECT_OPMODE),
		    p < 4 ? " qseed3 op " : " (no scaler) ", p < 4 ? rd(mdp, base + VIG_SCALER + QSEED3_OP_MODE) : 0,
		    rd(mdp, base + SSPP_PE_C0_LR), rd(mdp, base + SSPP_PE_C0_LR + 4), rd(mdp, base + SSPP_PE_C0_LR + 8));
	}
	if (dpu_srcs == 0) {
		printf("display: fb_dpu: no source pipe fetches the GOP frame at %#llx; CPU path kept\n", phys);
		return 0;
	}
	if (npipes != 1) dpu_pipe = UINT32_MAX;
	else dpu_geo_read(&dpu_uefi);
	dpu_flush = pipes | lms | CTL_FLUSH_CTL;
	uint64_t stride = (dpu_frame_bytes + 0xffffULL) & ~0xffffULL;
	carve = ml_io_map_unmappable((vm_offset_t)phys, (vm_size_t)UMI_SPLASH_SIZE, DPU_WIMG_WCOMB);
	if (!carve) return 0;
	dpu_base = phys;
	dpu_count = (unsigned)(UMI_SPLASH_SIZE / stride);
	if (dpu_count > max) dpu_count = max;
	if (dpu_count > DPU_MAX_BUFFERS) dpu_count = DPU_MAX_BUFFERS;
	if (dpu_count < 2) {
		printf("display: fb_dpu: no second buffer in the carve-out; CPU path kept\n");
		return 0;
	}
	for (unsigned i = 0; i < dpu_count; i++) {
		dpu_phys[i] = phys + i * stride;
		virt[i] = carve + (vm_offset_t)(i * stride);
	}
	dpu_shown = 0;
	dpu_pending = -1;
	dpu_ready = true;
	printf("display: fb_dpu: CTL_%d flush mask %#x (pipes %#x, mixers %#x), %u address registers, %u buffers of %llu bytes at "
	    "%#llx + %#llx*i\n", ctl, dpu_flush, pipes, lms, dpu_srcs, dpu_count, dpu_frame_bytes, phys, stride);
	return dpu_count;
}

/*
 * fb_dpu setup: the pipes are found and the carve-out mapped here. The boot log draws nothing from
 * before the first frame is looked at until the caller has run its self-tests and re-laid the frames
 * (they call their own pause/resume); this one names UEFI's frame for the case nothing else does.
 */
unsigned
hakios_display_dpu_start(uint64_t phys, uint32_t row_bytes, uint32_t height, vm_offset_t *virt, unsigned max)
{
	unsigned n;

	dpu_gop_phys = phys;
	dpu_gop_w = row_bytes / 4;
	dpu_gop_h = height;
	dpu_gop_stride = row_bytes;
	hakios_bootlog_pause();
	n = dpu_start_locked(phys, row_bytes, height, virt, max);
	dpu_bootlog_resume();
	return n;
}

#include "umi_screenoff.c"

/* Session switches are read only once, before the first framebuffer client. */
#include "umi_display_session.h"

#include "umi_dpu_compose.h"
