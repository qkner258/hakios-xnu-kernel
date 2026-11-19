/* Native Settings control and BSD ap1. Included by hakios.lx.c. */
#include <sys/sysctl.h>

static bool lx_ap_attached, lx_ap_sta_powered;
static char lx_ap_setting[40] = "off";
static struct lx_ap_status lx_ap_state = { .version = 1 };

static bool
lx_ap_transmit(mbuf_t packet, size_t length)
{
	return lx_link_transmit_common(packet, length, true);
}

static struct hakios_net_link lx_ap_link = {
	.name = "hotspot-ap: ath11k AP", .unit = 3, .wireless = true, .hostap = true,
	.transmit = lx_ap_transmit,
};

int
lx_os_ap_attach(const unsigned char *address)
{
	if (!lx_ap_attached) {
		memcpy(lx_ap_link.address, address, sizeof(lx_ap_link.address));
		lx_os_ap_trace("create:net-attach-enter", 0, 0);
		lx_ap_attached = hakios_net_attach(&lx_ap_link);
		lx_os_ap_trace("create:net-attach-exit", 0, lx_ap_attached);
	}
	return lx_ap_attached ? 0 : ENXIO;
}

static int
lx_ap_create_locked(void)
{
	unsigned char address[6];
	if (lx_ap_attached) return 0;
	if (!lx_ap_ops || hakios_net_address_unit(0, address)) return ENXIO;
	address[0] = (address[0] | 2) & ~1; address[5] ^= 0x80;
	return lx_os_ap_attach(address);
}

