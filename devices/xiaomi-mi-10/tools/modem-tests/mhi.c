/* Host protocol model: actual MHI parser with fake MMIO, DMA and read-only VFS. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <stdarg.h>
#define MD_BIT(n) (1u<<(n))
#define MD_TRY(x) do {int e=(x);if(e)return e;} while(0)
static char probe_log[4096];
static void test_log(const char *format,...) __attribute__((format(printf,1,2)));
static void test_log(const char *format,...)
{
    char line[512];va_list ap;va_start(ap,format);vsnprintf(line,sizeof(line),format,ap);va_end(ap);
    if (strstr(line,"data selftest") || strstr(line,"data packets")) {
        assert(strlen(probe_log)+strlen(line)<sizeof(probe_log));strcat(probe_log,line);
    }
}
#define MD_LOG(...) test_log(__VA_ARGS__)
#define MD_BAR 0x64300000u
#define MD_SMMU 0x15000000u
#define MD_GPIO 0x0f500000u
#define MD_PARF 0x01c10000u
#define MD_IOVA 0xb0000000ull
#define MD_DMA_SIZE 0x1000000u
#define NSEC_PER_MSEC 1000000
static uint32_t md_bhi=0x100,md_bar_size=0x100000,md_cb=0x15080000;
static unsigned md_cb_index=5;
static uint64_t md_deadline=240000000,now;
static const char *md_stage;
static uint32_t registers[1024];
static uint64_t next_address=MD_IOVA;
struct md_buf {uint8_t *p;uint64_t a;size_t size;};
static struct md_buf md_alloc_aligned(size_t n,size_t alignment)
{
    n=(n+4095)&~4095ul;if (alignment<4096 || (alignment&(alignment-1))) alignment=4096;
    next_address=(next_address+alignment-1)&~(alignment-1);
    struct md_buf b={calloc(1,n),next_address,n};next_address+=n;
    assert(next_address<MD_IOVA+MD_DMA_SIZE);return b;
}
static struct md_buf md_alloc(size_t n){return md_alloc_aligned(n,4096);}
static void md_publish(void){}
static uint64_t md_now_us(void){return now;}
static void (*delay_hook)(void);
static void delay(unsigned us){now+=us;if(delay_hook)delay_hook();}
static uint32_t md_read(uint32_t a){if(a>=md_cb&&a<md_cb+4096)return 0;
    /* v30 fault diagnostics: TLMM, the RC PARF block and the SMMU stay readable with the link down. */
    if(a>=MD_GPIO&&a<MD_GPIO+0x100000)return 0;
    if(a>=MD_PARF&&a<MD_PARF+0x3000)return 0;
    if(a>=MD_SMMU&&a<MD_SMMU+0x100000)return 0;
    assert(a>=MD_BAR&&a<MD_BAR+4096);return registers[(a-MD_BAR)/4];}
static void md_write(uint32_t a,uint32_t v){if((a>=md_cb&&a<md_cb+4096)||(a>=MD_GPIO&&a<MD_GPIO+0x100000)||
    (a>=MD_PARF&&a<MD_PARF+0x3000)||(a>=MD_SMMU&&a<MD_SMMU+0x100000))return;
    assert(a>=MD_BAR&&a<MD_BAR+4096);registers[(a-MD_BAR)/4]=v;}
static void md_address(uint32_t a,uint64_t v){md_write(a+4,v>>32);md_write(a,v);}
static int md_update(uint32_t a,uint32_t clear,uint32_t set){md_write(a,(md_read(a)&~clear)|set);return 0;}
static int md_wait(uint32_t a,uint32_t m,uint32_t v,unsigned us){(void)a;(void)m;(void)v;(void)us;return 0;}
static int md_dma_install(void){return 0;}
static int md_enable_dma(void){return 0;}
static void delay_for_interval(unsigned v,unsigned scale){(void)scale;delay(v*1000);}
static void *kalloc_data(size_t n,int flags){(void)flags;return calloc(1,n);}
static void kfree_data(void *p,size_t n){(void)n;free(p);}
#define Z_WAITOK 0
/* Minimal read-only vnode contract; no filesystem or hardware calls. */
static const char *file_data="21 modem.mbn\n25 tz.mbn\n";
static uint64_t file_size;
static unsigned file_reads;
static uint8_t *fat_data;
static size_t fat_bytes;
static size_t fat_partition_bytes;
struct ufs_fixture { const char *name; uint8_t *data; size_t size; };
static struct ufs_fixture ufs_fixtures[4];
static unsigned ufs_fixture_count;
struct vnode_attr {uint64_t va_data_size;};
typedef void *vnode_t;
typedef void *vfs_context_t;
static void *rootvnode=(void *)1;
#define NULLVP NULL
#define VREG 1
#define UIO_READ 0
#define UIO_SYSSPACE 0
#define VATTR_INIT(a) memset(a,0,sizeof(*(a)))
#define VATTR_WANTED(a,b) ((void)(a))
static void *vfs_context_kernel(void){return NULL;}
static void *vfs_context_ucred(void *p){return p;}
static void *vfs_context_proc(void *p){return p;}
static int vnode_lookup(const char *path,int flags,void **v,void *ctx){(void)path;(void)flags;(void)ctx;*v=(void *)1;return 0;}
static int vnode_getattr(void *v,struct vnode_attr *a,void *ctx){(void)v;(void)ctx;a->va_data_size=file_size?file_size:strlen(file_data);return 0;}
static int vnode_vtype(void *v){(void)v;return VREG;}
static void vnode_put(void *v){(void)v;}
static int vn_rdwr(int op,void *v,char *dst,int n,off_t off,int seg,int flags,void *cred,int *left,void *proc){assert(op==UIO_READ);file_reads++;(void)v;(void)seg;(void)flags;(void)cred;(void)proc;memcpy(dst,file_data+off,n);*left=0;return 0;}
static int hakios_ufs_read_partition(const char *name,uint64_t offset,uint8_t *dst,uint64_t len,uint64_t *bytes)
{
    if (!strcmp(name,"modem")) {
        if (!fat_data || offset > fat_bytes || len > fat_bytes - offset) return ENOENT;
        if (bytes) *bytes=fat_partition_bytes ? fat_partition_bytes : fat_bytes;
        if (dst && len) memcpy(dst,fat_data+offset,len); return 0;
    }
    for (unsigned i=0;i<ufs_fixture_count;i++) if (!strcmp(name,ufs_fixtures[i].name)) {
        struct ufs_fixture *f=&ufs_fixtures[i];
        if (bytes) *bytes=f->size;
        if (offset > f->size || len > f->size-offset) return EINVAL;
        if (dst && len) memcpy(dst,f->data+offset,len); return 0;
    }
    return ENOENT;
}
/* Network boundary: record raw IPv4 delivered by the actual QMAP/MHI path. */
#include "umi_modem_qmi.h"
static bool net_up;
static unsigned net_attaches,net_rx;
static struct umi3_state net_settings;
static uint8_t net_tx_packet[64],net_rx_packet[64];
static size_t net_tx_length;
static int md_net_attach(const struct umi3_state *s){net_settings=*s;net_up=true;net_attaches++;return 0;}
static void md_net_down(void){net_up=false;}
static uint64_t md_net_drops(void){return 0;}
static int md_net_input(const uint8_t *p,size_t n){if(!net_up)return ENETDOWN;assert(n<=64);memcpy(net_rx_packet,p,n);net_rx++;return 0;}
static size_t md_net_take(uint8_t *p,size_t cap){size_t n=net_tx_length;assert(n<=cap);memcpy(p,net_tx_packet,n);net_tx_length=0;return n;}
static void md_idle(unsigned ms){delay_for_interval(ms,NSEC_PER_MSEC);}
/* Voice mailbox maps a separately allocated low-memory page range into the
 * existing modem context; the host records actual PTE writes. */
