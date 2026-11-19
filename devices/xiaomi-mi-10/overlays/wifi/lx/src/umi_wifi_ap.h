/* Firmware-limited WPA2-PSK/CCMP clients, HT20/VHT20, firmware AP power save. Included by lx_80211.c.
 * Management WMI operations run on their own queue: never on the RX/poll thread
 * that must receive the firmware's completion events. No STA+AP concurrency. */
#include "core.h"
#include "mac.h"
#include "peer.h"

/* Per AP session, values only; submit is not an over-air completion. */
static void
ap_stats(struct lx_radio *radio)
{
	uint64_t c[AP_COUNT];
	struct ath11k *ar = radio->hw.priv;
	struct ath11k_soc_dp_stats *s = &ar->ab->soc_stats;
	unsigned reo = 0, rxdma = 0, hal = 0, desc = 0;
	for (unsigned i = 0; i < nitems(s->reo_error); i++) reo += READ_ONCE(s->reo_error[i]);
	for (unsigned i = 0; i < nitems(s->rxdma_error); i++) rxdma += READ_ONCE(s->rxdma_error[i]);
	for (unsigned i = 0; i < nitems(s->hal_reo_error); i++) hal += READ_ONCE(s->hal_reo_error[i]);
	for (unsigned i = 0; i < nitems(s->tx_err.desc_na); i++) desc += READ_ONCE(s->tx_err.desc_na[i]);
	lx_os_ap_trace("ap_stats:wait", 20, 0); lx_os_lock(); lx_os_ap_trace("ap_stats:held", 20, 0); memcpy(c, radio->ap_count, sizeof(c)); lx_os_ap_trace("ap_stats:release", 20, 0); lx_os_unlock(); lx_os_ap_trace("ap_stats:released", 20, 0);
	lx_tell("hotspot-ap: rx total=%llu ethernet=%llu raw=%llu management=%llu monitor=%llu fcs=%llu ap1=%llu\n",
	    c[AP_RX], c[AP_RX_8023], c[AP_RX_RAW], c[AP_RX_MGMT], c[AP_RX_MONITOR], c[AP_RX_BAD], c[AP_RX_EN3]);
	lx_tell("hotspot-ap: rx-drop format=%llu ds=%llu null=%llu inactive=%llu source=%llu eapol=%llu\n",
	    c[AP_RX_FORMAT], c[AP_RX_DS], c[AP_RX_NULL], c[AP_RX_INACTIVE], c[AP_RX_SOURCE], c[AP_RX_EAPOL]);
	lx_tell("hotspot-ap: tx total=%llu group=%llu submit=%llu inactive=%llu destination=%llu alloc=%llu complete=%llu ack=%llu noack=%llu released=%llu\n",
	    c[AP_TX], c[AP_TX_GROUP], c[AP_TX_SUBMIT], c[AP_TX_INACTIVE], c[AP_TX_DEST], c[AP_TX_ALLOC],
	    c[AP_TX_COMPLETE], c[AP_TX_ACK], c[AP_TX_NOACK], c[AP_TX_FREE]);
	/* These existing hardware-ring counters are cumulative since driver start;
	 * sample at AP entry too, so a prior STA error is not blamed on the AP. */
	lx_tell("hotspot-ap: driver-total err-ring=%u invalid-rbm=%u reo=%u rxdma=%u hal-reo=%u tx-desc=%u tx-misc=%d\n",
	    READ_ONCE(s->err_ring_pkts), READ_ONCE(s->invalid_rbm), reo, rxdma, hal, desc, atomic_read(&s->tx_err.misc_fail));
}

static void
ap_stats_work(struct work_struct *work)
{
 lx_os_ap_trace("ap_stats_work:enter", 0, 0);
	struct lx_radio *radio = container_of(work, struct lx_radio, ap_stats_work.work);
	ap_stats(radio);
	lx_os_ap_trace("ap_stats_work:wait", 39, 0); lx_os_lock(); lx_os_ap_trace("ap_stats_work:held", 39, 0); bool active = radio->ap_active; lx_os_ap_trace("ap_stats_work:release", 39, 0); lx_os_unlock(); lx_os_ap_trace("ap_stats_work:released", 39, 0);
	lx_os_ap_trace("ap_stats_work:exit", 0, active);
	if (active) queue_delayed_work(radio->ap_queue, &radio->ap_stats_work, msecs_to_jiffies(5000));
}

/* Called with ap_mutex, after the driver's real AUTHORIZED transition. */
static void
ap_peer_stats(struct lx_radio *radio, struct lx_ap_peer *p)
{
	struct ath11k *ar = radio->hw.priv;
	struct ath11k_vif *vif = (void *)radio->vif->drv_priv;
	unsigned found = 0, authorized = 0, setup = 0, tids = 0;
	int id = -1;
	spin_lock_bh(&ar->ab->base_lock);
	struct ath11k_peer *peer = ath11k_peer_find(ar->ab, vif->vdev_id, p->address);
	if (peer) {
		found = 1; id = peer->peer_id; authorized = peer->is_authorized; setup = peer->dp_setup_done;
		for (unsigned i = 0; i <= IEEE80211_NUM_TIDS; i++)
			if (peer->rx_tid[i].active) tids |= 1u << i;
	}
	spin_unlock_bh(&ar->ab->base_lock);
	struct ath11k_sta *sta = (void *)p->sta->drv_priv;
	lx_tell("hotspot-ap: peer found=%u id=%d authorized=%u dp-setup=%u tid-mask=%#x fouraddr=%u\n",
	    found, id, authorized, setup, tids, sta->use_4addr_set);
 lx_os_ap_trace("ap_peer_stats:wait", 62, 0); spin_lock_bh(&ar->data_lock); lx_os_ap_trace("ap_peer_stats:held", 62, 0);
 unsigned ps = sta->peer_ps_state;
 lx_os_ap_trace("ap_peer_stats:release", 64, 0); spin_unlock_bh(&ar->data_lock); lx_os_ap_trace("ap_peer_stats:released", 64, 0);
 lx_tell("hotspot-ap: PS slot=%u state=%u aid=%u listen=%u uapsd=%#x\n",
     (unsigned)(p - radio->ap_peers), ps, p->sta->aid, radio->hw.conf.listen_interval, p->sta->uapsd_queues);
}

/* China non-DFS operating classes, still intersected with firmware regulatory. */
static bool
ap_channel_number(unsigned c)
{
 return (c >= 1 && c <= 13) || (c >= 36 && c <= 48 && !(c & 3)) ||
     (c >= 149 && c <= 165 && (c - 149) % 4 == 0);
}

static const uint8_t ap_wmm[] = {
 221, 24, 0, 0x50, 0xf2, 2, 1, 1, 0x80, 0, /* Parameter, v1, uAPSD */
 0x03, 0xa4, 0, 0, 0x27, 0xa4, 0, 0,
 0x42, 0x43, 94, 0, 0x62, 0x32, 47, 0
};

static unsigned
ap_rate_elements(struct lx_radio *radio, uint8_t *out)
{
 struct ieee80211_supported_band *b = radio->wiphy->bands[radio->channel->def.chan->band];
 uint8_t *p = out;
 *p++ = 1; *p++ = b->n_bitrates < 8 ? b->n_bitrates : 8;
 for (int i = 0; i < b->n_bitrates; i++) {
  if (i == 8) { *p++ = 50; *p++ = b->n_bitrates - 8; }
  *p++ = b->bitrates[i].bitrate / 5 | (radio->vif->bss_conf.basic_rates & (1u << i) ? 0x80 : 0);
 }
 return (unsigned)(p - out);
}

static unsigned
ap_phy_elements(struct lx_radio *radio, uint8_t *out)
{
 struct ieee80211_supported_band *b = radio->wiphy->bands[radio->channel->def.chan->band];
 uint8_t *p = out;
 if (b->ht_cap.ht_supported) {
  uint16_t cap = b->ht_cap.cap & ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40);
  *p++ = 45; *p++ = 26; bzero(p, 26);
  p[0] = cap; p[1] = cap >> 8;
  p[2] = b->ht_cap.ampdu_factor | b->ht_cap.ampdu_density << 2;
  memcpy(p + 3, &b->ht_cap.mcs, sizeof(b->ht_cap.mcs)); p += 26;
  *p++ = 61; *p++ = 22; bzero(p, 22);
  p[0] = channel_of(radio->channel->def.chan->center_freq);
  p[2] = 3; /* mixed mode protection, including legacy/HT peers */
  p[6] = 0xff; p += 22; /* basic MCS 0..7 */
 }
 if (b->band == NL80211_BAND_5GHZ && b->vht_cap.vht_supported) {
  uint32_t cap = b->vht_cap.cap & ~(IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK |
      IEEE80211_VHT_CAP_SHORT_GI_80 | IEEE80211_VHT_CAP_SHORT_GI_160);
  *p++ = 191; *p++ = 12;
  p[0] = cap; p[1] = cap >> 8; p[2] = cap >> 16; p[3] = cap >> 24;
  memcpy(p + 4, &b->vht_cap.vht_mcs, sizeof(b->vht_cap.vht_mcs)); p += 12;
  /* Width 0: use HT operation (20 MHz), not an 80 MHz center segment. */
  memcpy(p, "\xc0\x05\x00\x00\x00\xfc\xff", 7); p += 7;
 }
 memcpy(p, ap_wmm, sizeof(ap_wmm)); p += sizeof(ap_wmm);
 if (!radio->ap_uapsd) p[-(int)sizeof(ap_wmm) + 8] = 0;
 return (unsigned)(p - out);
}

