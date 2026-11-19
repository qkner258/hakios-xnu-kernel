/* Host register model: execute the actual PCIe2 sequence, never MMIO. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define MD_BIT(n) (1u<<(n))
#define MD_TRY(x) do { int e=(x);if(e)return e; } while(0)
#define MD_LOG(...) do {} while(0)
#define MD_GCC 0x00100000u
#define MD_GPIO 0x0f500000u
#define MD_PARF 0x01c10000u
#define MD_PHY 0x01c16000u
#define MD_DBI 0x64000000u
#define MD_ATU 0x64001000u
#define MD_CONF 0x64100000u
#define MD_BAR 0x64300000u
static const char *md_stage;
static bool md_gpio_owned,md_link,rails,clock_ok,phy_started;
static uint32_t md_bar_size;
static int fault;
static unsigned touches,unclocked,phy_writes,pcie0_aux_touches;
static uint64_t elapsed;
static struct {uint32_t a,v;} regs[4096];
static unsigned used;
static uint32_t raw(uint32_t a) {for(unsigned i=0;i<used;i++)if(regs[i].a==a)return regs[i].v;return 0;}
static bool peripheral(uint32_t a) {return (a>=MD_PARF&&a<MD_PHY+0x1000)||(a>=MD_DBI&&a<MD_BAR+0x100000);}
static void md_write(uint32_t a,uint32_t v)
{
    touches++;
    assert((a>=MD_GCC&&a<MD_GCC+0xa0000)||(a>=MD_GPIO&&a<MD_GPIO+0x100000)||peripheral(a));
    if(a==MD_GCC+0x6b038||a==MD_GCC+0x6b03c) pcie0_aux_touches++;
    if(peripheral(a)&&!clock_ok)unclocked++;
    if(a==MD_GCC+0x52010){assert(rails);clock_ok=(v&0x7c00)==0x7c00;}
    if(a>=MD_PHY&&a<MD_PHY+0x1000)phy_writes++;
    if(a==MD_PHY+0xa44&&v==3)phy_started=true;
    if(a==MD_CONF+4)assert(!(v&4)); /* enumeration must never enable DMA */
    for(unsigned i=0;i<used;i++)if(regs[i].a==a){regs[i].v=v;return;}
    assert(used<4096);regs[used].a=a;regs[used++].v=v;
}
static uint32_t md_read(uint32_t a)
{
    if(peripheral(a)&&!clock_ok)unclocked++;
    if(a==MD_GCC+0x6f02c&&fault==1)return 1;
    if(a==MD_GCC+0x6004)return fault==2?0:0x80000000;
    if(a==MD_GCC)return fault==3?0:0x80000000;
    if(a==MD_GCC+0x6b038||a==MD_GCC+0x6038||a==MD_GCC+0x6f014)return 0;
    if(a==MD_PHY+0xa14)return fault==4?64:0;
    /* The host model has no modem GPIO device; once AP_STATUS is raised,
       model the SDX55 PBL asserting MDM_STATUS so link sequencing can run. */
    if(a==MD_GPIO+3*0x1000+4)return raw(MD_GPIO+56*0x1000+4)==2?1:0;
    if(a==MD_DBI+0x72c)return fault==5?0:(1u<<4);
    if(a==MD_DBI+0x80)return raw(a)|(1u<<29);
    if(a==MD_CONF){assert(phy_started);assert(raw(MD_GPIO+85*0x1000+4)==2);return fault==6?0xffffffff:0x030617cb;}
    if(a==MD_CONF+0x10&&raw(a)==0xffffffff)return fault==7?0xffe00000:0xfff00000;
    return raw(a);
}
static void delay(unsigned us){elapsed+=us;}
static int md_update(uint32_t a,uint32_t clear,uint32_t set){uint32_t v=md_read(a);if(v==UINT32_MAX)return EIO;md_write(a,(v&~clear)|set);return 0;}
static int md_wait(uint32_t a,uint32_t mask,uint32_t value,unsigned us){for(unsigned t=0;t<us;t+=100){if((md_read(a)&mask)==value)return 0;delay(100);}return ETIMEDOUT;}
static int md_output(unsigned pin,bool high,unsigned cfg){MD_TRY(md_update(MD_GPIO+pin*4096+4,2,high?2:0));return md_update(MD_GPIO+pin*4096,0x3ff,cfg|512);}
static void md_address(uint32_t a,uint64_t v){md_write(a+4,(uint32_t)(v>>32));md_write(a,(uint32_t)v);}
static int umi_rpmh_modem_power(void){rails=true;return 0;}
#include "umi_modem_pcie.inc"
static void reset(int mode){
    memset(regs,0,sizeof(regs));used=touches=unclocked=phy_writes=0;elapsed=0;
    md_link=md_gpio_owned=rails=clock_ok=phy_started=false;md_bar_size=0;fault=mode;pcie0_aux_touches=0;
    if(mode==1||mode==8){regs[used].a=MD_GCC+0x6f02c;regs[used++].v=1;
        regs[used].a=MD_GCC+0x6f018;regs[used++].v=mode==8?1:0x10b;}
}
int main(void)
{
    reset(0);assert(md_pcie()==0);assert(md_link&&md_gpio_owned&&md_bar_size==0x100000);assert(!unclocked&&phy_writes>100&&pcie0_aux_touches==0);
    assert(raw(MD_GCC+0x6f018)==0x10b);assert(raw(MD_CONF+4)==0x402);assert(elapsed>=202000);
    reset(1);assert(md_pcie()==0);assert(md_link&&rails&&pcie0_aux_touches==0);
    reset(8);assert(md_pcie()==0);assert(md_link&&raw(MD_GCC+0x6f018)==0x10b);
    reset(2);assert(md_pcie()==ETIMEDOUT);assert(!unclocked&&phy_writes==0);
    reset(3);assert(md_pcie()==0);assert(!unclocked&&phy_writes>100&&pcie0_aux_touches==0);
    reset(4);assert(md_pcie()==ETIMEDOUT);assert(!md_link&&!unclocked);
    reset(5);assert(md_pcie()==ETIMEDOUT);assert(!md_link&&!unclocked);
    reset(6);assert(md_pcie()==ENODEV);assert(!md_link&&!unclocked);
    reset(7);assert(md_pcie()==ERANGE);assert(!(raw(MD_CONF+4)&4));
    assert(md_stage!=NULL);
    puts("PASS PCIe2 model: sequencing, no unclocked access, clock conflict, PHY/link timeouts, PCI identity, BAR bounds, DMA disabled");
}
