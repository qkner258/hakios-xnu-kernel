/* Synthetic packets only. No payloads or location identifiers are printed. */
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "coachz_bb_qmux.h"
#include "umi_cellinfo_qmi.h"
static cbq_bridge bb_qmux;
static uint8_t bb_out[4096];
static uint32_t now_ms=100;
static uint32_t bb_now_ms() { return now_ms; }
static uint32_t bb_qmux_channel(uint32_t c) { return c; }
struct Delivery { unsigned channel; std::vector<uint8_t> data; };
static std::vector<Delivery> delivered;
static void bb_push(uint32_t c,const uint8_t *p,uint32_t n) { delivered.push_back({c,{p,p+n}}); }
static void bb_answer_error(uint32_t channel,uint32_t service,uint32_t cid,uint32_t txn,uint32_t message,uint32_t error,const char *) {
    uint32_t n=cbq_private_message(2,txn,message,service,cid,error,nullptr,0,bb_out,sizeof bb_out);
    assert(n); bb_push(channel,bb_out,n);
}
static void hakios_tl(const char *fmt,...) {
    assert(strncmp(fmt,"cellinfo: ",10)==0);
    assert(!strstr(fmt,"cell ID") && !strstr(fmt,"latitude") && !strstr(fmt,"longitude"));
}
static int bb_lock;
static void IOLockLock(int) {}
static void IOLockUnlock(int) {}
static void umi_modem_wake() {}
typedef void *thread_call_param_t;
struct ThreadCall { void (*fn)(void *,void *); bool queued; };
typedef ThreadCall *thread_call_t;
static thread_call_t thread_call_allocate(void (*fn)(void *,void *),void *) {
    static ThreadCall call;call={fn,false};return &call;
}
static void thread_call_enter(thread_call_t c) { c->queued=true; }
static bool switch_on,switch_off;
typedef int vnode_t;
static int vfs_context_kernel() { return 0; }
static int vnode_lookup(const char *p,int,vnode_t *v,int) {
    *v=1;
    bool exists=strstr(p,"cellinfo-on") ? switch_on : switch_off;
    return exists ? 0 : 2;
}
static void vnode_put(vnode_t) {}
#include "umi_cellinfo.inc"
struct Packet {
    std::vector<uint8_t> p;
    Packet(unsigned type,unsigned msg,unsigned txn=1):p(7) {
        p[0]=(uint8_t)type; uci_w16(p.data()+1,txn); uci_w16(p.data()+3,msg);
    }
    void add(unsigned type,const std::vector<uint8_t>& v) {
        size_t n=p.size();p.resize(n+3+v.size());p[n]=(uint8_t)type;
        uci_w16(p.data()+n+1,(unsigned)v.size());memcpy(p.data()+n+3,v.data(),v.size());
        uci_w16(p.data()+5,(unsigned)p.size()-7);
    }
    void result(unsigned error=0) { add(2,{(uint8_t)(error!=0),0,(uint8_t)error,0}); }
};
static Packet observation(unsigned txn=1,unsigned count=3) {
    Packet p(2,0x43,txn);p.result();
    std::vector<uint8_t> v(19+10*count);
    // Test PLMN 001/01 (reserved laboratory network), synthetic TAC/ECI.
    v[0]=1;v[1]=0;v[2]=0xf1;v[3]=0x10;
    uci_w16(v.data()+4,0x1234);uci_w32(v.data()+6,0x01234567);
    uci_w16(v.data()+10,1234);uci_w16(v.data()+12,42);v[18]=(uint8_t)count;
    for(unsigned i=0;i<count;i++) {
        uint8_t *r=v.data()+19+10*i;
        uci_w16(r,42+i);uci_w16(r+2,(uint16_t)-125);uci_w16(r+4,(uint16_t)-985);
        uci_w16(r+6,(uint16_t)-700);uci_w16(r+8,15);
    }
    p.add(0x13,v);return p;
}
static const uint8_t *tlv(const std::vector<uint8_t>& p,unsigned type,unsigned *len) {
    // Independent bounded decoder, also used for the Apple wire golden checks.
    assert(p.size()>=7 && p.size()==7u+(p[5]|p[6]<<8));
    for(size_t i=7;i<p.size();) {
        assert(p.size()-i>=3);unsigned n=p[i+1]|p[i+2]<<8;
        assert(n<=p.size()-i-3);
        if(p[i]==type){*len=n;return p.data()+i+3;}i+=3+n;
    }
    *len=0;return nullptr;
}
static unsigned rd16(const uint8_t *p) { return p[0]+256u*p[1]; }
static uint32_t rd32(const uint8_t *p) { return rd16(p)+65536u*rd16(p+2); }
static void translator() {
    Packet p=observation();uci_lte lte;
    assert(uci_parse(p.p.data(),(unsigned)p.p.size(),&lte)==1);
    assert(lte.mcc==1 && lte.mnc==1 && lte.tac==0x1234 && lte.cell==0x01234567);
    assert(lte.pci==42 && lte.earfcn==1234 && lte.count==2);
    assert(lte.measurement[0].rsrp==-98 && lte.measurement[0].rsrq==-12);
    uint8_t out[UCI_TLVS_MAX];unsigned n=uci_encode(&lte,1,out,sizeof out);
    assert(n==229);assert(!uci_encode(&lte,1,out,n-1));
    n=uci_encode(&lte,1,out,sizeof out);
    Packet apple(2,0x5556);apple.result();apple.p.insert(apple.p.end(),out,out+n);
    uci_w16(apple.p.data()+5,(unsigned)apple.p.size()-7);
    unsigned len;const unsigned types[]={0xbb,0xbc,0xd2,0xd3}, sizes[]={26,28,31,50};
    for(unsigned ver=0;ver<4;ver++) {
        const uint8_t *r=tlv(apple.p,types[ver],&len);assert(r && len==1+sizes[ver] && *r++==1);
        assert(rd16(r)==0 && rd16(r+2)==1 && rd16(r+4)==1 && r[6]==255);
        assert((ver<2 ? rd16(r+7) : rd32(r+7))==0x1234);
        assert(rd32(r+(ver<2 ? 9 : 11))==0x01234567);
        assert((ver==0 ? rd16(r+13) : rd32(r+(ver==1 ? 13 : 15)))==1234);
        assert(rd16(r+(ver==0 ? 15 : ver==1 ? 17 : 19))==42);
        assert(rd32(r+(ver==0 ? 17 : ver==1 ? 19 : 21))==0);
        assert(rd32(r+(ver==0 ? 21 : ver==1 ? 23 : 25))==0);
        assert(r[ver==0 ? 25 : ver==1 ? 27 : 29]==255);
    }
    const uint8_t *r=tlv(apple.p,0xbf,&len);assert(len==17 && *r++==2);
    assert(rd16(r)==1234 && rd16(r+2)==43 && (int16_t)rd16(r+4)==-98 && (int16_t)rd16(r+6)==-12);
    r=tlv(apple.p,0xd4,&len);assert(len==55 && *r++==2);
    assert(rd32(r)==1234 && rd16(r+4)==43 && (int16_t)rd16(r+6)==-98 && (int16_t)rd16(r+8)==-12);
    p.add(0x1e,{0x70,0x11,1,0}); // extended LTE EARFCN 70000
    p.add(0x2e,{0,0,0,0}); // NR extension must not override LTE
    assert(uci_parse(p.p.data(),(unsigned)p.p.size(),&lte)==1 && lte.earfcn==70000);
    n=uci_encode(&lte,1,out,sizeof out);assert(n>0);
    assert(out[0]==0xbb && out[3]==0); // never truncate wide frequency into V1
    Packet q(2,0x43);q.result();assert(uci_parse(q.p.data(),(unsigned)q.p.size(),&lte)==0);
    assert(uci_encode(&lte,0,out,sizeof out)==24); // explicit empty lists, no invented cells
    uint16_t mcc,mnc;const uint8_t plmn[]={0x13,0x00,0x62};
    assert(uci_plmn(plmn,&mcc,&mnc) && mcc==310 && mnc==260);
    const uint8_t bad[]={0xa3,0xf0,0x10};assert(!uci_plmn(bad,&mcc,&mnc));
    p=observation(1,255);assert(uci_parse(p.p.data(),(unsigned)p.p.size(),&lte)==1 && lte.count==32);
    assert(uci_encode(&lte,1,out,sizeof out)>0);
    for(unsigned cut=0;cut<p.p.size();cut++) assert(!uci_valid(p.p.data(),cut));
    p=observation();p.add(2,{0,0,0,0});assert(!uci_valid(p.p.data(),(unsigned)p.p.size()));
    p=observation();p.p[17]=2;uci_w16(p.p.data()+5,(unsigned)p.p.size()-7);
    assert(uci_parse(p.p.data(),(unsigned)p.p.size(),&lte)==-1);
    p=observation();p.p[35]=255; // inconsistent LTE array count
    assert(uci_parse(p.p.data(),(unsigned)p.p.size(),&lte)==-1);
    // Missing/invalid result, modem failure and invalid cell identity.
    q=Packet(2,0x43);assert(uci_result(q.p.data(),(unsigned)q.p.size())==-1);
    q.result(0x47);assert(uci_result(q.p.data(),(unsigned)q.p.size())==0x47);
    p=observation();uci_w32(p.p.data()+23,0xffffffffu);
    assert(uci_parse(p.p.data(),(unsigned)p.p.size(),&lte)==0);
    uint32_t rng=0x12345678;
    for(unsigned i=0;i<20000;i++) {
        auto f=observation();
        rng=rng*1664525u+1013904223u;
        unsigned at=rng%(unsigned)f.p.size();f.p[at]^=(uint8_t)(rng>>24);
        int v=uci_parse(f.p.data(),(unsigned)f.p.size(),&lte);
        if(v>=0) { n=uci_encode(&lte,v,out,sizeof out);assert(n && n<=sizeof out); }
    }
}
static cbq_result client(unsigned channel=0,unsigned service=3) {
    int slot=cbq_client_alloc(&bb_qmux,channel,service);assert(slot>=0);
    cellinfo_prune();cbq_result r={};r.channel=(uint8_t)channel;r.service=(uint8_t)service;
    r.cid=bb_qmux.client[slot].cid;r.local_port=CBQ_PORT_BASE+(unsigned)slot;return r;
}
static bool host(const cbq_result &r,const Packet &p) { return cellinfo_host(r,p.p.data(),(unsigned)p.p.size()); }
static unsigned poll(bool on=true) {
    uint8_t p[7]={};uint32_t node=0,port=0;
    unsigned n=cellinfo_poll(on,p,&node,&port);
    if(n){assert(n==7 && p[0]==0 && rd16(p+3)==0x43 && rd16(p+5)==0 && node==3 && port==77);}
    return n;
}
static void receive(const Packet &p,unsigned node=3,unsigned port=77) { cellinfo_receive(node,port,p.p.data(),(unsigned)p.p.size()); }
static void reset() { cellinfo={};bb_qmux={};delivered.clear();cbq_server_new(&bb_qmux,3,1,3,77); }
static void session() {
    cellinfo_switch_call=thread_call_allocate(cellinfo_check_switch,nullptr);
    assert(!cellinfo_switch_wanted);switch_on=true;
    cellinfo_schedule_switch();assert(cellinfo_switch_call->queued && !cellinfo_switch_wanted);
    cellinfo_switch_call->fn(nullptr,nullptr);assert(cellinfo_switch_wanted);
    switch_off=true;now_ms+=2000;cellinfo_schedule_switch();
    cellinfo_switch_call->fn(nullptr,nullptr);assert(!cellinfo_switch_wanted);
    switch_off=false;switch_on=false;
    reset();auto a=client(),b=client(1);Packet get(0,0x5556,100),get2(0,0x5556,101);
    assert(!host(a,get) && !poll(false)); // disabled preserves original forwarding
    poll();assert(host(a,get) && host(b,get2));assert(poll()==7);unsigned txn=cellinfo.txn;
    assert(!poll());auto answer=observation(txn);
    receive(answer,4);assert(delivered.empty() && cellinfo.pending);
    auto wrong=observation(txn+1);receive(wrong);assert(delivered.empty());
    receive(answer);assert(delivered.size()==2 && !cellinfo.pending);
    assert(rd16(delivered[0].data.data()+7)==100 && rd16(delivered[1].data.data()+7)==101);
    assert(delivered[0].data[5]==a.cid && delivered[1].data[5]==b.cid);
    assert(delivered[0].channel==0 && delivered[1].channel==1);
    receive(answer);assert(delivered.size()==2); // duplicate response never delivered
    // Original NAS registration is byte-identical and success-gated.
    Packet reg(0,3,200);reg.add(0xa0,{1});auto saved=reg.p;
    assert(!host(a,reg) && saved==reg.p);
    unsigned slot=a.local_port-CBQ_PORT_BASE;
    Packet ack(2,3,200);ack.result(0x47);cellinfo_observe(slot,ack.p.data(),(unsigned)ack.p.size());
    assert(!cellinfo.owner[slot].subscribed);
    host(a,reg);ack=Packet(2,3,200);ack.result();cellinfo_observe(slot,ack.p.data(),(unsigned)ack.p.size());
    assert(cellinfo.owner[slot].subscribed && poll()==7);
    receive(observation(cellinfo.txn));assert(delivered.back().data[6]==4);
    auto before=delivered.size();assert(!poll());now_ms+=5000;assert(poll()==7);
    receive(observation(cellinfo.txn));assert(delivered.size()==before+1);
    // Unsubscribe only after a successful matching original response.
    Packet unsub(0,3,201);unsub.add(0xa0,{0});assert(!host(a,unsub));
    ack=Packet(2,3,202);ack.result();cellinfo_observe(slot,ack.p.data(),(unsigned)ack.p.size());
    assert(cellinfo.owner[slot].subscribed);
    ack=Packet(2,3,201);ack.result();cellinfo_observe(slot,ack.p.data(),(unsigned)ack.p.size());
    assert(!cellinfo.owner[slot].subscribed);now_ms+=5000;assert(!poll());
    host(a,reg);ack=Packet(2,3,200);ack.result();cellinfo_observe(slot,ack.p.data(),(unsigned)ack.p.size());
    // Turn off with a waiting request: exactly one error, release own port.
    assert(host(a,get));assert(poll()==7);before=delivered.size();assert(!poll(false));
    assert(cellinfo.release && delivered.size()==before+1 && !cellinfo.pending);
    receive(observation(cellinfo.txn));assert(delivered.size()==before+1);cellinfo.release=false;
    // Releasing a client invalidates waiters/subscriptions before pooled reuse.
    assert(poll()==7);host(a,get);assert(!poll());cbq_client_free(&bb_qmux,slot);cellinfo_prune();
    auto c=client();assert(c.local_port==a.local_port && c.cid!=a.cid);
    assert(!cellinfo.owner[slot].subscribed);before=delivered.size();receive(observation(cellinfo.txn));
    assert(delivered.size()==before);
    // A timeout answers local Apple transactions, no response for stale query.
    host(c,get);assert(poll()==7);auto late=observation(cellinfo.txn);now_ms+=8000;
    before=delivered.size();assert(!poll());assert(delivered.size()==before+1);
    receive(late);assert(delivered.size()==before+1);
    // Endpoint replacement cancels local requests and releases before reuse.
    host(c,get);assert(poll()==7);cbq_server_del(&bb_qmux,3,3,77);
    assert(!poll() && !cellinfo.pending && cellinfo.release);cellinfo.release=false;
    cbq_server_new(&bb_qmux,3,1,3,77);host(c,get);assert(poll()==7);
    Packet fail(2,0x43,cellinfo.txn);fail.result(0x47);before=delivered.size();receive(fail);
    assert(delivered.size()==before+1 && cellinfo.len==0);
    // Empty standard observation: response succeeds with zero identity counts.
    host(c,get);assert(poll()==7);Packet empty(2,0x43,cellinfo.txn);empty.result();
    before=delivered.size();receive(empty);assert(delivered.size()==before+1);
    unsigned zlen;std::vector<uint8_t> sdu(delivered.back().data.begin()+6,delivered.back().data.end());
    const uint8_t *z=tlv(sdu,0xbc,&zlen);assert(z && zlen==1 && !*z);
    // Queue bound, transaction coalescing, and original unrelated traffic.
    for(unsigned i=0;i<32;i++) assert(host(c,Packet(0,0x5556,300+i)));
    before=delivered.size();host(c,Packet(0,0x5556,400));assert(delivered.size()==before+1);
    host(c,Packet(0,0x5556,300));assert(delivered.size()==before+1);
    for(unsigned msg:{0x24u,0x4du,0x4fu,0x5557u,0x5562u,0x5566u,0x5567u,0x5568u,0x5569u,0x556au}) {
        Packet original(0,msg);auto bytes=original.p;assert(!host(c,original) && bytes==original.p);
    }
    auto wds=client(0,1);assert(!host(wds,get));
    cbq_reset_host_clients(&bb_qmux);cellinfo_prune();
    for(auto &w:cellinfo.wait) assert(!w.used);
    for(auto &o:cellinfo.owner) assert(!o.cid && !o.subscribed);
}
int main(){translator();session();puts("cellinfo: translator + session + 20000 malformed-input cases passed (ASan/UBSan)");}
