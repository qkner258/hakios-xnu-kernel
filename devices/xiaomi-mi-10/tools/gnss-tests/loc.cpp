/* Hardware-free tests of the actual parser and kernel session state machine. */
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <initializer_list>
#include "umi_gnss_qmi.h"
/* Host shims replace only kernel facilities, not GNSS implementation code. */
static unsigned now_ms = 100, uid, wakeups, logs;
static int bb_published = 1, bb_lock, bb_qmux;
struct cbq_server { uint32_t node, port; };
static cbq_server server = {3, 75};
static bool present = true;
static const cbq_server *cbq_server_find(int *, uint32_t service)
{ assert(service == UGL_SERVICE); return present ? &server : nullptr; }
static uint32_t bb_now_ms() { return now_ms; }
static void IOLockLock(int) {}
static void IOLockUnlock(int) {}
static void umi_modem_wake() { wakeups++; }
/* Validate log formats without formatting arguments (never print location data). */
static void hakios_tl(const char *fmt, ...) {
    assert(strncmp(fmt, "gnss: ", 6) == 0);
    assert(!strstr(fmt, "latitude") && !strstr(fmt, "longitude")); logs++;
}
struct sysctl_oid {};
struct sysctl_req { void *newptr; void *oldptr; size_t oldlen; };
static int sysctl_handle_int(sysctl_oid *, int *v, int, sysctl_req *r)
{ if (r->newptr) memcpy(v, r->newptr, sizeof *v); return 0; }
static int sysctl_out(sysctl_req *r, const void *p, size_t n)
{ assert(r->oldlen == n); memcpy(r->oldptr, p, n); return 0; }
#define SYSCTL_OUT(r,p,n) sysctl_out(r,p,n)
#define SYSCTL_DECL(...)
#define SYSCTL_PROC(...)
static unsigned kauth_cred_get() { return uid; }
static unsigned kauth_cred_getuid(unsigned cred) { return cred; }
#include "umi_gnss.inc"

struct Packet {
    uint8_t p[1024] = {}; uint32_t n = 7;
    Packet(unsigned type, unsigned message, unsigned txn = 0) {
        p[0] = (uint8_t)type; ugl_w16(p+1,txn); ugl_w16(p+3,message); finish();
    }
    void finish() { ugl_w16(p+5,n-7); }
    void add(unsigned type, uint64_t bits, unsigned len) {
        p[n++] = (uint8_t)type; ugl_w16(p+n,len); n+=2;
        for (unsigned i=0;i<len;i++) p[n++] = (uint8_t)(bits>>(8*i)); finish();
    }
};
static Packet position(unsigned session = 1) {
    Packet p(4,UGL_POSITION);
    p.add(1,0,4); p.add(2,session,1);
    /* Synthetic IEEE patterns, only compared in memory. */
    p.add(0x10,UINT64_C(0x4034000000000000),8); p.add(0x11,UINT64_C(0xc054000000000000),8);
    p.add(0x12,0x40b00000,4); return p;
}
static void set(unsigned on) {
    int v=(int)on; sysctl_req r = {&v,nullptr,0};
    assert(gnss_enable_sysctl(nullptr,nullptr,0,&r)==0);
}
static uint32_t poll(uint8_t *p) { uint32_t node=0,port=0; auto n=gnss_poll(p,&node,&port);
    if(n) { assert(node==server.node && port==server.port); assert(ugl_valid(p,n)); } return n; }
