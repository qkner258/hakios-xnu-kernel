/* Actual receiver implementation with register/SoundWire/AFE firmware model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "umi_voice_audio.h"
struct block {const char *name;uint32_t phys,size;uintptr_t base;};
struct afe_param {uint32_t module,param;uint16_t size,reserved;} __attribute__((packed));
struct afe_clock_set {uint32_t version,id,hz;uint16_t attribute,root;uint32_t enable;} __attribute__((packed));
#define AFE_MODULE_CLOCK_SET 0x1028f
#define AFE_PARAM_ID_CLOCK_SET 0x10290
#define AFE_MODULE_AUDIO_DEV_INTERFACE 0x1020c
#define AFE_PARAM_ID_CODEC_DMA_CONFIG 0x102b8
#define AFE_PORT_CMD_SET_PARAM_V2 0x100ef
#define AFE_PORT_CMD_DEVICE_START 0x100e5
#define AFE_PORT_CMD_DEVICE_STOP 0x100e6
#define APR_SVC_AFE 4
#define SWRM_MCP_STATUS 0x1040
#define SWRM_MCP_SLV_STATUS 0x1090
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_1(n) (0x530+8*(n))
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_2(n) (0x534+8*(n))
#define SWRM_MCP_FRAME_CTRL_BANK(n) (0x101c+0x40*(n))
#define SWRM_DP_PORT_CTRL_BANK(n,m) (0x1024+0x100*(n)+0x40*(m))
#define SWRM_DP_PORT_CTRL_2_BANK(n,m) (0x1028+0x100*(n)+0x40*(m))
#define SWRM_DP_PORT_HCTRL_BANK(n,m) (0x1034+0x100*(n)+0x40*(m))
#define SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(n) (0xe0+0x10*(n))
#define SWRS_SCP_FRAME_CTRL_BANK(n) (0x60+0x10*(n))
#define SWRS_DP_CHANNEL_ENABLE_BANK(n,m) (0x120+0x100*(n)+0x10*(m))
#define SWRS_DP_SAMPLE_CONTROL_1_BANK(n,m) (0x122+0x100*(n)+0x10*(m))
#define SWRS_DP_SAMPLE_CONTROL_2_BANK(n,m) (0x123+0x100*(n)+0x10*(m))
#define SWRS_DP_OFFSET_CONTROL_1_BANK(n,m) (0x124+0x100*(n)+0x10*(m))
#define SWRS_DP_OFFSET_CONTROL_2_BANK(n,m) (0x125+0x100*(n)+0x10*(m))
#define SWRS_DP_LANE_CONTROL_BANK(n,m) (0x128+0x100*(n)+0x10*(m))
static uint32_t registers[5][3072];
static uint8_t wcd[65536];
static unsigned mapped_count,step,fail,clocks,starts,stops;
static bool broken,locked,master_taken=true;
static uint8_t codec_device=7;
static int mic_lock;
static struct block tx_bus={"TX",0,0x2000,0},*swr_bus=&tx_bus;
static struct block lpi_slew={"slew",0,0x1000,0};
static bool result(void){return ++step!=fail;}
static void say(const char *format,...){(void)format;}
static void lck_mtx_lock(int *p){assert(p==&mic_lock&&!locked);locked=true;}
static void lck_mtx_unlock(int *p){assert(p==&mic_lock&&locked);locked=false;}
static void sleep_ms(unsigned ms){(void)ms;}
static void spin_us(unsigned us){(void)us;}
static bool mapped(struct block *b,uint32_t offset){assert(offset<b->size);if(!b->base)b->base=(uintptr_t)registers[mapped_count++];return true;}
static uint32_t rd(uintptr_t p){return *(uint32_t *)p;}
static bool put(struct block *b,const char *name,uint32_t offset,uint32_t value,uint32_t check){
 (void)name;(void)check;assert(locked&&offset<b->size&&b->base);
 if(!result()){broken=true;return false;}
 *(uint32_t *)(b->base+offset)=value;return true;
}
static bool put_bits(struct block *b,const char *name,uint32_t off,uint32_t mask,uint32_t value){return put(b,name,off,(rd(b->base+off)&~mask)|(value&mask),mask);}
static void restore(struct block *b,uint32_t offset,uint32_t value,uint32_t check){(void)put(b,"restore",offset,value,check);}
static bool afe_service_param(const char *name,uint32_t module,uint32_t param,const void *p,uint16_t n){
 (void)name;assert(locked&&module==0x1028f&&param==0x10290&&n==sizeof(struct afe_clock_set));
 const struct afe_clock_set *c=p;assert(c->id==0x30e||c->id==0x30f);
 if(!result())return false;
 if(c->enable)clocks++;else {assert(clocks);clocks--;}return true;
}
static bool master_on(void){master_taken=true;return result();}
static void master_off(void){master_taken=false;}
static uint32_t swr_rd(uint32_t off){
 if(off==SWRM_ENUMERATOR_SLAVE_DEV_ID_1(1))return 0x01170224;
 if(off==SWRM_ENUMERATOR_SLAVE_DEV_ID_2(1))return 0x0d;
 if(off==SWRM_MCP_SLV_STATUS)return 1u<<2;
 return rd(swr_bus->base+off);
}
static bool swr_read(uint8_t dev,uint16_t reg,uint8_t *v){assert(dev==1);*v=wcd[reg];return true;}
static bool codec_put(uint16_t reg,uint8_t value,uint8_t check){(void)check;assert(codec_device==1);if(!result()){broken=true;return false;}wcd[reg]=value;return true;}
static bool codec_bits(uint16_t reg,uint8_t mask,uint8_t value){return codec_put(reg,(wcd[reg]&~mask)|(value&mask),mask);}
static void to_all(uint16_t reg,uint8_t value){wcd[reg]=value;}
static void broadcast(uint16_t reg,uint8_t value){wcd[reg]=value;*(uint32_t *)(swr_bus->base+SWRM_MCP_STATUS)=(reg==SWRS_SCP_FRAME_CTRL_BANK(1));}
static bool command(const char *name,uint8_t svc,uint32_t opcode,const void *p,uint32_t n,uint32_t wanted,void *answer){
 (void)name;(void)wanted;(void)answer;assert(svc==4);
 if(!result())return false;
 if(opcode==AFE_PORT_CMD_SET_PARAM_V2){assert(n==44&&((const uint16_t *)p)[0]==0xb030);}
 else {assert(n==4&&((const uint16_t *)p)[0]==0xb030);if(opcode==0x100e5)starts++;else{assert(opcode==0x100e6);stops++;}}
 return true;
}
#include "umi_voice_audio_receiver.inc"
int main(void){
 mapped(&lpi_slew,0);
 wcd[0x3408]=0xf0;wcd[0x3409]=0xf0; /* TX clocks must survive the RX path */
 assert(!umi_voice_receiver(1,70));assert(starts==1&&uva_rx.port&&clocks==2);
 assert(codec_device==7&&swr_bus==&tx_bus&&master_taken);
 assert(wcd[0x300a]&0x80);assert((wcd[0x3408]&0xf0)==0xf0);
 assert(!umi_voice_receiver(2,0));assert(rd(uva_rx_macro.base+0x400)&0x10);
 assert(!umi_voice_receiver(1,100));assert(!(rd(uva_rx_macro.base+0x400)&0x10));
 assert(!umi_voice_receiver(0,0));assert(stops==1&&!clocks&&!uva_rx.saved);
 assert(wcd[0x3408]==0xf0&&wcd[0x3409]==0xf0&&!wcd[0x300a]);
 assert(codec_device==7&&swr_bus==&tx_bus&&master_taken&&!locked);
 // Fail each MMIO/SoundWire write after clock votes; teardown restores TX.
 for(unsigned f=3;f<80;f++){
  step=0;fail=f;broken=false;
  int error=umi_voice_receiver(1,70);
  fail=0;
  assert(error==0||error==EIO);
  assert(!umi_voice_receiver(0,0));
  assert(!clocks&&!uva_rx.port&&!uva_rx.saved&&wcd[0x3408]==0xf0&&wcd[0x3409]==0xf0);
 }
 puts("PASS voice receiver: actual RX clock/pin/SoundWire/EAR/AFE setup, mute/volume, teardown, register restoration and TX isolation through injected write failures");
}
