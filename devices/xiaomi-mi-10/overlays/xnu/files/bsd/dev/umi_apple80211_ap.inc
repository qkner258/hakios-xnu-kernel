/* iOS 16.5 / 20F66 raw HostAP ABI. Never retain or return credentials here. */
struct apple80211_hostap {
	uint32_t version;
	uint16_t mode, auth_lower, auth_upper, reserved;
	struct apple80211_channel channel;
	uint32_t ssid_length;
	uint8_t ssid[32];
	uint32_t key_version, key_length, key_cipher;
	uint16_t key_flags, key_index;
	uint8_t key[64], tail[1500 - 0x8c];
};
struct apple80211_virtual_create {
	uint32_t version, flags, reserved, role;
	char name[16];
};
struct apple80211_virtual_delete { uint32_t version; char name[16]; };
struct apple80211_softap_params {
	uint32_t version, active_timeout, dynamic_timeout, disable_interval, rx_threshold;
	uint16_t beacon_interval;
	uint8_t state, enable, awake, reserved[3];
};
_Static_assert(sizeof(struct apple80211_hostap) == 1500 &&
    offsetof(struct apple80211_hostap, ssid) == 0x1c &&
    offsetof(struct apple80211_hostap, key) == 0x4c, "20F66 HOST_AP_MODE ABI");
_Static_assert(sizeof(struct apple80211_virtual_create) == 32 &&
    offsetof(struct apple80211_virtual_create, role) == 12, "20F66 CREATE ABI");
_Static_assert(sizeof(struct apple80211_virtual_delete) == 20, "20F66 DELETE ABI");
_Static_assert(sizeof(struct apple80211_softap_params) == 28, "20F66 SOFTAP_PARAMS ABI");

/* Pure decoder, also exercised by the host ABI harness. A native protected
 * request must never turn into an open network, even with a missing key. */
static int
apple_ap_decode(const struct apple80211_hostap *a, struct lx_ap_config *c)
{
	if (a->version != 1 || (a->mode & 255) != 2 ||
	    a->auth_lower != 1 || a->auth_upper != 8 ||
	    !a->ssid_length || a->ssid_length > 32 ||
	    (a->channel.flags & 0x2000))
		return EINVAL;
	c->security = 1; c->ssid_length = a->ssid_length;
	memcpy(c->ssid, a->ssid, a->ssid_length);
	/* Zero is Apple's automatic channel request. Regulatory checks stay in ath11k. */
	c->channel = a->channel.channel ? a->channel.channel : (a->channel.flags & CHANNEL_5GHZ ? 149 : 6);
 unsigned ch = c->channel;
 if (!((ch >= 1 && ch <= 13) || (ch >= 36 && ch <= 48 && !(ch & 3)) ||
     (ch >= 149 && ch <= 165 && (ch - 149) % 4 == 0)) ||
     ((a->channel.flags & CHANNEL_5GHZ) && ch <= 13) ||
     ((a->channel.flags & CHANNEL_2GHZ) && ch > 13)) return EINVAL;
	if (a->key_cipher == CIPHER_PASSPHRASE && a->key_length >= 8 && a->key_length <= 63) {
		for (unsigned i = 0; i < a->key_length; i++) if (a->key[i] < 0x20 || a->key[i] > 0x7e) return EINVAL;
		c->passphrase_length = a->key_length;
		memcpy(c->passphrase, a->key, a->key_length);
	} else if (a->key_cipher == CIPHER_PMK && a->key_length == 32) {
		memcpy(c->pmk, a->key, 32);
	} else return EINVAL;
	return 0;
}

static bool
apple_ap_interface(const struct apple80211req *r)
{
	return !strncmp(r->req_if_name, "ap1", sizeof(r->req_if_name));
}

static int
apple_ap_set(const struct apple80211req *r)
{
	lx_os_ap_trace("ap-set-dispatch", r->req_type, 0);
	switch (r->req_type) {
	case 94: {
		struct apple80211_virtual_create a;
		if (r->req_len != sizeof(a)) return EINVAL;
		int error = take(r, &a, sizeof(a));
		if (error) return error;
		if (a.version != 1 || a.role != 7) return EOPNOTSUPP;
		error = hakios_wifi_ap_create();
		if (!error) { strlcpy(a.name, "ap1", sizeof(a.name)); error = give(r, &a, sizeof(a)); }
		printf("hotspot-ap: Settings interface create role=7 interface=ap1 error=%d\n", error);
		return error;
	}
	case 95: {
		struct apple80211_virtual_delete a;
		if (r->req_len != sizeof(a)) return EINVAL;
		int error = take(r, &a, sizeof(a));
		if (error) return error;
		if (a.version != 1 || strncmp(a.name, "ap1", sizeof(a.name))) return EINVAL;
		error = hakios_wifi_ap_delete();
		printf("hotspot-ap: Settings interface delete interface=ap1 error=%d\n", error);
		return error;
	}
	case 25: {
		if (r->req_val == 2 && !r->req_len) {
			printf("hotspot-ap: Settings stop received\n");
			return hakios_wifi_ap_configure(NULL);
		}
		if (r->req_val != 1 || r->req_len != sizeof(struct apple80211_hostap)) return EINVAL;
		struct apple80211_hostap *a = kalloc_data(sizeof(*a), Z_WAITOK | Z_ZERO);
		struct lx_ap_config c = { 0 };
		if (!a) return ENOMEM;
		int error = take(r, a, sizeof(*a));
		if (!error) error = apple_ap_decode(a, &c);
		printf("hotspot-ap: Settings start received security=WPA2-PSK channel=%u ssid-length=%u error=%d\n",
		    c.channel, c.ssid_length, error);
		if (!error) error = hakios_wifi_ap_configure(&c);
		memset_s(&c, sizeof(c), 0, sizeof(c));
		memset_s(a, sizeof(*a), 0, sizeof(*a)); kfree_data(a, sizeof(*a));
		return error;
	}
	case 26: /* AP_MODE is a status/mode scalar, never an unconfigured start. */
		return r->req_val == 0 || r->req_val == 2 ? hakios_wifi_ap_configure(NULL) : EINVAL;
	case 336:
		return hakios_wifi_ap_hidden((unsigned)r->req_val);
	case 347: {
		struct apple80211_softap_params p;
		if (r->req_len != sizeof(p)) return EINVAL;
		int error = take(r, &p, sizeof(p));
		if (error) return error;
		/* There is no AP sleep scheduler. Do not let its timeout switch off a
		 * Settings-owned AP while clients are using it. */
		return p.enable ? EOPNOTSUPP : 0;
	}
	default: return EOPNOTSUPP;
	}
}

