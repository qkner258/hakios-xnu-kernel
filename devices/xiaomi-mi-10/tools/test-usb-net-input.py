#!/usr/bin/env python3
"""Exercise the actual network input function with consuming mbuf KPI mocks.
Checks exact bytes from NCM offset34 and ownership/metadata at ifnet_input.
"""
from pathlib import Path
import os, subprocess, tempfile
base=Path(__file__).resolve().parent
pinned=base/'sources/hakios.net.c'
source=(pinned if pinned.exists() else base.parent/'overlays/xnu/files/bsd/dev/hakios.net.c').read_text()
state=source[source.index('static struct {\n    uint64_t calls'):source.index('static errno_t\nusb_net_demux')]
function=source[source.index('void\nhakios_net_input_unit('):source.index('\nvoid\nhakios_net_input(const')]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
typedef int errno_t;
#define ETHER_HDR_LEN 14
#define HAKIOS_NET_FRAME_MAX 1514
#define MBUF_DONTWAIT 0
#define DLT_EN10MB 1
typedef void *ifnet_t;
struct mbuf { uint8_t buf[2048]; uint8_t *data; size_t len, plen; void *header; ifnet_t rcvif; };
typedef struct mbuf *mbuf_t;
struct net_if {ifnet_t ifp; struct {uint64_t dropped_in, received, received_bytes;} count;};
static struct net_if net={.ifp=(void*)0x42};
static bool absent;
static int inject_alloc,inject_copy,inject_setdata,inject_input;
static unsigned submitted,freed,printed;
static const uint8_t *expected;
static size_t expected_len;
static char output[4096];
static struct net_if *net_if_unit(unsigned u) {return u==2 && !absent ? &net : NULL;}
static void trace_frame(const char*a,const uint8_t*b,size_t c,size_t d){(void)a;(void)b;(void)c;(void)d;}
static void hakios_tl(const char *fmt,...) {va_list ap;va_start(ap,fmt);vsnprintf(output,sizeof(output),fmt,ap);va_end(ap);++printed;}
static int mbuf_allocpacket(int how,size_t len,unsigned *chunks,mbuf_t *m) {
 (void)how;assert(len<=1514 && *chunks==1);if(inject_alloc)return ENOMEM;
 *m=calloc(1,sizeof(**m));assert(*m);(*m)->data=(*m)->buf;return 0;
}
static int mbuf_copyback(mbuf_t m,size_t off,size_t len,const void *p,int how) {
 (void)how;assert(off==0);if(inject_copy)return ENOBUFS;
 memcpy(m->data,p,len);m->len=m->plen=len;return 0;
}
static void mbuf_freem(mbuf_t m){++freed;free(m);}
static void *mbuf_data(mbuf_t m){return m->data;}
static void mbuf_pkthdr_setrcvif(mbuf_t m,ifnet_t i){m->rcvif=i;}
static void mbuf_pkthdr_setheader(mbuf_t m,void *p){m->header=p;}
static void mbuf_pkthdr_adjustlen(mbuf_t m,int n){m->plen+=n;}
static int mbuf_setdata(mbuf_t m,void *p,size_t n){if(inject_setdata)return EINVAL;assert((uint8_t*)p>=m->buf && (uint8_t*)p+n<=m->buf+sizeof(m->buf));m->data=p;m->len=n;return 0;}
static int mbuf_copydata(mbuf_t m,size_t off,size_t n,void *p){assert(off+n<=m->len);memcpy(p,m->data+off,n);return 0;}
static size_t mbuf_pkthdr_len(mbuf_t m){return m->plen;}
static size_t mbuf_len(mbuf_t m){return m->len;}
static ifnet_t mbuf_pkthdr_rcvif(mbuf_t m){return m->rcvif;}
static void *mbuf_pkthdr_header(mbuf_t m){return m->header;}
static void bpf_tap_in(ifnet_t i,int d,mbuf_t m,void *h,size_t n){assert(i==net.ifp && d==DLT_EN10MB && h==m->header && n==14);}
struct ifnet_stat_increment_param {unsigned packets_in,bytes_in;};
static int ifnet_input(ifnet_t i,mbuf_t m,struct ifnet_stat_increment_param *s) {
 assert(i==net.ifp && m->rcvif==i && m->header==m->data-14);
 assert(m->len==expected_len-14 && m->plen==expected_len-14);
 assert(!memcmp(m->header,expected,14));
 assert(!memcmp(m->data,expected+14,expected_len-14));
 assert(s->packets_in==1 && s->bytes_in==expected_len);
 ++submitted;mbuf_freem(m); /* includes error path: caller must NEVER touch m again */
 return inject_input;
}
'''+state+function+r'''
int main(void) {
 uint8_t ntb[2048];uint8_t *frame=ntb+34;
 for(unsigned i=0;i<1514;i++)frame[i]=(uint8_t)(i*23+1);
 expected=frame;
 for(expected_len=14;expected_len<=1514;expected_len++)hakios_net_input_unit(2,frame,expected_len);
 assert(submitted==1501 && usb_rx_diag.accepted==1501 && printed==3 && usb_rx_diag.dumps==3);
 assert(strstr(output,"exact=1 input-rc=0") && strstr(output,"rcvif=1 header=1"));
 expected_len=42;unsigned before=submitted;
 absent=true;hakios_net_input_unit(2,frame,42);absent=false;assert(usb_rx_diag.no_if==1);
 hakios_net_input_unit(2,frame,13);assert(usb_rx_diag.invalid==1);
 inject_alloc=1;hakios_net_input_unit(2,frame,42);inject_alloc=0;assert(usb_rx_diag.alloc==1);
 inject_copy=1;hakios_net_input_unit(2,frame,42);inject_copy=0;assert(usb_rx_diag.copy==1);
 inject_setdata=1;hakios_net_input_unit(2,frame,42);inject_setdata=0;assert(usb_rx_diag.setdata==1);
 assert(submitted==before);
 /* A broadcast ARP request for the phone, at Mac's unaligned offset34. */
 const uint8_t arp[42]={255,255,255,255,255,255,2,0x42,0,0,0,2,8,6,
  0,1,8,0,6,4,0,1,2,0x42,0,0,0,2,172,16,42,2,0,0,0,0,0,0,172,16,42,1};
 memcpy(frame,arp,42);usb_rx_diag.dumps=0;inject_input=EINVAL;
 hakios_net_input_unit(2,frame,42);inject_input=0;
 assert(usb_rx_diag.rejected==1 && usb_rx_diag.last_input==EINVAL);
 assert(strstr(output,"pkthdr=28 mlen=28 rcvif=1 header=1 exact=1") && strstr(output,"first42=ff ff ff ff ff ff 02 42"));
 assert(freed==submitted+2); /* copy and setdata failures free before submission */
 puts("PASS: actual RX function, exact offset34 bytes for all Ethernet lengths, ARP, pkthdr/rcvif/header, consuming input success/error, bounded dumps, failure counters (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-usb-net-') as tmp:
 c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(harness)
 subprocess.run([os.environ.get('CC','clang'),'-std=c11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
