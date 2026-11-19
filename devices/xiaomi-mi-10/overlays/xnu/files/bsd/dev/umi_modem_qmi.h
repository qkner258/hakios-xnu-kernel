/* EE=2 QMI/QMAP protocol pieces for the external X55 path, shared by the
 * kernel (umi_modem_mhi.inc sends them over QRTR on MHI IPCR 20/21) and the
 * host tests (tools/modem-tests/stage3.c). No transport dependency here. */
#ifndef UMI_MODEM_QMI_H
#define UMI_MODEM_QMI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct umi3_state {
	uint16_t transaction;
	bool uim_inserted, nas_registered, wds_bound, wds_up;
	/* Status codes only: UIM card/app state, NAS registration, CS/PS attach, first RAT. */
	uint8_t uim_card, uim_app, nas_reg, nas_cs, nas_ps, nas_rat;
	uint32_t wds_handle, address, gateway, mask, dns1, dns2, mtu;
};

static inline uint16_t umi3_le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static inline uint32_t umi3_le32(const uint8_t *p) { return umi3_le16(p) | (uint32_t)umi3_le16(p + 2) << 16; }
static inline void umi3_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void umi3_put32(uint8_t *p, uint32_t v) { umi3_put16(p, (uint16_t)v); umi3_put16(p + 2, (uint16_t)(v >> 16)); }

static inline int umi3_begin(uint8_t *out, size_t cap, uint16_t *transaction, uint16_t message, size_t *length)
{
	if (!out || !transaction || !length || cap < 7) return -1;
	*transaction = *transaction == 0xffff ? 1 : (uint16_t)(*transaction + 1);
	memset(out, 0, 7); out[1] = (uint8_t)*transaction; out[2] = (uint8_t)(*transaction >> 8);
	umi3_put16(out + 3, message); *length = 7; return 0;
}
static inline int umi3_tlv(uint8_t *out, size_t cap, size_t *length, uint8_t type, const void *value, size_t size)
{
	if (!out || !length || size > 0xffff || cap < 3 || size > cap - 3 || *length > cap - 3 - size) return -1;
	out[(*length)++] = type; umi3_put16(out + *length, (uint16_t)size); *length += 2;
	memcpy(out + *length, value, size); *length += size; return 0;
}
static inline void umi3_finish(uint8_t *out, size_t length) { umi3_put16(out + 5, (uint16_t)(length - 7)); }

static inline int umi3_uim_card_status(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	return umi3_begin(out, cap, txn, 0x002f, length) == 0 ? (umi3_finish(out, *length), 0) : -1;
}
static inline int umi3_nas_serving_system(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	return umi3_begin(out, cap, txn, 0x0024, length) == 0 ? (umi3_finish(out, *length), 0) : -1;
}
static inline int umi3_nas_system_info(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	return umi3_begin(out, cap, txn, 0x004d, length) == 0 ? (umi3_finish(out, *length), 0) : -1;
}
static inline int umi3_nas_register(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	const uint8_t value[4] = { 1, 1, 0, 1 };
	if (umi3_begin(out, cap, txn, 0x0022, length) || umi3_tlv(out, cap, length, 1, value, sizeof(value))) return -1;
	umi3_finish(out, *length); return 0;
}
/* DMS operating mode: 0 online, 1 low power, 3 offline, 4 resetting, 5 shutting down. */
static inline int umi3_dms_get_operating_mode(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	return umi3_begin(out, cap, txn, 0x002d, length) == 0 ? (umi3_finish(out, *length), 0) : -1;
}
static inline int umi3_dms_set_operating_mode(uint8_t *out, size_t cap, uint16_t *txn, uint8_t mode, size_t *length)
{
	if (umi3_begin(out, cap, txn, 0x002e, length) || umi3_tlv(out, cap, length, 1, &mode, 1)) return -1;
	umi3_finish(out, *length); return 0;
}
static inline int umi3_wds_bind(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	/* libqmi PCIe=3; ModemManager mhi_net uses interface 4 (IP_HW0). */
	const uint8_t endpoint[8] = { 3, 0, 0, 0, 4, 0, 0, 0 }, mux = 1;
	if (umi3_begin(out, cap, txn, 0x00a2, length) || umi3_tlv(out, cap, length, 0x10, endpoint, sizeof(endpoint)) ||
	    umi3_tlv(out, cap, length, 0x11, &mux, 1)) return -1;
	umi3_finish(out, *length); return 0;
}

/* Basic QMAP (5), without checksum extensions. Same PCIe/mux route as
 * umi-linux's rmnet path; no AP-side IPA or DPM port is involved. */
