#!/usr/bin/env python3
"""Run the actual shim decapsulator, AP ingress gate and TX peer selection on host."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

src = Path(__file__).resolve().parents[1] / 'overlays/wifi/lx/src'
linux = (src / 'lx_80211.c').read_text()
ap = (src / 'umi_wifi_ap.h').read_text()

def function(text, name):
    start = re.search(r'static (?:bool|void|struct lx_ap_peer \*)\n' + name + r'\(', text).start()
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define lx_os_ap_trace(stage,subject,value) ((void)(stage),(void)(subject),(void)(value))
#define ETH_ALEN 6
#define IEEE80211_TX_CTL_HW_80211_ENCAP 1
struct ieee80211_tx_info { unsigned flags; };
struct rx_status { unsigned flag; };
struct sk_buff { uint8_t *data; unsigned len; struct ieee80211_tx_info info; struct rx_status rx; };
#define IEEE80211_SKB_RXCB(s) (&(s)->rx)
#define RX_FLAG_DECRYPTED 1
#define IEEE80211_SKB_CB(s) (&(s)->info)
#define rcu_access_pointer(p) (p)
static uint8_t *skb_pull(struct sk_buff *s, unsigned n) {
    assert(n <= s->len); s->data += n; s->len -= n; return s->data;
}
struct ieee80211_sta { int unused; };
struct ieee80211_hw { int unused; };
struct ieee80211_tx_control { struct ieee80211_sta *sta; };
struct ops { void (*tx)(struct ieee80211_hw *, struct ieee80211_tx_control *, struct sk_buff *); };
static __attribute__((unused)) bool is_multicast_ether_addr(const uint8_t *a) { return a[0] & 1; }
static unsigned inputs, lock_depth, eapols;
static void lx_os_lock(void) { assert(!lock_depth++); }
static void lx_os_unlock(void) { assert(lock_depth-- == 1); }
static void lx_os_ap_input(const unsigned char *p, unsigned long n) {
    assert(!lock_depth && p && n >= 14); inputs++;
}
'''
prefix += re.search(r'enum \{ AP_RX,.*?AP_COUNT \};', linux, re.S).group() + '\n'
prefix += r'''
struct lx_ap_peer { struct ieee80211_sta *sta; uint8_t address[6]; bool associated, authorized; };
struct lx_radio {
    bool ap, ap_active, joined, ap_associated, ap_secure;
    unsigned ap_capacity; uint8_t ap_client[6]; uint64_t ap_count[AP_COUNT];
    struct lx_ap_peer ap_peers[8]; struct ieee80211_sta *sta; struct ops *ops; struct ieee80211_hw hw;
};
'''
prefix += '''static void ap_enqueue(struct lx_radio *r, const uint8_t *p, unsigned n, bool eapol, int signal) { assert(r && p && n >= 14 && eapol && !lock_depth); (void)signal; eapols++; }\n'''
body = '\n'.join([function(linux, 'to_ethernet'), function(linux, 'ap_peer_find'), function(linux, 'frame_send'), function(ap, 'ap_input')])
tests = r'''
static struct ieee80211_sta *selected;
static void tx(struct ieee80211_hw *h, struct ieee80211_tx_control *c, struct sk_buff *s) {
    assert(h && s); selected = c->sta;
}
int main(void) {
    const uint8_t a1[6] = {2,1,1,1,1,1}, a2[6] = {2,2,2,2,2,2};
    const uint8_t a3[6] = {2,3,3,3,3,3}, a4[6] = {2,4,4,4,4,4};
    /* All DS mappings, QoS/HT and retained/stripped CCMP header. */
    for (unsigned ds = 0; ds < 4; ds++) for (unsigned qos = 0; qos < 2; qos++)
    for (unsigned ht = 0; ht <= qos; ht++) for (unsigned iv = 0; iv < 3; iv++) {
        uint8_t frame[128] = {0};
        frame[0] = 8 | (qos ? 0x80 : 0); frame[1] = ds | (ht ? 0x80 : 0) | (iv ? 0x40 : 0);
        memcpy(frame+4,a1,6); memcpy(frame+10,a2,6); memcpy(frame+16,a3,6); memcpy(frame+24,a4,6);
        unsigned h = 24 + (ds == 3 ? 6 : 0) + qos*2 + ht*4 + (iv == 1 ? 8 : 0);
        memcpy(frame+h, "\xaa\xaa\x03\0\0\0\x08\0", 8);
        memcpy(frame+h+8, "hello", 5);
        struct sk_buff s = { .data=frame, .len=h+13 };
        assert(to_ethernet(&s, iv == 2) && s.len == 19);
        assert(!memcmp(s.data, ds & 1 ? a3 : a1, 6));
        assert(!memcmp(s.data+6, ds == 3 ? a4 : ds & 2 ? a3 : a2, 6));
        assert(!memcmp(s.data+12, "\x08\0hello", 7));
    }
    /* Allocate exactly the advertised size: ASan catches short-frame reads. */
    for (unsigned n = 0; n < 48; n++) {
        uint8_t *f = calloc(n ? n : 1, 1);
        if (n) f[0] = 0x88;
        if (n > 1) f[1] = 0xc3; /* 4addr + QoS + HT + retained IV */
        struct sk_buff s = { .data=f, .len=n };
        assert(!to_ethernet(&s, false)); free(f);
    }
    struct lx_radio r = { .ap=true, .ap_active=true, .joined=true, .ap_associated=true, .ap_secure=false, .ap_capacity=8 };
    r.ap_peers[0].sta = (struct ieee80211_sta *)&r.ap_peers[0]; memcpy(r.ap_peers[0].address, a2, 6); r.ap_peers[0].associated=true; r.ap_peers[0].authorized=true;
    memcpy(r.ap_client, a2, 6);
    uint8_t ethernet[14] = {0}; memcpy(ethernet+6, a2, 6); ethernet[12]=8;
    struct sk_buff s = { .data=ethernet, .len=14 };
    ap_input(&r, &s); assert(inputs == 1 && r.ap_count[AP_RX_EN3] == 1);
    r.ap_active=false; ap_input(&r,&s); assert(r.ap_count[AP_RX_INACTIVE] == 1); r.ap_active=true;
    ethernet[6]^=2; ap_input(&r,&s); assert(r.ap_count[AP_RX_SOURCE] == 1); ethernet[6]^=2;
    ethernet[12]=0x88; ethernet[13]=0x8e; ap_input(&r,&s); assert(r.ap_count[AP_RX_EAPOL] == 1 && eapols == 1);
    r.ap_active=false; ap_input(&r,&s); assert(eapols == 1); r.ap_active=true; /* EAPOL before authorization */
    ethernet[12]=8;ethernet[13]=0; ap_input(&r,&s);assert(inputs==2);
    r.ap_secure=true;ap_input(&r,&s);assert(inputs==2); /* plaintext blocked */
    s.rx.flag=RX_FLAG_DECRYPTED;ap_input(&r,&s);assert(inputs==3);
    s.len=0; ap_input(&r,&s); assert(r.ap_count[AP_RX_FORMAT] == 2 && inputs == 3); s.len=14;
    struct ieee80211_sta peer; struct ops ops = {.tx=tx}; r.ap_peers[0].sta=&peer; r.sta=&peer; r.ops=&ops; memcpy(ethernet, a2, 6); memcpy(s.data, ethernet, 14);
    s.info.flags=IEEE80211_TX_CTL_HW_80211_ENCAP;
    frame_send(&r,&s); assert(selected == &peer); /* unicast */
    memset(ethernet,255,6); frame_send(&r,&s); assert(!selected); /* DHCP broadcast */
    ethernet[0]=1; ethernet[1]=0; frame_send(&r,&s); assert(!selected); /* multicast */
    r.ap=false; memcpy(ethernet,a2,6); memcpy(s.data,ethernet,14); selected=NULL; frame_send(&r,&s); assert(selected == &peer); /* STA behavior */
    r.ap=true; s.info.flags=0; selected=NULL; frame_send(&r,&s); /* management path */
    puts("PASS real AP decapsulation/short bounds/ingress authorization/TX group peer (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-ap-data-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(prefix + body + tests)
    subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
