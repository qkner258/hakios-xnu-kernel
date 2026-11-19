/* Read-only PMIC observation before any bobc1 RPMh request.
 * Live DT: PM8150L SID4/5; SPMI arbiter v5, EE0, core c440000,
 * observer e600000. No PMIC write channel is mapped here.
 * PMIC5 BOB subtype5 on this UMI (SID5 ID01/00/01/00/29/05/00):
 * STATUS=08 bit7 ready, VSET=40/41, VALID_VSET=42/43 (LE mV),
 * MODE=45, ENABLE=46 bit7. See MIC-umi.md for source and hardware evidence.
 * The older PmicDxeLa BOB table used in capture3/3b does not describe
 * this register layout. In particular 40 is NOT a voltage limit, 45/46
 * are NOT valid VSET, and 48 is NOT mode. Do not infer ULS from them.
 */
extern int umi_rpmh_mic_bob(bool enable, bool *owned);

static struct block bob_core = { .name = "SPMI arbiter core (read-only)", .phys = 0x0c440000, .size = 0x1100 };
static struct block bob_observer = { .name = "SPMI EE0 observer (READ commands only)", .phys = 0x0e600000, .size = 0x10000 };
static uint16_t bob_channel = 0xffff, bob_ppid;
static bool bob_owned;

static bool
bob_read(unsigned channel, uint8_t reg, uint8_t *bytes, unsigned count)
{
	/* These three non-destructive ranges only. */
	if (channel >= 512 || !((reg == 0 && count == 7) || (reg == 8 && count == 1) ||
	    (reg == 0x40 && count == 8)) || !mapped(&bob_observer, 0x80 * channel + 0x1c)) return false;
	for (unsigned retry = 0; retry < 3; retry++) {
		uint64_t start = mach_absolute_time(), interval;
		uint32_t status, data[2];
		nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &interval);
		hakios_spmi_lock();
		/* Read command to observer only; never SPMI extended-write opcode. */
		wr(bob_observer.base + 0x80 * channel, (1u << 27) | (uint32_t)reg << 4 | (count - 1));
		do {
			status = rd(bob_observer.base + 0x80 * channel + 8);
		} while (!(status & 1) && mach_absolute_time() - start < interval);
		data[0] = rd(bob_observer.base + 0x80 * channel + 0x18);
		data[1] = count > 4 ? rd(bob_observer.base + 0x80 * channel + 0x1c) : 0;
		hakios_spmi_unlock();
		if ((status & 15) == 1) {
			for (unsigned n = 0; n < count; n++) bytes[n] = (uint8_t)(data[n / 4] >> (8 * (n % 4)));
			return true;
		}
		if ((status & 15) == 9 && retry < 2) continue; /* retry DROPPED only */
		say("umi-mic: BOB observer READ FAILED channel=%u reg=%#x count=%u status=%#x done=%u denied=%u dropped=%u timeout=%u\n",
		    channel, reg, count, status, !!(status & 1), !!(status & 4), !!(status & 8), !(status & 1));
		break;
	}
	return false;
}

struct bob_state { uint8_t status, cfg[8]; };

static unsigned
bob_valid_mv(const struct bob_state *s)
{
	return s->cfg[2] | (unsigned)s->cfg[3] << 8;
}