static unsigned
ap_elements(struct lx_radio *radio, uint8_t *out, bool beacon,
    struct ieee80211_mutable_offsets *offsets)
{
 uint8_t *p = out;
 unsigned ssid_len = beacon && radio->ap_hidden ? 0 : radio->vif->cfg.ssid_len;
 *p++ = 0; *p++ = (uint8_t)ssid_len;
 memcpy(p, radio->vif->cfg.ssid, ssid_len); p += ssid_len;
 p += ap_rate_elements(radio, p);
 if (radio->channel->def.chan->band == NL80211_BAND_2GHZ) {
  *p++ = 3; *p++ = 1; *p++ = channel_of(radio->channel->def.chan->center_freq);
 }
 if (beacon) {
  if (offsets) offsets->tim_offset = (uint16_t)(36 + p - out);
  /* Reserve the full live AID bitmap; firmware patches TIM/DTIM. */
  unsigned bitmap = radio->ap_capacity / 8 + 1;
  *p++ = 5; *p++ = 3 + bitmap; *p++ = 0; *p++ = 2; *p++ = 0;
  bzero(p, bitmap); p += bitmap;
 }
 if (radio->ap_secure) { memcpy(p, lx_wpa_ap_rsn, sizeof(lx_wpa_ap_rsn)); p += sizeof(lx_wpa_ap_rsn); }
 if (radio->channel->def.chan->band == NL80211_BAND_2GHZ) {
  memcpy(p, "\x2a\x01\x02", 3); p += 3; /* protect mixed ERP/non-ERP */
 }
 p += ap_phy_elements(radio, p);
 return (unsigned)(p - out);
}

static void
ap_header(struct lx_radio *radio, uint8_t *f, unsigned kind, const uint8_t *to)
{
	bzero(f, 24);
	f[0] = (uint8_t)kind;
	memcpy(f + 4, to, ETH_ALEN);
	memcpy(f + 10, radio->vif->addr, ETH_ALEN);
	memcpy(f + 16, radio->vif->addr, ETH_ALEN);
	unsigned sequence = (radio->ap_sequence++ & 4095) << 4;
	f[22] = (uint8_t)sequence; f[23] = (uint8_t)(sequence >> 8);
}

struct sk_buff *
hakios_ieee80211_beacon_get_template(struct ieee80211_hw *hw,
    struct ieee80211_vif *vif, struct ieee80211_mutable_offsets *offsets, uint32_t link_id)
{
	struct lx_radio *radio = radio_of(hw);
	struct sk_buff *skb;
	uint8_t *f;
	unsigned length;
	if (!radio->ap || vif != radio->vif || !radio->channel || link_id) return NULL;
	skb = frame_new(radio, 384, &f);
	if (!skb) return NULL;
	bzero(f, 384);
	static const uint8_t broadcast[ETH_ALEN] = { 255, 255, 255, 255, 255, 255 };
	ap_header(radio, f, 0x80, broadcast);
	f[32] = 100; f[34] = radio->ap_secure ? 0x11 : 1; f[35] = 4; /* ESS + privacy */
	if (offsets) bzero(offsets, sizeof(*offsets));
	length = 36 + ap_elements(radio, f + 36, true, offsets);
	skb_trim(skb, length);
	return skb;
}

static void ap_enqueue(struct lx_radio *, const uint8_t *, unsigned, bool, int);

/* RX only queues EAPOL; WMI key/peer operations must run off the RX thread. */
static void
ap_input(struct lx_radio *radio, struct sk_buff *skb)
{
	unsigned drop;
	bool eapol = skb->len >= 14 && skb->data[12] == 0x88 && skb->data[13] == 0x8e;
	lx_os_ap_trace("ap_input:wait", 194, 0); lx_os_lock(); lx_os_ap_trace("ap_input:held", 194, 0);
 struct lx_ap_peer *p = skb->len >= 14 ? ap_peer_find(radio, skb->data + 6) : NULL;
 drop = skb->len < 14 ? AP_RX_FORMAT : !radio->ap_active ? AP_RX_INACTIVE :
     !p ? AP_RX_SOURCE : !p->associated ? AP_RX_INACTIVE :
     eapol ? AP_RX_EAPOL : !p->authorized ? AP_RX_INACTIVE :
     radio->ap_secure && !(IEEE80211_SKB_RXCB(skb)->flag & RX_FLAG_DECRYPTED) ? AP_RX_FORMAT : AP_RX_EN3;
	radio->ap_count[drop]++;
	lx_os_ap_trace("ap_input:release", 201, 0); lx_os_unlock(); lx_os_ap_trace("ap_input:released", 201, 0);
	if (drop == AP_RX_EN3) lx_os_ap_input(skb->data, skb->len);
	else if (drop == AP_RX_EAPOL) ap_enqueue(radio, skb->data, skb->len, true, 0);
}

static void
ap_enqueue(struct lx_radio *radio, const uint8_t *f, unsigned length, bool eapol, int signal)
{
	if (length > sizeof(radio->ap_rx[0].frame)) return;
	lx_os_ap_trace("ap_enqueue:wait", 210, 0); lx_os_lock(); lx_os_ap_trace("ap_enqueue:held", 210, 0);
	if (radio->ap_active && radio->ap_in - radio->ap_out < nitems(radio->ap_rx)) {
		unsigned at = radio->ap_in++ % nitems(radio->ap_rx);
		memcpy(radio->ap_rx[at].frame, f, length);
		radio->ap_rx[at].length = length;
		radio->ap_rx[at].eapol = eapol;
		radio->ap_rx[at].signal = signal;
		radio->ap_queuing++;
		/* queue_work takes lx_os_lock itself, so queue after releasing it. */
		lx_os_ap_trace("ap_enqueue:release", 219, 0); lx_os_unlock(); lx_os_ap_trace("ap_enqueue:released", 219, 0);
		queue_work(radio->ap_queue, &radio->ap_work);
		lx_os_ap_trace("ap_enqueue:wait", 221, 0); lx_os_lock(); lx_os_ap_trace("ap_enqueue:held", 221, 0);
		radio->ap_queuing--;
		lx_os_wakeup(&radio->ap_queuing);
		lx_os_ap_trace("ap_enqueue:release", 224, 0); lx_os_unlock(); lx_os_ap_trace("ap_enqueue:released", 224, 0);
		return;
	}
	lx_os_ap_trace("ap_enqueue:release", 227, 0); lx_os_unlock(); lx_os_ap_trace("ap_enqueue:released", 227, 0);
}

static void
ap_received(struct lx_radio *radio, const uint8_t *f, unsigned length, int signal)
{
	ap_enqueue(radio, f, length, false, signal);
}

static void
ap_peer_drop(struct lx_radio *radio, struct lx_ap_peer *p, unsigned reason)
{
 lx_os_ap_trace("ap_peer_drop:enter", 0, 0);
 if (!p->sta) return;
 lx_os_ap_trace("ap_peer_drop:wait", 240, 0); lx_os_lock(); lx_os_ap_trace("ap_peer_drop:held", 240, 0); p->authorized = p->associated = false; lx_os_ap_trace("ap_peer_drop:release", 240, 0); lx_os_unlock(); lx_os_ap_trace("ap_peer_drop:released", 240, 0);
 key_drop_peer(radio, p->sta, &p->key, true);
 lx_os_ap_peer(0, 0, p->address);
 for (int state = p->reached; state > IEEE80211_STA_NOTEXIST; state--) {
  int ret = station_state_peer(radio, p->sta, state, state - 1);
  if (ret) lx_tell("hotspot-ap: peer remove failed state=%d error=%d\n", state, -ret);
 }
 lx_tell("hotspot-ap: disassoc %02x:%02x:**:**:**:%02x reason=%u slot=%u\n",
     p->address[0], p->address[1], p->address[5], reason, (unsigned)(p - radio->ap_peers));
 void *memory = p->memory;
 lx_os_ap_trace("ap_peer_drop:wait", 250, 0); lx_os_lock(); lx_os_ap_trace("ap_peer_drop:held", 250, 0); rcu_assign_pointer(p->sta, NULL); lx_os_ap_trace("ap_peer_drop:release", 250, 0); lx_os_unlock(); lx_os_ap_trace("ap_peer_drop:released", 250, 0);
 synchronize_rcu();
 kfree(memory);
 lx_os_ap_trace("ap_peer_drop:wait", 253, 0); lx_os_lock(); lx_os_ap_trace("ap_peer_drop:held", 253, 0); explicit_bzero(p, sizeof(*p)); lx_os_ap_trace("ap_peer_drop:release", 253, 0); lx_os_unlock(); lx_os_ap_trace("ap_peer_drop:released", 253, 0);
}

