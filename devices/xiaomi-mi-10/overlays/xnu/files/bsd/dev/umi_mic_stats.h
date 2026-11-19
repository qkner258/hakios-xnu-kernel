/* Integer PCM diagnostics; no acoustic claim follows from varying samples.
 * Caller bounds each accumulator to at most 60 s of mono 48 kHz S16 PCM.
 */
struct mic_stats {
	uint64_t frames, nonzero, changes, squares, periods, adjacent, slot_repeats;
	int64_t sum;
	int32_t minimum, maximum, last;
	uint32_t peak;
};

static uint64_t
mic_period_hash(const int16_t *pcm, unsigned count, uint16_t poison, bool *unchanged)
{
	uint64_t hash = 14695981039346656037ULL;
	*unchanged = count != 0;
	for (unsigned i = 0; i < count; i++) {
		uint16_t v = (uint16_t)pcm[i];
		*unchanged &= v == poison;
		hash = (hash ^ v) * 1099511628211ULL;
	}
	return hash;
}

static uint32_t
mic_sqrt(uint64_t value)
{
	uint32_t lo = 0, hi = 0x80000000u;
	while (hi - lo > 1) {
		uint32_t mid = (hi + lo) / 2;
		if ((uint64_t)mid * mid <= value) lo = mid;
		else hi = mid;
	}
	return lo;
}

static void
mic_stats_add(struct mic_stats *s, const int16_t *pcm, uint32_t count, bool adjacent, bool slot_repeat)
{
	if (!count) return;
	s->periods++;
	s->adjacent += adjacent;
	s->slot_repeats += slot_repeat;
	for (uint32_t i = 0; i < count; i++) {
		int32_t v = pcm[i];
		uint32_t a = (uint32_t)(v < 0 ? -v : v);
		if (!s->frames) s->minimum = s->maximum = v;
		else {
			s->changes += v != s->last;
			if (v < s->minimum) s->minimum = v;
			if (v > s->maximum) s->maximum = v;
		}
		s->frames++;
		s->sum += v;
		s->squares += (uint64_t)((int64_t)v * v);
		s->nonzero += v != 0;
		if (a > s->peak) s->peak = a;
		s->last = v;
	}
}

static uint32_t
mic_ac_milli(const struct mic_stats *s)
{
	if (!s->frames) return 0;
	int64_t mean = s->sum / (int64_t)s->frames;
	/* Center before division; subtract the fractional mean as well. */
	int64_t energy = (int64_t)s->squares - 2 * mean * s->sum + mean * mean * (int64_t)s->frames;
	int64_t fraction = (s->sum % (int64_t)s->frames) * 1000000 / (int64_t)s->frames;
	uint64_t variance = (uint64_t)energy / s->frames * 1000000 +
	    ((uint64_t)energy % s->frames) * 1000000 / s->frames;
	uint64_t correction = (uint64_t)(fraction * fraction / 1000000);
	return mic_sqrt(variance > correction ? variance - correction : 0);
}

static const char *
mic_signal_class(const struct mic_stats *s)
{
	if (!s->frames) return "NO_DATA";
	if (!s->nonzero) return "DIGITAL_SILENCE";
	if (s->minimum == s->maximum) return "CONSTANT_DC";
	if (s->periods >= 10 && (s->adjacent * 2 >= s->periods || s->slot_repeats * 2 >= s->periods))
		return "REPEATED_DMA";
	return "VARYING";
}

/* Approximate dBFS in tenths from milli-LSB RMS, -999 for zero. */
static int
mic_db10(uint32_t rms)
{
	if (!rms) return -999;
	int result = 0;
	while (rms < 16384000) { rms *= 2; result -= 60; }
	return result - (int)((32768000 - rms) * 60 / 16384000);
}
