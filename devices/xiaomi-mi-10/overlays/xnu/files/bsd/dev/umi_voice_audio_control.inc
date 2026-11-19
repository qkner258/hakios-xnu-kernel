/* SPDX-License-Identifier: BSD-2-Clause
 * Request state: bb_lock. Firmware/route state: serialized switch worker only.
 */
#include <sys/errno.h>
#include <sys/uio.h>
#include "umi_voice_audio.h"
#define UVA_LOG(...) hakios_tl("voice-audio: " __VA_ARGS__)
static struct {
 bool active, known;
 uint32_t call, generation, completed;
 enum umi_voice_network network;
 enum umi_voice_route route;
 unsigned muted, volume;
 int result;
} uva_control;
static uint64_t uva_physical, uva_adsp, uva_pcie;
static bool uva_memory_ready, uva_mailbox_ready;
static int uva_transport_error;
#include "umi_voice_audio_session.inc"

static uint32_t uva_option(const char *name, uint32_t value, unsigned base)
{
 char path[96], text[32] = {0}; vnode_t node;
 snprintf(path, sizeof(path), "/var/db/hakios-voice/%s", name);
 if (vnode_lookup(path, 0, &node, vfs_context_kernel())) return value;
 int residual = 0;
 int error = vnode_vtype(node) == VREG ? vn_rdwr(UIO_READ, node, text, sizeof(text)-1, 0,
     UIO_SYSSPACE, 0, vfs_context_ucred(vfs_context_kernel()), &residual, vfs_context_proc(vfs_context_kernel())) : EINVAL;
 vnode_put(node);
 if (error) { UVA_LOG("option %s rc=%d\n", name, error); return value; }
 char *end;
 unsigned long parsed = strtoul(text, &end, base);
 if (end == text || parsed > UINT32_MAX) { UVA_LOG("option %s rc=%d\n", name, EINVAL); return value; }
 return (uint32_t)parsed;
}
static void uva_queue(void)
{
 uva_control.generation++;
 uva_control.result = EINPROGRESS;
 if (voice_switch_call) thread_call_enter(voice_switch_call);
}
extern "C" int umi_voice_audio_start_locked(uint32_t call, enum umi_voice_network network, enum umi_voice_route route)
{
 if (!voice_switch_wanted) return EPERM;
 if ((network != UMI_VOICE_GSM && network != UMI_VOICE_LTE) ||
     (route != UMI_VOICE_RECEIVER && route != UMI_VOICE_SPEAKER)) return EINVAL;
 if (uva_control.active) {
  if (uva_control.call != call) return EBUSY;
  if (uva_control.network != network || uva_control.route != route) return EINVAL;
  return uva_control.result;
 }
 if (uva_control.generation != uva_control.completed || (uva_control.known && uva_control.result)) return EBUSY;
 uva_control.known = uva_control.active = true; uva_control.call = call;
 uva_control.network = network; uva_control.route = route; uva_control.muted = 0; uva_control.volume = 70;
 uva_queue(); return EINPROGRESS;
}
static void uva_stop_locked(uint32_t call)
{
 if (!uva_control.active || uva_control.call != call) return;
 uva_control.active = false; uva_queue();
}
extern "C" int umi_voice_audio_stop_locked(uint32_t call)
{
 if (!uva_control.known || uva_control.call != call) return ENOENT;
 if (!uva_control.active && uva_control.result != EINPROGRESS && uva_control.result) uva_queue();
 else uva_stop_locked(call);
 return uva_control.result;
}
extern "C" int umi_voice_audio_start(uint32_t call, enum umi_voice_network network, enum umi_voice_route route)
{
 IOLockLock(bb_lock); int error = umi_voice_audio_start_locked(call, network, route); IOLockUnlock(bb_lock); return error;
}
extern "C" int umi_voice_audio_stop(uint32_t call)
{
 IOLockLock(bb_lock); int error = umi_voice_audio_stop_locked(call); IOLockUnlock(bb_lock); return error;
}
extern "C" int umi_voice_audio_status(uint32_t call)
{
 IOLockLock(bb_lock);
 int error = !uva_control.known || uva_control.call != call ? ENOENT : uva_control.result;
 IOLockUnlock(bb_lock); return error;
}
static int uva_change(uint32_t call, unsigned which, unsigned value)
{
 IOLockLock(bb_lock);
 int error = !voice_switch_wanted ? EPERM : !uva_control.active || uva_control.call != call ? ENOENT : 0;
 if (!error) {
  unsigned current = which == 0 ? (unsigned)uva_control.route : which == 1 ? uva_control.muted : uva_control.volume;
  if (current != value) {
   if (which == 0) uva_control.route = (enum umi_voice_route)value;
   else if (which == 1) uva_control.muted = value;
   else uva_control.volume = value;
   uva_queue();
  }
  error = uva_control.result;
 }
 IOLockUnlock(bb_lock); return error;
}
extern "C" int umi_voice_audio_route(uint32_t call, enum umi_voice_route route)
{
 if (route != UMI_VOICE_RECEIVER && route != UMI_VOICE_SPEAKER) return EINVAL;
 return uva_change(call, 0, (unsigned)route);
}
extern "C" int umi_voice_audio_mute(uint32_t call, int muted)
{
 if (muted != 0 && muted != 1) return EINVAL;
 return uva_change(call, 1, (unsigned)muted);
}
extern "C" int umi_voice_audio_volume(uint32_t call, uint32_t percent)
{
 if (percent > 100) return EINVAL;
 return uva_change(call, 2, percent);
}
static void uva_call_state(const uint8_t *call)
{
    if (call[1] == 9) uva_stop_locked(call[0]);
    if (call[1] != 3 || (call[2] != 0 && call[2] != 2)) return;
    if (call[4] == 2 || call[4] == 4)
        (void)umi_voice_audio_start_locked(call[0], call[4] == 4 ? UMI_VOICE_LTE : UMI_VOICE_GSM, UMI_VOICE_RECEIVER);
}