static uint64_t voice_tables[12][512];
static uintptr_t md_tables=(uintptr_t)voice_tables;
static uint64_t *md_table(unsigned n){assert(n<12);return voice_tables[n];}
static int hakios_smmu_lock,voice_locked;
static void lck_mtx_lock(int *lock){assert(lock==&hakios_smmu_lock&&!voice_locked);voice_locked=1;}
static void lck_mtx_unlock(int *lock){assert(lock==&hakios_smmu_lock&&voice_locked);voice_locked=0;}
static void md_clean(uintptr_t address,size_t bytes){assert(address==md_tables&&bytes==12*4096);}
#define round_page(n) (((n)+4095)&~4095ul)
#include "umi_modem_mhi.inc"
/* Bridge boundary (iokit/Kernel/hakios.umi_baseband.cpp): recorded, to check umi_modem=2's routing. */
static __attribute__((unused)) unsigned bb_servers,bb_dels,bb_ports,bb_ready,bb_in,bb_in_mux,bb_in_version;
static __attribute__((unused)) uint32_t bb_link_max=1,bb_take_length;static uint8_t bb_take_packet[64],bb_take_mux;
void umi_bb_publish(void){}
void umi_bb_modem_ready(void){bb_ready++;}
void umi_bb_new_server(uint32_t s,uint32_t v,uint32_t n,uint32_t p){(void)s;(void)v;(void)n;(void)p;bb_servers++;}
void umi_bb_del_server(uint32_t s,uint32_t n,uint32_t p){assert(s==1&&n==3&&p==52);bb_dels++;}
int umi_bb_qrtr(uint32_t dst,uint32_t n,uint32_t p,const void *b,uint32_t size){(void)n;(void)p;(void)b;(void)size;if(dst<0x4100||dst>=0x4140)return 0;bb_ports++;return 1;}
unsigned umi_bb_poll(umi_bb_send_t send,umi_bb_release_t release){(void)send;(void)release;return 0;}
void umi_bb_link(uint32_t dl_max){bb_link_max=dl_max;}
int umi_bb_data_input(uint32_t mux,const uint8_t *ip,uint32_t n){(void)n;bb_in++;bb_in_mux=mux;bb_in_version=ip[0]>>4;return 1;}
uint32_t umi_bb_data_take(uint8_t *mux,uint8_t *ip,uint32_t room){uint32_t n=bb_take_length;assert(n<=room);memcpy(ip,bb_take_packet,n);*mux=bb_take_mux;bb_take_length=0;return n;}
void umi_bb_report(unsigned second){(void)second;}
void umi_modem_wake(void){}

static void fat16_fixture(void)
{
    uint8_t *b=calloc(100,512),*root=b+2*512,*lfn=root,*shortent=root+32;
    const char *longname="long_image.mbn";
    uint32_t cluster2=0xffff,cluster3=0xffff;
    assert(b); fat_data=b;fat_bytes=100*512;
    b[0]=0xeb;b[1]=0x3c;b[2]=0x90;b[3]='M';b[4]='S';b[5]='D';b[6]='O';b[7]='S';b[8]='5';b[9]='.';b[10]='0';
    b[11]=0;b[12]=2;b[13]=1;b[14]=1;b[15]=0;b[16]=1;b[17]=16;b[18]=0;b[19]=100;b[20]=0;b[22]=1;b[23]=0;b[510]=0x55;b[511]=0xaa;
    memcpy(b+512,&cluster2,2);memcpy(b+514,&cluster3,2);
    memset(lfn,0,64);lfn[0]=0x42;lfn[11]=0x0f;
    for (unsigned i=13;i<strlen(longname);i++) { unsigned at=(i-13)<5?1+(i-13)*2:(i-13)<11?14+(i-18)*2:28+(i-24)*2; lfn[at]=longname[i]; }
    lfn+=32;memset(lfn,0,32);lfn[0]=0x01;lfn[11]=0x0f;
    for (unsigned i=0;i<13 && i<strlen(longname);i++) { unsigned at=i<5?1+i*2:i<11?14+(i-5)*2:28+(i-11)*2; lfn[at]=longname[i]; }
    lfn+=32; /* short entry follows the two LFN records */
    shortent=lfn;
    lfn[13]=0; /* UTF-16 terminator; remaining entry bytes are already zero */
    memset(shortent,0,32);memset(shortent,' ',11);memcpy(shortent,"MULTI~1",7);memcpy(shortent+8,"MBN",3);shortent[11]=0x20;shortent[26]=2;shortent[28]=7;
    memcpy(b+3*512,"fixture",7);
}
static void fat4096_nested_fixture(void)
{
    const unsigned bps=4096, sectors=44; uint8_t *b=calloc(sectors,bps),*root,*image,*sdx,*fat;
    assert(b); fat_data=b;fat_bytes=(size_t)sectors*bps;fat_partition_bytes=(size_t)114688*bps;
    b[0]=0xeb;b[1]=0x3c;b[2]=0x90;b[3]='M';b[4]='S';b[5]='D';b[6]='O';b[7]='S';b[8]='5';b[9]='.';b[10]='0';
    b[11]=0;b[12]=16;b[13]=4;b[14]=1;b[15]=0;b[16]=2;b[17]=0;b[18]=2;b[19]=0;b[20]=0;
    b[22]=15;b[23]=0;b[32]=0;b[33]=192;b[34]=1;b[35]=0;b[510]=0x55;b[511]=0xaa;
    fat=b+bps; memcpy(fat+2*2,"\xff\xff",2);memcpy(fat+3*2,"\xff\xff",2);memcpy(fat+4*2,"\xff\xff",2);
    memcpy(b+16*bps,fat,15*bps);
    root=b+31*bps;memset(root,0,32);memset(root,' ',11);memcpy(root,"IMAGE",5);root[11]=0x10;root[26]=2;
    image=b+35*bps;memset(image,0,32);memset(image,' ',11);memcpy(image,"SDX55M",6);image[11]=0x10;image[26]=3;
    sdx=b+39*bps;memset(sdx,0,32);memset(sdx,' ',11);memcpy(sdx,"SBL1    MBN",11);sdx[11]=0x20;sdx[26]=4;sdx[28]=7;memcpy(b+43*bps,"fixture",7);
}
static void setup(void)
{
    registers[0x18/4]=0x300;registers[0x20/4]=0x800;
    registers[0x10/4]=0x0280;registers[0x48/4]=0x200;registers[(md_bhi+0x28)/4]=2;
    assert(md_contexts()==0);assert((registers[0x10/4]&0xffff)==0x0280);
    assert(md_events[4].wp==0&&md_events[5].wp==0);
    assert(md_u64((uint32_t *)md_erctx.p+4*11+9)==md_events[4].mem.a);
    assert(md_u64((uint32_t *)md_erctx.p+5*11+9)==md_events[5].mem.a);
}
static void event(unsigned ring,uint64_t pointer,unsigned ch,unsigned code,unsigned bytes)
{
    struct md_ring *r=&md_events[ring];unsigned i=r->consumer;
    uint32_t *ev=(uint32_t *)r->mem.p+4*i;
    md_set64(ev,0,pointer);ev[2]=code<<24|bytes;ev[3]=ch<<24|0x22<<16;
    md_set64((uint32_t *)md_erctx.p+11*ring,7,r->mem.a+((i+1)%r->count)*16);
}
/* Device side of UL20: record each host packet and complete its TRE. */
static uint32_t ul_packets[8][64];
static unsigned ul_count;
static void complete_ul(void)
{
    struct md_channel *c=&md_channels[2];
    if (!c->tx_pending) return;
    if (ul_count<8) memcpy(ul_packets[ul_count],c->buffer.p,sizeof(ul_packets[0]));
    ul_count++;
    event(0,c->ring.a+((c->wp+MD_TRES-1)%MD_TRES)*16,20,2,52);
}
/* A QRTR v2 packet from node 3 (ports are u16 on the wire). */
static size_t from3(uint8_t *out,unsigned type,unsigned port,unsigned dst_port,const void *payload,size_t n)
{
    memset(out,0,16);out[0]=3;out[1]=(uint8_t)type;out[4]=(uint8_t)n;out[5]=(uint8_t)(n>>8);
    out[8]=3;out[10]=(uint8_t)port;out[11]=(uint8_t)(port>>8);out[12]=1;out[14]=(uint8_t)dst_port;out[15]=(uint8_t)(dst_port>>8);
    memcpy(out+16,payload,n);return 16+n;
}
static const uint8_t *ul_payload(unsigned i,unsigned type,unsigned src_port,unsigned node,unsigned port,size_t size)
{
    const uint32_t *h=ul_packets[i];
    assert(h[0]==1&&h[1]==type&&h[2]==1&&h[3]==src_port&&h[5]==size&&h[6]==node&&h[7]==port);
    return (const uint8_t *)(h+8);
}
/* Modem side of EFS sync (UL 10 / DL 11, ER 1): records host commands,
   answers memory reads with zeros, RESET with RESET_RESP; 4000-byte DL
   transfers so messages straddle completions. */
