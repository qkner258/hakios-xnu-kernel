/* Included last by hakios.display.c. No OTP/NV/rail/PLL/DT writes.
 * Samsung J2 MP 42 02 0b timing commands, vendor MP dtsi lines 153/294.
 * Called before any user client is allowed to draw or register vsync callbacks.
 */
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
extern kern_return_t thread_terminate(thread_t thread);

static bool
display_switch_file(const char *name)
{
	char path[96];
	vnode_t vp;
	snprintf(path, sizeof(path), "/var/db/hakios-display/%s", name);
	if (vnode_lookup(path, 0, &vp, vfs_context_kernel())) return false;
	bool found = vnode_vtype(vp) == VREG;
	vnode_put(vp);
	return found;
}

static bool bl_inplace_demote = true;   /* fb_dpu=4 (a2): on unless /var/db/hakios-display/inplace-demote-off */
static bool bl_compose_copies = true;   /* fb_dpu=4: on unless /var/db/hakios-display/compose-off */
static bool bl_write_kick = true;       /* fb_dpu=4: on unless /var/db/hakios-display/write-kick-off */
static bool bl_write_shown = true;      /* fb_dpu=4: on unless /var/db/hakios-display/write-shown-off */
static bool bl_zero_recover = true;     /* fb_dpu=4: on unless /var/db/hakios-display/zero-recover-off */

/* fb_dpu=4: MiniIOMobileFramebuffer.cpp's zero copy is tried again after zeroLeave() took it away (zeroRecover()),
 * sampled with the switches above (default on) */
int
hakios_display_zero_recover(void)
{
	return bl_zero_recover;
}

/* fb_dpu=4: MiniIOMobileFramebuffer.cpp's in-place demotion, sampled with the switches above */
int
hakios_display_inplace_demote(void)
{
	return bl_inplace_demote;
}

/* fb_dpu=4: a swap with layers is composed into the cached staging buffer, copied into a free scanout frame in one
 * piece and flipped to (MiniIOMobileFramebuffer.cpp, show()); 0 with /var/db/hakios-display/compose-off -- the
 * layers are then drawn straight into the frame, as before (the pixel-by-pixel, row-scratch way) */
int
hakios_display_compose_copies(void)
{
	return bl_compose_copies;
}

/* fb_dpu=4: a GPU submission that WROTES a framed surface after the panel fetched the frame is followed by one more
 * fetch of that frame once the GPU is done with it (MiniIOMobileFramebuffer.cpp: healWrites(), default on) */
int
hakios_display_write_kick(void)
{
	return bl_write_kick;
}

/* fb_dpu=4: a GPU submission into the frame on the panel waits for the successor flip's latch, not for "this
 * refresh's transfer is done" (hakios.display.c: hakios_display_dpu_write_busy(), default on). With
 * /var/db/hakios-display/write-shown-off the older test is back: a shown frame is free once its transfer is done
 * and the successor is still pending, which is up to one TE before the latch -- in command mode with autorefresh
 * that frame may be fetched once more, and the GPU's drawing lands on the glass (v29t: App Library's black). */
int
hakios_display_write_shown(void)
{
	return bl_write_shown;
}

/* Qualcomm DMA headers: bit30 long, bit31 end of batch. Pad to 4 bytes.
 * Preserve the vendor batching: page select, then B4 + 2F + 26 together.
 * The lane clock (682 MHz), DSC PPS and auto divisor stay as handed over.
 * Stop new TE kickoffs and drain MDP before touching panel timing. This also
 * makes the undo independent of the old software copy window.
 */
