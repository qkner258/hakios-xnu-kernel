#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include <cerrno>
#include "coachz_bb_qmux.h"
#include "coachz_bb_data.h"

static cbq_bridge bb_qmux;
static uint32_t now, lookups, vnode_puts;
static bool file_exists;
static std::string option_name, option_value;
static const char *cvd_version="2.4", *session_name="11C05000";
static int file_type = 1;
using vnode_t = int;
using thread_call_t = int;
using thread_call_param_t = void *;
static bool locked, service_thread = true, worker_queued;
static void *bb_lock;
static void IOLockLock(void *) { assert(!locked); locked = true; }
static void IOLockUnlock(void *) { assert(locked); locked = false; }
static void thread_call_enter(thread_call_t call) { assert(call == 1); worker_queued = true; }
enum { VREG = 1 };
static uint32_t bb_now_ms() { return now; }
static int vfs_context_kernel() { return 0; }
static int vnode_lookup(const char *path, int flags, vnode_t *node, int context)
{
    assert(!flags && !context);
    if (strcmp(path, "/var/db/hakios-voice/on")) {
      if (std::string(path)=="/var/db/hakios-voice/"+option_name) {*node=2;return 0;}
      return ENOENT;
    }
    assert(!locked && !service_thread);
    lookups++;
    *node = 1;
    return file_exists ? 0 : 2;
}
static int vnode_vtype(vnode_t node) { assert(node==1||node==2); return node==2?VREG:file_type; }
static void vnode_put(vnode_t node) { assert(node==1||node==2); vnode_puts++; }
static std::string logs;
static void hakios_tl(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    logs += line;
}
static int vfs_context_ucred(int) { return 0; }
static int vfs_context_proc(int) { return 0; }
#define UIO_READ 0
#define UIO_SYSSPACE 0
static int vn_rdwr(int,int node,char *p,int n,int,int,int,int,int *left,int) {
 assert(node==2);memcpy(p,option_value.data(),option_value.size());*left=n-(int)option_value.size();return 0;
}
#include "umi_voice.inc"
struct AudioCommand { uint8_t service; uint16_t port; uint32_t op; std::vector<uint8_t> data; };
static std::vector<AudioCommand> audio_commands;
static unsigned fail_command;
static int command_error=EIO;
static bool route_owned;
static int route_health;
extern "C" int umi_voice_route_health() { return route_health; }
static unsigned route_takes,route_releases;
static enum umi_voice_route physical_route;
extern "C" int umi_voice_route_take() { assert(!route_owned&&!locked);route_owned=true;route_takes++;return 0; }
extern "C" int umi_voice_route_release() { assert(route_owned&&!locked);route_owned=false;route_releases++;return 0; }
extern "C" int umi_voice_route_set(enum umi_voice_route route,uint32_t percent) {
 assert(route_owned&&!locked&&percent<=100);physical_route=route;return 0;
}
extern "C" void umi_voice_receiver_clock(uint32_t hz) { assert(hz==19200000); }
extern "C" int umi_voice_apr(uint8_t service,uint16_t port,uint32_t op,const void *p,uint32_t n,struct uva_reply *r)
{
 assert(!locked&&!service_thread);
 const uint8_t *data=(const uint8_t *)p;
 audio_commands.push_back({service,port,op,n?std::vector<uint8_t>(data,data+n):std::vector<uint8_t>()});
 *r={};
 if(fail_command==audio_commands.size()){r->result=3;return command_error;}
 if(op==0x11378){r->bytes=(uint32_t)strlen(cvd_version)+1;memcpy(r->data,cvd_version,r->bytes);return 0;}
 if(op==0x110ff||op==0x11140){
  assert(port==0&&n==20&&!memcmp(data,session_name,strlen(session_name)+1));r->port=service;
 } else if(op==0x13169||op==0x112bf){
  assert(n==44&&port==0&&cbd_rd16(data)==2&&cbd_rd16(data+2)==0xb037);
  assert(cbd_rd32(data+4)==0x10f70);
  assert(cbd_rd16(data+8)==(physical_route==UMI_VOICE_SPEAKER?0x1004:0xb030));
  assert(cbd_rd32(data+10)==0x10f77&&cbd_rd32(data+14)==0x1135e);
  assert(cbd_rd32(data+18)==0x10f7c&&cbd_rd16(data+22)==0xffff);
  r->port=11;
 } else if(op==0x11327){assert(service==9&&port==9&&n==1&&data[0]==1);}
 else if(op==0x1123e||op==0x1123f){assert(service==9&&port==9&&n==2&&cbd_rd16(data)==11);}
 else if(op==0x1138b){assert(service==10&&port==10&&n==6&&cbd_rd16(data)==0&&cbd_rd16(data+4)==20);}
 else if(op==0x112c2){assert(service==11&&port==11&&n==8&&cbd_rd16(data)==1&&cbd_rd16(data+6)==20);}
 else if(op==0x13199){assert(n==4&&cbd_rd16(data)==1&&cbd_rd16(data+2)==1);}
 else if(op==0x1133d){
  assert(service==11&&port==11&&n>=28&&cbd_rd32(data+12)==n-16);
  assert(!cbd_rd32(data)&&!cbd_rd32(data+4)&&!cbd_rd32(data+8));
  assert(cbd_rd16(data+24)==n-28);
  if(cbd_rd32(data+16)==0x10912){
   assert(physical_route==UMI_VOICE_SPEAKER);
   if(cbd_rd32(data+20)==0x10342){assert(n==48&&cbd_rd16(data+32)==2&&cbd_rd16(data+34)==1&&cbd_rd16(data+42)==0x4000&&cbd_rd16(data+44)==0x4000);}
   else {assert(cbd_rd32(data+20)==0x10913&&n==100&&cbd_rd32(data+28)==48000&&cbd_rd16(data+34)==2);}
  } else assert(cbd_rd32(data+16)==0x1316e);
 }
 else {assert(!n);assert(port==service);}
 return 0;
}

