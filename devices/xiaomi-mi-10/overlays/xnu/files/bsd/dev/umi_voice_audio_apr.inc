/* SPDX-License-Identifier: BSD-2-Clause
 * Included before apr_input(). Voice services have their own reply ownership.
 * apr_lock serializes senders; the link thread only publishes a completed reply.
 */
#include "umi_voice_audio.h"
static struct {
 uint32_t token, sequence, opcode;
 uint16_t port;
 uint8_t service;
 bool waiting, done;
 struct uva_reply reply;
} uva_apr;

static bool
uva_apr_input(const struct apr_header *h, const uint8_t *p, uint32_t n)
{
 if (h->from_service < 9 || h->from_service > 11 || h->from_domain != APR_DOMAIN_ADSP ||
     h->to_service != h->from_service || h->to_domain != APR_DOMAIN_APPS || h->to_port != UVA_APR_PORT) return false;
 /* Consume late replies/events here, never route voice into the media handlers. */
 if (!__atomic_load_n(&uva_apr.waiting, __ATOMIC_ACQUIRE) || h->token != uva_apr.token ||
     h->from_service != uva_apr.service) return true;
 bool create = uva_apr.opcode == 0x110ff || uva_apr.opcode == 0x11140 ||
     uva_apr.opcode == 0x112bf || uva_apr.opcode == 0x13169;
 if (!create && h->from_port != uva_apr.port) return true;
 if (h->opcode == APR_BASIC_RSP_RESULT) {
  uint32_t words[2];
  if (n != sizeof(words)) uva_apr.reply.result = UINT32_MAX;
  else {
   memcpy(words, p, sizeof(words));
   if (words[0] != uva_apr.opcode) return true;
   /* GET success is only acceptance; the version response completes it. */
   if (uva_apr.opcode == 0x11378 && !words[1]) return true;
   uva_apr.reply.result = words[1];
   if (create && !words[1] && !h->from_port) uva_apr.reply.result = UINT32_MAX;
  }
 } else if (uva_apr.opcode == 0x11378 && h->opcode == 0x11379) {
  if (!n || n > 31 || p[n - 1] != 0) uva_apr.reply.result = UINT32_MAX;
  else { memcpy(uva_apr.reply.data, p, n); uva_apr.reply.bytes = n; uva_apr.reply.result = 0; }
 } else return true; /* APR_RSP_ACCEPTED is not completion. */
 uva_apr.reply.port = h->from_port;
 __atomic_store_n(&uva_apr.done, true, __ATOMIC_RELEASE);
 return true;
}

int
umi_voice_apr(uint8_t service, uint16_t port, uint32_t opcode, const void *data, uint32_t bytes, struct uva_reply *reply)
{
 struct { struct apr_header h; uint8_t data[192]; } packet;
 bzero(&packet, sizeof(packet));
 packet.h.field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(struct apr_header));
 packet.h.size = (uint16_t)(sizeof(packet.h) + bytes);
 packet.h.from_service = packet.h.to_service = service;
 packet.h.from_domain = APR_DOMAIN_APPS; packet.h.to_domain = APR_DOMAIN_ADSP;
 packet.h.from_port = UVA_APR_PORT; packet.h.to_port = port; packet.h.opcode = opcode;
 if (bytes > sizeof(packet.data)) return EMSGSIZE;
 if (bytes) memcpy(packet.data, data, bytes);
 int error = ENXIO;
 lck_mtx_lock(&apr_lock);
 bzero(&uva_apr.reply, sizeof(uva_apr.reply)); uva_apr.reply.result = UINT32_MAX;
 if (!audio_up || !glink.channel[CHANNEL_APR].open) goto out;
 packet.h.token = uva_apr.token = 0x76000000u | (++uva_apr.sequence & 0xffffffu);
 uva_apr.service = service; uva_apr.port = port; uva_apr.opcode = opcode;
 uva_apr.done = false;
 __atomic_store_n(&uva_apr.waiting, true, __ATOMIC_RELEASE);
 tell("voice-audio: apr send svc=%u op=0x%x bytes=%u\n", service, opcode, bytes);
 error = glink_send(&glink.channel[CHANNEL_APR], &packet, packet.h.size);
 if (!error) {
  error = ETIMEDOUT;
  for (unsigned ms = 0; ms < 500; ms++) {
   if (__atomic_load_n(&uva_apr.done, __ATOMIC_ACQUIRE)) {
    error = uva_apr.reply.result == UINT32_MAX ? EPROTO : uva_apr.reply.result ? EIO : 0;
    break;
   }
   sleep_ms(1);
  }
 }
 __atomic_store_n(&uva_apr.waiting, false, __ATOMIC_RELEASE);
out:
 *reply = uva_apr.reply;
 tell("voice-audio: apr result svc=%u op=0x%x rc=%d dsp=%u handle=%u\n",
     service, opcode, error, reply->result, reply->port);
 lck_mtx_unlock(&apr_lock);
 return error;
}

int
umi_voice_mailbox_config(uint64_t adsp, uint64_t pcie, uint32_t *dsp_result)
{
 uint32_t words[5] = { (uint32_t)adsp, (uint32_t)(adsp >> 32), (uint32_t)pcie, (uint32_t)(pcie >> 32), UVA_MAILBOX_BYTES };
 struct uva_reply reply;
 int error = umi_voice_apr(9, 0, UVA_MAILBOX_COMMAND, words, sizeof(words), &reply);
 *dsp_result = reply.result;
 return error;
}