static int
display_panel_timing(unsigned hz, uint32_t config)
{
	static const uint32_t page[] = {0xc0390006, 0x52aa55f0, 0xffff0008};
	static const uint32_t timing60[] = {
		0x4039001e, 0xbbbbcbb4, 0x667799aa, 0xb4000000, 0xd0d0d0d0,
		0x3c8686d0, 0xa8f2f23c, 0x363636a8, 0xffff0a36, 0x0015012f, 0x80150126
	};
	static const uint32_t timing90[] = {
		0x4039001e, 0x888898b4, 0x44556677, 0x60000000, 0xb8b8b8b8,
		0xd8c8c8b8, 0xf8e8e8d8, 0x484848f8, 0xffff0a48, 0x0015042f, 0x80150226
	};
	if (hz != 60 && hz != 90) return EINVAL;
	uint32_t old_config = rd(intf, 0x288), refresh = rd(intf, 0x2b4);
	/* Vendor autorefresh disable sequence: disconnect external TE, disable
	 * autorefresh, wait for the current transfer. Do not reset the DSI host. */
	wr(intf, 0x288, old_config & ~(1u << 20));
	wr(intf, 0x2b4, refresh & ~(1u << 31));
	uint64_t start = mach_absolute_time(), elapsed;
	int error = 0;
	for (;;) {
		uint32_t written = rd(intf, 0x2b0) & 0xffff;
		if (!(rd(dsi0, DSI_STATUS) & DSI_STATUS_CMD_BUSY) &&
		    !(written > 0 && written < 2340)) break;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= 40000000) { error = ETIMEDOUT; break; }
		delay_for_interval(50, NSEC_PER_USEC);
	}
	if (!error) error = bl_dma(page, 3, 0xf0, 0);
	if (!error) error = bl_dma(hz == 90 ? timing90 : timing60, 11, 0x2f, hz);
	/* Program the synthetic counter while external TE is disconnected. Enable
	 * one transfer per TE before reconnecting TE, as in the vendor kickoff.
	 * CMD geometry/start/thresholds remain 2340 rows; no video-mode prefetch. */
	uint32_t next_config = error ? old_config : config;
	wr(intf, 0x288, next_config & ~(1u << 20));
	wr(intf, 0x2b4, refresh);
	wr(intf, 0x288, next_config);
	return error;
}

/* Read-only observation of TE wraps AND actual DMA restarts. A TE rate alone
 * cannot distinguish 60 Hz TE / divisor 2 from 60 transfers/second.
 * Opt-in: ~0.55 seconds of 250us sampling, once every 5 seconds.
 */