static uint8_t efs_q[70000];static size_t efs_qn,efs_qp;
static uint32_t efs_cmds[8][6];static unsigned efs_ncmds;
static void efs_put(const void *p,size_t n){assert(efs_qn+n<=sizeof(efs_q));memcpy(efs_q+efs_qn,p,n);efs_qn+=n;}
static void efs_device(void)
{
    struct md_channel *ul=&md_channels[4],*dl=&md_channels[5];
    if (ul->tx_pending) {
        uint32_t *m=(uint32_t *)ul->buffer.p;
        if (efs_ncmds<8) memcpy(efs_cmds[efs_ncmds],m,24);
        efs_ncmds++;
        if (m[0]==10 && m[2]==0x1000) { uint32_t t[13]={0,0x100000,40000};efs_put(t,52); }
        else if (m[0]==10) { static uint8_t zero[32768];efs_put(zero,m[3]); }
        else if (m[0]==7) { uint32_t r[2]={8,8};efs_put(r,8); }
        else assert(m[0]==2);
        event(1,ul->ring.a+((ul->wp+MD_TRES-1)%MD_TRES)*16,10,2,m[1]);
    } else if (dl->rx_posted && efs_qp<efs_qn) {
        size_t n=efs_qn-efs_qp<4000?efs_qn-efs_qp:4000;
        memcpy(dl->buffer.p,efs_q+efs_qp,n);efs_qp+=n;
        event(1,dl->ring.a+((dl->wp+MD_TRES-1)%MD_TRES)*16,11,2,(unsigned)n);
    }
}
static uint64_t service_start;static unsigned service_wakes;
static void service_hook(void){service_wakes++;if(now-service_start>=10000000)md_service_stop=true;}

static bool dma_range(uint64_t address,uint64_t length)
{
    return address>=MD_IOVA && address<=MD_IOVA+MD_DMA_SIZE &&
        length<=MD_IOVA+MD_DMA_SIZE-address;
}
static bool dma_ptr(uint64_t pointer,uint64_t base,uint64_t length)
{
    return pointer>=base && pointer<base+length && !((pointer-base)&15);
}
static void assert_ip_hw_dma_layout(void)
{
    assert(md_hw_events_ready);
    assert(dma_range(md_chctx.a,128*44)&&dma_range(md_erctx.a,MD_NER*44));
    for (unsigned e=4;e<6;e++) {
        struct md_ring *r=&md_events[e];
        uint32_t *ctx=(uint32_t *)md_erctx.p+e*11;
        uint64_t base=md_u64(ctx+3),len=md_u64(ctx+5);
        assert(ctx[0]==(1u<<16)&&ctx[1]==1&&ctx[2]==e+1);
        assert(dma_range(r->mem.a,r->mem.size)&&base==r->mem.a&&len==(uint64_t)r->count*16);
        assert(!(base%len));
        assert(dma_ptr(md_u64(ctx+7),base,len)&&dma_ptr(md_u64(ctx+9),base,len));
        assert(md_read(MD_BAR+md_erdb+e*8)==(uint32_t)md_u64(ctx+9));
    }
    for (unsigned n=0;n<2;n++) {
        struct md_channel *c=&md_channels[6+n];
        uint32_t *ctx=md_channel_context(c->id);
        uint64_t base=md_u64(ctx+3),len=md_u64(ctx+5);
        assert(ctx[0]==0x301&&ctx[1]==(n?2u:1u)&&ctx[2]==4+n);
        assert(dma_range(c->ring.a,c->ring.size)&&dma_range(c->buffer.a,c->buffer.size));
        assert(base==c->ring.a&&len==(uint64_t)MD_TRES*16&&!(base%len));
        assert(dma_ptr(md_u64(ctx+7),base,len)&&dma_ptr(md_u64(ctx+9),base,len));
        assert(dma_range(c->buffer.a,MD_PACKET));
        if (c->tx_pending || c->rx_posted) {
            uint32_t *tre=(uint32_t *)c->ring.p+((c->wp+MD_TRES-1)%MD_TRES)*4;
            assert(dma_range(c->ring.a+((c->wp+MD_TRES-1)%MD_TRES)*16,16));
            assert(md_u64(tre)==c->buffer.a);
        }
        assert(md_u64(ctx+9)==c->ring.a+(uint64_t)c->wp*16);
        assert(c->er==4+n&&md_channel_context(c->id)[2]==4+n);
    }
}