static int
ap_peer_auth(struct lx_radio *radio, struct lx_ap_peer *p, const uint8_t *address)
{
 if (radio->ap_rekey_pending) return EBUSY;
 if (p->sta) ap_peer_drop(radio, p, 2);
 void *memory = kzalloc(64 + sizeof(struct ieee80211_sta) + radio->hw.sta_data_size, GFP_KERNEL);
 if (!memory) return ENOMEM;
 struct ieee80211_sta *sta = (void *)roundup2((uintptr_t)memory, 64);
 memcpy(sta->addr, address, ETH_ALEN); memcpy(sta->deflink.addr, address, ETH_ALEN);
 sta->deflink.sta = sta; sta->link[0] = &sta->deflink;
 p->memory = memory; p->reached = IEEE80211_STA_NOTEXIST;
 p->signal = -128; p->created_at = lx_os_nanoseconds();
 lx_os_ap_trace("ap_peer_auth:wait", 268, 0); lx_os_lock(); lx_os_ap_trace("ap_peer_auth:held", 268, 0); memcpy(p->address, address, ETH_ALEN); rcu_assign_pointer(p->sta, sta); lx_os_ap_trace("ap_peer_auth:release", 268, 0); lx_os_unlock(); lx_os_ap_trace("ap_peer_auth:released", 268, 0);
 for (int state = IEEE80211_STA_NOTEXIST; state < IEEE80211_STA_AUTH; state++) {
  int ret = station_state_peer(radio, sta, state, state + 1);
  if (ret) { ap_peer_drop(radio, p, 1); return -ret; }
  p->reached = state + 1;
 }
 return 0;
}

static void
ap_reply(struct lx_radio *radio, const uint8_t *to, unsigned kind, unsigned status)
{
	struct sk_buff *skb;
	uint8_t *f;
	skb = frame_new(radio, 384, &f);
	if (!skb) return;
	bzero(f, 384);
	ap_header(radio, f, kind, to);
	if (kind == 0xb0) {
		f[26] = 2; f[28] = (uint8_t)status; f[29] = (uint8_t)(status >> 8);
		skb_trim(skb, 30);
	} else {
		f[24] = radio->ap_secure ? 0x11 : 1;
		f[26] = (uint8_t)status; f[27] = (uint8_t)(status >> 8);
		struct lx_ap_peer *p = ap_peer_find(radio, to);
  f[25] = 4; /* short slot, mixed-mode protection via ERP/HT */
  if (!status) { f[28] = p->sta->aid; f[29] = 0xc0; }
  unsigned n = ap_rate_elements(radio, f + 30);
  n += ap_phy_elements(radio, f + 30 + n);
  skb_trim(skb, 30 + n);
	}
	frame_send(radio, skb);
}

static void
ap_block_ack(struct lx_radio *radio, struct lx_ap_peer *p, const uint8_t *f, unsigned length)
{
 if (!p->associated || !p->sta->deflink.ht_cap.ht_supported || length < 28 || f[24] != 3) return;
 bool start = f[25] == 0;
 if (!(start || f[25] == 2) || (start && length < 33) || (!start && length < 30)) return;
 unsigned v = start ? f[27] | f[28] << 8 : f[26] | f[27] << 8;
 unsigned tid = start ? v >> 2 & 15 : v >> 12;
 if (tid >= 8 || (!start && !(v & (1u << 11)))) return;
 struct ieee80211_ampdu_params params = { .sta = p->sta, .tid = tid,
     .ssn = start ? (f[31] | f[32] << 8) >> 4 : 0,
     .timeout = start ? f[29] | f[30] << 8 : 0,
     .buf_size = start ? v >> 6 : 0, .amsdu = false };
 if (!params.buf_size || params.buf_size > radio->hw.max_rx_aggregation_subframes)
  params.buf_size = radio->hw.max_rx_aggregation_subframes;
 int ret = start && !(v & 2) ? -EINVAL : 0;
 lx_os_ap_trace("ap_block_ack:wait", 318, 0); mutex_lock(&radio->wiphy->mtx); lx_os_ap_trace("ap_block_ack:held", 318, 0);
 if (!ret && (p->ba_mask & (1u << tid))) {
  params.action = IEEE80211_AMPDU_RX_STOP;
  ret = radio->ops->ampdu_action(&radio->hw, radio->vif, &params);
  p->ba_mask &= ~(1u << tid);
 }
 if (!ret && start) {
  params.action = IEEE80211_AMPDU_RX_START;
  ret = radio->ops->ampdu_action(&radio->hw, radio->vif, &params);
  if (!ret) p->ba_mask |= 1u << tid;
 }
 lx_os_ap_trace("ap_block_ack:release", 329, 0); mutex_unlock(&radio->wiphy->mtx); lx_os_ap_trace("ap_block_ack:released", 329, 0);
 if (!start) return;
 uint8_t *out;
 struct sk_buff *skb = frame_new(radio, 33, &out);
 if (!skb) return;
 ap_header(radio, out, 0xd0, p->address);
 out[24] = 3; out[25] = 1; out[26] = f[26];
 out[27] = ret || !(v & 2) ? 37 : 0; out[28] = 0;
 unsigned answer = 2 | tid << 2 | params.buf_size << 6;
 out[29] = answer; out[30] = answer >> 8;
 out[31] = params.timeout; out[32] = params.timeout >> 8;
 frame_send(radio, skb);
}