static void uva_work(void)
{
 IOLockLock(bb_lock);
 if (!voice_switch_wanted && uva_control.active) uva_stop_locked(uva_control.call);
 auto request = uva_control;
 IOLockUnlock(bb_lock);
 if (request.generation == request.completed) {
  if (!request.active || !uva_session.started) return;
  int health = umi_voice_route_health();
  if (!health) return;
  int cleanup = uva_session_stop();
  IOLockLock(bb_lock);
  if (request.generation == uva_control.generation) uva_control.result = health;
  IOLockUnlock(bb_lock);
  UVA_LOG("route-health rc=%d cleanup=%d\n", health, cleanup);
  return;
 }
 int error = 0;
 const char *stage = "stop";
 uint32_t dsp_result = UINT32_MAX;
 struct uva_config config = {};
 bool current;
 if (!request.active) { error = uva_session_stop(); goto done; }
 if (uva_transport_error) { error = uva_transport_error; stage = "transport-latched"; goto done; }
 if (!uva_memory_ready) {
  stage = "memory"; error = umi_voice_audio_memory(&uva_physical, &uva_adsp);
  if (error) goto transport;
  uva_memory_ready = true; UVA_LOG("memory rc=0\n");
 }
 stage = "mhi-offload"; error = umi_voice_mhi_map(uva_physical, &uva_pcie);
 if (error == EAGAIN) return;
 if (error) goto transport;
 if (!uva_mailbox_ready) {
  UVA_LOG("mhi-offload rc=0\n");
  stage = "mailbox"; error = umi_voice_mailbox_config(uva_adsp, uva_pcie, &dsp_result);
  if (error) goto transport;
  uva_mailbox_ready = true; UVA_LOG("mailbox rc=0 dsp=0\n");
 }
 IOLockLock(bb_lock);
 current = voice_switch_wanted && uva_control.active && request.generation == uva_control.generation;
 IOLockUnlock(bb_lock);
 if (!current) return;
 stage = "session";
 if (!uva_session.started || uva_session.output != request.route) {
  error = uva_session_stop();
  if (error) goto done;
  config.vsid = uva_option("vsid", 0x11c05000, 16);
  config.tx_topology = uva_option("tx-topology", 0x10f70, 16);
  config.rx_topology = uva_option("rx-topology", 0x10f77, 16);
  config.cvp_version = uva_option("cvp-api", 2, 10);
  config.volume_steps = uva_option("volume-steps", 5, 10);
  umi_voice_receiver_clock(uva_option("receiver-clock", 19200000, 10));
  error = uva_session_start(config, request.route, request.muted, request.volume);
 } else {
  stage = "mute"; error = uva_set_mute(request.muted);
  if (!error) { stage = "volume"; error = uva_set_volume(request.volume); }
 }
 if (error) {
  int cleanup = uva_session_stop();
  UVA_LOG("unwind rc=%d\n", cleanup);
 }
 goto done;
transport:
 uva_transport_error = error;
done:
 IOLockLock(bb_lock);
 if (request.generation == uva_control.generation) {
  uva_control.completed = request.generation; uva_control.result = error;
 }
 IOLockUnlock(bb_lock);
 UVA_LOG("%s rc=%d dsp=%u running=%u\n", error ? stage : request.active ? "running" : "stopped", error,
     dsp_result, uva_session.started ? 1u : 0u);
}
