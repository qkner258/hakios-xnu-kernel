/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef UMI_VOICE_AUDIO_H
#define UMI_VOICE_AUDIO_H
#include <stdint.h>
#define UVA_MAILBOX_BYTES (128u * 1024u)
#define UVA_MAILBOX_COMMAND 0x0001333bu
#define UVA_APR_PORT 0x0103u
#define UVA_APR_SERVICE 9u /* ADSP MVM, not modem MVM (4 / ADSP AFE). */
#ifdef __cplusplus
extern "C" {
#endif
enum umi_voice_network { UMI_VOICE_GSM = 1, UMI_VOICE_LTE = 2 };
enum umi_voice_route { UMI_VOICE_RECEIVER = 0, UMI_VOICE_SPEAKER = 1 };
/* Callable by the QMI observer or IMS owner. No filesystem or DSP wait here.
 * start/stop/changed controls return EINPROGRESS; poll status until completion.
 * status=0 means the requested DSP commands and physical route completed.
 * Route changes rebuild passive sessions and briefly interrupt speech.
 * mute is uplink only; volume is downlink 0..100 (0 mutes output).
 * Call route/mute/volume/status outside bb_lock; only *_locked may run under it.
 * After stop status reports teardown/restore result until the next start.
 * One call owns the path; a duplicate start is idempotent, another is EBUSY.
 * Start and controls require the sampled /var/db/hakios-voice/on switch.
 * Stop/status remain available to finish teardown after switch removal.
 */
int umi_voice_audio_start(uint32_t call, enum umi_voice_network network, enum umi_voice_route route);
int umi_voice_audio_stop(uint32_t call);
int umi_voice_audio_route(uint32_t call, enum umi_voice_route route);
int umi_voice_audio_mute(uint32_t call, int muted);
int umi_voice_audio_volume(uint32_t call, uint32_t percent);
int umi_voice_audio_status(uint32_t call);
/* QMI/IMS dispatch already holding the bridge's bb_lock must use these two
 * variants; ordinary entry points acquire bb_lock themselves. */
int umi_voice_audio_start_locked(uint32_t call, enum umi_voice_network network, enum umi_voice_route route);
int umi_voice_audio_stop_locked(uint32_t call);
/* Private transport interfaces, called only by the serialized voice worker. */
int umi_voice_audio_memory(uint64_t *physical, uint64_t *adsp);
int umi_voice_mhi_map(uint64_t physical, uint64_t *pcie);
int umi_voice_mailbox_config(uint64_t adsp, uint64_t pcie, uint32_t *dsp_result);
struct uva_reply { uint32_t result, bytes; uint16_t port; uint8_t data[64]; };
int umi_voice_apr(uint8_t service, uint16_t port, uint32_t opcode,
    const void *data, uint32_t bytes, struct uva_reply *reply);
/* Serialized worker only. take parks media ASM/ADM and claims both AFE paths;
 * release restores media. No iOS PCM is fed into the modem speech path. */
int umi_voice_route_take(void);
int umi_voice_route_set(enum umi_voice_route route, uint32_t percent);
int umi_voice_route_release(void);
int umi_voice_route_health(void);
int umi_voice_mic_take(void);
int umi_voice_mic_release(void);
int umi_voice_receiver(int on, uint32_t percent);
void umi_voice_receiver_clock(uint32_t hz);
#ifdef __cplusplus
}
#endif
#endif
