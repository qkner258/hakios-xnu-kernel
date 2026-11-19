/* Included by hakios.display.c. bl=1 only; UEFI keeps the panel/TE running.
 * DSI6G register offsets include the +4 HW_VERSION word (mainline dsi.xml).
 * Command FIFO avoids any assumptions about the display SMMU's DMA mappings.
 */
#include <kern/thread.h>
#include <kern/sched_prim.h>

#define BL_IRQ_STATUS 0x01110101u /* W1C: DMA, MDP, video, BTA, error */
#define BL_IRQ_DMA_MASK (1u << 1) /* bit 24 is ERROR, NOT DMA mask */
#define BL_BLOCK_IN_FRAME (1u << 12)
static int bl_mode;
static bool bl_ready, bl_failed, bl_pending, bl_control_set, bl_known;
static unsigned bl_base = 600, bl_factor = 1000, bl_min_mnits;
static unsigned bl_level, bl_from, bl_target, bl_duration_ms;
static uint64_t bl_started;
static unsigned bl_logs, bl_requests, bl_misses;
static bool bl_no_idle_dim, bl_no_fade, bl_measure;
static LCK_GRP_DECLARE(bl_group, "umi-backlight");
static LCK_MTX_DECLARE(bl_lock, &bl_group);

/* Nominal 500 nit normal range, NOT a measured luminance calibration or HBM.
 * BrightnessMinPhysical is a floor in nits; zero removes that floor. Apply the
 * display factor last, so factor=0 always extinguishes the AMOLED. Slider zero
 * is the panel's minimum visible level, distinct from display off.
 */
static unsigned
bl_map(unsigned base, unsigned factor, unsigned min_mnits)
{
	unsigned level = UMI_BL_MIN_LEVEL +
	    ((UMI_BL_MAX_LEVEL - UMI_BL_MIN_LEVEL) * base + 500) / 1000;
	unsigned floor = ((uint64_t)min_mnits * UMI_BL_MAX_LEVEL + 250000) / 500000;
	if (floor > level) level = floor;
	return (level * factor + 500) / 1000;
}

static unsigned
bl_interpolate(unsigned from, unsigned target, uint64_t elapsed_ms, unsigned duration_ms)
{
	if (!duration_ms || elapsed_ms >= duration_ms) return target;
	/* Signed arithmetic keeps downward ramps monotonic, including the last zero. */
	return (int64_t)from + ((int64_t)target - from) * (int64_t)elapsed_ms / duration_ms;
}

/* bl_lock serializes every command, including legacy sysctl requests in bl=1.
 * Never submit on a failed TE wait. BLOCK_DMA_WITHIN_FRAME closes the race if
 * the next autorefresh kickoff occurs after the software idle check.
 * Returns errno, not a panel acknowledgement (these are unacknowledged writes).
 */