static unsigned audio_memory_calls, audio_mhi_calls, audio_apr_calls;
static int audio_memory_error, audio_mhi_error, audio_apr_error;
static bool audio_cancel_in_apr;
extern "C" int umi_voice_audio_memory(uint64_t *physical, uint64_t *adsp)
{
    assert(!locked && !service_thread);
    audio_memory_calls++; *physical = 0x90000000; *adsp = 0x110020000;
    return audio_memory_error;
}
extern "C" int umi_voice_mhi_map(uint64_t physical, uint64_t *pcie)
{
    assert(!locked && !service_thread && physical == 0x90000000);
    audio_mhi_calls++; *pcie = 0xb0200000;
    return audio_mhi_error;
}
extern "C" int umi_voice_mailbox_config(uint64_t adsp, uint64_t pcie, uint32_t *result)
{
    assert(!locked && !service_thread && adsp == 0x110020000 && pcie == 0xb0200000);
    audio_apr_calls++; *result = audio_apr_error ? 3 : 0;
    if (audio_cancel_in_apr) assert(umi_voice_audio_stop(uva_control.call)==EINPROGRESS);
    return audio_apr_error;
}

static uint8_t packet[512];
static uint32_t length, node, port;
static bool released;
static void start(uint8_t type, uint16_t message, uint16_t txn = 1)
{
    length = cbd_request(packet, txn, message, 0);
    packet[0] = type;
}
static void tlv(uint8_t type, const uint8_t *v, uint32_t n)
{
    length += cbd_tlv(packet + length, sizeof packet - length, type, v, n);
    cbd_wr16(packet + 5, length - 7);
}
static void scalar(uint8_t type, uint32_t v, uint32_t bytes)
{
    uint8_t b[4]; cbd_wr32(b, v); tlv(type, b, bytes);
}
static void result(uint16_t error = 0)
{
    uint8_t b[4]; cbd_wr16(b, !!error); cbd_wr16(b + 2, error); tlv(2, b, 4);
}
static cbq_result metadata(uint8_t service)
{
    cbq_result r = {};
    r.service = service; r.local_port = CBQ_PORT_BASE;
    return r;
}
static uint32_t poll(bool enabled)
{
    return voice_poll(enabled, packet, &node, &port, &released);
}
static void reply(uint16_t message, uint16_t error = 0)
{
    start(2, message, voice.pending); result(error);
    voice_receive(3, 47, packet, length);
}
static void reset()
{
    voice = {}; now = 1000; logs.clear();
    voice_switch_wanted = false; uva_control = {}; uva_session = {};
    option_name.clear();option_value.clear();cvd_version="2.4";session_name="11C05000";
    audio_commands.clear();fail_command=0;command_error=EIO;route_owned=false;route_takes=route_releases=0;route_health=0;
    uva_memory_ready = uva_mailbox_ready = false; uva_transport_error = 0;
    audio_memory_calls = audio_mhi_calls = audio_apr_calls = 0;
    audio_memory_error = audio_mhi_error = audio_apr_error = 0; audio_cancel_in_apr = false;
    cbq_init(&bb_qmux);
    assert(cbq_server_new(&bb_qmux, 3, 1, 3, 47));
}