static void
ap_manage(struct lx_radio *radio, const uint8_t *f, unsigned length)
{
	const uint8_t *ssid;
 struct lx_ap_peer *p = ap_peer_find(radio, f + 10);
	unsigned kind = f[0] & 0xfc;
	bool own = !memcmp(f + 4, radio->vif->addr, ETH_ALEN) &&
	    !memcmp(f + 16, radio->vif->addr, ETH_ALEN);
	/* No management fragmentation or PMF. */
	if ((f[0] & 3) || (f[1] & 0xc7) || (f[22] & 15)) return;
	if (is_multicast_ether_addr(f + 10) || is_zero_ether_addr(f + 10)) return;
	unsigned fixed = kind == 0x40 ? 24 : kind == 0 ? 28 : kind == 0x20 ? 34 : 0;
	if (fixed) {
		if (length < fixed) return;
		/* element() finds individual IEs; also reject truncated trailing IEs. */
		for (unsigned at = fixed; at < length;) {
			if (length - at < 2 || f[at + 1] > length - at - 2) return;
			at += 2 + f[at + 1];
		}
	}
	if (kind == 0x40) { /* wildcard/directed probe: firmware has the periodic beacon */
		if (memcmp(f + 4, radio->vif->addr, ETH_ALEN) && !is_broadcast_ether_addr(f + 4)) return;
		ssid = element(f + 24, length - 24, 0);
		if (!ssid || (radio->ap_hidden && !ssid[1]) || (ssid[1] && (ssid[1] != radio->vif->cfg.ssid_len ||
		    memcmp(ssid + 2, radio->vif->cfg.ssid, ssid[1])))) return;
		uint8_t *out;
		struct sk_buff *skb = frame_new(radio, 384, &out);
		if (!skb) return;
		bzero(out, 384);
		ap_header(radio, out, 0x50, f + 10);
		out[32] = 100; out[34] = radio->ap_secure ? 0x11 : 1; out[35] = 4;
		skb_trim(skb, 36 + ap_elements(radio, out + 36, false, NULL));
		frame_send(radio, skb);
		return;
	}
	if (!own) return;
 if (kind == 0xd0 && p) { ap_block_ack(radio, p, f, length); return; }
	if (kind == 0xb0 && length >= 30) {
		unsigned status = 0;
		if (f[24] || f[25]) status = 13; /* unsupported auth algorithm */
		else if (f[26] != 1 || f[27] || f[28] || f[29]) return;
		else if ((f[1] & 8) && p && p->sta &&
		    !memcmp(p->sta->addr, f + 10, ETH_ALEN) &&
		    p->auth_sequence == (unsigned)(f[22] | f[23] << 8)) {
			/* Lost auth response: repeat it without deleting an associated peer. */
		}
		else {
			if (!p) for (unsigned i = 0; i < radio->ap_capacity; i++)
    if (!radio->ap_peers[i].sta) { p = &radio->ap_peers[i]; break; }
   int error = p ? ap_peer_auth(radio, p, f + 10) : ENOSPC;
			if (!error) p->auth_sequence = (uint16_t)(f[22] | f[23] << 8);
			if (error) {
				status = 17;
				lx_tell("hotspot-ap: auth failed %02x:%02x:**:**:**:%02x error=%d\n",
				    f[10], f[11], f[15], error);
			}
		}
		ap_reply(radio, f + 10, 0xb0, status);
	} else if ((kind == 0 || kind == 0x20) && length >= (kind == 0 ? 28u : 34u)) {
		unsigned fixed = kind == 0 ? 28 : 34, status = 0;
		bool was_joined = p && p->associated;
		const uint8_t *rsn = NULL;
		const uint8_t *ies = f + fixed;
		unsigned ies_length = length - fixed;
		ssid = element(ies, ies_length, 0);
		if (!p || !p->sta || memcmp(p->sta->addr, f + 10, ETH_ALEN) ||
		    p->reached < IEEE80211_STA_AUTH) status = 9;
		else if (!ssid || ssid[1] != radio->vif->cfg.ssid_len ||
		    memcmp(ssid + 2, radio->vif->cfg.ssid, ssid[1]) ||
		    (radio->ap_secure ? (!(rsn = element(ies, ies_length, 48)) ||
		    !lx_wpa_auth_rsn(rsn, rsn[1] + 2u) || !(f[24] & 0x10)) :
		    (element(ies, ies_length, 48) != NULL || (f[24] & 0x10)))) status = 12;
		else if (p->associated &&
		    (!(f[1] & 8) || p->assoc_sequence != (unsigned)(f[22] | f[23] << 8))) {
			/* New association => new peer/nonce/keys. Retried association keeps
			 * the current handshake and its counters unchanged. */
			int error = ap_peer_auth(radio, p, f + 10);
			if (error) status = 17;
			was_joined = false;
		}
		if (!status && p->reached == IEEE80211_STA_AUTH) {
   struct ieee80211_supported_band *band = radio->wiphy->bands[radio->channel->def.chan->band];
   const uint8_t *e;
   uint32_t basic = 0, rates = 0;
   if ((e = element(ies, ies_length, 1))) rates |= rates_of(band, e + 2, e[1], &basic);
   if ((e = element(ies, ies_length, 50))) rates |= rates_of(band, e + 2, e[1], &basic);
   if ((rates & radio->vif->bss_conf.basic_rates) != radio->vif->bss_conf.basic_rates) status = 18;
   else {
    struct ieee80211_sta *sta = p->sta;
    sta->aid = (uint16_t)(p - radio->ap_peers + 1);
    radio->vif->bss_conf.assoc_capability = (uint16_t)(f[24] | f[25] << 8);
    radio->hw.conf.listen_interval = (uint16_t)(f[26] | f[27] << 8);
    sta->deflink.supp_rates[band->band] = rates;
    sta->deflink.bandwidth = IEEE80211_STA_RX_BW_20;
    sta->deflink.rx_nss = 1; sta->deflink.smps_mode = IEEE80211_SMPS_OFF;
    e = vendor_element(ies, ies_length, wmm);
    sta->wme = e && e[1] >= 7 && e[6] == 0 && e[7] == 1;
    if (sta->wme && radio->ap_uapsd) {
     sta->uapsd_queues = e[8] & 15; sta->max_sp = e[8] >> 5 & 3;
    }
    if (sta->wme && (e = element(ies, ies_length, 45)) && e[1] == 26 && band->ht_cap.ht_supported) {
     struct ieee80211_sta_ht_cap *ht = &sta->deflink.ht_cap;
     ht->ht_supported = true;
     ht->cap = (uint16_t)(e[2] | e[3] << 8) & ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40);
     ht->ampdu_factor = e[4] & 3; ht->ampdu_density = e[4] >> 2 & 7;
     memcpy(&ht->mcs, e + 5, sizeof(ht->mcs));
     for (unsigned n = 0; n < sizeof(ht->mcs.rx_mask); n++) ht->mcs.rx_mask[n] &= band->ht_cap.mcs.rx_mask[n];
     sta->deflink.rx_nss = ht->mcs.rx_mask[1] ? 2 : 1;
    }
    if (sta->deflink.ht_cap.ht_supported && band->band == NL80211_BAND_5GHZ &&
        (e = element(ies, ies_length, 191)) && e[1] == 12 && band->vht_cap.vht_supported) {
     struct ieee80211_sta_vht_cap *vht = &sta->deflink.vht_cap;
     vht->vht_supported = true;
     vht->cap = e[2] | e[3] << 8 | e[4] << 16 | (uint32_t)e[5] << 24;
     memcpy(&vht->vht_mcs, e + 6, sizeof(vht->vht_mcs));
     uint16_t rx = le16_to_cpu(vht->vht_mcs.rx_mcs_map), tx = le16_to_cpu(vht->vht_mcs.tx_mcs_map);
     uint16_t local_rx = le16_to_cpu(band->vht_cap.vht_mcs.rx_mcs_map);
     uint16_t local_tx = le16_to_cpu(band->vht_cap.vht_mcs.tx_mcs_map);
     for (unsigned n = 0; n < 8; n++) {
      unsigned r = rx >> (2*n) & 3, t = tx >> (2*n) & 3;
      unsigned lr = local_tx >> (2*n) & 3, lt = local_rx >> (2*n) & 3;
      r = r == 3 || lr == 3 ? 3 : min(r, lr);
      t = t == 3 || lt == 3 ? 3 : min(t, lt);
      rx = (rx & ~(3u << (2*n))) | r << (2*n);
      tx = (tx & ~(3u << (2*n))) | t << (2*n);
     }
     vht->vht_mcs.rx_mcs_map = cpu_to_le16(rx);
     vht->vht_mcs.tx_mcs_map = cpu_to_le16(tx);
    }
				for (int state = IEEE80211_STA_AUTH; state < (radio->ap_secure ? IEEE80211_STA_ASSOC : IEEE80211_STA_AUTHORIZED); state++) {
					int ret = station_state_peer(radio, p->sta, state, state + 1);
					if (ret) { status = 17; break; }
					p->reached = state + 1;
				}
			}
		}
		ap_reply(radio, f + 10, kind == 0 ? 0x10 : 0x30, status);
		if (!status) {
			lx_os_ap_trace("ap_manage:wait", 481, 0); lx_os_lock(); lx_os_ap_trace("ap_manage:held", 481, 0); p->associated = true; if (!was_joined) p->authorized = !radio->ap_secure; lx_os_ap_trace("ap_manage:release", 481, 0); lx_os_unlock(); lx_os_ap_trace("ap_manage:released", 481, 0);
			if (!was_joined) {
				p->assoc_sequence = (uint16_t)(f[22] | f[23] << 8);
				lx_os_ap_peer(1, !radio->ap_secure, p->address);
				lx_tell("hotspot-ap: assoc %02x:%02x:**:**:**:%02x aid=%u security=%s HT=%u VHT=%u WMM=%u uapsd=%#x\n",
				    f[10], f[11], f[15], p->sta->aid, radio->ap_secure ? "WPA2-PSK" : "open", p->sta->deflink.ht_cap.ht_supported,
                    p->sta->deflink.vht_cap.vht_supported, p->sta->wme, p->sta->uapsd_queues);
				if (radio->ap_secure) {
					uint8_t nonce[32], replay[8];
					lx_os_random(nonce, sizeof(nonce)); lx_os_random(replay, sizeof(replay));
					replay[0] &= 0x7f; /* leave room for the M3 counter increment */
					lx_wpa_auth_start(&p->auth, radio->ap_pmk, radio->vif->addr,
					    p->address, nonce, radio->ap_gtk, replay, rsn, rsn[1] + 2u);
					explicit_bzero(nonce, sizeof(nonce));
					p->auth.keys.group_index = radio->ap_gtk_index;
					p->retries = 0; p->retry_at = lx_os_nanoseconds() + 50000000ull;
				} else {
					ap_peer_stats(radio, p);
					lx_tell("hotspot-ap: handshake skipped security=open\n");
				}
			}
		} else {
			lx_tell("hotspot-ap: assoc failed %02x:%02x:**:**:**:%02x status=%u\n", f[10], f[11], f[15], status);
			if (p && p->sta && !memcmp(p->sta->addr, f + 10, ETH_ALEN)) ap_peer_drop(radio, p, 1);
		}
	} else if ((kind == 0xa0 || kind == 0xc0) && length >= 26 &&
	    p && p->sta && !memcmp(p->sta->addr, f + 10, ETH_ALEN)) {
		ap_peer_drop(radio, p, f[24] | f[25] << 8);
	}
}