/* Stateful fake QRTR modem. It checks literal libqmi wire fields and keeps
 * the WDS binding associated with the same host client for the whole call. */
static unsigned data_requests,wda_bad,wds_reject;
static uint64_t data_command;
static uint8_t data_reply[512];static size_t data_reply_length;
static void reply_tlv(uint8_t *m,size_t *n,uint8_t type,uint32_t value)
{uint8_t v[4];umi3_put32(v,value);assert(!umi3_tlv(m,256,n,type,v,4));}
static void want_tlv(const uint8_t *m,size_t n,uint8_t type,const uint8_t *v,size_t vn)
{const uint8_t *p;size_t pn;assert(!umi3_find_tlv(m,n,type,&p,&pn)&&pn==vn&&!memcmp(p,v,vn));}
static void data_device(void)
{
    if (md_command_wait!=data_command) {
        data_command=md_command_wait;
        event(0,data_command,0,1,0);
        ((uint32_t *)md_events[0].mem.p)[4*md_events[0].consumer+3]=0x21u<<16;
        return;
    }
    struct md_channel *ul=&md_channels[2],*dl=&md_channels[3];
    if (ul->tx_pending) {
        const uint32_t *h=(const uint32_t *)ul->buffer.p;
        const uint8_t *m=ul->buffer.p+32;size_t n=h[5];
        uint16_t msg=umi3_le16(m+3);uint8_t r[256]={2};size_t rn=7;
        assert(h[0]==1&&h[1]==1&&h[2]==1&&h[6]==3);
        memcpy(r+1,m+1,4);reply_tlv(r,&rn,2,0);
        if (data_requests==0) {
            assert(h[3]==0x4005&&h[7]==54&&msg==0x20);
            const uint8_t ep[8]={3,0,0,0,4,0,0,0},raw[4]={2,0,0,0},qmap[4]={5,0,0,0};
            const uint8_t size[4]={0,0x80,0,0},count[4]={32,0,0,0};
            want_tlv(m,n,0x17,ep,8);want_tlv(m,n,0x11,raw,4);want_tlv(m,n,0x12,qmap,4);want_tlv(m,n,0x13,qmap,4);
            want_tlv(m,n,0x15,count,4);want_tlv(m,n,0x16,size,4);
            reply_tlv(r,&rn,0x11,2);reply_tlv(r,&rn,0x12,5);reply_tlv(r,&rn,0x13,wda_bad?9:5);reply_tlv(r,&rn,0x16,32768);
        } else {
            assert(h[3]==0x4004&&h[7]==52);
            if (data_requests==1) {
                const uint8_t ep[8]={3,0,0,0,4,0,0,0},mux=1;
                assert(msg==0xa2);want_tlv(m,n,0x10,ep,8);want_tlv(m,n,0x11,&mux,1);
            } else if (data_requests==2) {
                const uint8_t family=4;assert(msg==0x4d);want_tlv(m,n,1,&family,1);
            } else if (data_requests==3) {
                const uint8_t family=4;assert(msg==0x20);want_tlv(m,n,0x19,&family,1);want_tlv(m,n,0x14,(const uint8_t *)"cmnet",5);
                if (wds_reject) {r[10]=1;r[12]=14;} else reply_tlv(r,&rn,1,0x12345678);
            } else {
                const uint8_t mask[4]={0x10,0x23,0,0};assert(data_requests==4&&msg==0x2d);want_tlv(m,n,0x10,mask,4);
                reply_tlv(r,&rn,0x1e,0x0a010203);reply_tlv(r,&rn,0x20,0x0a010201);reply_tlv(r,&rn,0x21,0xfffffffc);
                reply_tlv(r,&rn,0x15,0x08080808);reply_tlv(r,&rn,0x16,0x01010101);reply_tlv(r,&rn,0x29,1410);
            }
        }
        umi3_finish(r,rn);data_requests++;
        data_reply_length=from3(data_reply,1,h[7],h[3],r,rn);
        event(0,ul->ring.a+((ul->wp+MD_TRES-1)%MD_TRES)*16,20,2,(unsigned)n+32);
    } else if (data_reply_length&&dl->rx_posted) {
        memcpy(dl->buffer.p,data_reply,data_reply_length);
        event(0,dl->ring.a+((dl->wp+MD_TRES-1)%MD_TRES)*16,21,2,(unsigned)data_reply_length);
        data_reply_length=0;
    }
}
#include "selftest.inc"
static void test_data(void)
{
    md_service_stop=false;md_deadline=UINT64_MAX;
    md_wda_node=md_wds_node=3;md_wda_port=54;md_wds_port=52;
    delay_hook=data_device;data_command=md_command_wait;
    assert(md_start_channel(3,21,0)==0);
    wda_bad=1;assert(md_data_start()==EPROTONOSUPPORT&&data_requests==1&&!net_attaches);
    wda_bad=0;wds_reject=1;data_requests=0;md_data_open=false;
    memset(&md_channels[6],0,2*sizeof(md_channels[0]));
    assert(md_data_start()==EIO&&data_requests==4&&!net_attaches);
    wds_reject=0;data_requests=0;md_data_open=false;
    memset(&md_channels[6],0,2*sizeof(md_channels[0]));
    assert(md_data_start()==0&&data_requests==5&&net_attaches==1&&net_up);
    assert(net_settings.address==0x0a010203&&net_settings.gateway==0x0a010201&&net_settings.mask==0xfffffffc&&net_settings.mtu==1410);
    assert(net_settings.dns1==0x08080808&&net_settings.dns2==0x01010101);
    struct md_channel *tx=&md_channels[6],*rx=&md_channels[7];
    assert(tx->id==100&&tx->er==4&&rx->id==101&&rx->er==5);
    assert(md_channel_context(100)[0]==0x301&&md_channel_context(101)[0]==0x301);
    assert_ip_hw_dma_layout();
    delay_hook=NULL;
    test_selftest();
    uint64_t tx_before=md_data_tx;
    memset(net_tx_packet,0,20);net_tx_packet[0]=0x45;net_tx_packet[3]=20;net_tx_length=20;
    assert(md_poll()==0&&tx->tx_pending&&!tx->db_mode);
    assert(tx->buffer.p[0]==0&&tx->buffer.p[1]==1&&tx->buffer.p[2]==0&&tx->buffer.p[3]==20&&!memcmp(tx->buffer.p+4,net_tx_packet,20));
    /* Hardware OOB/DB_MODE doesn't complete a packet and has no TRE pointer. */
    event(4,0,100,5,0);assert(md_poll()==0&&tx->tx_pending&&!tx->db_mode);
    md_write(MD_BAR+md_chdb+100*8,0);
    event(0,0,0,2,0);
    ((uint32_t *)md_events[0].mem.p)[4*md_events[0].consumer+3]=0x20u<<16;
    assert(md_poll()==0&&tx->tx_pending&&!tx->db_mode&&md_read(MD_BAR+md_chdb+100*8)==(uint32_t)(tx->ring.a+tx->wp*16));
    event(4,tx->ring.a+((tx->wp+127)%128)*16,100,2,24);assert(md_poll()==0&&!tx->tx_pending&&md_data_tx==tx_before+1);
    event(4,0,100,6,0);assert(md_poll()==0&&tx->db_mode);
    /* Two aggregated IPv4 frames, one with QMAP padding, plus wrong mux and command. */
    uint8_t *p=rx->buffer.p;memset(p,0,102);
    p[0]=2;p[1]=1;p[3]=22;memcpy(p+4,net_tx_packet,20);
    p[27]=1;p[29]=20;memcpy(p+30,net_tx_packet,20);
    p[51]=2;p[53]=20;memcpy(p+54,net_tx_packet,20);
    p[74]=0x80;p[75]=1;p[77]=20;memcpy(p+78,net_tx_packet,20);
    event(5,rx->ring.a+((rx->wp+127)%128)*16,101,4,98);
    assert(md_poll()==0&&net_rx==2&&md_data_drop==2&&rx->rx_posted&&!memcmp(net_rx_packet,net_tx_packet,20));
    p[0]=0;p[1]=1;p[3]=30;
    event(5,rx->ring.a+((rx->wp+127)%128)*16,101,2,24);assert(md_poll()==0&&net_rx==2&&md_data_drop==3);
    /* Ring wrap, with one outstanding TRE, preserves the exact completion pointer. */
    tx->wp=127;net_tx_length=20;assert(md_poll()==0&&tx->wp==0&&tx->tx_pending);
    event(4,tx->ring.a+127*16,100,2,24);assert(md_poll()==0&&md_data_tx==tx_before+2);
    uint8_t indication[12]={4,0,0,0x22,0,5,0,1,2,0,1,0},packet[32];
    size_t n=from3(packet,1,52,0x4004,indication,sizeof(indication));
    assert(md_qrtr_packet(packet,n)==0&&!net_up);
    /* umi_modem=2: QRTR control and the bridge's ports go to the bridge; QMAP carries every mux and IPv6. */
    md_bridge=true;
    uint8_t bpkt[64];
    uint32_t del_server[5]={5,1,0x101,3,52};n=from3(bpkt,5,0xfffe,0xfffe,del_server,20);
    assert(md_qrtr_packet(bpkt,n)==0&&bb_dels==1);
    uint8_t sdu[7]={2,1,0,0x20,0,0,0};n=from3(bpkt,1,52,0x4105,sdu,7);
    assert(md_qrtr_packet(bpkt,n)==0&&bb_ports==1);
    n=from3(bpkt,1,52,0x4004,sdu,7);assert(md_qrtr_packet(bpkt,n)==0&&bb_ports==1); /* the kernel's own port */
    memset(p,0,30);p[1]=3;p[3]=24;p[4]=0x60;
    event(5,rx->ring.a+((rx->wp+127)%128)*16,101,2,28);
    assert(md_poll()==0&&bb_in==1&&bb_in_mux==3&&bb_in_version==6);
    tx->tx_pending=false;memset(bb_take_packet,0,24);bb_take_packet[0]=0x60;bb_take_length=24;bb_take_mux=2;
    assert(md_poll()==0&&tx->tx_pending&&tx->buffer.p[1]==2&&tx->buffer.p[3]==24&&tx->buffer.p[4]==0x60);
    md_bridge=false;
    puts("PASS data: WDA raw-IP/QMAP v1, PCIe/4 mux1 on persistent WDS client, cmnet IPv4/start/settings, format/start rejection, IP_HW0 ER4/5 burst OOB/DB_MODE/M0, TX completion/wrap, RX aggregate/padding/bounds/mux, disconnect; bridge DEL_SERVER/ports/any-mux IPv6 RX/muxed TX");
}
static void test_voice_mailbox(void)
{
    uint64_t iova;
    assert(umi_voice_mhi_map(0x90000000,&iova)==ENXIO);
    now+=100;md_voice_service();
    assert(umi_voice_mhi_map(0xffff0000,&iova)==EINVAL);
    assert(umi_voice_mhi_map(0x90000001,&iova)==EINVAL);
    uint32_t *ctx=md_channel_context(80);
    for(unsigned i=0;i<11;i++)assert(!ctx[i]);
    ctx[0]=1;assert(umi_voice_mhi_map(0x90000000,&iova)==EAGAIN);
    md_voice_service();assert(umi_voice_mhi_map(0x90000000,&iova)==EBUSY);
    ctx[0]=0;memset(&md_voice,0,sizeof(md_voice));md_voice_service();
    // Cross a 2 MiB table boundary; the 32 pages must map one allocation.
    next_address=MD_IOVA+0x1f0000;
    assert(umi_voice_mhi_map(0xfffe0000,&iova)==EAGAIN);
    assert(umi_voice_mhi_map(0xfffe0000,&iova)==EAGAIN);
    md_voice_service();assert(!umi_voice_mhi_map(0xfffe0000,&iova)&&iova==MD_IOVA+0x1f0000);
    for(unsigned page=0;page<32;page++) {
        uint64_t offset=iova-MD_IOVA+page*4096;
        assert(md_table(2+(unsigned)(offset>>21))[(offset>>12)&511]==((0xfffe0000ull+page*4096)|0xe43));
    }
    for(unsigned i=0;i<11;i++)assert(!ctx[i]); // no AP START/context/transfer doorbell
    assert(umi_voice_mhi_map(0x90000000,&iova)==EBUSY);
    now+=1000001;assert(umi_voice_mhi_map(0xfffe0000,&iova)==ENXIO);
    puts("PASS voice-audio MHI: service-thread mapping, below-4-GiB bounds, 128 KiB across L3 tables, offload context untouched, ownership and stale transport");
}

