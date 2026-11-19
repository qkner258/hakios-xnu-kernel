/* SPDX-License-Identifier: BSD-2-Clause
 * q6voice passive modem path. No AP vocoder/network/timing or RTP commands.
 * All objects and route ownership belong to the serialized switch worker.
 */
struct uva_config {
 uint32_t vsid, tx_topology, rx_topology, cvp_version, volume_steps;
};
static struct {
 uint16_t mvm, cvs, cvp;
 unsigned cvd;
 bool route, attached, started, unknown;
 enum umi_voice_route output;
 struct uva_config config;
} uva_session;

static int uva_cmd(uint8_t service, uint16_t port, uint32_t opcode, const void *data, uint32_t bytes)
{
 struct uva_reply reply;
 return umi_voice_apr(service, port, opcode, data, bytes, &reply);
}
static int uva_create(uint8_t service, uint32_t opcode, const void *p, uint32_t n, uint16_t *handle)
{
 struct uva_reply reply;
 int error = umi_voice_apr(service, 0, opcode, p, n, &reply);
 if (!error) *handle = reply.port;
 else if (error == ETIMEDOUT || error == EPROTO) uva_session.unknown = true;
 return error;
}
static int uva_module_param(uint32_t module, uint32_t id, const void *data, uint16_t n)
{
 struct {
  uint32_t address_low, address_high, handle, size;
  uint32_t module, id; uint16_t bytes, reserved;
  uint8_t data[96];
 } __attribute__((packed)) p = { 0, 0, 0, (uint32_t)(12 + n), module, id, n, 0, {0} };
 memcpy(p.data, data, n);
 return uva_cmd(11, uva_session.cvp, 0x1133d, &p, 28 + n);
}
static int uva_param(uint32_t id, const void *data, uint16_t n)
{
 return uva_module_param(0x1316e, id, data, n);
}
static int uva_formats(enum umi_voice_route route)
{
 int error;
 unsigned rx_channels = route == UMI_VOICE_SPEAKER ? 2 : 1;
 if (uva_session.cvd < 22) return 0;
 if (uva_session.config.cvp_version < 2) {
  uint16_t channels[2] = { 1, 1 }; /* vendor legacy command permits RX mono only */
  error = uva_cmd(11, uva_session.cvp, 0x13199, channels, sizeof(channels));
  if (error) return error;
 } else {
  for (unsigned tx = 0; tx < 3; tx++) {
   struct { uint32_t channels, bits; uint8_t map[32]; } p = { tx == 1 ? 1u : rx_channels, 16, {1,0} };
   if (tx != 1 && rx_channels == 2) p.map[1] = 2;
   error = uva_param(tx == 2 ? 0x13290 : tx == 1 ? 0x1328e : 0x1328f, &p, sizeof(p));
   if (error) return error;
  }
 }
 if (uva_session.cvd >= 23) {
  for (unsigned tx = 0; tx < 2; tx++) {
   struct {
    uint32_t port; uint16_t channels, bits; uint32_t rate; uint8_t map[32];
   } __attribute__((packed)) p = { tx ? 0xb037u : route == UMI_VOICE_SPEAKER ? 0x1004u : 0xb030u,
       (uint16_t)(tx ? 1 : rx_channels), 16, 48000, {1,0} };
   if (!tx && rx_channels == 2) p.map[1] = 2;
   error = uva_param(tx ? 0x13253 : 0x13254, &p, sizeof(p));
   if (error) return error;
  }
 }
 error = uva_cmd(11, uva_session.cvp, 0x13198, NULL, 0);
 if (error || route != UMI_VOICE_SPEAKER || uva_session.config.cvp_version < 2) return error;
 /* q6voice duplicates the mono vocproc input into both speaker channels. */
 struct {
  uint32_t index; uint16_t outputs, inputs, out_map[2], in_map, gain[2], reserved;
 } __attribute__((packed)) mixer = {0, 2, 1, {1,2}, 1, {0x4000,0x4000}, 0};
 error = uva_module_param(0x10912, 0x10342, &mixer, sizeof(mixer));
 if (error) return error;
 struct { uint32_t rate; uint16_t bits, channels, map[32]; } format = {48000,16,2,{1,2}};
 return uva_module_param(0x10912, 0x10913, &format, sizeof(format));
}
static int uva_set_mute(unsigned muted)
{
 uint16_t p[3] = {0, (uint16_t)muted, 20};
 return uva_cmd(10, uva_session.cvs, 0x1138b, p, sizeof(p));
}
static int uva_set_volume(unsigned percent)
{
 struct { uint16_t direction; uint32_t value; uint16_t ramp; } __attribute__((packed)) p = {
  1, (100 - percent) * uva_session.config.volume_steps / 100, 20 };
 int error = uva_cmd(11, uva_session.cvp, 0x112c2, &p, sizeof(p));
 if (!error) error = umi_voice_route_set(uva_session.output, percent);
 return error;
}
/* Stop on the first teardown failure: never give AFE back while firmware may
 * still own it. Another explicit stop retries the remaining real operation. */