static bool
bob_sample(struct bob_state *s, const char *when)
{
	if (bob_channel == 0xffff) {
		say("umi-mic: BOB no supported identity; state reads NOT attempted; STOPPING HERE\n");
		return false;
	}
	if (((rd(bob_core.base + 0x900 + 4 * bob_channel) >> 8) & 0xfff) != bob_ppid) {
		say("umi-mic: BOB channel mapping changed; state reads NOT attempted; STOPPING HERE\n");
		return false;
	}
	if (!bob_read(bob_channel, 8, &s->status, 1) || !bob_read(bob_channel, 0x40, s->cfg, 8)) {
		say("umi-mic: BOB READ FAILED (%s); no guessed supply state\n", when ? when : "poll");
		return false;
	}
	if (when) say("umi-mic: BOB %s READ-ONLY: SID%u:a000 status=%#x ready=%u mode45=%#x enable46=%#x enabled=%u requested=%u valid=%u mV\n",
	    when, bob_ppid >> 8, s->status, !!(s->status & 0x80), s->cfg[5], s->cfg[6], !!(s->cfg[6] & 0x80),
	    s->cfg[0] | (unsigned)s->cfg[1] << 8, bob_valid_mv(s));
	if (when) say("umi-mic: BOB raw 40..47: %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    s->cfg[0], s->cfg[1], s->cfg[2], s->cfg[3], s->cfg[4], s->cfg[5], s->cfg[6], s->cfg[7]);
	return true;
}

static bool
bob_ready(const struct bob_state *s)
{
	unsigned mv = bob_valid_mv(s);
	/* Inherited gate requested for capture3c. Mode is diagnostic only:
	 * do not reprogram an enabled rail or treat unrelated STATUS bits as
	 * failure. Valid VSET is the PMIC latched setpoint, not an ADC reading.
	 */
	return (s->status & 0x80) && (s->cfg[6] & 0x80) && mv >= 3000 && mv <= 3600;
}

static bool
bob_on(void)
{
	struct bob_state s;
	if (bob_owned || !mapped(&bob_core, 0x10fc) || !mapped(&bob_observer, 0xfffc)) return false;
	uint32_t version = rd(bob_core.base), channels = rd(bob_core.base + 4) & 0x7ff;
	if (version != 0x50010000 || channels != 512) {
		say("umi-mic: BOB unsupported SPMI arbiter %#x channels=%u; STOPPING HERE\n", version, channels);
		return false;
	}
	bob_channel = 0xffff;
	for (unsigned n = 0; n < channels; n++) {
		uint32_t ppid = (rd(bob_core.base + 0x900 + 4 * n) >> 8) & 0xfff;
		uint8_t id[7];
		if (ppid != 0x4a0 && ppid != 0x5a0) continue;
		if (!bob_read(n, 0, id, 7)) continue;
		say("umi-mic: BOB candidate SID%u:a000 channel=%u ID=%02x/%02x/%02x/%02x/%02x/%02x/%02x\n",
		    ppid >> 8, n, id[0], id[1], id[2], id[3], id[4], id[5], id[6]);
		/* Fail closed on other packages/revisions. This tuple was read on
		 * UMI channel294; never select a layout by digital major alone.
		 * APID may vary, so discover it rather than hardcoding294.
		 */
		if (ppid != 0x5a0 || id[0] != 1 || id[1] != 0 || id[2] != 1 ||
		    id[3] != 0 || id[4] != 0x29 || id[5] != 5 || id[6] != 0) {
			say("umi-mic: BOB identity unsupported; expected SID5 ID=01/00/01/00/29/05/00\n");
			continue;
		}
		if (bob_channel != 0xffff) {
			say("umi-mic: BOB identity ambiguous; STOPPING HERE\n");
			return false;
		}
		bob_channel = (uint16_t)n;
		bob_ppid = (uint16_t)ppid;
	}
	if (bob_channel != 0xffff)
		say("umi-mic: BOB identity PASS: UMI PMIC5 subtype5; VSET40/41 VALID42/43 MODE45 EN46; EE0 observer ID read succeeded\n");
	if (!bob_sample(&s, "BEFORE")) return false;
	if (bob_ready(&s)) {
		say("umi-mic: BOB PASS: inherited supply ready; NO RPMh vote, cleanup will preserve it\n");
		return true;
	}
	/* Never change an enabled/transitioning rail. OFF must be proven by
	 * STATUS=0 and EN_CTL=0 (including all pin-enable bits). Native ULS
	 * is not decoded here. The existing RPMh request is bounded instead
	 * by live bobc1 DT: 3000..3960mV, init3312mV, PMIC5 AUTO6.
	 */
	if (s.status != 0 || s.cfg[6] != 0 ||
	    !(s.cfg[5] == 0 || s.cfg[5] == 4 || s.cfg[5] == 6 || s.cfg[5] == 7)) {
		say("umi-mic: BOB state unsuitable/uncertain; NO vote; STOPPING HERE\n");
		return false;
	}
	int error = umi_rpmh_mic_bob(true, &bob_owned);
	say("umi-mic: BOB off: bobc1 3312 mV AUTO6 request errno=%d owned=%u\n", error, bob_owned);
	if (error) return false;
	for (unsigned ms = 0; ms < 100; ms++) {
		sleep_ms(1);
		if (!bob_sample(&s, NULL)) return false;
		if (bob_ready(&s)) {
			(void)bob_sample(&s, "AFTER");
			say("umi-mic: BOB PASS: RPMh supply ready; owned=%u (only owned vote is released)\n", bob_owned);
			return true;
		}
	}
	(void)bob_sample(&s, "TIMEOUT");
	say("umi-mic: BOB NOT READY after 100 ms; STOPPING HERE\n");
	return false;
}

static void
bob_off(void)
{
	if (!bob_owned) return;
	int error = umi_rpmh_mic_bob(false, &bob_owned);
	say("umi-mic: BOB owned vote RELEASE %s errno=%d owned=%u\n", error || bob_owned ? "FAILED" : "PASS", error, bob_owned);
	if (error || bob_owned) cleanup_failed = true;
}