int main(void)
{
    setup();
    assert(!md_ringptr(0x1000,128,0xff0));assert(!md_ringptr(0x1000,128,0x1001));assert(!md_ringptr(0x1000,128,0x1800));
    struct md_channel *c=&md_channels[2];c->id=20;c->er=0;c->wp=1;c->ring=md_alloc(MD_TRES*16);c->buffer=md_alloc(MD_PACKET);c->tx_pending=true;
    event(0,c->ring.a,20,2,8);assert(md_poll()==0);assert(c->tx_done&&!c->tx_pending);
    c->tx_pending=true;event(0,c->ring.a+16,20,2,8);assert(md_poll()==EPROTO); /* wrong TRE */
    event(0,c->ring.a,20,3,8);assert(md_poll()==EPROTO); /* bad completion */
    event(0,c->ring.a,99,2,8);assert(md_poll()==EPROTO); /* unopened channel */
    event(0,c->ring.a,20,2,8);assert(md_poll()==0);
    md_set64((uint32_t *)md_erctx.p,7,md_events[0].mem.a-16);assert(md_poll()==EPROTO);
    now=md_deadline+1;assert(md_poll()==ETIMEDOUT);now=0;
    assert(md_image_map()==0&&md_image_count==2);
    /* Board-local Sahara IDs are backed by read-only GPT partitions, never
       by similarly named FAT distribution files. */
    assert(!strcmp(md_ufs_partition_for_id(16),"mdm1m9kefs1"));
    assert(!strcmp(md_ufs_partition_for_id(17),"mdm1m9kefs2"));
    assert(!strcmp(md_ufs_partition_for_id(20),"mdm1m9kefs3"));
    assert(!strcmp(md_ufs_partition_for_id(34),"mdmddr"));
    assert(md_ufs_partition_for_id(21)==NULL);
    file_data="21 a.mbn\n21 b.mbn\n";md_image_count=0;assert(md_image_map()==EEXIST);
    file_data="0 a.mbn\n";md_image_count=0;assert(md_image_map()==EINVAL);
    file_data="65536 a.mbn\n";md_image_count=0;assert(md_image_map()==EINVAL);
    file_data="21 a.mbn\n";md_image_count=0;assert(md_image_map()==0);
    char out[64];size_t n=4;assert(md_file("../modem",0,out,&n,false)==EINVAL);
    uint8_t local_efs[11]="local-efs",local_ddr[10]="local-ddr";
    ufs_fixtures[0]=(struct ufs_fixture){"mdm1m9kefs1",local_efs,sizeof(local_efs)};
    ufs_fixtures[1]=(struct ufs_fixture){"mdmddr",local_ddr,sizeof(local_ddr)};
    ufs_fixture_count=2;
    n=sizeof(local_ddr)-2;assert(md_ufs_file("mdmddr",2,out,&n,false)==0&&n==sizeof(local_ddr)-2&&
        !memcmp(out,"cal-ddr",7));
    n=sizeof(out);assert(md_ufs_file("mdmddr",sizeof(local_ddr),out,&n,false)==ERANGE);
    n=sizeof(out);assert(md_ufs_file("modemst1",0,out,&n,false)==EROFS);
    assert(md_file("sbl1.mbn",UINT64_MAX,out,&n,false)==ERANGE);
    assert(md_sahara_read(999,0,1)==ENOENT);assert(md_sahara_read(21,0,0)==ERANGE);
    assert(md_sahara_read(21,512ull*1024*1024,1)==ERANGE);
    /* An invalid tail must be rejected before sending even the first chunk. */
    unsigned reads=file_reads;file_size=40000;
    assert(md_sahara_read(21,0,40001)==ERANGE&&file_reads==reads);
    file_size=84004996;n=(size_t)file_size;
    assert(md_file("qdsp6sw.mbn",0,NULL,&n,false)==0&&file_reads==reads);
    file_size=0;
    uint32_t hello[12]={1,48,2,1,256,0},response[12];
    assert(md_sahara_hello((uint8_t *)hello,48,response)==0&&response[5]==0);
    hello[5]=1;assert(md_sahara_hello((uint8_t *)hello,48,response)==0&&response[5]==1);
    hello[5]=2;assert(md_sahara_hello((uint8_t *)hello,48,response)==ENOTSUP);
    hello[5]=1;hello[3]=3;assert(md_sahara_hello((uint8_t *)hello,48,response)==ENOTSUP);hello[3]=1;
    /* Header split after byte three; the next completion also contains a
       second whole command. Test fragment retention and coalesced messages. */
    md_set64((uint32_t *)md_erctx.p,7,md_events[0].mem.a+md_events[0].consumer*16);
    registers[(md_bhi+0x28)/4]=1;
    uint8_t wire[60];memcpy(wire,hello,48);uint32_t done[3]={6,12,1};memcpy(wire+48,done,12);
    memcpy(md_sahara_stream,wire,3);md_sahara_size=3;md_sahara_pos=0;
    struct md_channel *rx=&md_channels[1];rx->id=3;rx->er=1;rx->ring=md_alloc(MD_TRES*16);rx->buffer=md_alloc(MD_PACKET);
    memcpy(rx->buffer.p,wire+3,57);rx->rx_length=57;rx->rx_ready=true;
    uint8_t packet[256];n=sizeof(packet);assert(md_sahara_message(packet,&n)==0&&n==48&&!memcmp(packet,hello,48));
    n=sizeof(packet);assert(md_sahara_message(packet,&n)==0&&n==12&&!memcmp(packet,done,12));
    registers[(md_bhi+0x28)/4]=2;n=sizeof(packet);assert(md_sahara_message(packet,&n)==EAGAIN);
    uint32_t bad_header[2]={1,257};memcpy(md_sahara_stream,bad_header,8);md_sahara_pos=0;md_sahara_size=8;
    n=sizeof(packet);assert(md_sahara_message(packet,&n)==EPROTO);
    assert(md_stage!=NULL);
    /* QRTR v1 (32-byte) and v2 (version byte 3, 16-byte) NEW_SERVER. */
    uint32_t v1[13]={1,4,3,0xfffffffe,0,20,1,0xfffffffe,4,3,1,3,60};
    md_services=0;assert(md_qrtr_packet((uint8_t *)v1,52)==0&&md_services==1&&md_nas_port==60);
    uint8_t v2[36]={3,4,0,0,20,0,0,0,3,0,0xfe,0xff,1,0,0xfe,0xff};
    uint32_t srv[5]={4,1,0,3,61};memcpy(v2+16,srv,20);
    assert(md_qrtr_packet(v2,36)==0&&md_services==2);
    srv[1]=11;srv[4]=64;memcpy(v2+16,srv,20);assert(md_qrtr_packet(v2,36)==0&&md_uim_node==3&&md_uim_port==64);
    /* QMI response DATA from node 3 port 64 to the host client port 0x4000. */
    uint8_t data[16+14]={3,1,0,0,14,0,0,0,3,0,64,0,1,0,0x00,0x40,2,1,0,0x2f,0,7,0,2,4,0,0,0,0,0};
    md_qmi_reply_size=0;assert(md_qrtr_packet(data,30)==0&&md_qmi_reply_size==14&&md_qmi_reply_node==3&&md_qmi_reply_port==64);
    uint16_t qres=9,qerr=9;assert(umi3_result(md_qmi_reply,md_qmi_reply_size,&qres,&qerr)==0&&!qres&&!qerr);
    data[14]=0x01;md_qmi_reply_size=0;assert(md_qrtr_packet(data,30)==0&&md_qmi_reply_size==0); /* other port: not a reply */
    /* Every HELLO from the SDX55 router is answered as Linux ctrl_cmd_hello:
       HELLO back to its control port, then NEW_SERVER for the servreg locator. */
    uint8_t hello_v2[36]={3,2,0,0,20,0,0,0,2,0,0xfe,0xff,1,0,0xfe,0xff,2};
    delay_hook=complete_ul;
    for (unsigned round=1;round<=2;round++) {
        ul_count=0;md_qrtr_hello=false;
        assert(md_qrtr_packet(hello_v2,36)==0&&md_qrtr_hello&&ul_count==2);
        uint32_t reply[13]={1,2,1,0xfffffffe,0,20,2,0xfffffffe,2,0,0,0,0};
        uint32_t locator[13]={1,4,1,0xfffffffe,0,20,2,0xfffffffe,4,64,0x101,1,0x4001};
        assert(!memcmp(ul_packets[0],reply,sizeof(reply))&&!memcmp(ul_packets[1],locator,sizeof(locator)));
    }
    delay_hook=NULL;
    v2[4]=21;assert(md_qrtr_packet(v2,36)==EPROTO);v2[0]=2;assert(md_qrtr_packet(v2,36)==EPROTO);
    fat16_fixture();
    n=7;assert(md_fat_file("long_image.mbn",0,packet,&n,false)==0&&!memcmp(packet,"fixture",7));
    n=8;assert(md_fat_file("long_image.mbn",0,packet,&n,false)==ERANGE);
    free(fat_data);fat_data=NULL;fat_bytes=0;fat_partition_bytes=0;memset(&md_fat,0,sizeof(md_fat));md_fat_loaded_sector=UINT32_MAX;
    fat4096_nested_fixture();
    n=4096;assert(md_fat_file("sbl1.mbn",0,packet,&n,true)==0&&n==7&&!memcmp(packet,"fixture",7));
    n=7;assert(md_fat_file("IMAGE/SDX55M/sbl1.mbn",0,packet,&n,false)==0&&!memcmp(packet,"fixture",7));
    /* TFTP: announced once node-3 EFS (service 21) appears; a v3 RRQ with
       options gets the OACK, ACK 0 gets the rsize-limited DATA from seek. */
    uint8_t pkt[256];size_t pn;const uint8_t *pl;
    delay_hook=complete_ul;
    uint32_t efs[5]={4,21,1,3,10};ul_count=0;pn=from3(pkt,4,0xfffe,0xfffe,efs,20);
    assert(md_qrtr_packet(pkt,pn)==0&&md_tftp_published&&ul_count==2);
    uint32_t tv1[5]={4,4096,1,1,0x4002},tv3[5]={4,4096,3,1,0x4003};
    /* Addressed to the router node from HELLO (2), as Linux; never 0xffffffff. */
    assert(!memcmp(ul_payload(0,4,0xfffffffe,2,0xfffffffe,20),tv1,20)&&!memcmp(ul_payload(1,4,0xfffffffe,2,0xfffffffe,20),tv3,20));
    ul_count=0;assert(md_qrtr_packet(pkt,pn)==0&&ul_count==0); /* once */
    /* A later HELLO re-announces every host service (Linux announce_servers). */
    ul_count=0;assert(md_qrtr_packet(hello_v2,36)==0&&ul_count==4);
    assert(!memcmp(ul_payload(2,4,0xfffffffe,2,0xfffffffe,20),tv1,20)&&!memcmp(ul_payload(3,4,0xfffffffe,2,0xfffffffe,20),tv3,20));
    static const uint8_t rrq[]="\0\1/readonly/firmware/image/sdx55m/sbl1.mbn\0octet\0blksize\0" "512\0tsize\0" "0\0rsize\0" "5\0seek\0" "2";
    ul_count=0;pn=from3(pkt,1,0x321,0x4003,rrq,sizeof(rrq));assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&md_tftp[0].used);
    static const uint8_t oack[]="\0\6blksize\0" "512\0timeoutms\0" "1000\0tsize\0" "7\0wsize\0" "1\0rsize\0" "5\0seek\0" "2";
    assert(!memcmp(ul_payload(0,1,0x4010,3,0x321,sizeof(oack)),oack,sizeof(oack)));
    uint8_t ack[4]={0,4,0,0};ul_count=0;pn=from3(pkt,1,0x321,0x4010,ack,4);assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1);
    pl=ul_payload(0,1,0x4010,3,0x321,9);assert(pl[0]==0&&pl[1]==3&&pl[2]==0&&pl[3]==1&&!memcmp(pl+4,"xture",5));
    ack[3]=1;ul_count=0;pn=from3(pkt,1,0x321,0x4010,ack,4);assert(md_qrtr_packet(pkt,pn)==0&&ul_count==0&&!md_tftp[0].used);
    ul_count=0;assert(md_qrtr_packet(pkt,pn)==0&&ul_count==0); /* unsolicited: dropped */
    /* Missing file and anything outside /readonly/firmware/image/: TFTP error 1. */
    static const uint8_t missing[]="\0\1/readonly/firmware/image/nope.mbn\0octet",rw[]="\0\1/readwrite/ota_firewall/ruleset\0octet";
    ul_count=0;pn=from3(pkt,1,0x322,0x4003,missing,sizeof(missing));assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&!md_tftp[0].used);
    pl=ul_payload(0,1,0x4010,3,0x322,19);assert(pl[1]==5&&pl[3]==1&&!strcmp((const char *)pl+4,"file not found"));
    ul_count=0;pn=from3(pkt,1,0x323,0x4002,rw,sizeof(rw));assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&!md_tftp[0].used);
    /* No options: DATA 1 at once; DEL_CLIENT of that modem port ends it. */
    static const uint8_t plain[]="\0\1/readonly/firmware/image/sdx55m/sbl1.mbn\0octet";
    ul_count=0;pn=from3(pkt,1,0x324,0x4003,plain,sizeof(plain));assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&md_tftp[0].used);
    pl=ul_payload(0,1,0x4010,3,0x324,11);assert(pl[1]==3&&pl[3]==1&&!memcmp(pl+4,"fixture",7));
    uint32_t del[5]={6,3,0x324,0,0};ul_count=0;pn=from3(pkt,6,0xfffe,0xfffe,del,20);assert(md_qrtr_packet(pkt,pn)==0&&ul_count==0&&!md_tftp[0].used);
    /* WRQ: acknowledged per block and discarded; a short block ends it. */
    static const uint8_t wrq[]="\0\2/readwrite/server_check.txt\0octet\0blksize\0" "8";
    ul_count=0;pn=from3(pkt,1,0x325,0x4003,wrq,sizeof(wrq));assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&md_tftp[0].writer);
    static const uint8_t woack[]="\0\6blksize\0" "8\0timeoutms\0" "1000\0wsize\0" "1";
    assert(!memcmp(ul_payload(0,1,0x4010,3,0x325,sizeof(woack)),woack,sizeof(woack)));
    uint8_t wdata[12]={0,3,0,1,'1','2','3','4','5','6','7','8'};
    ul_count=0;pn=from3(pkt,1,0x325,0x4010,wdata,12);assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&md_tftp[0].used);
    pl=ul_payload(0,1,0x4010,3,0x325,4);assert(pl[1]==4&&pl[3]==1);
    wdata[3]=2;ul_count=0;pn=from3(pkt,1,0x325,0x4010,wdata,7);assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1&&!md_tftp[0].used);
    assert(md_tftp_reads==2&&md_tftp_writes==1&&md_tftp_missing==2&&md_tftp_bytes==12);
    /* Route B probe (INVESTIGATION-routeB-x55-noreg.md §4 P1): umi_bb_announce records IMSDCM
       770 v1 instance 0 and, with no HELLO yet, sends nothing; the next router HELLO publishes it
       as {4, 770, 1, 1, 0x4205} (service, version|instance<<8, host node 1, port) to the router
       node, and a NEW_LOOKUP for it is answered to the inquirer's node/port. */
    md_bridge=true;
    md_qrtr_peer=0;   /* before the router's first HELLO the request is only recorded */
    ul_count=0;
    assert(umi_bb_announce(770,1,0,0x4205)==0&&ul_count==0&&md_host_known);
    uint8_t hello3[36];memcpy(hello3,hello_v2,36);hello3[8]=3;   /* the router re-HELLOs from node 3 */
    uint32_t dcm[5]={4,770,1,1,0x4205};
    ul_count=0;assert(md_qrtr_packet(hello3,36)==0&&ul_count==5);   /* HELLO, locator, TFTP v1/v3, DCM */
    assert(!memcmp(ul_payload(4,4,0xfffffffe,3,0xfffffffe,20),dcm,20));
    ul_count=0;uint32_t lookup[5]={10,770,1,0,0};pn=from3(pkt,10,0x330,0xfffffffe,lookup,20);
    assert(md_qrtr_packet(pkt,pn)==0&&ul_count==1);
    assert(!memcmp(ul_payload(0,4,0xfffffffe,3,0x330,20),dcm,20));
    ul_count=0;lookup[1]=40;pn=from3(pkt,10,0x331,0xfffffffe,lookup,20);
    assert(md_qrtr_packet(pkt,pn)==0&&ul_count==0);   /* IMSRTP 40 is not hosted yet: log only */
    md_bridge=false;
    /* EFS sync: HELLO(mode 2) -> HELLO_RESP; MEMORY_DEBUG -> table read, region
       read in 32 KiB pieces, RESET; RESET_RESP acknowledged. Nothing else sent. */
    for (unsigned i=4;i<6;i++) { struct md_channel *e=&md_channels[i];e->id=6+i;e->er=1;e->ring=md_alloc(MD_TRES*16);e->buffer=md_alloc(MD_PACKET); }
    assert(md_post(&md_channels[5],MD_PACKET)==0);
    delay_hook=efs_device;
    uint32_t efs_hello[12]={1,48,2,1,1024,2},efs_debug[4]={9,16,0x1000,52};
    efs_put(efs_hello,48);assert(md_efs_message()==0&&efs_ncmds==1&&efs_cmds[0][0]==2&&efs_cmds[0][2]==2&&efs_cmds[0][3]==1&&efs_cmds[0][5]==2);
    efs_put(efs_debug,16);assert(md_efs_message()==0&&efs_ncmds==5);
    assert(efs_cmds[1][0]==10&&efs_cmds[1][2]==0x1000&&efs_cmds[1][3]==52);
    assert(efs_cmds[2][2]==0x100000&&efs_cmds[2][3]==32768&&efs_cmds[3][2]==0x108000&&efs_cmds[3][3]==7232&&efs_cmds[4][0]==7);
    assert(md_efs_regions==1&&md_efs_bytes==40000&&md_efs_message()==0&&efs_ncmds==5&&efs_qp==efs_qn);
    uint32_t efs_bad[2]={3,8};efs_put(efs_bad,8);assert(md_efs_message()==EPROTO);
    /* Idle service: 1 ms wakes for 2 s after traffic, then 100 ms; stops on quiesce. */
    md_nas_port=0;service_start=now;delay_hook=service_hook;
    assert(md_service()==0&&md_deadline==UINT64_MAX&&service_wakes>=2000&&service_wakes<2100);
    delay_hook=NULL;
    test_data();
    test_voice_mailbox();
    /* v30 fault autopsy (md_fault): the v29t/u/v signature (MHI status all ones) is one fault event,
       reported through the AP-side line; PARF reads LTSSM 0 (not L0) here, so the endpoint's config
       window is never touched. */
    md_write(MD_BAR+0x48,UINT32_MAX);
    assert(md_poll()==EIO);
    md_write(MD_BAR+0x48,0);
    /* Restart reset (umi_modem.c: md_link_reset before a new bring-up): every
       per-link static goes back to zero and the Sahara image holds are
       released, so a restart inherits no ring, port, stream position or vnode
       from the link that faulted. */
    md_fault_reason="MHI/SMMU fault";
    md_services=7;md_qrtr_peer=6;md_qrtr_hello=true;md_registered=true;
    md_nas_node=3;md_nas_port=3;md_uim_port=4;md_dms_port=5;md_wda_port=7;md_wds_port=8;
    md_sahara_pos=9;md_sahara_size=10;md_efs_pos=11;md_efs_size=12;
    md_qmi_reply_size=13;md_qmi_reply_client=0x4000;
    md_tftp_published=true;md_tftp[0].used=true;md_tftp[0].blksize=512;
    md_channels[0].ring=(struct md_buf){(uint8_t *)1,0,4096};md_channels[0].id=2;md_channels[0].tx_pending=true;
    md_events[0].count=64;md_hw_events_ready=true;md_data_open=true;md_active_at=99;
    md_bhi=0x100;md_bar_size=0x100000;
    md_image_count=1;md_images[0].vnode=(void *)1;md_images[0].source=MD_SOURCE_PAYLOAD;
    md_link_reset();
    assert(!md_services&&md_qrtr_peer==0&&!md_qrtr_hello&&!md_registered);
    assert(!md_nas_node&&!md_nas_port&&!md_uim_port&&!md_dms_port&&!md_wda_port&&!md_wds_port);
    assert(!md_sahara_pos&&!md_sahara_size&&!md_efs_pos&&!md_efs_size&&!md_qmi_reply_size&&!md_qmi_reply_client);
    assert(!md_tftp_published&&!md_tftp[0].used&&!md_tftp[0].blksize);
    assert(!md_channels[0].ring.p&&!md_channels[0].id&&!md_channels[0].tx_pending&&!md_events[0].count);
    assert(!md_hw_events_ready&&!md_data_open&&!md_active_at&&!md_bhi&&!md_bar_size);
    assert(!md_images[0].vnode&&md_images[0].source==MD_SOURCE_NONE&&md_image_count==1);
    free(fat_data);fat_data=NULL;fat_bytes=0;fat_partition_bytes=0;
    puts("PASS MHI model: ring bounds, completion identity/status, unopened channel, deadline, image-map validation, whole-request bounds, 84MB images, HELLO 0/1, QRTR HELLO reply + locator per HELLO, TFTP announce/RRQ/OACK/DATA/missing/WRQ/DEL_CLIENT, fragmented/coalesced headers, mission transition, QRTR UIM service and QMI reply capture, EFS sync RAM-only HELLO/table/regions/RESET, idle service backoff and stop");
}