static void switch_and_queries()
{
    reset();
    voice_schedule_switch(); assert(!worker_queued); // unavailable worker leaves the feature off
    voice_switch_call = 1;
    auto sample = [] {
        voice_schedule_switch();
        if (worker_queued) {
            assert(!locked); service_thread = false;
            worker_queued = false; voice_check_switch(nullptr, nullptr);
            service_thread = true;
        }
        return voice_switch_wanted;
    };
    assert(!sample() && lookups == 1 && !vnode_puts);
    file_exists = true;
    now += 999; assert(!sample() && lookups == 1);
    now++; assert(sample() && lookups == 2 && vnode_puts == 1);
    file_type = 2; now += 1000; assert(!sample() && vnode_puts == 2);
    file_type = VREG;
    assert(!poll(false) && logs.empty());
    assert(poll(true) == 7 && node == 3 && port == 47 && !released);
    const uint8_t want[] = {0, 1, 0, 0x34, 0, 0, 0};
    assert(!memcmp(packet, want, 7));
    start(2, 0x34, voice.pending); result();
    scalar(0x11, 0x14, 2); scalar(0x18, 2, 4);
    scalar(0x1f, 1, 4); scalar(0x20, 0, 4);
    voice_receive(4, 47, packet, length); assert(voice.pending);
    voice_receive(3, 46, packet, length); assert(voice.pending);
    packet[1]++; voice_receive(3, 47, packet, length); assert(voice.pending); packet[1]--;
    packet[3]++; voice_receive(3, 47, packet, length); assert(voice.pending); packet[3]--;
    packet[0] = 4; voice_receive(3, 47, packet, length); assert(voice.pending); packet[0] = 2;
    voice_receive(3, 47, packet, length); assert(!voice.pending);
    assert(logs.find("current mode 20 service-domain 2 usage 1 voice-domain 0") != std::string::npos);
    assert(poll(true) == 7 && cbd_rd16(packet + 3) == 0x24);
    reply(0x24);
    assert(poll(true) == 7 && cbd_rd16(packet + 3) == 0x4d);
    reply(0x4d);
    assert(!poll(true));
    voice_note("tx", metadata(9), packet, 0); // truncated protocol input
    start(0, 0x20);
    const uint8_t secret[] = "PRIVATE-DIAL";
    tlv(1, secret, sizeof secret);
    uint8_t before[512]; memcpy(before, packet, length);
    voice_note("tx", metadata(9), packet, length);
    assert(!memcmp(before, packet, length) && logs.find("PRIVATE-DIAL") == std::string::npos);
    assert(poll(true) == 7 && cbd_rd16(packet + 3) == 0x34);
    assert(!poll(false) && released && !voice.pending);
    auto after = logs;
    start(0, 0x20); voice_note("tx", metadata(9), packet, length);
    assert(!poll(false) && !released && after == logs);
    // Re-enable cannot accept a late response from the previous generation.
    uint16_t previous = voice.sequence;
    assert(poll(true) == 7 && voice.pending != previous);
    start(2, 0x34, previous); result(); voice_receive(3, 47, packet, length);
    assert(voice.pending);
}

