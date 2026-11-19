/* SPDX-License-Identifier: BSD-2-Clause
 * WCD9385 EAR: static handset mixer, rx-macro.c, wcd938x.c and wcd-clsh.c.
 * RX0 -> INT0 -> HPHL PDM -> WCD RX1 -> RDAC3 -> EAR. Class AB uses
 * the vendor EAR supply sequence without a second class-H control stream.
 * TX is already up and owns codec reset, BOB and the shared fs generator.
 */
static struct block uva_rx_macro = {"voice RX macro", 0x03200000, 0x1000, 0};
static struct block uva_rx_swr = {"voice RX SoundWire", 0x03210000, 0x2000, 0};
static struct block uva_rx_pins = {"voice RX LPASS pins3..5", 0x033c3000, 0x3000, 0};
static struct block uva_rx_mux = {"voice RX clock mux", 0x03324000, 0x1000, 0};
static uint32_t uva_rx_clock = 19200000;
void umi_voice_receiver_clock(uint32_t hz) { uva_rx_clock = hz; }
static struct {
 bool saved, core, npl, master, port;
 uint8_t device, codec_saved;
 uint32_t pin[6], slew, mux;
 uint32_t macro[12];
 uint8_t codec[26];
} uva_rx;
/* Every changed register is saved/restored. Codec writes are over the RX
 * slave; it exposes the same analog register file as the existing TX slave. */