static void
display_measure_worker(__unused void *arg, __unused wait_result_t result)
{
	for (;;) {
		if (!scan.ok || !intf || !regs_alive()) break;
		uint32_t previous = rd(intf, 0x298) & 0xffff;
		uint32_t written = rd(intf, 0x2b0) & 0xffff;
		uint32_t frame0 = rd(intf, INTF_FRAME_COUNT), dma = 0, wraps = 0;
		uint64_t start = mach_absolute_time(), first = 0, last = 0, polled = start, poll_max = 0;
		uint64_t min = UINT64_MAX, max = 0, elapsed = 0;
		while (wraps < 33 && elapsed < NSEC_PER_SEC) {
			delay_for_interval(250, NSEC_PER_USEC);
			if (!regs_alive()) goto done;
			uint32_t phase = rd(intf, 0x298) & 0xffff;
			uint32_t next_written = rd(intf, 0x2b0) & 0xffff;
			uint64_t now = mach_absolute_time(), interval;
			absolutetime_to_nanoseconds(now - polled, &interval);
			if (interval > poll_max) poll_max = interval;
			polled = now;
			if (phase < previous) {
				if (wraps++) {
					absolutetime_to_nanoseconds(now - last, &interval);
					if (interval < min) min = interval;
					if (interval > max) max = interval;
				} else { first = now; dma = 0; frame0 = rd(intf, INTF_FRAME_COUNT); }
				last = now;
			}
			if (wraps && next_written < written) dma++;
			previous = phase;
			written = next_written;
			absolutetime_to_nanoseconds(now - start, &elapsed);
		}
		uint64_t span = 0;
		if (wraps > 1) absolutetime_to_nanoseconds(last - first, &span);
		uint64_t mean = wraps > 1 ? span / (wraps - 1) : 0;
		printf("display-measure: TE intervals=%u mean_us=%llu min_us=%llu max_us=%llu poll_max_us=%llu DMA-restarts=%u INTF-frames=%u auto=%#x TE-en=%#x config=%#x init=%#x height=%#x DSI-status=%#x%s\n",
		    wraps > 1 ? wraps - 1 : 0, mean / 1000, min == UINT64_MAX ? 0 : min / 1000,
		    max / 1000, poll_max / 1000, dma, rd(intf, INTF_FRAME_COUNT) - frame0, rd(intf, 0x2b4),
		    rd(intf, 0x284), rd(intf, 0x288), rd(intf, 0x294), rd(intf, 0x28c),
		    rd(dsi0, DSI_STATUS), wraps < 33 ? " INCOMPLETE" : "");
		uint64_t deadline;
		clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
		assert_wait_deadline(&bl_measure, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
done:
	thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

int
hakios_display_session_prepare(void)
{
	if (!hakios_display_command_mode() || bl_mode != 1) return 0;
	bool hz90 = display_switch_file("90hz");
	bool demote_off = display_switch_file("inplace-demote-off");
	bool compose_off = display_switch_file("compose-off");
	bool write_kick_off = display_switch_file("write-kick-off");
	bool write_shown_off = display_switch_file("write-shown-off");
	bool zero_recover_off = display_switch_file("zero-recover-off");
	lck_mtx_lock(&bl_lock);
	bl_no_idle_dim = display_switch_file("no-idle-dim");
	bl_no_fade = display_switch_file("no-fade");
	bl_measure = display_switch_file("measure");
	/* fb_dpu=4: a copied swap with no frame free of aliases gives one alias its frame back (a2, default on; review F1-F3 fixed in v29i) */
	bl_inplace_demote = !demote_off;
	/* fb_dpu=4: a swap with layers composed into the cached staging buffer and copied into a free frame (default on) */
	bl_compose_copies = !compose_off;
	/* fb_dpu=4: a frame the GPU wrote after its picture was fetched is fetched once more when the GPU is done (default on) */
	bl_write_kick = !write_kick_off;
	/* fb_dpu=4: the frame on the panel is free to draw into only once a successor flip has latched (default on; off = the old "this transfer is done" test) */
	bl_write_shown = !write_shown_off;
	/* fb_dpu=4: zero copy left by a failed alias check is tried again on a later swap (default on; fbzero: realias=) */
	bl_zero_recover = !zero_recover_off;
	printf("display-session: 90hz=%u no-idle-dim=%u no-fade=%u measure=%u inplace-demote=%u compose-copies=%u write-kick=%u write-shown=%u zero-recover=%u (files sampled before first client)\n",
	    hz90, bl_no_idle_dim, bl_no_fade, bl_measure, bl_inplace_demote, bl_compose_copies, bl_write_kick, bl_write_shown, bl_zero_recover);
	int error = 0;
	if (hz90) {
		uint32_t override;
		/* Never change mode after brightness activity / while another producer
		 * owns command DMA, or on a handoff other than the tested 60 Hz state. */
		if (bl_failed || bl_known || bl_pending || !dpu_ready || !dpu_wait(100000000) ||
		    scan.millihz < 58000 || scan.millihz > 62000 ||
		    rd(intf, 0x288) != 0x180088 || rd(intf, 0x2b4) != 0x80000001 ||
		    (PE_parse_boot_argn("fb_hz", &override, sizeof(override)) && override != 90)) {
			printf("display-session: 90 Hz refused: needs idle bl=1/fb_dpu and the verified 60 Hz UEFI handoff; no mode writes\n");
		} else {
			uint32_t config60 = rd(intf, 0x288);
			/* Vendor vsync_count = 19.2 MHz / (vtotal * Hz).
			 * MP 90 Hz: 2340 + 4 + 4 + 8 = 2356 lines, count = 90.
			 * Save the exact UEFI 60 Hz count (136) for the inverse switch. */
			uint32_t config90 = (config60 & ~0xffffu) | (19200000u / (2356u * 90u));
			error = display_panel_timing(90, config90);
			if (!error) {
				delay_for_interval(40, NSEC_PER_MSEC);
				if (!command_scan_start() || scan.millihz < 87000 || scan.millihz > 93000) error = EIO;
			}
			if (error) {
				int undo = display_panel_timing(60, config60);
				if (!undo) {
					delay_for_interval(40, NSEC_PER_MSEC);
					if (!command_scan_start() || scan.millihz < 58000 || scan.millihz > 62000) undo = EIO;
				}
				printf("display-session: 90 Hz failed result=%d; explicit 60 Hz undo=%d\n", error, undo);
				/* An optional mode experiment must not remove the framebuffer
				 * from userland. Calibration commits only on success. */
				error = 0;
			} else {
				printf("display-session: 90 Hz measured=%u mHz period_ns=%llu phase_ticks=%u DMA-idle=%u TE-config=%#x auto=%#x; lane clock/DSC unchanged\n",
				    scan.millihz, scan.frame_ns, scan.lines, te_idle_start,
				    rd(intf, 0x288), rd(intf, 0x2b4));
			}
		}
	}
	lck_mtx_unlock(&bl_lock);
	if (bl_measure && !error) {
		thread_t thread;
		if (kernel_thread_start(display_measure_worker, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
	}
	return error;
}