static void failures()
{
    reset(); assert(poll(true)); now += 4999; assert(!poll(true) && !voice.failed);
    now++; assert(!poll(true) && voice.failed && !voice.pending);
    auto after = logs;
    now += 10000; assert(!poll(true) && after == logs);
    poll(false); assert(poll(true)); reply(0x34, 0x5e);
    assert(voice.failed && !voice.pending && !poll(true));
    assert(logs.find("snapshot failed msg 0x34 error 94") != std::string::npos);
    poll(false); assert(poll(true));
    start(2, 0x34, voice.pending); // missing mandatory result
    voice_receive(3, 47, packet, length);
    assert(voice.failed && logs.find("error -1") != std::string::npos);
    reset(); cbq_server_del(&bb_qmux, 3, 3, 47);
    assert(!poll(true) && !voice.pending);
    assert(cbq_server_new(&bb_qmux, 3, 1, 3, 47)); assert(poll(true));
}

static void nas_fields()
{
    reset(); poll(true); logs.clear();
    start(0, 0x33);
    scalar(0x11, 0x14, 2); scalar(0x18, 2, 4);
    scalar(0x21, 1, 4); scalar(0x23, 0, 4); scalar(0x17, 0, 1);
    const uint8_t bands[] = {0x80, 3, 0, 0, 0, 0, 0, 0}; tlv(0x12, bands, 8);
    voice_note("tx", metadata(3), packet, length);
    assert(logs.find("set mode 20 service-domain 2 usage 1 voice-domain 0 duration 0") != std::string::npos);
    assert(logs.find("legacy-band mask 0x0000000000000380") != std::string::npos);
    start(2, 0x24); result();
    const uint8_t serving[] = {1, 1, 1, 2, 2, 4, 8}; tlv(1, serving, sizeof serving);
    voice_note("rx", metadata(3), packet, length);
    assert(logs.find("CS-attach 1 PS-attach 1") != std::string::npos);
    assert(logs.find("serving RAT 4") != std::string::npos && logs.find("serving RAT 8") != std::string::npos);
    start(2, 0x4d); result();
    const uint8_t status[] = {2, 2, 1}; tlv(0x14, status, 3);
    const uint8_t info[] = {1, 3, 1, 3, 0, 0xff}; tlv(0x19, info, sizeof info);
    scalar(0x21, 1, 1); scalar(0x2a, 2, 4); scalar(0x31, 1, 4);
    voice_note("rx", metadata(3), packet, length);
    assert(logs.find("LTE domain 3 capability 3 roaming -1") != std::string::npos);
    assert(logs.find("LTE voice-support 1 voice-domain 2 registration-domain 1") != std::string::npos);
    auto after = logs;
    packet[5]++; voice_note("rx", metadata(3), packet, length);
    assert(after == logs);
}

static void incoming_delivery()
{
    reset(); poll(true);
    assert(cbq_server_new(&bb_qmux, 9, 1, 3, 46));
    // Both ABM QMUX channels, and two simultaneous Voice clients per channel.
    for (uint32_t channel = 0; channel < 2; channel++) for (uint32_t client = 0; client < 2; client++) {
        int slot = cbq_client_alloc(&bb_qmux, channel, 9); assert(slot >= 0);
        uint32_t local = CBQ_PORT_BASE + slot;
        for (uint8_t state : {2, 3, 9}) {
            start(4, 0x2e, 0);
            const uint8_t calls[] = {2, 7, state, 0, 2, 2, 0, 0, 8, 6, 0, 1, 3, 1, 0};
            tlv(1, calls, sizeof calls);
            const uint8_t ends[] = {2, 8, 0x11, 0, 7, 0x10, 0}; tlv(0x14, ends, sizeof ends);
            const uint8_t private_value[] = "PRIVATE-CALLER"; tlv(0x10, private_value, sizeof private_value);
            // Redirecting-number bytes can resemble a counted array too.
            const uint8_t redirect[] = {1, 7, 'A', 'B'}; tlv(0x19, redirect, sizeof redirect);
            uint8_t frame[1024];
            cbq_result r = cbq_from_modem(&bb_qmux, local, 3, packet, length, frame, sizeof frame);
            assert(r.kind == CBQ_TO_HOST && r.channel == channel && r.cid == client + 1);
            uint8_t before[512]; memcpy(before, packet, length);
            voice_note("rx", r, packet, length);
            assert(!memcmp(before, packet, length));
            assert(!memcmp(frame + CBQ_HDR, packet, length) && frame[4] == 9 && frame[5] == r.cid);
        }
    }
    assert(logs.find("call-id 7 state 9 type 0 direction 2 RAT 2 end-reason 16") != std::string::npos);
    assert(logs.find("call-id 8 state 6 type 0 direction 1 RAT 3 end-reason 17") != std::string::npos);
    assert(logs.find("end-reason 16961") == std::string::npos);
    assert(logs.find("PRIVATE-CALLER") == std::string::npos);
    start(2, 0x20); result(0x0d); scalar(0x10, 7, 1);
    voice_note("rx", metadata(9), packet, length);
    assert(logs.find("dial call-id 7 error 0xd") != std::string::npos);
    // Every truncated TLV/array length and arbitrary optional data is safe.
    for (uint32_t n = 0; n < 40; n++) {
        start(4, 0x2e);
        uint8_t bad[40]; memset(bad, 0xff, sizeof bad); tlv(1, bad, n);
        voice_note("rx", metadata(9), packet, length);
        for (uint32_t cut = 0; cut < length; cut++) voice_note("rx", metadata(9), packet, cut);
    }
    assert(logs.find("malformed metadata; forwarded unchanged") != std::string::npos);
}

