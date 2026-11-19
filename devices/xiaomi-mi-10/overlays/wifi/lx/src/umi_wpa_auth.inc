/* AP arithmetic, included by lx_wpa.c so the STA and AP use the same tested
 * SHA1/PRF/AES implementation. No driver, entropy source, clocks or logging. */
const unsigned char lx_wpa_ap_rsn[22] = {
	48, 20, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 4,
	1, 0, 0, 15, 172, 2, 0, 0 /* CCMP group/pairwise, PSK; no PMF */
};

int
lx_wpa_auth_rsn(const unsigned char *rsn, unsigned length)
{
	/* One negotiated pairwise cipher and AKM, both supported; no cached PMKID.
	 * MFPC clients may join, MFPR clients must not: we advertise no PMF. */
	if (length < 22 || length > 64 || rsn[0] != 48 || rsn[1] + 2u != length ||
	    memcmp(rsn + 2, lx_wpa_ap_rsn + 2, 18) || (rsn[20] & 0x43) ||
	    (rsn[21] & 0x3e)) return 0;
	if (length == 22) return 1;
	if (length < 24 || rsn[22] || rsn[23]) return 0;
	return length == 24 || (length == 28 && !memcmp(rsn + 24, "\x00\x0f\xac\x06", 4));
}

static void
aes_encrypt(const uint8_t keys[176], uint8_t s[16])
{
	for (unsigned i = 0; i < 16; i++) s[i] ^= keys[i];
	for (unsigned round = 1; round <= 10; round++) {
		uint8_t t[16];
		for (unsigned c = 0; c < 4; c++)
			for (unsigned r = 0; r < 4; r++)
				t[4*c+r] = sbox[s[4*((c+r)%4)+r]];
		if (round < 10) {
			for (unsigned c = 0; c < 4; c++) {
				const uint8_t *a = t + 4*c;
				s[4*c] = times(a[0], 2) ^ times(a[1], 3) ^ a[2] ^ a[3];
				s[4*c+1] = a[0] ^ times(a[1], 2) ^ times(a[2], 3) ^ a[3];
				s[4*c+2] = a[0] ^ a[1] ^ times(a[2], 2) ^ times(a[3], 3);
				s[4*c+3] = times(a[0], 3) ^ a[1] ^ a[2] ^ times(a[3], 2);
			}
		} else memcpy(s, t, 16);
		for (unsigned i = 0; i < 16; i++) s[i] ^= keys[16*round+i];
	}
}

static void
aes_wrap(const uint8_t kek[16], const uint8_t *plain, unsigned length, uint8_t *wrapped)
{
	uint8_t keys[176], block[16];
	unsigned n = length / 8;
	aes_keys(kek, keys);
	memset(wrapped, 0xa6, 8);
	memcpy(wrapped + 8, plain, length);
	for (unsigned j = 0; j < 6; j++) for (unsigned i = 1; i <= n; i++) {
		uint64_t t = (uint64_t)n*j + i;
		memcpy(block, wrapped, 8);
		memcpy(block + 8, wrapped + 8*i, 8);
		aes_encrypt(keys, block);
		for (unsigned k = 0; k < 8; k++) wrapped[k] = block[k] ^ (uint8_t)(t >> (56-8*k));
		memcpy(wrapped + 8*i, block + 8, 8);
	}
	wpa_wipe(keys, sizeof(keys)); wpa_wipe(block, sizeof(block));
}

void
lx_wpa_auth_start(struct lx_wpa_auth *a, const unsigned char pmk[32],
    const unsigned char ap[6], const unsigned char station[6],
    const unsigned char anonce[32], const unsigned char gtk[16],
    const unsigned char replay[8], const unsigned char *rsn, unsigned rsn_length)
{
	wpa_wipe(a, sizeof(*a));
	if (!lx_wpa_auth_rsn(rsn, rsn_length)) return;
	struct lx_wpa *k = &a->keys;
	memcpy(k->pmk, pmk, 32); memcpy(k->own, ap, 6); memcpy(k->peer, station, 6);
	memcpy(k->own_nonce, anonce, 32); memcpy(k->group, gtk, 16);
	k->group_length = k->pairwise_length = 16;
	k->group_index = 1; k->descriptor_version = 2;
	memcpy(a->rsn, rsn, rsn_length); a->rsn_length = rsn_length;
	uint8_t *m = a->m1;
	m[0] = 2; m[1] = 3; m[3] = 95; m[4] = 2;
	m[6] = 0x8a; m[8] = 16;
	memcpy(m + AT_REPLAY, replay, 8); memcpy(m + AT_NONCE, anonce, 32);
	a->state = 1;
}