static const uint16_t uva_rx_codec_regs[] = {
 0x3408, 0x3008, 0x340d, 0x340e, 0x340f, 0x3128, 0x344e, 0x3409,
 0x3414, 0x3450, 0x300b, 0x30a4, 0x30a8, 0x30af, 0x3099, 0x3098,
 0x30a5, 0x30c7, 0x3009, 0x3465, 0x300a, 0x30ec, 0x30bf, 0x30d1,
 0x3123, 0x344f
};
static const uint16_t uva_rx_macro_regs[] = {0x100,0x104,0x108,0x180,0x184,0x400,0x404,0x408,0x40c,0x414,0x434,0x440};
static bool uva_rx_supply(void)
{
 /* wcd938x_rx_clk_enable + EAR DAC event. RDAC3 selects RX1, compander
  * off, analog gain G_6_DB; digital attenuation is applied in the macro. */
 if (!codec_bits(0x3408, 0x03, 0x03) || !codec_bits(0x3008, 0x01, 1) ||
     !codec_bits(0x340d, 0x40, 0) || !codec_bits(0x340e, 0x40, 0) ||
     !codec_bits(0x340f, 0x40, 0) || !codec_bits(0x3128, 0x10, 0x10) ||
     !codec_bits(0x3450, 1, 0) || !codec_bits(0x344e, 4, 4) ||
     !codec_bits(0x3409, 1, 1) || !codec_bits(0x3414, 2, 2) ||
     !codec_bits(0x300b, 0xfc, 0x80)) return false;
 sleep_ms(5);
 /* wcd_clsh_state_ear(CLS_AB): regulator, flyback, force-IQ, buck. */
 if (!codec_bits(0x30a4, 4, 0) || !codec_bits(0x3008, 0x0e, 0) ||
     !codec_bits(0x30a8, 0xf0, 0x70) || !codec_bits(0x30af, 0xe0, 0xa0)) return false;
 spin_us(100);
 if (!codec_bits(0x3099, 2, 2) || !codec_bits(0x3098, 0xff, 0x1c) ||
     !codec_bits(0x30a5, 0xe0, 0xe0) || !codec_bits(0x3008, 0x40, 0x40)) return false;
 spin_us(100);
 if (!codec_bits(0x30af, 0xe0, 0xe0)) return false;
 spin_us(500);
 if (!codec_bits(0x30c7, 0xff, 0xaa)) return false;
 spin_us(100);
 if (!codec_bits(0x3008, 0x88, 0x80)) return false;
 spin_us(500);
 if (!codec_bits(0x3098, 0xff, 0x3a)) return false;
 spin_us(500);
 return codec_bits(0x3009, 0x0c, 0x0c) && codec_bits(0x3465, 0x17, 0x13) &&
     codec_bits(0x300a, 0x80, 0x80);
}
static bool uva_rx_bank(unsigned bank)
{
 to_all(SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(bank), 1);
 return codec_put(0x103, 1, 0xff) &&
     codec_put(SWRS_DP_CHANNEL_ENABLE_BANK(0, bank), 1, 0xff) &&
     codec_put(SWRS_DP_SAMPLE_CONTROL_1_BANK(0, bank), 3, 0xff) &&
     codec_put(SWRS_DP_SAMPLE_CONTROL_2_BANK(0, bank), 0, 0xff) &&
     codec_put(SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), 0, 0xff) &&
     codec_put(SWRS_DP_OFFSET_CONTROL_2_BANK(0, bank), 0, 0xff) &&
     codec_put(SWRS_DP_LANE_CONTROL_BANK(0, bank), 1, 0xff) &&
     put(&uva_rx_swr, "RX HPHL port1 SI3 offset0 channel1", SWRM_DP_PORT_CTRL_BANK(1, bank), 0x01000003, 0xffffffff) &&
     put(&uva_rx_swr, "RX port1 lane1", SWRM_DP_PORT_CTRL_2_BANK(1, bank), 1, 0xff) &&
     put(&uva_rx_swr, "RX port1 word length1", 0x112c, 1, 0xff) &&
     put(&uva_rx_swr, "RX port1 HCTRL", SWRM_DP_PORT_HCTRL_BANK(1, bank), 0xf0, 0xff);
}
static int uva_receiver_open(void)
{
 /* Votes and fs-gen are held by the main-mic claim. RX has its own clock IDs. */
 struct afe_clock_set clock = {1, 0x30e, uva_rx_clock, 1, 0, 1};
 uva_rx.core = true;
 if (!afe_service_param("voice RX core clock", AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock))) return EIO;
 clock.id = 0x30f; uva_rx.npl = true;
 if (!afe_service_param("voice RX NPL clock", AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock))) return EIO;
 if (!mapped(&uva_rx_macro, 0xffc) || !mapped(&uva_rx_swr, 0x1ffc) ||
     !mapped(&uva_rx_pins, 0x2004) || !mapped(&uva_rx_mux, 0xd8)) return EIO;
 for (unsigned i = 0; i < 6; i++) uva_rx.pin[i] = rd(uva_rx_pins.base + (i / 2) * 0x1000 + (i % 2) * 4);
 uva_rx.slew = rd(lpi_slew.base); uva_rx.mux = rd(uva_rx_mux.base + 0xd8);
 for (unsigned i = 0; i < 12; i++) uva_rx.macro[i] = rd(uva_rx_macro.base + uva_rx_macro_regs[i]);
 uva_rx.saved = true;
 if (!put(&uva_rx_mux, "RX mux select1", 0xd8, 1, 1) ||
     !put_bits(&lpi_slew, "RX slew1", 0, 0x3f00, 0x1500)) return EIO;
 for (unsigned i = 0; i < 3; i++) {
  if (!put(&uva_rx_pins, "RX SWR pin function", i * 0x1000, (uva_rx.pin[i*2] & ~0x3ffu) | (i == 2 ? 8u : 4u) | (i ? 3u : 0u), 0x3ff) ||
      !put(&uva_rx_pins, "RX SWR pin input", i * 0x1000 + 4, 0, 2)) return EIO;
 }
 if (!put_bits(&uva_rx_macro, "RX MCLK", 0x100, 3, 3) ||
     !put_bits(&uva_rx_macro, "RX FS", 0x104, 3, 1) ||
     !put_bits(&uva_rx_macro, "RX SWR reset", 0x108, 2, 2) ||
     !put_bits(&uva_rx_macro, "RX SWR clock", 0x108, 1, 1) ||
     !put_bits(&uva_rx_macro, "RX SWR reset off", 0x108, 2, 0)) return EIO;
 bool tx_master = master_taken;
 master_taken = false;
 bool ok = master_on(); uva_rx.master = master_taken; master_taken = tx_master;
 if (!ok) return EIO;
 for (unsigned ms = 0; ms < 100; ms++) {
  for (unsigned d = 1; d <= 11; d++) {
   uint64_t id = ((uint64_t)swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32) | swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));
   if ((id & 0xffffffffffffull) == 0x000d01170224ull && ((swr_rd(SWRM_MCP_SLV_STATUS) >> (d*2)) & 3) == 1) uva_rx.device = (uint8_t)d;
  }
  if (uva_rx.device) break;
  sleep_ms(1);
 }
 if (!uva_rx.device) return ENXIO;
 codec_device = uva_rx.device;
 for (unsigned i = 0; i < 26; i++) {
  if (!swr_read(codec_device, uva_rx_codec_regs[i], &uva_rx.codec[i])) return EIO;
  uva_rx.codec_saved++;
 }
 if (!put_bits(&uva_rx_macro, "RX0 input", 0x180, 0xff, 5) ||
     !put_bits(&uva_rx_macro, "RX0 other inputs zero", 0x184, 0xff, 0) ||
     !put_bits(&uva_rx_macro, "RX0 48k clock, mute", 0x400, 0x3f, 0x34) ||
     !put_bits(&uva_rx_macro, "RX0 compander off", 0x404, 0x42, 0) ||
     !put_bits(&uva_rx_macro, "RX0 ear, DSM", 0x408, 3, 2) ||
     !put_bits(&uva_rx_macro, "RX0 rate config", 0x40c, 3, 3) ||
     !put_bits(&uva_rx_macro, "RX0 SEC7", 0x434, 7, 2) ||
     !put_bits(&uva_rx_macro, "RX0 DSM clock", 0x440, 1, 1)) return EIO;
 unsigned bank = (swr_rd(SWRM_MCP_STATUS) & 1) ^ 1;
 if (!uva_rx_bank(bank)) return EIO;
 uint32_t frame = (swr_rd(SWRM_MCP_FRAME_CTRL_BANK(bank)) & ~0xff07ffu) | 1u << 3 | 7u | 5u << 16;
 if (!put(&uva_rx_swr, "RX 50x16 9.6MHz", SWRM_MCP_FRAME_CTRL_BANK(bank), frame, 0xff07ff)) return EIO;
 broadcast(SWRS_SCP_FRAME_CTRL_BANK(bank), 1u << 3 | 7u);
 if ((swr_rd(SWRM_MCP_STATUS) & 1) != bank || !uva_rx_bank(bank ^ 1)) return EIO;
 if (!uva_rx_supply()) return EIO;
 sleep_ms(6);
 if (!codec_bits(0x3008, 2, 2)) return EIO; /* CLASS_AB after PA settle */
 struct { uint32_t version, rate; uint16_t bits, format, channels, mask; } __attribute__((packed)) config = {1,48000,16,0,1,1};
 struct { uint16_t port, size; uint32_t low, high, handle; struct afe_param p; uint8_t data[16]; } __attribute__((packed)) packet = {
  0xb030, (uint16_t)(sizeof(struct afe_param) + sizeof(config)), 0,0,0,
  {AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_CODEC_DMA_CONFIG, sizeof(config), 0}, {0} };
 memcpy(packet.data, &config, sizeof(config));
 if (!command("voice RX AFE codec DMA mono48k", APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, &packet, sizeof(packet), 0, NULL)) return EIO;
 uint16_t start[2] = {0xb030, 0}; uva_rx.port = true;
 if (!command("voice RX AFE start", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL)) return EIO;
 return 0;
}