static int
apple_ap_get(struct apple80211req *r, const struct lx_ap_status *s)
{
	lx_os_ap_trace("ap-get-dispatch", r->req_type, s->active);
	switch (r->req_type) {
	case 25: case 26: r->req_val = s->active ? 1 : 0; return 0;
	case 15: r->req_val = 2; return 0;
	case 13: r->req_val = s->active ? 4 : 0; return 0;
	case 1:
		if (!s->active) return ENXIO;
		{ int error = give(r, s->ssid, s->ssid_length); if (!error) r->req_len = s->ssid_length; return error; }
	case 9: return s->active ? give(r, s->address, 6) : ENXIO;
	case 4: {
		struct { uint32_t version; struct apple80211_channel channel; } a = {
		    1, { 1, s->channel, CHANNEL_20MHZ | (s->channel <= 13 ? CHANNEL_2GHZ : CHANNEL_5GHZ) | CHANNEL_ACTIVE } };
		return s->active ? give(r, &a, sizeof(a)) : ENXIO;
	}
	case 2: {
		struct { uint32_t version, lower, upper, extended; } a = { 1, 1, s->security ? 8 : 0, 0 };
		return give(r, &a, sizeof(a));
	}
	case 42: {
		/* 128 versioned 16-byte entries; MAC at +4, RSSI at +12.
		 * IO80211's decoder: 0x1d30cd554..0x1d30cd684. */
		struct { uint32_t version, count; struct {
			uint32_t version; uint8_t mac[6], pad[2]; int32_t rssi;
		} entries[128]; } *a = kalloc_data(2056, Z_WAITOK | Z_ZERO);
		if (!a) return ENOMEM;
		struct lx_ap_station peers[LX_AP_STATIONS];
  a->version = 1; a->count = hakios_wifi_ap_stations(peers, LX_AP_STATIONS);
  for (unsigned i = 0; i < a->count; i++) {
   a->entries[i].version = 1; memcpy(a->entries[i].mac, peers[i].address, 6);
   a->entries[i].rssi = peers[i].rssi;
  }
		lx_os_ap_trace("ap-copyout-enter", r->req_type, sizeof(*a));
		int error = give(r, a, sizeof(*a));
		lx_os_ap_trace("ap-copyout-exit", r->req_type, error);
		kfree_data(a, sizeof(*a)); return error;
	}
	case 336: r->req_val = s->hidden; return 0;
	case 350: {
		/* No LPM scheduler exists; all its dwell/cycle statistics are zero. */
		struct { uint32_t version; uint8_t counters[84]; } a = { .version = 1 };
		return give(r, &a, sizeof(a));
	}
	case 254: {
		struct apple80211_channels *a = kalloc_data(sizeof(*a), Z_WAITOK | Z_ZERO);
		struct hakios_sta_channel c[96];
		if (!a) return ENOMEM;
		a->version = 1;
		lx_os_ap_trace("ap-channels-lock-wait", r->req_type, 0);
		unsigned count = hakios_sta_channels(c, 96);
		lx_os_ap_trace("ap-channels-lock-done", r->req_type, count);
		for (unsigned i = 0; i < count; i++) {
			unsigned f = c[i].frequency;
   if (!((f >= 2412 && f <= 2472) || (f >= 5180 && f <= 5240 && (f - 5180) % 20 == 0) ||
       (f >= 5745 && f <= 5825 && (f - 5745) % 20 == 0)) ||
			    (c[i].flags & (HAKIOS_STA_CHANNEL_PASSIVE | HAKIOS_STA_CHANNEL_RADAR))) continue;
			channel_fill(&a->channels[a->count++], c[i].frequency, c[i].flags);
		}
		lx_os_ap_trace("ap-copyout-enter", r->req_type, sizeof(*a));
		int error = give(r, a, sizeof(*a));
		lx_os_ap_trace("ap-copyout-exit", r->req_type, error);
		kfree_data(a, sizeof(*a)); return error;
	}
	case 347: {
		struct apple80211_softap_params p = { .version = 1, .beacon_interval = 100 };
		return give(r, &p, sizeof(p));
	}
	default: return EOPNOTSUPP;
	}
}