static int uva_session_stop(void)
{
 int error;
 if (uva_session.unknown) { UVA_LOG("unresolved-create rc=%d\n", EBUSY); return EBUSY; }
 if (uva_session.started) {
  error = uva_cmd(9, uva_session.mvm, 0x11192, NULL, 0);
  if (error) return error;
  uva_session.started = false;
 }
 if (uva_session.attached) {
  error = uva_cmd(9, uva_session.mvm, 0x1123f, &uva_session.cvp, 2);
  if (error) return error;
  uva_session.attached = false;
 }
 if (uva_session.cvp) {
  error = uva_cmd(11, uva_session.cvp, 0x1003c, NULL, 0);
  if (error) return error;
  uva_session.cvp = 0;
 }
 if (uva_session.cvs) {
  error = uva_cmd(10, uva_session.cvs, 0x1003c, NULL, 0);
  if (error) return error;
  uva_session.cvs = 0;
 }
 if (uva_session.mvm) {
  error = uva_cmd(9, uva_session.mvm, 0x1003c, NULL, 0);
  if (error) return error;
  uva_session.mvm = 0;
 }
 if (uva_session.route) {
  error = umi_voice_route_release();
  if (error) return error;
  uva_session.route = false;
 }
 return 0;
}
static int uva_session_start(struct uva_config config, enum umi_voice_route route, unsigned muted, unsigned volume)
{
 int error;
 struct uva_reply reply;
 char name[20] = {0};
 uint8_t dual = 1;
 struct {
  uint16_t direction, tx_port; uint32_t tx_topology;
  uint16_t rx_port; uint32_t rx_topology, profile, mode;
  uint16_t ec_port; char name[20];
 } __attribute__((packed)) cvp = { 2, 0xb037, config.tx_topology,
     (uint16_t)(route == UMI_VOICE_SPEAKER ? 0x1004 : 0xb030), config.rx_topology,
     0x1135e, 0x10f7c, 0xffff, {0} };
 uva_session.config = config;
 uva_session.output = route;
 error = umi_voice_apr(9, 0, 0x11378, NULL, 0, &reply);
 if (error) return error;
 /* Vendor strings are "2.1" through "2.4"; log only parsed version. */
 if (reply.bytes < 4 || reply.data[1] != '.' || reply.data[0] < '0' || reply.data[0] > '9' ||
     reply.data[2] < '0' || reply.data[2] > '9') return EPROTO;
 uva_session.cvd = (reply.data[0] - '0') * 10 + reply.data[2] - '0';
 UVA_LOG("cvd=%u cvp-api=%u\n", uva_session.cvd, config.cvp_version);
 if (config.vsid == 0x10c02000) memcpy(name, "default volte voice", 20);
 else if (config.vsid == 0x10c01000) memcpy(name, "default modem voice", 20);
 else snprintf(name, sizeof(name), "%08X", config.vsid);
 error = uva_create(9, 0x110ff, name, sizeof(name), &uva_session.mvm);
 if (error) return error;
 error = uva_create(10, 0x11140, name, sizeof(name), &uva_session.cvs);
 if (error) return error;
 error = uva_cmd(9, uva_session.mvm, 0x11327, &dual, sizeof(dual));
 if (error) return error;
 /* Claim is set even on partial acquisition: release restores that partial path. */
 uva_session.route = true;
 error = umi_voice_route_take();
 if (error) return error;
 error = umi_voice_route_set(route, volume);
 if (error) return error;
 error = uva_create(11, uva_session.cvd >= 22 ? 0x13169 : 0x112bf, &cvp, sizeof(cvp), &uva_session.cvp);
 if (error) return error;
 error = uva_formats(route);
 if (error) return error;
 error = uva_cmd(11, uva_session.cvp, 0x100c6, NULL, 0);
 if (error) return error;
 error = uva_cmd(9, uva_session.mvm, 0x1123e, &uva_session.cvp, 2);
 if (error) return error;
 uva_session.attached = true;
 error = uva_set_volume(volume);
 if (error) return error;
 error = uva_set_mute(muted);
 if (error) return error;
 error = uva_cmd(9, uva_session.mvm, 0x11190, NULL, 0);
 /* A lost START reply must also be stopped before releasing the route. */
 uva_session.started = error == 0 || error == ETIMEDOUT || error == EPROTO;
 return error;
}
