/* Actual APR sender/parser against a firmware reply model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#define bzero(p,n) memset(p,0,n)
#define APR_DOMAIN_ADSP 4
#define APR_DOMAIN_APPS 5
#define APR_MSG_TYPE_SEQ_CMD 2
#define APR_HDR_FIELD(type,bytes) ((uint16_t)((type)<<8|((bytes)/4)<<4))
#define APR_BASIC_RSP_RESULT 0x110e8u
struct apr_header {
 uint16_t field,size; uint8_t from_service,from_domain; uint16_t from_port;
 uint8_t to_service,to_domain; uint16_t to_port; uint32_t token,opcode;
};
enum { CHANNEL_APR };
struct channel { bool open; };
static struct { struct channel channel[1]; } glink;
static bool audio_up=true,locked;
static int apr_lock,send_error,mode;
static unsigned sent,waits,wire_bytes;
static uint8_t wire[212];
static void tell(const char *format,...){(void)format;}
static void lck_mtx_lock(int *p){assert(p==&apr_lock&&!locked);locked=true;}
static void lck_mtx_unlock(int *p){assert(p==&apr_lock&&locked);locked=false;}
static void sleep_ms(uint32_t ms);
static int glink_send(struct channel *c,const void *p,uint32_t n)
{
 assert(locked&&c==glink.channel&&n<=sizeof(wire));
 memcpy(wire,p,n);wire_bytes=n;sent++;return send_error;
}
#include "umi_voice_audio_apr.inc"
static void sleep_ms(uint32_t ms)
{
 assert(ms==1&&locked);waits++;
 if(mode==1)return;
 struct apr_header h;memcpy(&h,wire,sizeof(h));
 uint8_t d=h.from_domain;h.from_domain=h.to_domain;h.to_domain=d;
 uint16_t p=h.from_port;h.from_port=h.to_port;h.to_port=p;
 bool create=h.opcode==0x110ff||h.opcode==0x11140||h.opcode==0x13169||h.opcode==0x112bf;
 uint32_t command=h.opcode;
 if(create)h.from_port=0x40;
 h.opcode=APR_BASIC_RSP_RESULT;
 uint32_t payload[2]={command,0};
 struct apr_header wrong=h;
 wrong.from_service=4;assert(!uva_apr_input(&wrong,(uint8_t *)payload,8));
 wrong=h;wrong.to_domain=3;assert(!uva_apr_input(&wrong,(uint8_t *)payload,8));
 wrong=h;wrong.to_port++;assert(!uva_apr_input(&wrong,(uint8_t *)payload,8));
 wrong=h;wrong.from_domain=3;assert(!uva_apr_input(&wrong,(uint8_t *)payload,8));
 wrong=h;wrong.from_port++;if(!create)assert(uva_apr_input(&wrong,(uint8_t *)payload,8));
 wrong=h;wrong.token--;assert(uva_apr_input(&wrong,(uint8_t *)payload,8));
 wrong=h;wrong.opcode=0x100be;assert(uva_apr_input(&wrong,(uint8_t *)payload,4));
 payload[0]++;assert(uva_apr_input(&h,(uint8_t *)payload,8));payload[0]--;
 assert(!uva_apr.done);
 if(mode==2)payload[1]=3;
 if(mode==4&&create)h.from_port=0;
 assert(uva_apr_input(&h,(uint8_t *)payload,mode==3?4:8));
 if(command==0x11378&&!mode){
  assert(!uva_apr.done);h.opcode=0x11379;
  const uint8_t version[]="2.4";
  assert(uva_apr_input(&h,version,sizeof(version)));
 }
}
int main(void)
{
 glink.channel[0].open=true;
 uint32_t dsp;
 assert(!umi_voice_mailbox_config(0x110020000ull,0xb0fe0000,&dsp)&&!dsp&&sent==1&&waits==1);
 const uint8_t expected[]={0x50,2,40,0,9,5,3,1,9,4,0,0,1,0,0,0x76,0x3b,0x33,1,0,
 0,0,2,0x10,1,0,0,0,0,0,0xfe,0xb0,0,0,0,0,0,0,2,0};
 assert(wire_bytes==sizeof(expected)&&!memcmp(wire,expected,sizeof(expected)));
 struct uva_reply r;
 const uint32_t commands[]={0x110ff,0x11140,0x112bf,0x13169,0x11327,0x1123e,0x11190,0x11192,0x1123f,0x1003c,0x1138b,0x112c2};
 for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++){
  uint8_t service=i==1||i==10?10:i==2||i==3||i==11?11:9;
  uint16_t port=i<4?0:0x40;
  const uint8_t data[]={1,2,3,4,5,6};
  assert(!umi_voice_apr(service,port,commands[i],data,sizeof(data),&r));
  struct apr_header h;memcpy(&h,wire,sizeof(h));
  assert(h.size==26&&h.from_service==service&&h.to_service==service&&h.from_port==0x103&&h.to_port==port);
  assert(h.opcode==commands[i]&&h.from_domain==5&&h.to_domain==4&&!memcmp(wire+20,data,6));
  assert(r.port==0x40);
 }
 assert(!umi_voice_apr(9,0,0x11378,NULL,0,&r)&&r.bytes==4&&!strcmp((char *)r.data,"2.4"));
 mode=2;assert(umi_voice_mailbox_config(1,2,&dsp)==EIO&&dsp==3);
 mode=3;assert(umi_voice_mailbox_config(1,2,&dsp)==EPROTO&&dsp==UINT32_MAX);
 mode=4;assert(umi_voice_apr(9,0,0x110ff,NULL,0,&r)==EPROTO);
 mode=1;waits=0;assert(umi_voice_mailbox_config(1,2,&dsp)==ETIMEDOUT&&waits==500);
 assert(!uva_apr.waiting);
 send_error=ENOBUFS;assert(umi_voice_mailbox_config(1,2,&dsp)==ENOBUFS);
 audio_up=false;unsigned before=sent;assert(umi_voice_mailbox_config(1,2,&dsp)==ENXIO&&sent==before);
 assert(!locked);
 puts("PASS voice APR: exact mailbox/header/service/port/handle bytes; create, attach, start/stop, mute/volume; version, foreign/late/accepted/malformed/error/timeout replies");
}
