#!/usr/bin/env python3
"""Actual AP authenticator vs independent hashlib/hmac/AES-wrap and IEEE PMK vectors."""
import hashlib, hmac, os, random, struct, subprocess, tempfile, sys
from pathlib import Path
from cryptography.hazmat.primitives.keywrap import aes_key_wrap
ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'devices/xiaomi-mi-10/overlays/wifi/lx/src'
rng = random.Random(6390)
def data(n): return bytes(rng.randrange(256) for _ in range(n))
def message(info, replay, nonce, payload, kck=None):
    b = bytearray(struct.pack('>BBHBHHQ32s16s8s8s16sH',2,3,95+len(payload),2,info,
        16 if info & 128 else 0,replay,nonce,bytes(16),bytes(8),bytes(8),bytes(16),len(payload))+payload)
    if kck: b[81:97]=hmac.new(kck,b,hashlib.sha1).digest()[:16]
    return bytes(b)
def arr(n,b): return 'static const unsigned char '+n+'[]={'+','.join(str(x) for x in b)+'};\n'
rsn=bytes.fromhex('30140100000fac040100000fac040100000fac020000')
code='#include <assert.h>\n#include <stdio.h>\n#include "lx_wpa.c"\n'
tests=''
for i in range(32):
    ssid=data(1+i%32); password=data(8+i%56); pmk=hashlib.pbkdf2_hmac('sha1',password,ssid,4096,32)
    ap=b'\x02'+data(5);sta=b'\x02'+data(5);an=data(32);sn=data(32);gtk=data(16);r=rng.randrange(1<<62)
    seed=min(ap,sta)+max(ap,sta)+min(an,sn)+max(an,sn)
    ptk=b''.join(hmac.new(pmk,b'Pairwise key expansion\0'+seed+bytes([k]),hashlib.sha1).digest() for k in range(3))[:48]
    # Clients may be MFPC and include a zero PMKID count/BIP selector.
    client=bytearray(rsn)
    if i%3:client[20]=128
    if i%3==2:client+=b'\0\0\0\x0f\xac\x06';client[1]+=6
    client=bytes(client)
    m1=message(0x8a,r,an,b'');m2=message(0x10a,r,sn,client,ptk[:16])
    plain=rsn+b'\xdd\x16\0\x0f\xac\x01\x01\0'+gtk+b'\xdd\0'
    m3=message(0x13ca,r+1,an,aes_key_wrap(ptk[16:32],plain),ptk[:16]);m4=message(0x30a,r+1,bytes(32),b'',ptk[:16])
    fields=locals().copy()
    for n in ['ssid','password','pmk','ap','sta','an','gtk','client','m1','m2','m3','m4','ptk']:
        code+=arr('c%d_%s'%(i,n),fields[n])
    code+=arr('c%d_replay'%i,struct.pack('>Q',r))
    tests+='''{
 struct lx_wpa_auth a; unsigned char reply[256], bad[256], derived[32]; unsigned n=0;
 lx_wpa_pmk(P(password),sizeof(P(password)),P(ssid),sizeof(P(ssid)),derived);
 assert(!memcmp(derived,P(pmk),32));
 lx_wpa_auth_start(&a,P(pmk),P(ap),P(sta),P(an),P(gtk),P(replay),P(client),sizeof(P(client)));
 assert(a.state==1 && !memcmp(a.m1,P(m1),99));
 for(unsigned z=0;z<sizeof(P(m2));z++) { struct lx_wpa_auth copy=a; assert(lx_wpa_auth_message(&copy,P(m2),z,reply,&n)<=0); }
 memcpy(bad,P(m2),sizeof(P(m2)));bad[81]^=1;
 assert(lx_wpa_auth_message(&a,bad,sizeof(P(m2)),reply,&n)==-1 && a.state==1);
 memcpy(bad,P(m2),sizeof(P(m2)));bad[9]^=1;
 assert(lx_wpa_auth_message(&a,bad,sizeof(P(m2)),reply,&n)==-1 && a.state==1);
 assert(lx_wpa_auth_message(&a,P(m2),sizeof(P(m2)),reply,&n)==1);
 assert(n==sizeof(P(m3)) && !memcmp(reply,P(m3),n));
 assert(!memcmp(a.keys.pairwise,P(ptk)+32,16));
 assert(lx_wpa_auth_message(&a,P(m2),sizeof(P(m2)),reply,&n)==1);
 assert(n==sizeof(P(m3)) && !memcmp(reply,P(m3),n));
 memcpy(bad,P(m4),sizeof(P(m4)));bad[81]^=1;
 assert(lx_wpa_auth_message(&a,bad,sizeof(P(m4)),reply,&n)==-1 && a.state==2);
 assert(lx_wpa_auth_message(&a,P(m4),sizeof(P(m4)),reply,&n)==2 && a.state==3);
 assert(lx_wpa_auth_message(&a,P(m4),sizeof(P(m4)),reply,&n)==0 && a.state==3);
 assert(lx_wpa_auth_message(&a,P(m2),sizeof(P(m2)),reply,&n)==0);
}\n'''.replace('P(', 'P%d('%i)
    code+='#define P%d(n) c%d_##n\n'%(i,i)
code+='int main(void) {\n'+tests+'''
 unsigned char bad[64]; memcpy(bad,lx_wpa_ap_rsn,22);
 assert(lx_wpa_auth_rsn(bad,22)); bad[20]=64; assert(!lx_wpa_auth_rsn(bad,22));
 memcpy(bad,lx_wpa_ap_rsn,22);bad[13]=2;assert(!lx_wpa_auth_rsn(bad,22));
 puts("PASS AP: 32 independent WPA2 PMK/PTK/M1-M4/AES-wrapped GTK cases, bad MIC/replay/RSN/truncation, retry/no reinstall (ASan/UBSan)");
}\n'''
with tempfile.TemporaryDirectory(prefix='umi-wpa-auth-') as tmp:
    c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
    subprocess.run([os.environ.get('CC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(SRC),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    # Reuse the standards tests, explicitly compiling the device overlay (STA regression too).
    cases=Path(tmp)/'cases.inc'
    with cases.open('wb') as f:subprocess.run([sys.executable,str(ROOT/'tools/wpa-test/reference.py')],stdout=f,check=True)
    subprocess.run(['clang','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I'+tmp,'-I'+str(SRC),str(ROOT/'tools/wpa-test/test_wpa.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
