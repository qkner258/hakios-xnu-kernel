#!/usr/bin/env python3
"""Run the real Apple80211 dispatcher/AP queries and peer publication on host."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import argparse

ROOT = Path(__file__).resolve().parents[3]
OVER = ROOT / 'devices/xiaomi-mi-10/overlays'
parser = argparse.ArgumentParser()
parser.add_argument('--control', type=Path, default=OVER/'xnu/files/bsd/dev/umi_wifi_ap.inc')
args = parser.parse_args()

def function(s, name):
    m = re.search(r'(?:static )?(?:unsigned|errno_t|bool|int|void)\s+'+name+r'\(', s)
    a = m.start(); b = s.index('{', a); depth = 1; end = b+1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}'); end += 1
    return s[a:end]

h = (OVER/'xnu/files/bsd/sys/hakios.lx.h').read_text()
ap = (OVER/'xnu/files/bsd/dev/umi_apple80211_ap.inc').read_text()
apple = (OVER/'xnu/files/bsd/dev/hakios.apple80211.c').read_text()
control = args.control.read_text()
lx_ap = (OVER/'wifi/lx/src/umi_wifi_ap.h').read_text()
code = r'''
#include <assert.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
typedef int errno_t;
#define Z_WAITOK 0
#define Z_ZERO 0
#define CHANNEL_20MHZ 2
#define CHANNEL_2GHZ 8
#define CHANNEL_5GHZ 16
#define CHANNEL_ACTIVE 128
#define HAKIOS_STA_CHANNEL_PASSIVE 1
#define HAKIOS_STA_CHANNEL_RADAR 2
#define SIOCSA80211 0x802869c8u
#define SIOCGA80211 0xc02869c9u
#define lx_os_ap_trace(stage,subject,value) ((void)(stage),(void)(subject),(void)(value))
#define kalloc_data(n,flags) calloc(1,n)
#define kfree_data(p,n) free(p)
static pthread_mutex_t lx_lock=PTHREAD_MUTEX_INITIALIZER, lx_mode_lock=PTHREAD_MUTEX_INITIALIZER, apple80211_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned held;
static void lck_mtx_lock(pthread_mutex_t *p) { assert(!pthread_mutex_lock(p));held++; }
static void lck_mtx_unlock(pthread_mutex_t *p) { assert(held);held--;assert(!pthread_mutex_unlock(p)); }
'''
code += h[h.index('struct lx_ap_config {'):h.index('void lx_os_ap_register')]
code += apple[apple.index('struct apple80211req {'):apple.index('/* (as far as')].replace('user_addr_t','uintptr_t')
code += apple[apple.index('struct apple80211_channel {'):apple.index('/* CHANNELS_INFO')]
code += ap[ap.index('struct apple80211_softap_params {'):ap.index('_Static_assert')]
code += r'''
static struct lx_ap_status lx_ap_state={.version=1};
static struct { unsigned char address[6]; unsigned associated,authorized; } lx_ap_peers[LX_AP_STATIONS];
static unsigned events[256], driver_locks;
static void hakios_apple80211_ap_event(unsigned e,const unsigned char *p) { assert(!held && p);events[e]++; }
struct ath11k_sta { unsigned rssi_comb; };
struct ieee80211_sta { struct ath11k_sta drv_priv[1]; };
struct ath11k_base { struct { unsigned svc_map; } wmi_ab; };
struct ath11k { struct ath11k_base *ab; pthread_mutex_t data_lock; };
struct lx_ap_peer { struct ieee80211_sta *sta; bool authorized; unsigned char address[6];int signal; };
struct lx_radio {
 bool ap;pthread_mutex_t ap_mutex;unsigned ap_capacity;
 struct { void *priv; } hw;struct lx_ap_peer ap_peers[LX_AP_STATIONS];
};
static struct lx_radio *the_radio;
#define ATH11K_DEFAULT_NOISE_FLOOR (-95)
#define WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT 0
#define test_bit(bit,map) (((map)>>(bit))&1)
#define mutex_lock(p) (__atomic_fetch_add(&driver_locks,1,__ATOMIC_RELAXED), lck_mtx_lock(p))
#define mutex_unlock(p) lck_mtx_unlock(p)
#define spin_lock_bh(p) (__atomic_fetch_add(&driver_locks,1,__ATOMIC_RELAXED), lck_mtx_lock(p))
#define spin_unlock_bh(p) lck_mtx_unlock(p)
'''
code += function(lx_ap,'ap_stations')
code += r'''
static const struct lx_ap_ops ops={.stations=ap_stations};
static const struct lx_ap_ops *lx_ap_ops=&ops;
struct hakios_sta_channel { uint16_t frequency,flags; };
static unsigned hakios_sta_channels(struct hakios_sta_channel *out,unsigned most) {
 assert(!held && most>=8);
 struct hakios_sta_channel c[]={{2412,0},{2472,0},{2484,0},{5180,0},{5260,2},{5745,0},{5825,0},{5200,1}};
 memcpy(out,c,sizeof(c));return 8;
}
static void channel_fill(struct apple80211_channel *c,unsigned f,unsigned flags) {
 (void)flags;c->version=1;c->channel=(f<4000 ? f-2407 : f-5000)/5;
 c->flags=CHANNEL_ACTIVE|(f<4000?CHANNEL_2GHZ:CHANNEL_5GHZ);
}
static int copyout(const void *p,uintptr_t to,size_t n) { assert(!held);memcpy((void *)to,p,n);return 0; }
static int set(struct apple80211req *r,char *what,size_t n) {(void)r;(void)what;(void)n;return 0;}
static int get(struct apple80211req *r) {(void)r;return 0;}
static int apple_ap_set(const struct apple80211req *r) {(void)r;return 0;}
'''
code += apple[apple.index('static struct {\n\tint32_t type;'):apple.index('static void\nnote(')]
for source,name in [(apple,'give'),(control,'hakios_wifi_ap_status'),(control,'hakios_wifi_ap_stations'),(control,'lx_os_ap_peer'),(ap,'apple_ap_interface'),(ap,'apple_ap_get'),(apple,'note'),(apple,'hakios_apple80211_ioctl')]:
    code += '\n'+function(source,name)
code += r'''
static void *query_thread(void *arg) {
 (void)arg;unsigned char buf[2056];
 struct apple80211req r={.req_if_name="ap1",.req_type=42,.req_len=sizeof(buf),.req_data=(uintptr_t)buf};
 for(unsigned i=0;i<2000;i++) {
  assert(!hakios_apple80211_ioctl(SIOCGA80211,&r));
  assert(((uint32_t *)buf)[1]==128 && !held);
 }
 return NULL;
}
int main(void) {
 unsigned char buffer[4824];
 struct apple80211req r={.req_if_name="ap1",.req_type=42,.req_len=2056,.req_data=(uintptr_t)buffer};
 assert(!hakios_apple80211_ioctl(SIOCGA80211,&r));assert(!((uint32_t *)buffer)[1]);
 assert(!driver_locks && !lx_ap_state.active);
 struct ath11k_base base={0};struct ath11k ar={.ab=&base,.data_lock=PTHREAD_MUTEX_INITIALIZER};
 struct lx_radio radio={.hw={.priv=&ar},.ap_mutex=PTHREAD_MUTEX_INITIALIZER};the_radio=&radio;
 assert(!hakios_apple80211_ioctl(SIOCGA80211,&r) && !driver_locks);
 struct ieee80211_sta sta[LX_AP_STATIONS]={0};
 radio.ap=true;radio.ap_capacity=LX_AP_STATIONS;
 for(unsigned i=0;i<LX_AP_STATIONS;i++) {
  radio.ap_peers[i].sta=&sta[i];radio.ap_peers[i].authorized=true;
  memcpy(radio.ap_peers[i].address,(unsigned char[]){2,0,0,0,0,i},6);
  radio.ap_peers[i].signal=-30-(int)i;sta[i].drv_priv[0].rssi_comb=i&1?0:61;
 }
 struct lx_ap_station limited[7];
 assert(ap_stations(limited,7)==7 && limited[0].rssi==-34 && !held);
 base.wmi_ab.svc_map=1;sta[0].drv_priv[0].rssi_comb=(uint8_t)-42;
 assert(ap_stations(limited,1)==1 && limited[0].rssi==-42 && !held);
 base.wmi_ab.svc_map=0;sta[0].drv_priv[0].rssi_comb=61;
 for(unsigned i=0;i<LX_AP_STATIONS;i++) {
  unsigned char address[6]={2,0,0,0,0,i};lx_os_ap_peer(1,1,address);
 }
 assert(lx_ap_state.associated==128 && lx_ap_state.authorized==128);
 assert(!hakios_apple80211_ioctl(SIOCGA80211,&r));assert(((uint32_t *)buffer)[1]==128);
 assert(*(int32_t *)(buffer+8+127*16+12)==-157);
 /* The firmware queues a replacement ASSOC before delivering its old LEAVE.
  * A full host table must not use the sentinel as a writable slot. */
 unsigned char extra[6]={2,0,0,0,1,0};lx_os_ap_peer(1,1,extra);
 assert(lx_ap_state.authorized==128);
 lx_os_ap_peer(0,0,(unsigned char[]){2,0,0,0,0,0});
 lx_os_ap_peer(1,1,extra);assert(lx_ap_state.authorized==128);
 r.req_type=254;r.req_len=sizeof(buffer);
 assert(!hakios_apple80211_ioctl(SIOCGA80211,&r));assert(((uint32_t *)buffer)[1]==5);
 r.req_type=42;r.req_len=1;assert(!hakios_apple80211_ioctl(SIOCGA80211,&r));
 r.req_data=0;assert(!hakios_apple80211_ioctl(SIOCGA80211,&r));r.req_data=(uintptr_t)buffer;
 /* All page query numbers, including unknown queries, before/after AP start. */
 unsigned types[]={1,2,4,5,7,9,12,13,14,15,16,17,19,25,26,27,42,43,44,51,54,94,95,207,254,336,347,350,999};
 for(unsigned round=0;round<10000;round++) for(unsigned i=0;i<sizeof(types)/sizeof(*types);i++) {
  strcpy(r.req_if_name,round&1?"ap1":"en0");r.req_type=types[i];r.req_len=sizeof(buffer);
  (void)hakios_apple80211_ioctl(round&2?SIOCSA80211:SIOCGA80211,&r);
 }
 pthread_t threads[4];
 for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,query_thread,NULL));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 assert(!held && events[12]==129 && events[13]==1 && events[20]==129);
 puts("PASS actual Apple80211 dispatcher/AP queries: empty/full Get42, actual lx AP/STA guards/mutex/RSSI/ABI/copyout unlocked, Get254 filtering, peer overflow/reuse, 290000 mixed Get/Set requests + 8000 concurrent Get42/actual Apple80211 accounting lock (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-hotspot-queries-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
    subprocess.run([os.environ.get('CC','clang'),'-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
# Compile the real diagnostic deduper too: no driver/Apple80211 locks, repeated
# requests remain visible after each second and after arbitrarily many calls.
lx = (OVER/'xnu/files/bsd/dev/hakios.lx.c').read_text()
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
static unsigned long long now=1000000000ull, lines;
static unsigned long long lx_os_nanoseconds(void) {return now;}
static void *current_thread(void) {return (void *)1;}
static unsigned long long thread_tid(void *p) {assert(p);return 17;}
#define os_atomic_xchg(p,v,order) __atomic_exchange_n(p,v,__ATOMIC_RELAXED)
static void hakios_tl(const char *format,...) {(void)format;lines++;}
'''+function(lx,'lx_os_ap_trace')+r'''
int main(void) {
 for(unsigned i=0;i<100000;i++) lx_os_ap_trace("ioctl-get-en0-enter",254,4824);
 assert(lines==1);
 for(unsigned i=0;i<10000;i++) {
  now+=1000000000ull;
  lx_os_ap_trace("ioctl-get-en0-enter",254,4824);
 }
 assert(lines==10001);
 lx_os_ap_trace("ioctl-get-en0-enter",42,2056);assert(lines==10002);
 lx_os_ap_trace("ioctl-get-en0-enter",42,0);assert(lines==10003);
 puts("PASS actual lock-free USB probe deduper: 100000 duplicates, 10000 later windows, per-number/value changes, no lifetime budget");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-hotspot-trace-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
    subprocess.run(['clang','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
