/* SPDX-License-Identifier: BSD-2-Clause
 * Included by umi_modem_mhi.inc. The modem service thread alone changes DMA
 * tables. AUDIO_VOICE_0 is vendor DMA_BIDIRECTIONAL + offload (DT dir=0,
 * type=3, ER=0). Vendor mhi_init.c deliberately skips its channel context;
 * voice_mhi.c never prepares transfers or rings a channel doorbell. MVM owns
 * the mailbox handshake. Do not turn this into an ordinary UL PCM channel.
 */
#include "umi_voice_audio.h"
static struct {
	uint64_t physical, iova, heartbeat;
	unsigned state; /* 0 idle, 1 request published, 2 completed (release/acquire) */
	int result;
} md_voice;

int
umi_voice_mhi_map(uint64_t physical, uint64_t *pcie)
{
	if ((physical & 4095) || physical > 0x100000000ull - UVA_MAILBOX_BYTES) return EINVAL;
	uint64_t heartbeat = __atomic_load_n(&md_voice.heartbeat, __ATOMIC_ACQUIRE);
	if (!heartbeat || md_now_us() - heartbeat > 1000000) return ENXIO;
	unsigned state = __atomic_load_n(&md_voice.state, __ATOMIC_ACQUIRE);
	if (!state) {
		md_voice.physical = physical;
		__atomic_store_n(&md_voice.state, 1, __ATOMIC_RELEASE);
		umi_modem_wake();
		return EAGAIN;
	}
	if (physical != md_voice.physical) return EBUSY;
	if (state == 1) return EAGAIN;
	*pcie = md_voice.iova;
	return md_voice.result;
}

static int
md_voice_install(void)
{
	/* No AP-owned channel-80 context/START/transfer doorbells, as vendor. */
	const uint32_t *context = md_channel_context(80);
	for (unsigned word = 0; word < 11; word++) if (context[word]) return EBUSY;
	if (((md_read(MD_BAR + 0x48) >> 8) & 255) != 2) return EBUSY;
	struct md_buf slot = md_alloc(UVA_MAILBOX_BYTES);
	if (!slot.p) return ENOMEM;
	md_voice.iova = slot.a;
	lck_mtx_lock(&hakios_smmu_lock);
	for (unsigned page = 0; page < UVA_MAILBOX_BYTES / 4096; page++) {
		uint64_t offset = slot.a - MD_IOVA + page * 4096;
		md_table(2 + (unsigned)(offset >> 21))[(offset >> 12) & 511] =
		    (md_voice.physical + page * 4096) | 0xe43ull;
	}
	md_clean(md_tables, round_page(12 * 4096));
	md_write(md_cb + 0x610, md_cb_index);
	md_write(md_cb + 0x7f0, 1);
	int error = ETIMEDOUT;
	for (unsigned us = 0; us < 100000; us += 100) {
		if (!(md_read(md_cb + 0x7f4) & 1)) { error = 0; break; }
		delay(100);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return error;
}

static void
md_voice_service(void)
{
	__atomic_store_n(&md_voice.heartbeat, md_now_us(), __ATOMIC_RELEASE);
	if (__atomic_load_n(&md_voice.state, __ATOMIC_ACQUIRE) != 1) return;
	md_voice.result = md_voice_install();
	__atomic_store_n(&md_voice.state, 2, __ATOMIC_RELEASE);
}
