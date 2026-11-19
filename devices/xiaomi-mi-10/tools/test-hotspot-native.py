#!/usr/bin/env python3
"""Exercise actual native ioctl decoder and passive DHCP/DNS observer on host."""
from pathlib import Path
import os, re, subprocess, tempfile, plistlib
ROOT=Path(__file__).resolve().parents[3]
OVER=ROOT/'devices/xiaomi-mi-10/overlays'
def function(s,name):
    m=re.search(r'(?:static )?(?:bool|int|void)\s+'+name+r'\(',s);a=m.start();b=s.index('{',a);d=1;e=b+1
    while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
    return s[a:e]
ap=(OVER/'xnu/files/bsd/dev/umi_apple80211_ap.inc').read_text()
abi=ap[:ap.index('/* Pure decoder')]
# Public shim structures as compiled, not a test-side credential decoder.
h=(OVER/'xnu/files/bsd/sys/hakios.lx.h').read_text()
config=h[h.index('struct lx_ap_config {'):h.index('struct lx_ap_status {')]
code='''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>
#define CHANNEL_5GHZ 0x10
#define CHANNEL_2GHZ 0x8
#define CIPHER_PASSPHRASE 10
#define CIPHER_PMK 6
struct apple80211_channel { uint32_t version, channel, flags; };
'''+config+abi+function(ap,'apple_ap_decode')
s=(OVER/'userland/src/hakios_hotspot_native.c').read_text()
code+='\nstatic bool dns_seen;static uint32_t last_lease;\n'+function(s,'packet')
code+='''
int main(void) {
 struct apple80211_hostap a={.version=1,.mode=2,.auth_lower=1,.auth_upper=8,
  .ssid_length=5,.ssid={0xe4,0xb8,0xad,0x41,0x42},.key_cipher=10,.key_length=8,.key={'p','a','s','s','w','o','r','d'}};
 struct lx_ap_config c={0};
 assert(!apple_ap_decode(&a,&c) && c.security==1 && c.channel==6 && c.passphrase_length==8 && !memcmp(c.ssid,a.ssid,5));
 a.key_length=0;assert(apple_ap_decode(&a,&c)==EINVAL);a.key_length=8;
 a.auth_upper=0;assert(apple_ap_decode(&a,&c)==EINVAL);a.auth_upper=10;assert(apple_ap_decode(&a,&c)==EINVAL);a.auth_upper=8;
 a.key[0]=0;assert(apple_ap_decode(&a,&c)==EINVAL);a.key[0]='p';
 a.channel.channel=36;assert(!apple_ap_decode(&a,&c) && c.channel==36);a.channel.channel=6;
 a.channel.flags=CHANNEL_5GHZ;assert(apple_ap_decode(&a,&c)==EINVAL);a.channel.flags=0;
 a.key_cipher=6;a.key_length=32;memset(a.key,0x80,32);memset(&c,0,sizeof(c));
 assert(!apple_ap_decode(&a,&c)&&!c.passphrase_length&&!memcmp(c.pmk,a.key,32));
 a.ssid_length=33;assert(apple_ap_decode(&a,&c)==EINVAL);a.ssid_length=32;
 a.version=0;assert(apple_ap_decode(&a,&c)==EINVAL);
 unsigned char f[42+244]={0};f[12]=8;f[14]=0x45;f[23]=17;f[34]=0;f[35]=67;f[36]=0;f[37]=68;
 unsigned char *b=f+42;b[0]=2;b[1]=1;b[2]=6;b[16]=192;b[17]=168;b[18]=2;b[19]=2;
 b[28]=2;b[29]=4;memcpy(b+236,"\\x63\\x82\\x53\\x63",4);b[240]=53;b[241]=1;b[242]=5;b[243]=255;
 for(unsigned n=0;n<sizeof(f)-2;n++){unsigned char *p=malloc(n?n:1);memcpy(p,f,n);packet(p,n);free(p);assert(!last_lease);}
 packet(f,sizeof(f));assert(last_lease);last_lease=0;b[242]=2;packet(f,sizeof(f));assert(!last_lease);
 f[35]=53;packet(f,42);assert(dns_seen);
 unsigned seed=6390;
 for(unsigned i=0;i<100000;i++){unsigned n=i%512;unsigned char *p=malloc(n?n:1);for(unsigned j=0;j<n;j++){seed=1664525*seed+1013904223;p[j]=seed>>24;}packet(p,n);free(p);}
 puts("PASS native HOST_AP ABI/WPA2-only decoding/SSID/PMK/channel and observed DHCP/DNS/truncated/malformed packets (ASan/UBSan)");
}
'''
code='#include <stdlib.h>\n'+code
with tempfile.TemporaryDirectory(prefix='umi-hotspot-native-') as tmp:
    c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
    subprocess.run(['clang','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Exercise the real control completion against a driver that receives a station
# before configure() returns. Completing startup must not erase that station.
control=(OVER/'xnu/files/bsd/dev/umi_wifi_ap.inc').read_text()
code='''#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define lx_os_ap_trace(stage,subject,value) ((void)0)
#define lck_mtx_lock(p) ((void)(p))
#define lck_mtx_unlock(p) ((void)(p))
#define os_atomic_inc(p,order) (++*(p))
'''+h[h.index('struct lx_ap_config {'):h.index('void lx_os_ap_register')]+'''
static int lx_lock;
static unsigned lx_ap_generation;
static bool lx_ap_sta_powered, powered;
static char lx_ap_setting[40]="off";
static struct lx_ap_status lx_ap_state={.version=1};
static struct { unsigned char address[6]; } lx_ap_link={{2,4,6,8,10,12}};
static unsigned creates, configures, restores, events[256];
static struct { unsigned char address[6]; unsigned associated, authorized; } lx_ap_peers[LX_AP_STATIONS];
static int create_error, configure_error;
static int lx_ap_create_locked(void) { creates++;return create_error; }
static bool hakios_sta_powered(void) { return powered; }
static void hakios_sta_ask_power(bool p) { restores++;powered=p; }
#define hakios_wifi_tell(...) ((void)0)
static void hakios_apple80211_ap_event(unsigned e,const unsigned char *p) { (void)p;events[e]++; }
'''+function(control,'hakios_wifi_ap_status')+function(control,'lx_os_ap_peer')+'''
static int configure(const struct lx_ap_config *c) {
 configures++;assert(!lx_ap_state.active);
 if(lx_ap_state.associated) lx_os_ap_peer(0,0,lx_ap_link.address);
 if(c && !configure_error) {
  assert(lx_ap_state.ssid_length==c->ssid_length && !memcmp(lx_ap_state.ssid,c->ssid,c->ssid_length));
  lx_os_ap_peer(1,0,lx_ap_link.address);lx_os_ap_peer(1,1,lx_ap_link.address);
 }
 return configure_error;
}
static const struct lx_ap_ops ops={.configure=configure};
static const struct lx_ap_ops *lx_ap_ops=&ops;
'''+function(control,'lx_ap_apply_locked')+'''
int main(void) {
 struct lx_ap_config c={.ssid={'a','b'},.ssid_length=2,.channel=6,.security=1};
 assert(!lx_ap_apply_locked(NULL) && !configures && !restores && !creates);
 powered=true;
 assert(!lx_ap_apply_locked(&c) && lx_ap_state.active && lx_ap_state.authorized && lx_ap_state.associated);
 assert(!memcmp(lx_ap_state.peer,lx_ap_link.address,6) && !restores);
 assert(events[12]==1 && events[20]==1 && !events[13]);
 assert(!lx_ap_apply_locked(&c) && lx_ap_state.authorized && events[13]==1 && events[12]==2);
 assert(!lx_ap_apply_locked(NULL) && !lx_ap_state.active && !lx_ap_state.associated && restores==1 && powered);
 unsigned count=configures;
 assert(!lx_ap_apply_locked(NULL) && configures==count && restores==1);
 powered=false;configure_error=EIO;
 assert(lx_ap_apply_locked(&c)==EIO && !lx_ap_state.active && !lx_ap_state.authorized && !powered && restores==2);
 configure_error=0;create_error=ENXIO;count=configures;
 assert(lx_ap_apply_locked(&c)==ENXIO && configures==count && restores==2);
 puts("PASS actual AP control: early association survives startup, restart/leave, already-off inert, original STA power, failed startup/creation");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-hotspot-control-') as tmp:
    c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
    subprocess.run(['clang','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Generate jobs in a temporary directory using the actual current generator.
with tempfile.TemporaryDirectory(prefix='umi-hotspot-jobs-') as tmp:
    subprocess.run(['python3',str(OVER/'userland/mkjobs.py'),'--rest',str(ROOT/'sysimage/rootfs'),tmp,
        'com.apple.wifid','com.apple.MobileInternetSharing'],check=True,stdout=subprocess.DEVNULL)
    jobs=Path(tmp);bootpd=plistlib.loads((jobs/'bootps.plist').read_bytes())
    assert bootpd['Disabled'] and bootpd['Label']=='com.apple.bootpd'
    assert bootpd['Sockets']['Listeners']['ReceivePacketInfo'] and bootpd['inetdCompatibility']['Wait']
    assert plistlib.loads((jobs/'com.apple.wifid.plist').read_bytes())['Umask']==0o077
    for name in ['com.apple.pfd','com.apple.mDNSResponder']:
        assert (jobs/(name+'.plist')).exists(),name
p=plistlib.loads((OVER/'userland/com.hakios.hotspot-native.plist').read_bytes())
assert p['MachServices']=={'com.apple.wifi.hostapd':True} and 'RunAtLoad' not in p
assert p['ProgramArguments']==['/redwood/bin/hakios-hotspot-native']
print('PASS native jobs: pfd/mDNSResponder, misd-controlled disabled socket bootpd, private wifid umask, demand-only kernel authenticator service')
# AP capability contract: HT/WMM/TIM, non-DFS 5 GHz and firmware-derived peer capacity.
ap_src = (OVER/'wifi/lx/src/umi_wifi_ap.h').read_text()
assert 'ap_wmm[]' in ap_src and '*p++ = 45' in ap_src and '*p++ = 61' in ap_src
assert 'ap_capacity = min_t(unsigned, ar->max_num_stations, AP_PEERS)' in ap_src
assert 'WMI_PDEV_PEER_STA_PS_STATECHG_ENABLE' in ap_src and 'keepalive_seconds[] = { 30, 60, 65 }' in ap_src
assert 'lx_wpa_auth_group_start' in ap_src and 'interval=3600' in ap_src
assert 'c >= 36 && c <= 48' in ap_src and 'c >= 149 && c <= 165' in ap_src
print('PASS AP HT20/WMM/TIM, firmware peer capacity, PS keepalive, kickout and GTK rekey contracts')