static int
bl_dma(const uint32_t *packet, unsigned words, unsigned command, unsigned value)
{
	int error = 0;
	uint32_t irq = 0, status = 0, fifo = 0, ack = 0;
	bool submitted = false;
	if (!words || words > 16) return EINVAL; /* at most 64 FIFO bytes */
	if (!umi || !dsi0 || !hakios_display_command_mode() || !regs_alive()) return ENXIO;
	if ((rd(dsi0, DSI_CTRL) & 7) != 5) return ENOTSUP;
	/* A mode transaction has stopped autorefresh and drained MDP already.
	 * Its undo must not wait on scan timing from the mode being undone. */
	if ((rd(intf, 0x2b4) & (1u << 31)) &&
	    !hakios_display_wait_copy(500000, 40000000)) return EAGAIN;
	if (!regs_alive()) return ENXIO;
	status = rd(dsi0, DSI_STATUS);
	if (status & DSI_STATUS_CMD_BUSY) return EAGAIN;
	uint32_t old_fifo = rd(dsi0, 0x000c), old_ack = rd(dsi0, 0x0068);
	uint32_t old_tpg = rd(dsi0, DSI_TEST_PATTERN_GEN_CTRL);
	if (old_tpg) return EBUSY; /* do not steal another command/pattern generator */
	uint32_t old_dma = rd(dsi0, DSI_COMMAND_MODE_DMA_CTRL);
	uint32_t old_trig = rd(dsi0, DSI_TRIG_CTRL);
	uint32_t old_length = rd(dsi0, DSI_DMA_CMD_LENGTH);
	uint32_t old_irq = rd(dsi0, DSI_INT_CTRL) & ~BL_IRQ_STATUS;
	/* Only acknowledge DMA_DONE; preserve all other latched status bits. */
	wr(dsi0, DSI_INT_CTRL, (old_irq & ~BL_IRQ_DMA_MASK) | DSI_INT_CMD_DMA_DONE);
	wr(dsi0, DSI_TRIG_CTRL, (old_trig & ~7u) | 4u | BL_BLOCK_IN_FRAME);
	wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 1);
	wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 0);
	wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, 0x00030006);
	for (unsigned i = 0; i < words; i++)
		wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, packet[i]);
	/* DSI6G FIFO fetches pairs of DWORDs, even for an odd DMA length.
	 * Vendor dsi_ctrl_hw_cmn_kickoff_fifo_command requires this extra word.
	 * Keep it outside DMA_CMD_LENGTH and the packet's last-command bit. */
	if (words & 1) wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0);
	wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, (old_dma & ~0xc0000000u) | 0x14000000u);
	wr(dsi0, DSI_DMA_CMD_LENGTH, words * 4);
	wr(dsi0, DSI_CMD_MODE_DMA_SW_TRIGGER, 1);
	submitted = true;
	uint64_t start = mach_absolute_time(), ns;
	for (;;) {
		if (!regs_alive()) { error = ENXIO; break; }
		irq = rd(dsi0, DSI_INT_CTRL);
		status = rd(dsi0, DSI_STATUS);
		if ((irq & DSI_INT_CMD_DMA_DONE) && !(status & (1u << 1))) break;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		if (ns >= 40000000) { error = ETIMEDOUT; break; }
		delay_for_interval(50, NSEC_PER_USEC);
	}
	if (regs_alive()) {
		fifo = rd(dsi0, 0x000c);
		ack = rd(dsi0, 0x0068);
		if (!error && ((fifo & ~old_fifo & 0xcccc4489u) || (ack & ~old_ack) ||
		    (status & (1u << 31)))) error = EIO;
	}
	/* No global DSI reset on timeout: it would destroy UEFI's scanout. A live
	 * DMA must keep its FIFO/config; quarantine it until the next boot. */
	if (regs_alive() && !(status & (1u << 1))) {
		wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, old_tpg);
		wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 1);
		wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 0);
		wr(dsi0, DSI_DMA_CMD_LENGTH, old_length);
		wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, old_dma);
		wr(dsi0, DSI_TRIG_CTRL, old_trig);
		wr(dsi0, DSI_INT_CTRL, old_irq | DSI_INT_CMD_DMA_DONE);
	}
	if (submitted && (bl_logs++ < 96 || error))
		printf("umi-bl: DCS #%u cmd=0x%02x value=%u result=%d irq=%#x status=%#x fifo=%#x ack=%#x\n",
		    bl_logs, command, value, error, irq, status, fifo, ack);
	return error;
}

static int
bl_dcs(unsigned command, unsigned value)
{
	uint32_t packet[2];
	if (command == 0x51) {
		umi_dsi_backlight_packet(value, &packet[0], &packet[1]);
		return bl_dma(packet, 2, command, value);
	}
	packet[0] = 0x80150000u | (value << 8) | command;
	return bl_dma(packet, 1, command, value);
}

/* ABI: four uint32_t words, version=1, kind=1 slider permille / 2 factor
 * permille / 3 minimum physical milli-nits, value, fade milliseconds. */