static void reply(unsigned error = 0) {
    Packet p(2,gnss.pending_message,gnss.pending_txn); p.add(2,error? ((uint64_t)error<<16)|1:0,4);
    gnss_receive(server.node,server.port,p.p,p.n);
}
static void start(uint8_t *out) {
    set(1);assert(poll(out)==18);assert(ugl_u16(out+3)==UGL_REGISTER && out[10]==3);
    reply();assert(poll(out)==32);assert(ugl_u16(out+3)==UGL_START);
    uint64_t v;assert(ugl_field(out,32,0x10,4,&v)&&v==1);
    assert(ugl_field(out,32,0x12,4,&v)&&v==2);
    assert(ugl_field(out,32,0x13,4,&v)&&v==1000);reply();assert(gnss.phase==GNSS_RUNNING);
}
int main() {
    static_assert(sizeof(umi_gnss_fix)==64,"ABI layout");
    umi_gnss_fix f={}; auto p=position();
    assert(ugl_position(p.p,p.n,1,&f)==1);assert(f.flags==UMI_GNSS_POSITION);
    assert(ugl_accuracy_m(f.accuracy_bits)==6);
    assert(ugl_position(p.p,p.n,2,&f)==0);
    for(unsigned n=0;n<p.n;n++) assert(ugl_position(p.p,n,1,&f)==-1);
    p.add(0x24,1234567890,8);p.add(0x1c,0x42800000,4);p.add(0x1d,0x41000000,4);
    p.add(0x2c,0x0008000702,5);assert(ugl_position(p.p,p.n,1,&f)==1);
    assert(f.satellites_used==2 && (f.flags&UMI_GNSS_UTC) && (f.flags&UMI_GNSS_ALTITUDE));
    auto dup=p;dup.add(0x12,0x40b00000,4);assert(!ugl_valid(dup.p,dup.n));
    auto bad=p;bad.p[bad.n++]=0xff;bad.finish();assert(!ugl_valid(bad.p,bad.n));
    auto incomplete=position();incomplete.p[3]=UGL_SV;assert(ugl_position(incomplete.p,incomplete.n,1,&f)==-1);
    for(uint64_t bits : {UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x4057000000000000)}) {
        auto q=position();for(unsigned i=0;i<8;i++)q.p[21+i]=(uint8_t)(bits>>(8*i));
        assert(ugl_position(q.p,q.n,1,&f)==-1);
    }
    auto searching=position();searching.p[10]=1;assert(ugl_position(searching.p,searching.n,1,&f)==0);
    auto ellipse=position();ellipse.p[40]=0x14;assert(ugl_position(ellipse.p,ellipse.n,1,&f)==1);
    auto negative=position();negative.p[negative.n-1]|=0x80;assert(ugl_position(negative.p,negative.n,1,&f)==-1);
    Packet sv(4,UGL_SV);sv.add(1,0,1);sv.p[sv.n++]=0x10;ugl_w16(sv.p+sv.n,57);sv.n+=2;
    sv.p[sv.n++]=2;sv.n+=56;sv.finish();uint32_t view=0;
    assert(ugl_satellites(sv.p,sv.n,&view)&&view==2);sv.p[14]=3;assert(!ugl_satellites(sv.p,sv.n,&view));
    uint32_t rng=17;
    for(unsigned i=0;i<10000;i++) {
        uint8_t bytes[1024];for(auto &b:bytes){rng=rng*1664525+1013904223;b=(uint8_t)(rng>>24);}
        unsigned n=rng%1025;(void)ugl_position(bytes,n,1,&f);(void)ugl_satellites(bytes,n,&view);(void)ugl_result(bytes,n);
    }
    puts("PASS LOC parser: framing, TLV widths, duplicates, partial/status/session, IEEE bounds, uncertainty, UTC, altitude, satellite arrays, 10000 malformed inputs");
    uint8_t out[32];assert(poll(out)==0 && logs==0);
    uid=501;sysctl_req req={nullptr,&f,sizeof f};
    assert(gnss_fix_sysctl(nullptr,nullptr,0,&req)==EPERM);
    assert(gnss_enable_sysctl(nullptr,nullptr,0,&req)==EPERM);uid=0;
    bb_published=0;assert(gnss_fix_sysctl(nullptr,nullptr,0,&req)==ENXIO);bb_published=1;
    start(out);auto fix=position(gnss.session);
    gnss_receive(9,server.port,fix.p,fix.n);assert(gnss.fixes==0);
    gnss_receive(server.node,server.port,fix.p,fix.n);assert(gnss.fixes==1);
    assert(gnss_fix_sysctl(nullptr,nullptr,0,&req)==0 && f.sequence==1);
    now_ms+=15000;assert(poll(out)==11 && ugl_u16(out+3)==UGL_STOP);
    assert(!gnss.wanted && !gnss.latest.flags);reply();assert(poll(out)==18 && out[10]==0);reply();
    assert(gnss.phase==GNSS_IDLE && poll(out)==0);
    start(out);set(0);assert(!gnss.latest.flags && poll(out)==11);reply();assert(poll(out)==18);reply();
    set(1);assert(poll(out)==18);reply();assert(poll(out)==32);set(0);
    assert(poll(out)==0);reply();assert(poll(out)==11);reply();assert(poll(out)==18);reply();
    start(out);present=false;assert(poll(out)==0 && gnss.phase==GNSS_IDLE && !gnss.latest.flags);
    present=true;assert(poll(out)==18);now_ms+=8000;assert(poll(out)==18 && out[10]==0);reply();
    assert(gnss.phase==GNSS_FAILED && poll(out)==0);set(0);start(out);
    set(0);assert(poll(out)==11);reply();assert(poll(out)==18);reply();
    puts("PASS LOC lifecycle: default idle, root-only ABI, independent endpoint, foreign replies, lease expiry, stop/unregister, cancel during start, endpoint loss, timeout latch and toggle retry");
}
