/* SPDX-License-Identifier: BSD-2-Clause
 * Included by hakios.sound.c. All calls are on the serialized voice worker.
 * Publish the claim, park both media threads, close ASM/ADM, then transfer AFE.
 */
static bool uva_media_suspended, uva_mic_claim, uva_output_on;
static enum umi_voice_route uva_output;

int umi_voice_route_take(void)
{
 if (!umi_profile || !running || taken_down || level != 4) return ENXIO;
 __atomic_store_n(&voice_claimed, true, __ATOMIC_RELEASE);
 for (unsigned ms = 0; ms < 3000; ms++) {
  if (__atomic_load_n(&voice_feeder_parked, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&voice_watcher_parked, __ATOMIC_ACQUIRE)) break;
  if (ms == 2999) return ETIMEDOUT;
  sleep_ms(1);
 }
 lck_mtx_lock(&sound_lock);
 int error = 0;
 if (!stream_off()) error = EIO;
 if (!error && port_on) {
  uint16_t stop[2] = {active_port, 0};
  if (!command("voice owns playback AFE: media stop", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL)) error = EIO;
  else port_on = false;
 }
 if (!error && clock_on) {
  if (!afe_clock(false)) error = EIO;
  else clock_on = false;
 }
 if (!error) uva_media_suspended = true;
 lck_mtx_unlock(&sound_lock);
 if (error) return error;
 uva_mic_claim = true;
 error = umi_voice_mic_take();
 say("voice-audio: owner media->voice rc=%d\n", error);
 return error;
}

static int uva_output_stop(void)
{
 if (!uva_output_on) return 0;
 if (uva_output == UMI_VOICE_RECEIVER) {
  int error = umi_voice_receiver(0, 0);
  if (error) return error;
 } else {
  speaker_stop();
  uint16_t stop[2] = {active_port, 0};
  if (port_on && !command("voice speaker AFE stop", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL)) return EIO;
  port_on = false;
  if (clock_on && !afe_clock(false)) return EIO;
  clock_on = false;
 }
 uva_output_on = false;
 return 0;
}
int umi_voice_route_set(enum umi_voice_route route, uint32_t percent)
{
 if (!uva_media_suspended || !uva_mic_claim) return EBUSY;
 int error = 0;
 if (uva_output_on && uva_output != route) error = uva_output_stop();
 if (error) return error;
 bool opening = !uva_output_on;
 uva_output = route;
 uva_output_on = true; /* partial hardware setup also needs cleanup */
 if (route == UMI_VOICE_RECEIVER) error = umi_voice_receiver(percent ? 1 : 2, percent);
 else {
  if (opening && (!afe_on() || !speaker_start())) return EIO;
  /* No CPU guard in the direct CVP route. Keep at least -30 dB, plus
   * the existing board ceiling, DSP protection and hardware peak limits. */
  uint32_t attenuation = sound_attenuation((100 - percent) * 4);
  if (attenuation < 240) attenuation = 240;
  if (!percent) attenuation = 816;
  if (!speaker_set_volume(attenuation, 1)) error = EIO;
 }
 say("voice-audio: route=%u volume=%u rc=%d\n", (unsigned)route, percent, error);
 return error;
}
int umi_voice_route_release(void)
{
 int error = uva_output_stop();
 if (error) return error;
 if (uva_mic_claim) {
  error = umi_voice_mic_release();
  if (error) return error;
  uva_mic_claim = false;
 }
 if (!__atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE)) return 0;
 lck_mtx_lock(&sound_lock);
 /* Finish any partial media close before reopening it. */
 if (!stream_off()) error = EIO;
 if (!error && !port_on && !afe_on()) error = EIO;
 if (!error && !stream_on()) error = EIO;
 if (!error) {
  ring_tail = ring_head; last_sound_us = 0;
  uva_media_suspended = false;
  __atomic_store_n(&voice_claimed, false, __ATOMIC_RELEASE);
 }
 lck_mtx_unlock(&sound_lock);
 say("voice-audio: owner voice->media rc=%d\n", error);
 return error;
}

int umi_voice_route_health(void)
{
 return uva_output_on && uva_output == UMI_VOICE_SPEAKER && !umi_speaker_healthy() ? EIO : 0;
}