static bool
ap_eapol_send(struct lx_radio *radio, struct lx_ap_peer *p, const uint8_t *message, unsigned length)
{
	uint8_t frame[14 + LX_WPA_REPLY_MOST];
	if (length > LX_WPA_REPLY_MOST) return false;
	memcpy(frame, p->address, 6); memcpy(frame + 6, radio->vif->addr, 6);
	frame[12] = 0x88; frame[13] = 0x8e; memcpy(frame + 14, message, length);
	return ethernet_send(radio, frame, length + 14);
}

static void
ap_handshake_fail(struct lx_radio *radio, struct lx_ap_peer *p, const char *reason)
{
	lx_tell("hotspot-ap: 4-way handshake fail %02x:%02x:**:**:**:%02x stage=%u reason=%s\n",
	    p->address[0], p->address[1], p->address[5], p->auth.state, reason);
	uint8_t *f;
	struct sk_buff *skb = frame_new(radio, 26, &f);
	if (skb) { ap_header(radio, f, 0xc0, p->address); f[24] = 15; f[25] = 0; frame_send(radio, skb); }
	ap_peer_drop(radio, p, 15);
}

/* Called after ack, removal, or the retry deadline; never switch a GTK while
 * an authorized peer still needs to learn it. No key-slot PN resets on join. */
static void
ap_rekey_commit(struct lx_radio *radio)
{
 lx_os_ap_trace("ap_rekey_commit:enter", 0, 0);
 if (!radio->ap_rekey_pending) { lx_os_ap_trace("rekey:idle", 0, 0); return; }
 for (unsigned i = 0; i < radio->ap_capacity; i++)
  if (radio->ap_peers[i].authorized && radio->ap_peers[i].auth.group_pending) return;
 unsigned index = radio->ap_gtk_index == 1 ? 2 : 1;
 int error;
 struct ieee80211_key_conf *next = key_set(radio, radio->ap_next_gtk, index, false, &error);
 if (error) {
  key_drop(radio, &next, false);
  /* Clients have installed a GTK the firmware could not install: close their
   * associations, retain the current key, and retry at the next interval. */
  for (unsigned i = 0; i < radio->ap_capacity; i++) ap_peer_drop(radio, &radio->ap_peers[i], 2);
  lx_tell("hotspot-ap: GTK rekey install failed error=%d\n", error);
 } else {
  struct ieee80211_key_conf *old = radio->group_key;
  radio->group_key = next;
  memcpy(radio->ap_gtk, radio->ap_next_gtk, 16); radio->ap_gtk_index = index;
  key_drop(radio, &old, false);
  lx_tell("hotspot-ap: GTK rekey complete index=%u interval=3600\n", index);
 }
 explicit_bzero(radio->ap_next_gtk, 16);
 radio->ap_rekey_pending = false;
 radio->ap_rekey_at = lx_os_nanoseconds() + 3600000000000ull;
}