int
hakios_wifi_ap_create(void)
{
	lx_os_ap_trace("hakios_wifi_ap_create:lx_mode_lock:wait", 42, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_create:lx_mode_lock:held", 42, 0);
	int error = lx_ap_create_locked();
	lx_os_ap_trace("hakios_wifi_ap_create:lx_mode_lock:release", 44, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_create:lx_mode_lock:released", 44, 0);
	return error;
}

void lx_os_ap_link(int up) { hakios_net_link_state_unit(3, up != 0); }
void lx_os_ap_input(const unsigned char *frame, unsigned long length)
{
	hakios_net_input_unit(3, frame, length);
}

void
hakios_wifi_ap_status(struct lx_ap_status *status)
{
	lx_os_ap_trace("hakios_wifi_ap_status:lx_lock:wait", 57, 0); lck_mtx_lock(&lx_lock); lx_os_ap_trace("hakios_wifi_ap_status:lx_lock:held", 57, 0); *status = lx_ap_state; lx_os_ap_trace("hakios_wifi_ap_status:lx_lock:release", 57, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("hakios_wifi_ap_status:lx_lock:released", 57, 0);
}

static struct { unsigned char address[6]; unsigned associated, authorized; } lx_ap_peers[LX_AP_STATIONS];

unsigned
hakios_wifi_ap_stations(struct lx_ap_station *out, unsigned most)
{
 lx_os_ap_trace("hakios_wifi_ap_stations:lx_mode_lock:wait", 65, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_stations:lx_mode_lock:held", 65, 0);
 lx_os_ap_trace("stations:driver-enter", 0, most);
 unsigned count = lx_ap_ops ? lx_ap_ops->stations(out, most) : 0;
 lx_os_ap_trace("stations:driver-exit", 0, count);
 lx_os_ap_trace("hakios_wifi_ap_stations:lx_mode_lock:release", 67, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_stations:lx_mode_lock:released", 67, 0);
 return count;
}

void
lx_os_ap_peer(unsigned associated, unsigned authorized, const unsigned char *address)
{
 lx_os_ap_trace("lx_os_ap_peer:lx_lock:wait", 74, 0); lck_mtx_lock(&lx_lock); lx_os_ap_trace("lx_os_ap_peer:lx_lock:held", 74, 0);
 unsigned slot = LX_AP_STATIONS, empty = LX_AP_STATIONS;
 for (unsigned i = 0; i < LX_AP_STATIONS; i++) {
  if (lx_ap_peers[i].associated && !memcmp(lx_ap_peers[i].address, address, 6)) slot = i;
  if (!lx_ap_peers[i].associated) empty = i;
 }
 if (slot == LX_AP_STATIONS) {
  if (!associated) { lx_os_ap_trace("lx_os_ap_peer:lx_lock:release", 81, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("lx_os_ap_peer:lx_lock:released", 81, 0); return; }
  slot = empty;
 }
 if (slot == LX_AP_STATIONS) {
  lck_mtx_unlock(&lx_lock);
  lx_os_ap_trace("peer-table:full-released", 0, ENOSPC);
  return;
 }
 unsigned was_associated = lx_ap_peers[slot].associated, was_authorized = lx_ap_peers[slot].authorized;
 memcpy(lx_ap_peers[slot].address, address, 6);
 lx_ap_peers[slot].associated = associated; lx_ap_peers[slot].authorized = authorized;
 lx_ap_state.associated = lx_ap_state.authorized = 0;
 bzero(lx_ap_state.peer, sizeof(lx_ap_state.peer));
 for (unsigned i = 0; i < LX_AP_STATIONS; i++) {
  lx_ap_state.associated += lx_ap_peers[i].associated;
  lx_ap_state.authorized += lx_ap_peers[i].authorized;
  if (lx_ap_peers[i].authorized) memcpy(lx_ap_state.peer, lx_ap_peers[i].address, 6);
 }
 lx_os_ap_trace("lx_os_ap_peer:lx_lock:release", 94, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("lx_os_ap_peer:lx_lock:released", 94, 0);
 if (associated != was_associated) hakios_apple80211_ap_event(associated ? 12 : 13, address);
 if (authorized && !was_authorized) hakios_apple80211_ap_event(20, address);
}

static int
lx_ap_apply_locked(const struct lx_ap_config *config)
{
	struct lx_ap_status before;
	hakios_wifi_ap_status(&before);
	if (!config && !before.active) return 0;
	if (!lx_ap_ops) return ENXIO;
	if (config) {
		if (!config->ssid_length || config->ssid_length > 32 || !((config->channel >= 1 && config->channel <= 13) ||
            (config->channel >= 36 && config->channel <= 48 && !(config->channel & 3)) ||
            (config->channel >= 149 && config->channel <= 165 && (config->channel - 149) % 4 == 0)) ||
		    config->security > 1 || (config->security && config->passphrase_length &&
		    (config->passphrase_length < 8 || config->passphrase_length > 63))) return EINVAL;
		int error = lx_ap_create_locked();
		if (error) return error;
		if (!before.active) lx_ap_sta_powered = hakios_sta_powered();
	}
	os_atomic_inc(&lx_ap_generation, relaxed);
	/* Publish configuration before the driver can accept its first station.
	 * configure() stops the old AP (including its leave notification), then
	 * starts beacons; its peer callbacks must survive completion of this ioctl.
	 * Keep active false until firmware startup has actually succeeded. */
	lx_os_ap_trace("lx_ap_apply_locked:lx_lock:wait", 121, 0); lck_mtx_lock(&lx_lock); lx_os_ap_trace("lx_ap_apply_locked:lx_lock:held", 121, 0);
	lx_ap_state.active = 0;
	if (config) {
		lx_ap_state.security = config->security; lx_ap_state.hidden = 0;
		lx_ap_state.channel = config->channel; lx_ap_state.ssid_length = config->ssid_length;
		bzero(lx_ap_state.ssid, sizeof(lx_ap_state.ssid));
		memcpy(lx_ap_state.ssid, config->ssid, config->ssid_length);
		memcpy(lx_ap_state.address, lx_ap_link.address, 6);
	}
	lx_os_ap_trace("lx_ap_apply_locked:lx_lock:release", 130, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("lx_ap_apply_locked:lx_lock:released", 130, 0);
	lx_os_ap_trace("configure:driver-enter", 0, config != NULL);
	int error = lx_ap_ops->configure(config);
	lx_os_ap_trace("configure:driver-exit", 0, error);
	lx_os_ap_trace("lx_ap_apply_locked:lx_lock:wait", 132, 0); lck_mtx_lock(&lx_lock); lx_os_ap_trace("lx_ap_apply_locked:lx_lock:held", 132, 0);
	if (!error && config) lx_ap_state.active = 1;
	else { bzero(&lx_ap_state, sizeof(lx_ap_state)); bzero(lx_ap_peers, sizeof(lx_ap_peers)); lx_ap_state.version = 1; }
	lx_os_ap_trace("lx_ap_apply_locked:lx_lock:release", 135, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("lx_ap_apply_locked:lx_lock:released", 135, 0);
	strlcpy(lx_ap_setting, config && !error ? "on" : "off", sizeof(lx_ap_setting));
	if (!config || error) hakios_sta_ask_power(lx_ap_sta_powered);
	if (error) hakios_wifi_tell("hotspot-ap: configure failed error=%d\n", error);
	/* ap1 mode/SSID/link notifications; en0 is still the single STA controller. */
	hakios_apple80211_ap_event(8, NULL);
	hakios_apple80211_ap_event(2, NULL);
	hakios_apple80211_ap_event(4, NULL);
	return error;
}

int
hakios_wifi_ap_configure(const struct lx_ap_config *config)
{
	lx_os_ap_trace("hakios_wifi_ap_configure:lx_mode_lock:wait", 149, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_configure:lx_mode_lock:held", 149, 0);
	int error = lx_ap_apply_locked(config);
	lx_os_ap_trace("hakios_wifi_ap_configure:lx_mode_lock:release", 151, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_configure:lx_mode_lock:released", 151, 0);
	return error;
}

int
hakios_wifi_ap_hidden(unsigned hidden)
{
 if (hidden > 1) return EINVAL;
 lx_os_ap_trace("hakios_wifi_ap_hidden:lx_mode_lock:wait", 159, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_hidden:lx_mode_lock:held", 159, 0);
 struct lx_ap_status status; hakios_wifi_ap_status(&status);
 int error = status.active && lx_ap_ops ? lx_ap_ops->set_hidden(hidden) : ENXIO;
 if (!error) { lx_os_ap_trace("hakios_wifi_ap_hidden:lx_lock:wait", 162, 0); lck_mtx_lock(&lx_lock); lx_os_ap_trace("hakios_wifi_ap_hidden:lx_lock:held", 162, 0); lx_ap_state.hidden = hidden; lx_os_ap_trace("hakios_wifi_ap_hidden:lx_lock:release", 162, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("hakios_wifi_ap_hidden:lx_lock:released", 162, 0); }
 lx_os_ap_trace("hakios_wifi_ap_hidden:lx_mode_lock:release", 163, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_hidden:lx_mode_lock:released", 163, 0);
 return error;
}

int
hakios_wifi_ap_delete(void)
{
	lx_os_ap_trace("hakios_wifi_ap_delete:lx_mode_lock:wait", 170, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_delete:lx_mode_lock:held", 170, 0);
	int error = lx_ap_apply_locked(NULL);
	if (!error) {
		lx_os_ap_trace("delete:net-detach-enter", 0, 0);
		error = hakios_net_ap_detach();
		lx_os_ap_trace("delete:net-detach-exit", 0, error);
		if (!error) lx_ap_attached = false;
	}
	lx_os_ap_trace("hakios_wifi_ap_delete:lx_mode_lock:release", 176, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("hakios_wifi_ap_delete:lx_mode_lock:released", 176, 0);
	return error;
}

void
lx_os_ap_register(const struct lx_ap_ops *ops)
{
	lx_os_ap_trace("lx_os_ap_register:lx_mode_lock:wait", 183, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("lx_os_ap_register:lx_mode_lock:held", 183, 0);
	if (!ops && lx_ap_ops) {
		lx_ap_ops->shutdown();
		strlcpy(lx_ap_setting, "off", sizeof(lx_ap_setting));
		lx_os_ap_trace("lx_os_ap_register:lx_lock:wait", 187, 0); lck_mtx_lock(&lx_lock); lx_os_ap_trace("lx_os_ap_register:lx_lock:held", 187, 0);
		bzero(&lx_ap_state, sizeof(lx_ap_state)); bzero(lx_ap_peers, sizeof(lx_ap_peers)); lx_ap_state.version = 1;
		lx_os_ap_trace("lx_os_ap_register:lx_lock:release", 189, 0); lck_mtx_unlock(&lx_lock); lx_os_ap_trace("lx_os_ap_register:lx_lock:released", 189, 0);
		os_atomic_inc(&lx_ap_generation, relaxed);
	}
	lx_ap_ops = ops;
	lx_os_ap_trace("lx_os_ap_register:lx_mode_lock:release", 193, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("lx_os_ap_register:lx_mode_lock:released", 193, 0);
}

static int
lx_ap_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	char setting[sizeof(lx_ap_setting)];
	struct lx_ap_config config = { 0 };
	unsigned at = 0;
	lx_os_ap_trace("lx_ap_sysctl:lx_mode_lock:wait", 203, 0); lck_mtx_lock(&lx_mode_lock); lx_os_ap_trace("lx_ap_sysctl:lx_mode_lock:held", 203, 0);
	strlcpy(setting, lx_ap_setting, sizeof(setting));
	int error = sysctl_handle_string(oidp, setting, sizeof(setting), req);
	if (error || !req->newptr) goto out;
	if (strcmp(setting, "off")) {
		while (at < 2 && setting[at] >= '0' && setting[at] <= '9')
			config.channel = config.channel * 10 + (unsigned)(setting[at++] - '0');
		if (setting[at++] != ' ' || config.channel < 1 || config.channel > 11) { error = EINVAL; goto out; }
		config.ssid_length = (unsigned)strlen(setting + at);
		if (!config.ssid_length || config.ssid_length > 32) { error = EINVAL; goto out; }
		for (unsigned i = 0; i < config.ssid_length; i++)
			if ((unsigned char)setting[at+i] < 0x20 || (unsigned char)setting[at+i] > 0x7e) { error = EINVAL; goto out; }
		memcpy(config.ssid, setting + at, config.ssid_length);
	}
	error = lx_ap_apply_locked(config.ssid_length ? &config : NULL);
out:
	lx_os_ap_trace("lx_ap_sysctl:lx_mode_lock:release", 219, 0); lck_mtx_unlock(&lx_mode_lock); lx_os_ap_trace("lx_ap_sysctl:lx_mode_lock:released", 219, 0);
	return error;
}

static int
lx_ap_status_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	struct lx_ap_status status;
	hakios_wifi_ap_status(&status);
	return SYSCTL_OUT(req, &status, sizeof(status));
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, wifi_ap, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, lx_ap_sysctl, "A", "AP control: off or legacy open channel SSID; native control uses Apple80211");
SYSCTL_PROC(_kern_hakios, OID_AUTO, wifi_ap_status, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, lx_ap_status_sysctl, "S,lx_ap_status", "AP state without credentials");