static int
bl_request(unsigned kind, unsigned value, unsigned fade_ms)
{
	if (bl_mode != 1) return ENOTSUP;
	if (kind < 1 || kind > 3 || value > (kind == 3 ? 500000u : 1000u) || fade_ms > 10000) return EINVAL;
	lck_mtx_lock(&bl_lock);
	if (!bl_ready || bl_failed) {
		lck_mtx_unlock(&bl_lock);
		return ENXIO;
	}
	/* Session A/B controls; preserve genuine display off and on requests. */
	if (bl_no_idle_dim && kind == 2 && value > 0) value = 1000;
	if (bl_no_fade) fade_ms = 0;
	if (kind == 1) bl_base = value;
	if (kind == 2) bl_factor = value;
	if (kind == 3) bl_min_mnits = value;
	unsigned target = bl_map(bl_base, bl_factor, bl_min_mnits);
	/* Repeated minimum=0 and the two slider hooks must not restart a fade. */
	bool changed = !bl_known || target != bl_target;
	if (changed) {
		bl_from = bl_known ? bl_level : target;
		bl_target = target;
		bl_duration_ms = fade_ms;
		bl_started = mach_absolute_time();
		bl_pending = true;
		if (bl_factor) umi_pm_display(false, "brightness-factor");
		thread_wakeup(&bl_pending);
	} else if (!bl_pending) {
		umi_pm_display(bl_factor == 0, "brightness-factor");
	}
	if (bl_requests++ < 64 || (bl_measure && changed))
		printf("umi-bl: request kind=%u value=%u fade_ms=%u target=%u base=%u factor=%u min_mnits=%u\n",
		    kind, value, fade_ms, target, bl_base, bl_factor, bl_min_mnits);
	lck_mtx_unlock(&bl_lock);
	return 0;
}

static void
bl_worker(__unused void *arg, __unused wait_result_t result)
{
	lck_mtx_lock(&bl_lock);
	for (;;) {
		uint64_t deadline;
		clock_interval_to_deadline(16, NSEC_PER_MSEC, &deadline);
		if (bl_pending && !bl_failed) {
			uint64_t ns;
			absolutetime_to_nanoseconds(mach_absolute_time() - bl_started, &ns);
			unsigned level = bl_interpolate(bl_from, bl_target, ns / 1000000, bl_duration_ms);
			int error = 0;
			if (!bl_control_set) {
				error = bl_dcs(0x53, 0x20); /* vendor normal mode, HBM disabled */
				if (!error) bl_control_set = true;
			}
			if (!error && (!bl_known || level != bl_level)) error = bl_dcs(0x51, level);
			if (!error) {
				bl_known = true;
				bl_level = level;
				bl_misses = 0;
				if (level == bl_target) {
					bl_pending = false;
					umi_pm_display(bl_factor == 0, "brightness-factor");
					printf("umi-bl: settled level=%u display=%s base=%u factor=%u min_mnits=%u fade_ms=%u\n",
					    level, bl_factor ? "on" : "off", bl_base, bl_factor, bl_min_mnits, bl_duration_ms);
				}
			} else if (error != EAGAIN || ++bl_misses >= 8) {
				bl_failed = true;
				bl_pending = false;
				printf("umi-bl: disabled result=%d misses=%u; no DSI reset; existing blanking retained\n", error, bl_misses);
			}
		}
		if (bl_pending) assert_wait_deadline(&bl_pending, THREAD_UNINT, deadline);
		else assert_wait(&bl_pending, THREAD_UNINT);
		lck_mtx_unlock(&bl_lock);
		thread_block(THREAD_CONTINUE_NULL);
		lck_mtx_lock(&bl_lock);
	}
}

static void
bl_start(void)
{
	if (bl_mode != 1 || bl_ready || !umi || !dsi0) return;
	thread_t thread;
	bl_ready = true;
	if (kernel_thread_start(bl_worker, NULL, &thread) != KERN_SUCCESS) {
		bl_ready = false;
		printf("umi-bl: worker failed\n");
		return;
	}
	thread_deallocate(thread);
	printf("umi-bl: enabled bl=1; 11-bit DCS FIFO, TE + BLOCK_DMA_WITHIN_FRAME, normal 0x53=0x20; no panel sleep\n");
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, brightness_mode, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bl_mode, 0, "UMI brightness ABI (bl=1)");
static int
sysctl_bl_command(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	uint32_t words[4];
	if (!req->newptr || req->newlen != sizeof(words)) return EINVAL;
	int error = SYSCTL_IN(req, words, sizeof(words));
	if (error) return error;
	if (words[0] != 1) return EINVAL;
	return bl_request(words[1], words[2], words[3]);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness_command,
    CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    NULL, 0, sysctl_bl_command, "S,umi_bl_command", "UMI brightness transaction v1");