static void
ap_handshake_work(struct work_struct *work)
{
 lx_os_ap_trace("ap_handshake_work:enter", 0, 0);
 struct lx_radio *radio = container_of(work, struct lx_radio, ap_handshake_work.work);
 lx_os_ap_trace("ap_handshake_work:wait", 566, 0); mutex_lock(&radio->ap_mutex); lx_os_ap_trace("ap_handshake_work:held", 566, 0);
 if (!radio->ap_active) { lx_os_ap_trace("ap_handshake_work:release", 567, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_handshake_work:released", 567, 0); return; }
 unsigned long long now = lx_os_nanoseconds();
 bool negotiating = false;
 for (unsigned i = 0; i < radio->ap_capacity; i++) {
  struct lx_ap_peer *p = &radio->ap_peers[i];
  if (!p->sta) continue;
  if (READ_ONCE(p->kickout)) {
   lx_os_ap_trace("kickout:remove-enter", i, 0);
   lx_tell("hotspot-ap: firmware kickout slot=%u\n", i);
   ap_peer_drop(radio, p, 4); lx_os_ap_trace("kickout:remove-exit", i, 0); continue;
  }
  if (!p->associated && now - p->created_at >= 5000000000ull) { ap_peer_drop(radio, p, 4); continue; }
  bool fourway = p->associated && (p->auth.state == 1 || p->auth.state == 2);
  negotiating |= fourway;
  if (!(fourway || p->auth.group_pending) || now < p->retry_at) continue;
  if (p->retries++ >= 5) { ap_handshake_fail(radio, p, p->auth.group_pending ? "GTK-timeout" : "timeout"); continue; }
  unsigned state = p->auth.state;
  bool group = p->auth.group_pending;
  bool sent = ap_eapol_send(radio, p, group ? p->auth.group_message : state == 1 ? p->auth.m1 : p->auth.m3,
      group ? p->auth.group_length : state == 1 ? sizeof(p->auth.m1) : p->auth.m3_length);
  lx_tell("hotspot-ap: %s message=%u attempt=%u submitted=%u slot=%u\n",
      group ? "GTK" : "4-way", group || state == 1 ? 1 : 3, p->retries, sent, i);
  p->retry_at = now + 1000000000ull;
 }
 if (radio->ap_secure && !radio->ap_rekey_pending && !negotiating && now >= radio->ap_rekey_at) {
  lx_os_random(radio->ap_next_gtk, 16);
  radio->ap_rekey_pending = true;
  lx_os_ap_trace("rekey:begin", 0, radio->ap_gtk_index);
  for (unsigned i = 0; i < radio->ap_capacity; i++) {
   struct lx_ap_peer *p = &radio->ap_peers[i];
   if (!p->authorized) continue;
   lx_wpa_auth_group_start(&p->auth, radio->ap_next_gtk, radio->ap_gtk_index == 1 ? 2 : 1);
   p->retries = 0; p->retry_at = now;
  }
  lx_tell("hotspot-ap: GTK rekey begin interval=3600\n");
 }
 ap_rekey_commit(radio);
 lx_os_ap_trace("timer:handshake-arm-ms", 0, 100);
 queue_delayed_work(radio->ap_queue, &radio->ap_handshake_work, msecs_to_jiffies(100));
 lx_os_ap_trace("timer:handshake-armed", 0, 0);
 lx_os_ap_trace("ap_handshake_work:release", 603, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_handshake_work:released", 603, 0);
}

static void
ap_eapol(struct lx_radio *radio, const uint8_t *frame, unsigned length)
{
	struct lx_ap_peer *p = length >= 18 ? ap_peer_find(radio, frame + 6) : NULL;
 if (!radio->ap_active || !p || !p->associated || !radio->ap_secure || length < 18 ||
	    memcmp(frame + 6, p->address, 6) ||
	    (memcmp(frame, radio->vif->addr, 6) && memcmp(frame, "\x01\x80\xc2\x00\x00\x03", 6))) return;
	uint8_t reply[LX_WPA_REPLY_MOST]; unsigned reply_length = 0;
	if (p->authorized && length >= 21 && !(frame[20] & 8)) {
  int result = lx_wpa_auth_group_message(&p->auth, frame + 14, length - 14);
  if (result < 0) ap_handshake_fail(radio, p, "GTK-MIC-or-format");
  else if (result == 1) lx_tell("hotspot-ap: GTK message=2 MIC=ok slot=%u\n", (unsigned)(p - radio->ap_peers));
  ap_rekey_commit(radio); return;
 }
 unsigned old_state = p->auth.state;
	int result = lx_wpa_auth_message(&p->auth, frame + 14, length - 14, reply, &reply_length);
	if (result < 0) { ap_handshake_fail(radio, p, "MIC-or-format"); return; }
	if (result == 1) {
		ap_eapol_send(radio, p, reply, reply_length);
		if (old_state == 1) {
			p->retries = 1; p->retry_at = lx_os_nanoseconds() + 1000000000ull;
			lx_tell("hotspot-ap: 4-way message=2 MIC=ok message=3 submitted\n");
		}
	} else if (result == 2) {
		int error;

		p->key = key_set_peer(radio, p->sta, p->auth.keys.pairwise, 0, true, &error);
		if (!error && p->key) {
			error = station_state_peer(radio, p->sta, IEEE80211_STA_ASSOC, IEEE80211_STA_AUTHORIZED);
			if (!error) p->reached = IEEE80211_STA_AUTHORIZED;
		}
		if (error || !p->key) { ap_handshake_fail(radio, p, "key-install"); return; }
		lx_os_ap_trace("ap_eapol:wait", 638, 0); lx_os_lock(); lx_os_ap_trace("ap_eapol:held", 638, 0); p->authorized = true; lx_os_ap_trace("ap_eapol:release", 638, 0); lx_os_unlock(); lx_os_ap_trace("ap_eapol:released", 638, 0);
		lx_os_ap_peer(1, 1, p->address);
		ap_peer_stats(radio, p);
		lx_tell("hotspot-ap: 4-way handshake ok %02x:%02x:**:**:**:%02x PTK=CCMP GTK=CCMP authorized=1\n",
		    p->address[0], p->address[1], p->address[5]);
	}
	explicit_bzero(reply, sizeof(reply));
}

static void
ap_work(struct work_struct *work)
{
 lx_os_ap_trace("ap_work:enter", 0, 0);
	struct lx_radio *radio = container_of(work, struct lx_radio, ap_work);
	uint8_t frame[AP_MGMT_MOST];
	unsigned length;
 int signal;
	bool eapol;
	lx_os_ap_trace("ap_work:wait", 655, 0); mutex_lock(&radio->ap_mutex); lx_os_ap_trace("ap_work:held", 655, 0);
	for (;;) {
		lx_os_ap_trace("ap_work:wait", 657, 0); lx_os_lock(); lx_os_ap_trace("ap_work:held", 657, 0);
		if (!radio->ap_active || radio->ap_in == radio->ap_out) { lx_os_ap_trace("ap_work:release", 658, 0); lx_os_unlock(); lx_os_ap_trace("ap_work:released", 658, 0); break; }
		unsigned at = radio->ap_out++ % nitems(radio->ap_rx);
		length = radio->ap_rx[at].length;
		eapol = radio->ap_rx[at].eapol; signal = radio->ap_rx[at].signal;
		memcpy(frame, radio->ap_rx[at].frame, length);
		lx_os_ap_trace("ap_work:release", 663, 0); lx_os_unlock(); lx_os_ap_trace("ap_work:released", 663, 0);
		if (eapol) ap_eapol(radio, frame, length);
		else {
   ap_manage(radio, frame, length);
   struct lx_ap_peer *p = ap_peer_find(radio, frame + 10);
   if (p) { lx_os_ap_trace("ap_work:wait", 668, 0); lx_os_lock(); lx_os_ap_trace("ap_work:held", 668, 0); p->signal = signal; lx_os_ap_trace("ap_work:release", 668, 0); lx_os_unlock(); lx_os_ap_trace("ap_work:released", 668, 0); }
  }
	}
	lx_os_ap_trace("ap_work:release", 671, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_work:released", 671, 0);
}

static void
ap_stop(struct lx_radio *radio)
{
 lx_os_ap_trace("ap_stop:enter", 0, 0);
	if (!radio->ap) return;
	lx_os_ap_trace("ap_stop:wait", 678, 0); lx_os_lock(); lx_os_ap_trace("ap_stop:held", 678, 0);
	radio->ap_active = false; radio->joined = false;
	while (radio->ap_queuing) lx_os_sleep(&radio->ap_queuing, 1000000ull);
	lx_os_ap_trace("ap_stop:release", 681, 0); lx_os_unlock(); lx_os_ap_trace("ap_stop:released", 681, 0);
	lx_os_ap_link(0);
	lx_os_ap_trace("cancel_delayed_work_sync:ap_stats_work:enter", 0, 0); cancel_delayed_work_sync(&radio->ap_stats_work); lx_os_ap_trace("cancel_delayed_work_sync:ap_stats_work:exit", 0, 0);
	lx_os_ap_trace("cancel_delayed_work_sync:ap_handshake_work:enter", 0, 0); cancel_delayed_work_sync(&radio->ap_handshake_work); lx_os_ap_trace("cancel_delayed_work_sync:ap_handshake_work:exit", 0, 0);
	lx_os_ap_trace("cancel_work_sync:ap_work:enter", 0, 0); cancel_work_sync(&radio->ap_work); lx_os_ap_trace("cancel_work_sync:ap_work:exit", 0, 0);
	lx_os_ap_trace("ap_stop:wait", 686, 0); mutex_lock(&radio->ap_mutex); lx_os_ap_trace("ap_stop:held", 686, 0);
	for (unsigned i = 0; i < radio->ap_capacity; i++) {
  struct lx_ap_peer *p = &radio->ap_peers[i];
  if (!p->sta) continue;
		uint8_t *f;
		struct sk_buff *skb = frame_new(radio, 26, &f);
		if (skb) {
			ap_header(radio, f, 0xc0, p->address); f[24] = 3; f[25] = 0;
			frame_send(radio, skb);
			lx_pause_ms(30);
		}
		ap_peer_drop(radio, p, 3);
	}
	key_drop(radio, &radio->group_key, false);
	explicit_bzero(radio->ap_pmk, sizeof(radio->ap_pmk));
	explicit_bzero(radio->ap_gtk, sizeof(radio->ap_gtk));
 explicit_bzero(radio->ap_next_gtk, sizeof(radio->ap_next_gtk));
 radio->ap_rekey_pending = false;
	radio->ap_secure = false;
	/* The async management queue holds vif pointers: drain before remove/free. */
	lx_os_ap_trace("ath11k_mac_drain_tx:enter", 0, 0); ath11k_mac_drain_tx(radio->hw.priv); lx_os_ap_trace("ath11k_mac_drain_tx:exit", 0, 0);
	radio->vif->bss_conf.enable_beacon = false;
	bss_changed(radio, BSS_CHANGED_BEACON_ENABLED);
	lx_os_ap_trace("ap_stop:wait", 709, 0); mutex_lock(&radio->wiphy->mtx); lx_os_ap_trace("ap_stop:held", 709, 0);
	if (radio->ap_channel_assigned)
		radio->ops->unassign_vif_chanctx(&radio->hw, radio->vif, &radio->vif->bss_conf, radio->channel);
	radio->vif->bss_conf.chanctx_conf = NULL;
	if (radio->ap_channel_added) radio->ops->remove_chanctx(&radio->hw, radio->channel);
	lx_os_ap_trace("ap_stop:release", 714, 0); mutex_unlock(&radio->wiphy->mtx); lx_os_ap_trace("ap_stop:released", 714, 0);
	kfree(radio->channel); radio->channel = NULL;
	radio->ap_channel_added = radio->ap_channel_assigned = false;
	lx_os_ap_trace("station_down:enter", 0, 0); station_down(radio); lx_os_ap_trace("station_down:exit", 0, 0);
	radio->ap = false;
	lx_os_ap_trace("ap_stop:release", 719, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_stop:released", 719, 0);
	lx_tell("hotspot-ap: teardown complete\n");
}

static int
ap_start(struct lx_radio *radio, const struct lx_ap_config *config)
{
 lx_os_ap_trace("ap_start:enter", 0, 0);
	struct ieee80211_hw *hw = &radio->hw;
	struct ieee80211_vif *vif;
	struct ieee80211_bss_conf *conf;
	struct linuxkpi_ieee80211_channel *channel = linuxkpi_ieee80211_get_channel(radio->wiphy, (config->channel <= 13 ? 2407 : 5000) + config->channel * 5);
	int ret;
	if (!ap_channel_number(config->channel) || !radio->regd || !channel || channel->flags &
	    (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)) {
		lx_tell("hotspot-ap: channel refused channel=%u flags=%#x regulatory=%u\n",
		    config->channel, channel ? channel->flags : 0, radio->regd != NULL);
		return EACCES;
	}
	if (!(hw->wiphy->interface_modes & BIT(NL80211_IFTYPE_AP))) return ENOTSUP;
 struct ath11k *ar = hw->priv;
 if (!test_bit(WMI_TLV_SERVICE_BEACON_OFFLOAD, ar->ab->wmi_ab.svc_map) ||
     !ieee80211_hw_check(hw, AP_LINK_PS) || ar->max_num_stations < 8) return ENOTSUP;
 radio->ap_capacity = min_t(unsigned, ar->max_num_stations, AP_PEERS);
 radio->ap_uapsd = test_bit(WMI_TLV_SERVICE_AP_UAPSD, ar->ab->wmi_ab.svc_map);
	if (!radio->ap_queue) {
		mutex_init(&radio->ap_mutex);
		radio->ap_queue = alloc_ordered_workqueue("hotspot-ap", 0);
		if (!radio->ap_queue) return ENOMEM;
	}
	INIT_WORK(&radio->ap_work, ap_work);
	INIT_DELAYED_WORK(&radio->ap_stats_work, ap_stats_work);
	INIT_DELAYED_WORK(&radio->ap_handshake_work, ap_handshake_work);
	radio->ap_hidden = false; radio->ap_rekey_pending = false;
 radio->ap_gtk_index = 1;
 radio->ap_rekey_at = lx_os_nanoseconds() + 3600000000000ull;
	radio->ap_secure = config->security != 0;
	if (radio->ap_secure) {
		if (config->passphrase_length) lx_wpa_pmk(config->passphrase, config->passphrase_length, config->ssid, config->ssid_length, radio->ap_pmk);
		else memcpy(radio->ap_pmk, config->pmk, 32);
		lx_os_random(radio->ap_gtk, sizeof(radio->ap_gtk));
	}
	lx_os_ap_trace("ap_start:wait", 760, 0); lx_os_lock(); lx_os_ap_trace("ap_start:held", 760, 0); bzero(radio->ap_count, sizeof(radio->ap_count)); lx_os_ap_trace("ap_start:release", 760, 0); lx_os_unlock(); lx_os_ap_trace("ap_start:released", 760, 0);
	radio->ap_in = radio->ap_out = radio->ap_sequence = 0;
	radio->vif_memory = kzalloc(64 + sizeof(*vif) + hw->vif_data_size, GFP_KERNEL);
	if (!radio->vif_memory) { explicit_bzero(radio->ap_pmk, sizeof(radio->ap_pmk)); explicit_bzero(radio->ap_gtk, sizeof(radio->ap_gtk)); return ENOMEM; }
	vif = (struct ieee80211_vif *)roundup2((uintptr_t)radio->vif_memory, 64);
	vif->type = NL80211_IFTYPE_AP;
	vif->probe_req_reg = true;
	memcpy(vif->addr, hw->wiphy->perm_addr, ETH_ALEN);
	vif->addr[0] = (vif->addr[0] | 2) & ~1; /* locally administered AP BSSID */
	vif->addr[5] ^= 0x80;
	conf = &vif->bss_conf;
	memcpy(conf->addr, vif->addr, ETH_ALEN);
	conf->vif = vif; conf->bssid = vif->addr; vif->link_conf[0] = conf;
	conf->beacon_int = 100; conf->dtim_period = 2; conf->txpower = channel->max_power;
	conf->assoc_capability = radio->ap_secure ? 0x11 : 1;
	struct ieee80211_supported_band *band = hw->wiphy->bands[channel->band];
 for (int r = 0; r < band->n_bitrates; r++) {
  unsigned rate = band->bitrates[r].bitrate;
  if (channel->band == NL80211_BAND_2GHZ ? rate == 10 || rate == 20 || rate == 55 || rate == 110 :
      rate == 60 || rate == 120 || rate == 240) conf->basic_rates |= 1u << r;
 }
 conf->qos = true; conf->use_short_slot = true;
 conf->ht_operation_mode = 3;
	memcpy(vif->cfg.ssid, config->ssid, config->ssid_length);
	vif->cfg.ssid_len = config->ssid_length;
	if (ieee80211_hw_check(hw, SUPPORTS_TX_ENCAP_OFFLOAD)) vif->offload_flags |= IEEE80211_OFFLOAD_ENCAP_ENABLED;
	if (ieee80211_hw_check(hw, SUPPORTS_RX_DECAP_OFFLOAD)) vif->offload_flags |= IEEE80211_OFFLOAD_DECAP_ENABLED;
	lx_os_ap_trace("ap_start:wait", 787, 0); mutex_lock(&hw->wiphy->mtx); lx_os_ap_trace("ap_start:held", 787, 0);
	ret = radio->ops->start(hw);
	if (!ret) { radio->started = true; ret = radio->ops->add_interface(hw, vif); }
	lx_os_ap_trace("ap_start:release", 790, 0); mutex_unlock(&hw->wiphy->mtx); lx_os_ap_trace("ap_start:released", 790, 0);
	if (ret) { lx_os_ap_trace("station_down:enter", 0, 0); station_down(radio); lx_os_ap_trace("station_down:exit", 0, 0); explicit_bzero(radio->ap_pmk, sizeof(radio->ap_pmk)); explicit_bzero(radio->ap_gtk, sizeof(radio->ap_gtk)); return -ret; }
	radio->vif = vif; radio->ap = true; radio->width = 20;
	lx_tell("hotspot-ap: vdev created type=AP channel=%u width=20\n", config->channel);
	ret = lx_os_ap_attach(vif->addr);
	if (ret) goto fail;
	/* SSID, beacon interval and DTIM must reach arvif BEFORE vdev_start. */
	bss_changed(radio, BSS_CHANGED_SSID | BSS_CHANGED_BSSID | BSS_CHANGED_BEACON_INT | BSS_CHANGED_BEACON_INFO);
	radio->channel = kzalloc(sizeof(*radio->channel) + hw->chanctx_data_size + 64, GFP_KERNEL);
	if (!radio->channel) { ret = ENOMEM; goto fail; }
	radio->channel->def.chan = channel;
	radio->channel->def.width = NL80211_CHAN_WIDTH_20;
	radio->channel->def.center_freq1 = channel->center_freq;
	radio->channel->min_def = radio->channel->def;
	radio->channel->rx_chains_static = radio->channel->rx_chains_dynamic = 2;
	conf->chanreq.oper = radio->channel->def;
	lx_os_ap_trace("ap_start:wait", 806, 0); mutex_lock(&hw->wiphy->mtx); lx_os_ap_trace("ap_start:held", 806, 0);
	ret = radio->ops->add_chanctx(hw, radio->channel);
	if (!ret) {
		radio->ap_channel_added = true;
		conf->chanctx_conf = radio->channel;
		ret = radio->ops->assign_vif_chanctx(hw, vif, conf, radio->channel);
		if (!ret) radio->ap_channel_assigned = true;
	}
	lx_os_ap_trace("ap_start:release", 814, 0); mutex_unlock(&hw->wiphy->mtx); lx_os_ap_trace("ap_start:released", 814, 0);
	if (ret) { ret = -ret; goto fail; }
	lx_os_ap_trace("ap_start:wait", 816, 0); mutex_lock(&hw->wiphy->mtx); lx_os_ap_trace("ap_start:held", 816, 0);
 static const uint8_t classes[4] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO };
 for (unsigned i = 0; i < 4; i++) {
  const uint8_t *ac = ap_wmm + 10 + 4*i;
  struct ieee80211_tx_queue_params params = { .aifs = ac[0] & 15,
      .cw_min = (1u << (ac[1] & 15)) - 1, .cw_max = (1u << (ac[1] >> 4)) - 1,
      .txop = ac[2] | ac[3] << 8 };
  ret = radio->ops->conf_tx(hw, vif, 0, classes[i], &params);
  if (ret) break;
 }
 lx_os_ap_trace("ap_start:release", 826, 0); mutex_unlock(&hw->wiphy->mtx); lx_os_ap_trace("ap_start:released", 826, 0);
 if (ret) { ret = -ret; goto fail; }
 conf->enable_beacon = true;
 bss_changed(radio, BSS_CHANGED_BEACON_ENABLED | BSS_CHANGED_ERP_SLOT | BSS_CHANGED_QOS);
	/* bss_info_changed is void; examine the driver's real WMI vdev-up result. */
	struct ath11k_vif *arvif = (struct ath11k_vif *)vif->drv_priv;
	if (!arvif->is_up) { ret = EIO; goto fail; }
 lx_os_ap_trace("ap_start:wait", 833, 0); mutex_lock(&ar->conf_mutex); lx_os_ap_trace("ap_start:held", 833, 0);
 static const unsigned keepalive_param[] = {
     WMI_VDEV_PARAM_AP_KEEPALIVE_MIN_IDLE_INACTIVE_TIME_SECS,
     WMI_VDEV_PARAM_AP_KEEPALIVE_MAX_IDLE_INACTIVE_TIME_SECS,
     WMI_VDEV_PARAM_AP_KEEPALIVE_MAX_UNRESPONSIVE_TIME_SECS };
 static const unsigned keepalive_seconds[] = { 30, 60, 65 };
 for (unsigned i = 0; i < 3; i++) {
  ret = ath11k_wmi_vdev_set_param_cmd(ar, arvif->vdev_id, keepalive_param[i], keepalive_seconds[i]);
  if (ret) break;
 }
 if (!ret) ret = ath11k_wmi_pdev_set_param(ar, WMI_PDEV_PEER_STA_PS_STATECHG_ENABLE, 1, ar->pdev->pdev_id);
 lx_os_ap_trace("ap_start:release", 844, 0); mutex_unlock(&ar->conf_mutex); lx_os_ap_trace("ap_start:released", 844, 0);
 if (ret) { ret = -ret; goto fail; }
 lx_tell("hotspot-ap: PS owner=firmware TIM=offload dtim=2 uapsd=%u capacity=%u firmware-capacity=%u keepalive=30/60/65\n", radio->ap_uapsd, radio->ap_capacity, ar->max_num_stations);
	if (radio->ap_secure) {
		radio->pairwise_cipher = radio->group_cipher = 4;
		bzero(&radio->wpa, sizeof(radio->wpa)); radio->wpa.group_length = 16;
		radio->group_key = key_set(radio, radio->ap_gtk, 1, false, &ret);
		if (ret || !radio->group_key) { ret = ret < 0 ? -ret : EIO; goto fail; }
		lx_tell("hotspot-ap: GTK installed cipher=CCMP index=1\n");
	}
	lx_os_ap_trace("ap_start:wait", 854, 0); lx_os_lock(); lx_os_ap_trace("ap_start:held", 854, 0); radio->ap_active = true; lx_os_ap_trace("ap_start:release", 854, 0); lx_os_unlock(); lx_os_ap_trace("ap_start:released", 854, 0);
	lx_os_ap_link(1);
	lx_tell("hotspot-ap: vdev up id=%u channel=%u width=20\n", arvif->vdev_id, config->channel);
	lx_tell("hotspot-ap: beacon start interval=100 dtim=2 ssid-length=%u security=%s ap1\n", config->ssid_length, radio->ap_secure ? "WPA2-PSK" : "open");
	ap_stats(radio);
	queue_delayed_work(radio->ap_queue, &radio->ap_stats_work, msecs_to_jiffies(5000));
 lx_os_ap_trace("timer:handshake-arm-ms", 0, 100);
 queue_delayed_work(radio->ap_queue, &radio->ap_handshake_work, msecs_to_jiffies(100));
 lx_os_ap_trace("timer:handshake-armed", 0, 0);
	return 0;
fail:
	lx_tell("hotspot-ap: start failed error=%d\n", ret);
	ap_stop(radio);
	return ret;
}

static int
ap_configure(const struct lx_ap_config *config)
{
 lx_os_ap_trace("ap_configure:enter", 0, 0);
	struct lx_radio *radio = the_radio;
	int error;
	if (!radio || !radio->registered) return ENXIO;
	if (config && (!config->ssid_length || config->ssid_length > 32 || !ap_channel_number(config->channel))) return EINVAL;
	lx_os_ap_trace("lx_os_station:enter", 0, 0); lx_os_station(NULL); lx_os_ap_trace("lx_os_station:exit", 0, 0); /* drain outstanding scans/joins before owning the singleton */
	ap_stop(radio);
	if (config) {
		if (radio->sta) { lx_os_ap_trace("station_leave:enter", 0, 0); station_leave(radio); lx_os_ap_trace("station_leave:exit", 0, 0); }
		if (radio->started) {
			lx_os_ap_trace("ath11k_mac_drain_tx:enter", 0, 0); ath11k_mac_drain_tx(radio->hw.priv); lx_os_ap_trace("ath11k_mac_drain_tx:exit", 0, 0);
			lx_os_ap_trace("station_down:enter", 0, 0); station_down(radio); lx_os_ap_trace("station_down:exit", 0, 0);
		}
		lx_tell("hotspot-ap: mode AP-only; STA withdrawn\n");
		error = ap_start(radio, config);
		if (!error) return 0;
	} else error = 0;
	int restore = station_up(radio);
	if (!restore) lx_os_station(&boundary);
	lx_tell("hotspot-ap: STA restore error=%d\n", restore);
	return error ? error : restore;
}

static int
ap_send(const unsigned char *frame, unsigned long length)
{
 lx_os_ap_trace("ap_send:enter", 0, 0);
 struct lx_radio *radio = the_radio;
 bool sent = false;
 if (!radio || length < 14 || length > 1514) return ENETDOWN;
 ap_count(radio, AP_TX);
 bool group = is_multicast_ether_addr(frame);
 if (group) ap_count(radio, AP_TX_GROUP);
 lx_os_ap_trace("ap_send:wait", 902, 0); mutex_lock(&radio->ap_mutex); lx_os_ap_trace("ap_send:held", 902, 0);
 struct lx_ap_peer *p = ap_peer_find(radio, frame);
 bool authorized = p && p->authorized;
 if (group) for (unsigned i = 0; i < radio->ap_capacity; i++) authorized |= radio->ap_peers[i].authorized;
 if (!radio->ap_active) ap_count(radio, AP_TX_INACTIVE);
 else if (!authorized) ap_count(radio, AP_TX_DEST);
 else {
  sent = ethernet_send(radio, frame, (unsigned)length);
  ap_count(radio, sent ? AP_TX_SUBMIT : AP_TX_ALLOC);
 }
 lx_os_ap_trace("ap_send:release", 912, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_send:released", 912, 0);
 return sent ? 0 : ENETDOWN;
}

static unsigned
ap_stations(struct lx_ap_station *out, unsigned most)
{
 lx_os_ap_trace("ap_stations:enter", 0, 0);
 struct lx_radio *radio = the_radio;
 unsigned count = 0;
 if (!radio || !radio->ap) { lx_os_ap_trace("ap_stations:STA-empty", 0, 0); return 0; }
 lx_os_ap_trace("ap_stations:wait", 922, 0); mutex_lock(&radio->ap_mutex); lx_os_ap_trace("ap_stations:held", 922, 0);
 struct ath11k *ar = radio->hw.priv;
 for (unsigned i = 0; i < radio->ap_capacity && count < most; i++) {
  struct lx_ap_peer *p = &radio->ap_peers[i];
  if (!p->authorized) continue;
  struct ath11k_sta *sta = (void *)p->sta->drv_priv;
  lx_os_ap_trace("ap_stations:wait", 928, 0); spin_lock_bh(&ar->data_lock); lx_os_ap_trace("ap_stations:held", 928, 0);
  lx_os_ap_trace("stations:rssi-held", i, 0);
  unsigned rssi = sta->rssi_comb;
  lx_os_ap_trace("ap_stations:release", 930, 0); spin_unlock_bh(&ar->data_lock); lx_os_ap_trace("ap_stations:released", 930, 0);
  int signal = p->signal;
  if (rssi) signal = test_bit(WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT, ar->ab->wmi_ab.svc_map) ?
      (int8_t)rssi : (int)rssi + ATH11K_DEFAULT_NOISE_FLOOR;
  memcpy(out[count].address, p->address, 6); out[count++].rssi = signal;
 }
 lx_os_ap_trace("ap_stations:release", 936, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_stations:released", 936, 0);
 lx_os_ap_trace("ap_stations:exit", 0, count);
 return count;
}

/* WMI_PEER_STA_KICKOUT_EVENTID -> driver's report_low_ack. The WMI/RX thread
 * only marks the peer; the AP timer performs sleeping WMI deletion off RX. */
void
hakios_ieee80211_report_low_ack(struct ieee80211_sta *sta, int packets)
{
 lx_os_ap_trace("hakios_ieee80211_report_low_ack:enter", 0, 0);
 struct lx_radio *radio = the_radio;
 (void)packets;
 if (!radio || !radio->ap) { lx_os_ap_trace("kickout:STA-ignored", 0, packets); return; }
 lx_os_ap_trace("hakios_ieee80211_report_low_ack:wait", 948, 0); lx_os_lock(); lx_os_ap_trace("hakios_ieee80211_report_low_ack:held", 948, 0);
 struct lx_ap_peer *p = ap_peer_find(radio, sta->addr);
 if (p && p->sta == sta) { p->kickout = true; lx_os_ap_trace("kickout:marked", p - radio->ap_peers, packets); }
 lx_os_ap_trace("hakios_ieee80211_report_low_ack:release", 951, 0); lx_os_unlock(); lx_os_ap_trace("hakios_ieee80211_report_low_ack:released", 951, 0);
}

static int
ap_set_hidden(unsigned hidden)
{
 lx_os_ap_trace("ap_set_hidden:enter", 0, 0);
 struct lx_radio *radio = the_radio;
 if (!radio || !radio->ap) return ENXIO;
 lx_os_ap_trace("ap_set_hidden:wait", 959, 0); mutex_lock(&radio->ap_mutex); lx_os_ap_trace("ap_set_hidden:held", 959, 0);
 radio->ap_hidden = hidden != 0;
 radio->vif->bss_conf.hidden_ssid = radio->ap_hidden;
 bss_changed(radio, BSS_CHANGED_BEACON);
 lx_os_ap_trace("ap_set_hidden:release", 963, 0); mutex_unlock(&radio->ap_mutex); lx_os_ap_trace("ap_set_hidden:released", 963, 0);
 return 0;
}

static void ap_shutdown(void) { if (the_radio) ap_stop(the_radio); }
static const struct lx_ap_ops ap_boundary = {
	.configure = ap_configure, .send = ap_send, .stations = ap_stations, .set_hidden = ap_set_hidden, .shutdown = ap_shutdown,
};