static inline int umi3_wda_format(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	const uint8_t endpoint[8] = { 3, 0, 0, 0, 4, 0, 0, 0 };
	const uint8_t types[] = { 0x11, 0x12, 0x13, 0x15, 0x16 };
	const uint32_t values[] = { 2, 5, 5, 32, 32768 };
	uint8_t v[4];
	if (umi3_begin(out, cap, txn, 0x0020, length)) return -1;
	for (unsigned i = 0; i < sizeof(types); i++) {
		umi3_put32(v, values[i]);
		if (umi3_tlv(out, cap, length, types[i], v, 4)) return -1;
	}
	if (umi3_tlv(out, cap, length, 0x17, endpoint, 8)) return -1;
	umi3_finish(out, *length); return 0;
}
static inline int umi3_wds_settings(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	/* DNS (bit 4), IPv4 address (8), gateway/mask (9), MTU (13). */
	const uint8_t requested[4] = { 0x10, 0x23, 0, 0 };
	if (umi3_begin(out, cap, txn, 0x002d, length) || umi3_tlv(out, cap, length, 0x10, requested, 4)) return -1;
	umi3_finish(out, *length); return 0;
}
static inline int umi3_wds_family(uint8_t *out, size_t cap, uint16_t *txn, size_t *length)
{
	const uint8_t family = 4;
	if (umi3_begin(out, cap, txn, 0x004d, length) || umi3_tlv(out, cap, length, 1, &family, 1)) return -1;
	umi3_finish(out, *length); return 0;
}
static inline int umi3_wds_start(uint8_t *out, size_t cap, uint16_t *txn, const char *apn, size_t *length)
{
	const uint8_t family = 4; size_t n;
	if (!apn || (n = strlen(apn)) == 0 || n > 63 || umi3_begin(out, cap, txn, 0x0020, length) ||
	    umi3_tlv(out, cap, length, 0x19, &family, 1) || umi3_tlv(out, cap, length, 0x14, apn, n)) return -1;
	umi3_finish(out, *length); return 0;
}

static inline int umi3_find_tlv(const uint8_t *m, size_t length, uint8_t type, const uint8_t **value, size_t *size)
{
	if (!m || length < 7 || umi3_le16(m + 5) != length - 7) return -1;
	for (size_t at = 7; at + 3 <= length;) {
		size_t n = umi3_le16(m + at + 1); at += 3;
		if (n > length - at) return -1;
		if (m[at - 3] == type) { if (value) *value = m + at; if (size) *size = n; return 0; }
		at += n;
	}
	return -1;
}
static inline int umi3_dms_operating_mode(const uint8_t *m, size_t length, uint8_t *mode)
{
	const uint8_t *v; size_t n;
	if (!m || !mode || length < 7 || m[0] != 2 || umi3_le16(m + 3) != 0x002d || umi3_find_tlv(m, length, 1, &v, &n) || n < 1) return -1;
	*mode = v[0]; return 0;
}
static inline int umi3_result(const uint8_t *m, size_t length, uint16_t *result, uint16_t *error)
{
	const uint8_t *v; size_t n;
	if (!m || length < 7 || m[0] != 2 || umi3_find_tlv(m, length, 2, &v, &n) || n < 4) return -1;
	if (result) *result = umi3_le16(v); if (error) *error = umi3_le16(v + 2); return 0;
}
static inline int umi3_apply_uim(struct umi3_state *state, const uint8_t *m, size_t length)
{
	const uint8_t *v; size_t n; uint16_t result, error;
	if (!state || !m || length < 7 || umi3_le16(m + 3) != 0x002f || umi3_result(m, length, &result, &error) || result || error ||
	    umi3_find_tlv(m, length, 0x10, &v, &n) || n < 10) return -1;
	/* v[8] slot count; slot 1: v[9] card state, v[14] app count; app 1: v[16] state (7 ready). */
	state->uim_card = v[8] ? v[9] : 0;
	state->uim_app = v[8] && n >= 17 && v[14] ? v[16] : 0;
	state->uim_inserted = v[8] != 0 && v[9] == 1; return 0;
}
static inline int umi3_apply_nas(struct umi3_state *state, const uint8_t *m, size_t length)
{
	const uint8_t *v; size_t n; uint16_t result, error; uint16_t message;
	if (!state || !m || length < 7 || umi3_result(m, length, &result, &error) || result || error) return -1;
	message = umi3_le16(m + 3);
	if (message == 0x0024 && !umi3_find_tlv(m, length, 1, &v, &n) && n >= 5) {
		state->nas_reg = v[0]; state->nas_cs = v[1]; state->nas_ps = v[2];
		state->nas_rat = v[4] && n >= 6 ? v[5] : 0;
		state->nas_registered = v[0] == 1;
	}
	else if (message == 0x004d && !umi3_find_tlv(m, length, 0x14, &v, &n) && n >= 3) state->nas_registered = v[0] == 2 || v[2] != 0;
	return 0;
}
static inline int umi3_apply_wds(struct umi3_state *state, const uint8_t *m, size_t length)
{
	const uint8_t *v; size_t n; uint16_t result, error; uint16_t message;
	if (!state || !m || length < 7 || umi3_result(m, length, &result, &error) || result || error) return -1;
	message = umi3_le16(m + 3);
	if (message == 0x00a2) state->wds_bound = true;
	else if (message == 0x0020 && !umi3_find_tlv(m, length, 1, &v, &n) && n >= 4) {
		state->wds_handle = umi3_le32(v); state->wds_up = state->wds_handle != 0;
	} else if (message == 0x002d) {
		if (!umi3_find_tlv(m, length, 0x1e, &v, &n) && n >= 4) state->address = umi3_le32(v);
		if (!umi3_find_tlv(m, length, 0x20, &v, &n) && n >= 4) state->gateway = umi3_le32(v);
		if (!umi3_find_tlv(m, length, 0x21, &v, &n) && n >= 4) state->mask = umi3_le32(v);
		if (!umi3_find_tlv(m, length, 0x15, &v, &n) && n >= 4) state->dns1 = umi3_le32(v);
		if (!umi3_find_tlv(m, length, 0x16, &v, &n) && n >= 4) state->dns2 = umi3_le32(v);
		if (!umi3_find_tlv(m, length, 0x29, &v, &n) && n >= 4) state->mtu = umi3_le32(v);
	}
	return 0;
}
static inline int umi3_qmap_next(const uint8_t *frame, size_t length, size_t *at, uint8_t *mux, const uint8_t **payload, size_t *payload_length)
{
	size_t p, n, pad;
	if (!frame || !at || *at > length || length - *at < 4) return -1;
	p = *at; pad = frame[p] & 0x3f; n = ((size_t)frame[p + 2] << 8) | frame[p + 3];
	if (!n || pad > n || n > length - p - 4) return -1;
	if (mux) *mux = frame[p + 1]; if (payload) *payload = frame + p + 4; if (payload_length) *payload_length = n - pad;
	*at = p + 4 + n; return 0;
}