int umi_voice_receiver(int on, uint32_t percent)
{
 lck_mtx_lock(&mic_lock);
 struct block *tx_bus = swr_bus; uint8_t tx_device = codec_device;
 swr_bus = &uva_rx_swr; codec_device = uva_rx.device;
 int error = 0;
 if (on) {
  if (!uva_rx.saved) error = uva_receiver_open();
  if (!error && !put(&uva_rx_macro, "EAR digital attenuation", 0x414, (uint8_t)(-(int)(6 + (100-percent)*42/100)), 0xff)) error = EIO;
  if (!error && !put_bits(&uva_rx_macro, "EAR mute", 0x400, 0x10, on == 2 ? 0x10 : 0)) error = EIO;
 } else {
  broken = false;
  if (uva_rx.port) {
   uint16_t stop[2] = {0xb030, 0};
   if (!command("voice RX AFE stop", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL)) error = EIO;
   else uva_rx.port = false;
  }
  if (!error && uva_rx.saved) {
   if (uva_rx.device) {
    codec_device = uva_rx.device;
    if (!codec_bits(0x300a, 0x80, 0)) error = EIO;
    sleep_ms(7);
    for (int i = (int)uva_rx.codec_saved - 1; i >= 0 && !error; i--) if (!codec_put(uva_rx_codec_regs[i], uva_rx.codec[i], 0xff)) error = EIO;
   }
   if (!error && uva_rx.master) {
    bool tx_master = master_taken; master_taken = true; master_off(); master_taken = tx_master; uva_rx.master = false;
   }
   if (!error) {
    for (int i = 11; i >= 0; i--) restore(&uva_rx_macro, uva_rx_macro_regs[i], uva_rx.macro[i], 0);
    restore(&uva_rx_mux, 0xd8, uva_rx.mux, 0);
    for (unsigned i = 0; i < 6; i++) restore(&uva_rx_pins, (i/2)*0x1000+(i%2)*4, uva_rx.pin[i], 0);
    put_bits(&lpi_slew, "RX slew restore", 0, 0x3f00, uva_rx.slew & 0x3f00);
    uva_rx.saved = false; uva_rx.device = 0; uva_rx.codec_saved = 0;
   }
  }
  struct afe_clock_set clock = {1,0x30f,uva_rx_clock,1,0,0};
  if (!error && uva_rx.npl) {
   if (!afe_service_param("voice RX NPL off", AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock))) error = EIO;
   else uva_rx.npl = false;
  }
  clock.id = 0x30e;
  if (!error && uva_rx.core) {
   if (!afe_service_param("voice RX core off", AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock))) error = EIO;
   else uva_rx.core = false;
  }
 }
 swr_bus = tx_bus; codec_device = tx_device;
 say("voice-audio: receiver on=%d clock=%u rc=%d\n", on, uva_rx_clock, error);
 lck_mtx_unlock(&mic_lock);
 return error;
}