int
lx_wpa_auth_message(struct lx_wpa_auth *a, const unsigned char *m, unsigned length,
    unsigned char *reply, unsigned *reply_length)
{
	*reply_length = 0;
	if (!a->state || length < 4 || m[1] != 3) return 0;
	unsigned body = (unsigned)m[2] << 8 | m[3];
	if (body < 95 || body + 4 > length || body + 4 > MESSAGE_MOST ||
	    (m[0] != 1 && m[0] != 2) || m[4] != 2) return -1;
	length = body + 4;
	unsigned info = (unsigned)m[5] << 8 | m[6];
	unsigned n = (unsigned)m[97] << 8 | m[98];
	if (AT_DATA + n != length || m[7] || (m[8] != 0 && m[8] != 16)) return -1;
	struct lx_wpa candidate;
	uint8_t mic[16]; unsigned diff = 0;
	if (info == 0x010a) { /* message 2 */
		if (a->state == 3) return 0;
		if (memcmp(m + AT_REPLAY, a->m1 + AT_REPLAY, 8) || n < a->rsn_length ||
		    memcmp(m + AT_DATA, a->rsn, a->rsn_length)) return -1;
		for (unsigned at = a->rsn_length; at < n;) {
			if (n-at < 2 || m[AT_DATA+at+1] > n-at-2) return -1;
			at += 2 + m[AT_DATA+at+1];
		}
		unsigned nonzero = 0;
		for (unsigned i = 0; i < 32; i++) nonzero |= m[AT_NONCE+i];
		if (!nonzero) return -1;
		candidate = a->keys;
		memcpy(candidate.peer_nonce, m + AT_NONCE, 32);
		derive(&candidate);
		mic_of(&candidate, m, length, mic);
		for (unsigned i = 0; i < 16; i++) diff |= mic[i] ^ m[AT_MIC+i];
		if (diff || (a->state == 2 && memcmp(m + AT_NONCE, a->keys.peer_nonce, 32))) {
			wpa_wipe(&candidate, sizeof(candidate)); return -1;
		}
		if (a->state == 1) {
			a->keys = candidate;
			uint8_t plain[48] = { 0 }, wrapped[56], counter[99];
			memcpy(plain, lx_wpa_ap_rsn, 22);
			memcpy(plain + 22, "\xdd\x16\x00\x0f\xac\x01\x01\x00", 8);
			plain[28] = (uint8_t)a->keys.group_index;
			memcpy(plain + 30, a->keys.group, 16);
			plain[46] = 0xdd; /* IEEE 802.11 padding to AES-wrap multiple of 8 */
			aes_wrap(a->keys.kek, plain, sizeof(plain), wrapped);
			memcpy(counter, a->m1, sizeof(counter));
			for (int i = 16; i >= 9; i--) if (++counter[i]) break;
			answer(&a->keys, counter, 0x13ca, a->keys.own_nonce,
			    wrapped, sizeof(wrapped), a->m3, &a->m3_length);
			a->m3[8] = 16;
			mic_of(&a->keys, a->m3, a->m3_length, a->m3 + AT_MIC);
			wpa_wipe(plain, sizeof(plain)); wpa_wipe(wrapped, sizeof(wrapped));
			a->state = 2;
		}
		wpa_wipe(&candidate, sizeof(candidate));
		memcpy(reply, a->m3, a->m3_length); *reply_length = a->m3_length;
		return 1;
	}
	if (info != 0x030a || a->state < 2 || n ||
	    memcmp(m + AT_REPLAY, a->m3 + AT_REPLAY, 8)) return -1;
	mic_of(&a->keys, m, length, mic);
	for (unsigned i = 0; i < 16; i++) diff |= mic[i] ^ m[AT_MIC+i];
	if (diff) return -1;
	if (a->state == 3) return 0;
	a->state = 3;
	return 2;
}

/* The AP keeps transmitting with the old GTK until every authorized peer has
 * acknowledged the new one. Retransmissions use this identical cached G1. */
void
lx_wpa_auth_group_start(struct lx_wpa_auth *a, const unsigned char gtk[16], unsigned index)
{
 uint8_t plain[24] = { 0xdd, 22, 0, 15, 172, 1, 0, 0 }, wrapped[32];
 uint8_t counter[99] = { 0 };
 plain[6] = (uint8_t)index;
 memcpy(plain + 8, gtk, 16);
 aes_wrap(a->keys.kek, plain, sizeof(plain), wrapped);
 memcpy(counter, a->group_length ? a->group_message : a->m3, 99);
 for (int i = 16; i >= 9; i--) if (++counter[i]) break;
 answer(&a->keys, counter, 0x1382, NULL, wrapped, sizeof(wrapped),
     a->group_message, &a->group_length);
 a->group_message[8] = 16;
 mic_of(&a->keys, a->group_message, a->group_length, a->group_message + AT_MIC);
 a->group_pending = 1;
 wpa_wipe(plain, sizeof(plain)); wpa_wipe(wrapped, sizeof(wrapped));
}

int
lx_wpa_auth_group_message(struct lx_wpa_auth *a, const unsigned char *m, unsigned length)
{
 if (!a->group_pending || length < 4 || m[1] != 3) return 0;
 if (length < 99 || m[2] || m[3] != 95 || (m[0] != 1 && m[0] != 2) ||
     m[4] != 2 || m[5] != 3 || m[6] != 2 || m[7] ||
     (m[8] != 0 && m[8] != 16) || m[97] || m[98]) return -1;
 if (memcmp(m + AT_REPLAY, a->group_message + AT_REPLAY, 8)) return 0;
 uint8_t mic[16]; unsigned diff = 0;
 mic_of(&a->keys, m, 99, mic);
 for (unsigned i = 0; i < 16; i++) diff |= mic[i] ^ m[AT_MIC+i];
 if (diff) return -1;
 a->group_pending = 0;
 return 1;
}