static void audio_worker()
{
    service_thread = false; uva_work(); service_thread = true;
}

static void call_audio()
{
 reset();
 assert(umi_voice_audio_start(42,UMI_VOICE_LTE,UMI_VOICE_RECEIVER)==EPERM);
 voice_switch_wanted=true;poll(true);
 const uint32_t key=0x12345678;
 assert(umi_voice_audio_start(key,UMI_VOICE_LTE,UMI_VOICE_RECEIVER)==EINPROGRESS);
 assert(umi_voice_audio_start(42,UMI_VOICE_GSM,UMI_VOICE_SPEAKER)==EBUSY);
 audio_mhi_error=EAGAIN;audio_worker();assert(audio_memory_calls==1&&!audio_apr_calls);
 audio_mhi_error=0;audio_worker();
 assert(!umi_voice_audio_status(key)&&uva_session.started&&route_owned&&route_takes==1);
 const uint32_t expected[]={0x11378,0x110ff,0x11140,0x11327,0x13169,0x1133d,0x1133d,0x1133d,0x1133d,0x1133d,0x13198,0x100c6,0x1123e,0x112c2,0x1138b,0x11190};
 assert(audio_commands.size()==16);
 for(unsigned i=0;i<16;i++)assert(audio_commands[i].op==expected[i]);
 auto count=audio_commands.size();audio_worker();assert(count==audio_commands.size());
 assert(umi_voice_audio_mute(key,1)==EINPROGRESS);audio_worker();assert(!umi_voice_audio_status(key));
 assert(cbd_rd16(audio_commands[count].data.data()+2)==1);
 assert(umi_voice_audio_volume(key,0)==EINPROGRESS);audio_worker();
 assert(cbd_rd32(audio_commands.back().data.data()+2)==5);
 assert(umi_voice_audio_volume(key,101)==EINVAL);
 assert(umi_voice_audio_route(key,UMI_VOICE_SPEAKER)==EINPROGRESS);audio_worker();
 assert(!umi_voice_audio_status(key)&&physical_route==UMI_VOICE_SPEAKER&&route_takes==2&&route_releases==1);
 assert(umi_voice_audio_stop(42)==ENOENT);
 assert(umi_voice_audio_stop(key)==EINPROGRESS);audio_worker();
 assert(!umi_voice_audio_status(key)&&!route_owned&&!uva_session.started&&route_releases==2);
 const uint32_t ending[]={0x11192,0x1123f,0x1003c,0x1003c,0x1003c};
 for(unsigned i=0;i<5;i++)assert(audio_commands[audio_commands.size()-5+i].op==ending[i]);
 // CVD controls V2 versus V3; 2.2 legacy channel count is selectable.
 reset();voice_switch_wanted=true;cvd_version="2.1";
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 assert(!umi_voice_audio_status(7)&&audio_commands[4].op==0x112bf);
 umi_voice_audio_stop(7);audio_worker();
 reset();voice_switch_wanted=true;cvd_version="2.2";option_name="cvp-api";option_value="1\n";
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 assert(!umi_voice_audio_status(7)&&audio_commands[4].op==0x13169&&audio_commands[5].op==0x13199);
 umi_voice_audio_stop(7);audio_worker();
 reset();voice_switch_wanted=true;option_name="vsid";option_value="10c02000\n";session_name="default volte voice";
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 assert(!umi_voice_audio_status(7));umi_voice_audio_stop(7);audio_worker();
 reset();voice_switch_wanted=true;cvd_version="x";
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 assert(umi_voice_audio_status(7)==EPROTO&&!route_owned&&audio_commands.size()==1);
 // Amp health failure ends the physical route and reports the real error.
 reset();voice_switch_wanted=true;
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_SPEAKER);audio_worker();
 route_health=EIO;audio_worker();assert(umi_voice_audio_status(7)==EIO&&!route_owned&&!uva_session.started);
 // NACK each setup command: no success publication; destroy acquired handles.
 for(unsigned failed=1;failed<=16;failed++){
  reset();voice_switch_wanted=true;fail_command=failed;
  umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
  assert(umi_voice_audio_status(7)==EIO&&!route_owned&&!uva_session.started);
  auto before=audio_commands.size();audio_worker();assert(before==audio_commands.size());
 }
 // Lost CVP-create reply retains the AFE claim rather than allowing another owner.
 reset();voice_switch_wanted=true;fail_command=5;command_error=ETIMEDOUT;
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 assert(umi_voice_audio_status(7)==ETIMEDOUT&&route_owned&&uva_session.unknown);
 umi_voice_audio_stop(7);audio_worker();assert(umi_voice_audio_status(7)==EBUSY&&route_owned);
 // Failed STOP retains routes; explicit STOP retries the remaining teardown.
 reset();voice_switch_wanted=true;
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 fail_command=(unsigned)audio_commands.size()+1;
 umi_voice_audio_stop(7);audio_worker();assert(umi_voice_audio_status(7)==EIO&&route_owned);
 umi_voice_audio_stop(7);audio_worker();assert(!umi_voice_audio_status(7)&&!route_owned);
 // Cancellation during mailbox setup never creates a session; next worker completes END.
 reset();voice_switch_wanted=true;audio_cancel_in_apr=true;
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();audio_worker();
 assert(!uva_control.active&&!umi_voice_audio_status(7)&&audio_commands.empty());
 // File switch removal tears down an established call and restores media.
 reset();voice_switch_wanted=true;
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 voice_switch_wanted=false;audio_worker();assert(!uva_control.active&&!route_owned);
 // QMI duplicates and GSM/LTE trigger the same passive state machine.
 for(uint8_t rat:{2,4}){
  reset();voice_switch_wanted=true;poll(true);
  start(4,0x2e);const uint8_t calls[]={1,7,3,(uint8_t)(rat==4?2:0),2,rat,0,0};tlv(1,calls,sizeof calls);
  IOLockLock(bb_lock);voice_note("rx",metadata(9),packet,length);IOLockUnlock(bb_lock);
  auto generation=uva_control.generation;
  IOLockLock(bb_lock);voice_note("rx",metadata(9),packet,length);IOLockUnlock(bb_lock);
  assert(generation==uva_control.generation);audio_worker();assert(uva_session.started);
  packet[12]=9;IOLockLock(bb_lock);voice_note("rx",metadata(9),packet,length);IOLockUnlock(bb_lock);
  audio_worker();assert(!route_owned);
 }
 reset();voice_switch_wanted=true;audio_apr_error=ETIMEDOUT;
 umi_voice_audio_start(7,UMI_VOICE_LTE,UMI_VOICE_RECEIVER);audio_worker();
 assert(umi_voice_audio_status(7)==ETIMEDOUT&&uva_transport_error==ETIMEDOUT);
 assert(logs.find("12345678")==std::string::npos);
 puts("PASS voice sessions: vendor payloads and passive order, IMS/GSM/LTE ownership, mute/volume/route, failure at every setup step, teardown retry, cancellation, switch removal");
}

int main()
{
    switch_and_queries(); failures(); nas_fields(); incoming_delivery();
    call_audio();
    printf("PASS voice: switch, read-only NAS requests, source/transaction matching, timeout latch, sanitized metadata, incoming QMUX delivery, malformed arrays (ASan/UBSan)\n");
}