/* Host servreg locator (QMI service 64), the one host service Linux always
 * ran (in-kernel qcom_pd_mapper). GET_DOMAIN_LIST (0x21) is answered from
 * Linux's SM8250 table: success, db_rev 1, total, and entries {u8 len, name,
 * le32 instance, u8 data_valid, le32 data}. Unknown names get zero domains.
 * Returns the number of domains listed, or -1 for any other message. */
static const struct { const char *service, *domain; uint32_t instance; } umi3_servreg_table[] = {
	{ "tms/servreg", "msm/adsp/audio_pd", 74 }, { "avs/audio", "msm/adsp/audio_pd", 74 },
	{ "tms/servreg", "msm/adsp/root_pd", 74 }, { "tms/servreg", "msm/cdsp/root_pd", 76 },
	{ "tms/servreg", "msm/slpi/root_pd", 90 }, { "tms/servreg", "msm/slpi/sensor_pd", 90 },
};
static inline int umi3_servreg_domains(const uint8_t *req, size_t req_length, uint8_t *out, size_t cap, size_t *length)
{
	const uint8_t *name, *v; size_t n, vn, at = 1; uint32_t offset = 0; uint16_t total = 0;
	uint8_t ok[4] = { 0 }, value[2], list[200] = { 0 };
	if (!req || req_length < 7 || req[0] != 0 || umi3_le16(req + 3) != 0x0021 || cap < 7 ||
	    umi3_find_tlv(req, req_length, 1, &name, &n)) return -1;
	if (!umi3_find_tlv(req, req_length, 0x10, &v, &vn) && vn == 4) offset = umi3_le32(v);
	for (size_t i = 0; i < sizeof(umi3_servreg_table) / sizeof(umi3_servreg_table[0]); i++) {
		const char *domain = umi3_servreg_table[i].domain; size_t dn = strlen(domain);
		if (strlen(umi3_servreg_table[i].service) != n || memcmp(umi3_servreg_table[i].service, name, n)) continue;
		if (total++ < offset) continue;
		if (at + 10 + dn > sizeof(list)) return -1;
		list[at++] = (uint8_t)dn; memcpy(list + at, domain, dn); at += dn;
		umi3_put32(list + at, umi3_servreg_table[i].instance); at += 4;
		list[at++] = 0; umi3_put32(list + at, 0); at += 4; list[0]++;
	}
	memcpy(out, req, 7); out[0] = 2; *length = 7;
	if (umi3_tlv(out, cap, length, 2, ok, sizeof(ok))) return -1;
	umi3_put16(value, total);
	if (umi3_tlv(out, cap, length, 0x10, value, 2)) return -1;
	umi3_put16(value, 1);
	if (umi3_tlv(out, cap, length, 0x11, value, 2)) return -1;
	if (total && umi3_tlv(out, cap, length, 0x12, list, at)) return -1;
	umi3_finish(out, *length); return list[0];
}
#endif
