/* BHI -> SBL -> Sahara image transfer -> AMSS -> IPCR. No BHIE combined
 * Wi-Fi image assumption, FIREHOSE channel, EDL entry, or persistent writes.
 * All protocol lengths, pointers, image ranges and waits are bounded. */
#define MD_NER 6u
#define MD_TRES 128u
#define MD_PACKET 32768u
struct md_ring { struct md_buf mem; unsigned count,consumer,wp; };
struct md_channel {
	unsigned id,er,wp;
	struct md_buf ring,buffer;
	bool tx_pending,tx_done,rx_posted,rx_ready,db_mode;
	size_t rx_length;
};
static struct md_buf md_chctx,md_erctx,md_cmdctx,md_cmdring;
static struct md_ring md_events[MD_NER];
static struct md_channel md_channels[8];
static int md_data_poll(void);
/* umi_modem=2: CommCenter drives the modem through the bridge (umi_baseband.h, COMMCENTER-umi.md);
 * the kernel keeps the transport (boot, QRTR, TFTP, EFS, IP_HW0/QMAP) and asks the modem nothing itself. */
static bool md_bridge;
static uint64_t md_active_at;     /* last bridge message or data packet, for the service thread's poll rate */
/* The fault that stopped the previous link, for umi_modem.c's one restart line. Declared here so this file also compiles
 * on its own in the host protocol tests (tools/modem-tests/mhi.c). */
static const char *md_fault_reason;
#include "umi_baseband.h"
static uint32_t md_chdb,md_erdb;
static bool md_hw_events_ready;
static void md_channel_doorbell(struct md_channel *c)
{
	if (c->id>=100 && !c->db_mode) return;
	md_address(MD_BAR+md_chdb+c->id*8,c->ring.a+c->wp*16);
	if (c->id>=100) c->db_mode=false;
}
static unsigned md_cmdwp;
static uint64_t md_command_wait;
static int md_command_cc;
static unsigned md_last_event_channel,md_last_event_code,md_last_event_bytes;
static unsigned md_ee_event;
static bool md_qrtr_hello;
static uint8_t md_sahara_packet[MD_PACKET];
/* Sahara is a byte stream; MHI completions need not align with headers. */
static uint8_t md_sahara_stream[MD_PACKET];
static size_t md_sahara_pos,md_sahara_size;
static const unsigned md_event_counts[MD_NER]={64,128,64,64,1024,2048};
static uint32_t md_u32(const void *ptr)
{
	const uint8_t *p=ptr;
	return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t md_u64(const void *ptr)
{
	return md_u32(ptr)|(uint64_t)md_u32((const uint8_t *)ptr+4)<<32;
}
static uint64_t md_mmio64(uint32_t pa)
{
	return md_read(pa)|(uint64_t)md_read(pa+4)<<32;
}
static void md_set64(uint32_t *words,unsigned n,uint64_t v)
{
	words[n]=(uint32_t)v;words[n+1]=(uint32_t)(v>>32);
}
static uint32_t *md_channel_context(unsigned id) { return (uint32_t *)md_chctx.p+id*11; }
static bool md_ringptr(uint64_t base,unsigned count,uint64_t pointer)
{
	return pointer>=base && pointer-base<(uint64_t)count*16 && !((pointer-base)&15);
}
/* Fault autopsy, AP side only (v30 diagnostics, v29t/u/v): MDM_ERRFATAL (TLMM 1) asserted means
 * the X55 announced its own fatal error; MDM_STATUS (TLMM 3) says whether it still claims to be
 * alive. Both are the mdm0 esoc signals ("qcom,ext-sdx55m", kona.dtsi:2426-2429) and md_pcie() has
 * already left the pins as inputs. TLMM is on the always-on SoC interconnect, and PARF is the RC
 * wrapper this driver and the loader already sample with the link down (loader.c:1837, "LTSSM 0x11
 * = L0"; md_link_snapshot runs before PERST), so neither access can wait on the endpoint. The
 * function's own config window is read once and only when LTSSM still says L0: touching config,
 * DBI or a BAR on a dead link is what hung the phone at v29v 16:06:48. One line per fault event. */
#ifndef MD_GPIO
#define MD_GPIO 0x0f500000u
#endif
#ifndef MD_PARF
#define MD_PARF 0x01c10000u
#endif
#ifndef MD_CONF
#define MD_CONF 0x64100000u
#endif
static void md_fault_diag(uint32_t state)
{
	uint32_t errfatal=md_read(MD_GPIO+0x1000+4)&1;   /* TLMM 1, MDM2AP_ERRFATAL */
	uint32_t mdm_status=md_read(MD_GPIO+0x3000+4)&1; /* TLMM 3, MDM2AP_STATUS */
	uint32_t ltssm=md_read(MD_PARF+0x1b0)&0x3f,ctl=md_read(MD_PARF);
	if (ltssm!=0x11)
		MD_LOG("fault diag MDM_ERRFATAL=%u MDM_STATUS=%u LTSSM=%#x PARF=%#x: link not L0, config not read\n",
		    errfatal,mdm_status,ltssm,ctl);
	else
		MD_LOG("fault diag MDM_ERRFATAL=%u MDM_STATUS=%u LTSSM=L0 PARF=%#x config_ID=%#x MHI_STATUS=%#x (0xffffffff=UR)\n",
		    errfatal,mdm_status,ctl,md_read(MD_CONF),state);
}
static int md_fault(void)
{
	if (md_now_us()>md_deadline) return ETIMEDOUT;
	uint32_t state=md_read(MD_BAR+0x48),ee=md_read(MD_BAR+md_bhi+0x28);
	uint32_t fsr=md_read(md_cb+0x58);
	if (state==UINT32_MAX || (state&4) || ee==3 || ee==6 || ee>7 || (fsr&0xc00001fe)) {
		md_fault_reason="MHI/SMMU fault";
		MD_LOG("MHI/SMMU fault status=%#x EE=%u FSR=%#x\n",state,ee,fsr);
		MD_LOG("MHI/SMMU fault detail CB=%u FAR=%#llx FSYNR0=%#x SGFSR=%#x SGFSYNR0=%#x SGFSYNR1=%#x\n",
		    md_cb_index,(unsigned long long)md_mmio64(md_cb+0x60),md_read(md_cb+0x68),
		    md_read(MD_SMMU+0x48),md_read(MD_SMMU+0x50),md_read(MD_SMMU+0x54));
		md_fault_diag(state);
		return EIO;
	}
	/* M1 may be offered by the modem; keep this bounded diagnostic in M0. */
	if (((state>>8)&255)==3) MD_TRY(md_update(MD_BAR+0x38,0xff00,0x200));
	return 0;
}
static int md_poll(void)
{
	MD_TRY(md_fault());
	for (unsigned e=0;e<MD_NER;e++) {
		struct md_ring *r=&md_events[e];
		uint32_t *ctx=(uint32_t *)md_erctx.p+e*11;
		__asm__ volatile("dmb oshld" ::: "memory");
		uint64_t producer=md_u64(ctx+7);
		if (!md_ringptr(r->mem.a,r->count,producer)) return EPROTO;
		unsigned end=(unsigned)((producer-r->mem.a)/16),processed=0;
		while (r->consumer!=end && processed++<r->count) {
			uint32_t *event=(uint32_t *)r->mem.p+4*r->consumer;
			__asm__ volatile("dmb oshld" ::: "memory");
			uint64_t pointer=md_u64(event);
			unsigned type=(event[3]>>16)&255,ch=event[3]>>24,cc=event[2]>>24;
			md_last_event_channel=ch;md_last_event_code=cc;md_last_event_bytes=event[2]&65535;
			if (type==0x40 && e==0) md_ee_event=cc;
			/* Linux IP_HW0 doorbell_mode_switch: M0 restores burst doorbells
			 * and re-publishes any transfer queued before the transition. */
			if (type==0x20 && cc==2 && e==0) {
				for (unsigned n=6;n<8;n++) {
					struct md_channel *c=&md_channels[n];
					c->db_mode=true;
					if (c->ring.p && (c->tx_pending || c->rx_posted)) md_channel_doorbell(c);
				}
			}
			if (type==0x21 && pointer==md_command_wait && e==0) md_command_cc=(int)cc;
			if (type==0x22) {
				struct md_channel *c=NULL;
				for (unsigned n=0;n<8;n++) if (md_channels[n].ring.p && md_channels[n].id==ch) c=&md_channels[n];
				/* Hardware channels request a new doorbell with OOB/DB_MODE;
				 * these events carry no completed TRE. Linux uses burst mode. */
				if (c && c->er==e && ch>=100 && (cc==5 || cc==6)) {
					c->db_mode=true;
					if (c->tx_pending || c->rx_posted) md_channel_doorbell(c);
					goto next_event;
				}
				if (!c || c->er!=e || !md_ringptr(c->ring.a,MD_TRES,pointer) ||
				    (cc!=2 && !(ch>=100 && cc==4))) {
					MD_LOG("MHI bad transfer event ER=%u ch=%u code=%u bytes=%u\n",e,ch,cc,event[2]&65535);
					return EPROTO;
				}
				unsigned last=(c->wp+MD_TRES-1)%MD_TRES;
				if (pointer!=c->ring.a+last*16) return EPROTO;
				if (!(ch&1)) {
					if (!c->tx_pending) return EPROTO;
					c->tx_pending=false;c->tx_done=true;
				} else {
					if (!c->rx_posted || c->rx_ready || (event[2]&65535)>MD_PACKET) return EPROTO;
					c->rx_length=event[2]&65535;c->rx_ready=true;c->rx_posted=false;
				}
			}
next_event:
			r->consumer=(r->consumer+1)%r->count;
			r->wp=(r->wp+1)%r->count;
		}
		if (processed) {
			md_set64(ctx,9,r->mem.a+r->wp*16);md_publish();
			md_address(MD_BAR+md_erdb+e*8,r->mem.a+r->wp*16);
		}
	}
	return md_data_poll();
}
static int md_post(struct md_channel *,size_t);
static int md_start_channel(unsigned slot,unsigned id,unsigned er)
{
	struct md_channel *c=&md_channels[slot];
	c->id=id;c->er=er;c->db_mode=true;
	c->ring=md_alloc_aligned(MD_TRES*16,MD_TRES*16);c->buffer=md_alloc(MD_PACKET);
	if (!c->ring.p || !c->buffer.p) return ENOMEM;
	uint32_t *ctx=md_channel_context(id);
	if (id>=100) MD_LOG("IP_HW0 ctx write begin id=%u ER=%u ctx=%llx erctx=%llx\n",id,er,
	    (unsigned long long)(md_chctx.a+(uint64_t)id*44),
	    (unsigned long long)(md_erctx.a+(uint64_t)er*44));
	ctx[0]=id>=100?0x301:0x201;ctx[1]=(id&1)?2:1;ctx[2]=er;
	md_set64(ctx,3,c->ring.a);md_set64(ctx,5,MD_TRES*16);
	md_set64(ctx,7,c->ring.a);md_set64(ctx,9,c->ring.a);
	if (id>=100)
		MD_LOG("IP_HW0 channel setup begin id=%u ER=%u ctx=%llx erctx=%llx er_ring=%llx/%u ring=%llx/%u buffer=%llx/%u chcfg=%#x pollcfg=%u\n",
		    id,er,(unsigned long long)(md_chctx.a+(uint64_t)id*44),
		    (unsigned long long)(md_erctx.a+(uint64_t)er*44),
		    (unsigned long long)md_events[er].mem.a,md_events[er].count*16,
		    (unsigned long long)c->ring.a,(unsigned)(MD_TRES*16),
		    (unsigned long long)c->buffer.a,(unsigned)c->buffer.size,ctx[0],ctx[0]>>10&63);
	uint32_t *cmd=(uint32_t *)md_cmdring.p+4*md_cmdwp;
	cmd[0]=cmd[1]=cmd[2]=0;cmd[3]=(id<<24)|(18u<<16);
	md_command_wait=md_cmdring.a+md_cmdwp*16;md_command_cc=-1;
	if (id>=100) MD_LOG("IP_HW0 START command prepare id=%u cmd=%llx cmd_wp=%llx\n",id,
	    (unsigned long long)md_command_wait,
	    (unsigned long long)(md_cmdring.a+(uint64_t)(md_cmdwp+1)%MD_TRES*16));
	md_cmdwp=(md_cmdwp+1)%MD_TRES;
	md_set64((uint32_t *)md_cmdctx.p,9,md_cmdring.a+md_cmdwp*16);
	md_publish();
	if (id>=100) MD_LOG("IP_HW0 START command doorbell id=%u addr=%llx\n",id,
	    (unsigned long long)(md_cmdring.a+(uint64_t)md_cmdwp*16));
	md_address(MD_BAR+0x70,md_cmdring.a+md_cmdwp*16);
	for (unsigned ms=0;ms<5000;ms++) {
		MD_TRY(md_poll());
		if (md_command_cc>=0) {
			if (md_command_cc!=1) return EIO;
			if (id>=100) MD_LOG("IP_HW0 channel setup complete id=%u ER=%u db_mode=%u\n",id,er,c->db_mode);
			else MD_LOG("channel open id=%u ER=%u\n",id,er);
			/* As Linux mhi_prepare_channel: queue the DL buffer after START_CHAN. */
			return (id&1)?md_post(c,MD_PACKET):0;
		}
		delay(1000);
	}
	return ETIMEDOUT;
}
/* Linux does not arm hardware event rings during READY->M0.  It arms them
 * only after the AMSS mission-mode transition, immediately before clients
 * open the hardware channels. */
static int md_enable_hw_events(void)
{
	if (md_hw_events_ready) return 0;
	for (unsigned e=4;e<MD_NER;e++) {
		struct md_ring *r=&md_events[e];
		uint32_t *ctx=(uint32_t *)md_erctx.p+e*11;
		if (!r->mem.p || !r->count || md_u64(ctx+3)!=r->mem.a ||
		    md_u64(ctx+5)!=(uint64_t)r->count*16 || md_u64(ctx+7)!=r->mem.a)
			return EPROTO;
		r->wp=r->count-1;
		md_set64(ctx,9,r->mem.a+r->wp*16);
		md_publish();
		md_address(MD_BAR+md_erdb+e*8,r->mem.a+r->wp*16);
	}
	md_hw_events_ready=true;
	MD_LOG("MHI hardware event rings ready ER4=%u/%u ER5=%u/%u\n",
	    md_events[4].count,(unsigned)md_events[4].mem.size,
	    md_events[5].count,(unsigned)md_events[5].mem.size);
	return 0;
}
static int md_post(struct md_channel *c,size_t length)
{
	if (!length || length>MD_PACKET || c->tx_pending || c->rx_posted || c->rx_ready) return EBUSY;
	uint32_t *tre=(uint32_t *)c->ring.p+c->wp*4;
	md_set64(tre,0,c->buffer.a);tre[2]=(uint32_t)length;tre[3]=(2u<<16)|MD_BIT(9);
	c->wp=(c->wp+1)%MD_TRES;
	if (c->id&1) c->rx_posted=true; else { c->tx_pending=true;c->tx_done=false; }
	md_set64(md_channel_context(c->id),9,c->ring.a+c->wp*16);
	md_publish();
	if (c->id>=100) MD_LOG("IP_HW0 channel doorbell id=%u tre=%llx wp=%llx mode=%u\n",c->id,
	    (unsigned long long)(c->ring.a+(uint64_t)((c->wp+MD_TRES-1)%MD_TRES)*16),
	    (unsigned long long)(c->ring.a+(uint64_t)c->wp*16),c->db_mode);
	md_channel_doorbell(c);
	return 0;
}
static int md_send(struct md_channel *c,const void *data,size_t length)
{
	if (length>MD_PACKET || c->tx_pending) return EINVAL;
	memcpy(c->buffer.p,data,length);
	MD_TRY(md_post(c,length));
	for (unsigned ms=0;ms<5000;ms++) {
		MD_TRY(md_poll());
		if (c->tx_done) return 0;
		delay(1000);
	}
	uint32_t *ctx=md_channel_context(c->id);
	MD_LOG("MHI TX stall channel=%u ER=%u ring_consumer=%u ring_wp=%u rp=%llu ctx_rp=%llu last_event_ch=%u code=%u bytes=%u\n",
	    c->id,c->er,md_events[c->er].consumer,c->wp,c->ring.a+((uint64_t)c->wp*16),
	    md_u64(ctx+7),md_last_event_channel,md_last_event_code,md_last_event_bytes);
	return ETIMEDOUT;
}
static int md_receive(struct md_channel *c,void *data,size_t *length,unsigned timeout_ms)
{
	if (!c->rx_ready && !c->rx_posted) MD_TRY(md_post(c,MD_PACKET));
	for (unsigned ms=0;ms<timeout_ms;ms++) {
		MD_TRY(md_poll());
		if (c->rx_ready) {
			if (c->rx_length>*length) return EMSGSIZE;
			*length=c->rx_length;memcpy(data,c->buffer.p,*length);
			c->rx_ready=false;
			MD_TRY(md_post(c,MD_PACKET));
			return 0;
		}
		delay(1000);
	}
	return ETIMEDOUT;
}
static int md_sahara_exact(void *data,size_t length)
{
	uint8_t *p=data;
	while (length) {
		if (md_sahara_pos==md_sahara_size) {
			/* Removal of SBL is not proof of success. The caller must
			 * verify live AMSS and open mission IPCR. */
			MD_TRY(md_fault());
			if (md_read(MD_BAR+md_bhi+0x28)==2) return EAGAIN;
			md_sahara_size=sizeof(md_sahara_stream);md_sahara_pos=0;
			MD_TRY(md_receive(&md_channels[1],md_sahara_stream,&md_sahara_size,20000));
			if (!md_sahara_size) return EIO;
		}
		size_t n=md_sahara_size-md_sahara_pos;
		if (n>length) n=length;
		memcpy(p,md_sahara_stream+md_sahara_pos,n);
		md_sahara_pos+=n;p+=n;length-=n;
	}
	return 0;
}
static int md_sahara_message(uint8_t *packet,size_t *length)
{
	MD_TRY(md_sahara_exact(packet,8));
	uint32_t size=md_u32(packet+4);
	if (size<8 || size>*length) return EPROTO;
	*length=size;
	return md_sahara_exact(packet+8,size-8);
}
static int md_sahara_hello(const uint8_t *packet,size_t length,uint32_t response[12])
{
	if (length!=48 || md_u32(packet+8)<2 || md_u32(packet+12)>2 || md_u32(packet+20)>1) return ENOTSUP;
	memset(response,0,48);
	response[0]=2;response[1]=48;response[2]=2;response[3]=1;
	response[5]=md_u32(packet+20); /* Echo image transfer pending/complete. */
	return 0;
}
static int md_contexts(void)
{
	md_stage="MHI-READY";
	MD_TRY(md_wait(MD_BAR+0x38,2,0,20000000));
	MD_TRY(md_wait(MD_BAR+0x48,0xff05,0x101,20000000));
	md_chdb=md_read(MD_BAR+0x18);md_erdb=md_read(MD_BAR+0x20);
	if ((md_chdb&7) || md_chdb<0xa8 || md_chdb>md_bar_size-128*8 ||
	    (md_erdb&7) || md_erdb<0xa8 || md_erdb>md_bar_size-MD_NER*8) return EPROTO;
	md_chctx=md_alloc(128*44);md_erctx=md_alloc(MD_NER*44);
	md_cmdctx=md_alloc(44);md_cmdring=md_alloc_aligned(MD_TRES*16,MD_TRES*16);
	if (!md_chctx.p || !md_erctx.p || !md_cmdctx.p || !md_cmdring.p) return ENOMEM;
	static const unsigned channels[]={2,3,4,5,10,11,14,15,20,21,100,101};
	for (unsigned i=0;i<sizeof(channels)/sizeof(channels[0]);i++) {
		unsigned ch=channels[i];uint32_t *c=md_channel_context(ch);
		c[0]=(ch>=100?3u:2u)<<8;c[1]=(ch&1)?2:1;
		c[2]=ch>=100?ch-96:(ch<12?1:0);
	}
	for (unsigned e=0;e<MD_NER;e++) {
		struct md_ring *r=&md_events[e];
		r->count=md_event_counts[e];r->consumer=0;r->wp=0;
		r->mem=md_alloc_aligned(r->count*16,r->count*16);
		if (!r->mem.p) return ENOMEM;
		uint32_t *c=(uint32_t *)md_erctx.p+e*11;
		c[0]=e==1?5u<<16:(e>=4?1u<<16:0);c[1]=1;c[2]=e+1;
		md_set64(c,3,r->mem.a);md_set64(c,5,r->count*16);
		md_set64(c,7,r->mem.a);md_set64(c,9,r->mem.a);
	}
	md_hw_events_ready=false;
	/* Only software rings are populated before READY->M0. */
	for (unsigned e=0;e<4;e++) {
		struct md_ring *r=&md_events[e];
		uint32_t *c=(uint32_t *)md_erctx.p+e*11;
		r->wp=r->count-1;
		md_set64(c,9,r->mem.a+r->wp*16);
	}
	uint32_t *cmd=(uint32_t *)md_cmdctx.p;
	md_set64(cmd,3,md_cmdring.a);md_set64(cmd,5,MD_TRES*16);
	md_set64(cmd,7,md_cmdring.a);md_set64(cmd,9,md_cmdring.a);
	md_address(MD_BAR+0x58,md_chctx.a);md_address(MD_BAR+0x60,md_erctx.a);
	md_address(MD_BAR+0x68,md_cmdctx.a);
	md_address(MD_BAR+0x80,MD_IOVA);md_address(MD_BAR+0x88,MD_IOVA+MD_DMA_SIZE-1);
	md_address(MD_BAR+0x98,MD_IOVA);md_address(MD_BAR+0xa0,MD_IOVA+MD_DMA_SIZE-1);
	uint32_t cfg=md_read(MD_BAR+0x10);
	if (cfg==UINT32_MAX) return EIO;
	md_write(MD_BAR+0x10,(cfg&0xffff)|MD_NER<<16|2u<<24);
	md_publish();
	for (unsigned e=0;e<4;e++)
		md_address(MD_BAR+md_erdb+e*8,md_events[e].mem.a+md_events[e].wp*16);
	MD_TRY(md_update(MD_BAR+0x38,0xff00,0x200));
	MD_TRY(md_wait(MD_BAR+0x48,0xff04,0x200,20000000));
	MD_LOG("MHI M0 contexts ready events=6\n");return 0;
}

/* Only files in /fw/sdx55/, never block devices. Names cannot escape this
 * directory. Sahara serves reads of signed boot images; all other modes fail.
 * If the private payload did not carry an image, use the read-only Android
 * modem FAT partition. */
#include "umi_modem_fat.inc"
static int md_file(const char *name,uint64_t off,void *data,size_t *length,bool whole)
{
	if (!name || !name[0] || strlen(name)>63 || strnstr(name,"..",64)) return EINVAL;
	for (const char *p=name;*p;p++) if (!((*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') ||
	    (*p>='0'&&*p<='9') || *p=='.' || *p=='_' || *p=='-')) return EINVAL;
	char path[96];snprintf(path,sizeof(path),"/fw/sdx55/%s",name);
	vnode_t vn;vfs_context_t context=vfs_context_kernel();
	int error=vnode_lookup(path,0,&vn,context);
	if (!error) {
		struct vnode_attr attr;VATTR_INIT(&attr);VATTR_WANTED(&attr,va_data_size);
		error=vnode_getattr(vn,&attr,context);
		if (!error && (vnode_vtype(vn)!=VREG || !attr.va_data_size || attr.va_data_size>512ull*1024*1024)) error=EINVAL;
		if (!error && whole) {
			if (attr.va_data_size>*length) error=EFBIG;else *length=(size_t)attr.va_data_size;
		}
		if (!error && (off>attr.va_data_size || *length>attr.va_data_size-off || *length>INT_MAX)) error=ERANGE;
		/* A NULL buffer validates the whole range before sending any bytes. */
		if (!error && data) {
			int left=0;
			error=vn_rdwr(UIO_READ,vn,(caddr_t)data,(int)*length,(off_t)off,UIO_SYSSPACE,0,
			    vfs_context_ucred(context),&left,vfs_context_proc(context));
			if (!error && left) error=EIO;
		}
		vnode_put(vn);
	}
	if (!error || !strcmp(name, "sahara.map")) return error;
	/* The map is metadata supplied by the build; proprietary image bytes may
	 * come from modem, but never a path selected by a Sahara request. */
	if (error == ENOENT) {
		error = md_fat_file(name, off, data, length, whole);
		if (!error) MD_LOG("image %s source=UFS modem FAT\n", name);
	}
	return error;
}
enum md_image_source { MD_SOURCE_NONE, MD_SOURCE_PAYLOAD, MD_SOURCE_FAT, MD_SOURCE_UFS };
struct md_image {
	unsigned id;
	char name[64];
	enum md_image_source source;
	vnode_t vnode;
	uint64_t size;
	const char *partition;
	struct md_fat_file fat;
};
static struct md_image md_images[32];
static unsigned md_image_count;
static uint64_t md_image_bytes[32];

/* These four Sahara IDs are the modem's board-local calibration/DDR stores.
 * They are never firmware files and never a writable transport endpoint. */
static const char *md_ufs_partition_for_id(unsigned id)
{
	switch (id) {
	case 16: return "mdm1m9kefs1";
	case 17: return "mdm1m9kefs2";
	case 20: return "mdm1m9kefs3";
	case 34: return "mdmddr";
	default: return NULL;
	}
}
/* Compile-time policy guard: this modem path has no UFS write primitive. */
#define MD_UFS_READ_ONLY 1
_Static_assert(MD_UFS_READ_ONLY == 1, "external modem UFS sources must remain read-only");

static int md_ufs_file(const char *partition, uint64_t off, void *data, size_t *length, bool whole)
{
	uint64_t size = 0;
	int error;
	if (!MD_UFS_READ_ONLY || !partition || !length ||
	    (strcmp(partition,"mdm1m9kefs1") && strcmp(partition,"mdm1m9kefs2") &&
	     strcmp(partition,"mdm1m9kefs3") && strcmp(partition,"mdmddr"))) {
		MD_LOG("Sahara UFS partition refused name=%s\n",partition ? partition : "(null)");
		return EROFS;
	}
	/* Querying with len=0 obtains the GPT partition byte size without a read. */
	error = hakios_ufs_read_partition(partition, 0, NULL, 0, &size);
	if (error || !size) return error ? error : ENOENT;
	if (off > size || (!whole && (uint64_t)*length > size - off) ||
	    (whole && size > SIZE_MAX)) {
		MD_LOG("Sahara UFS range partition=%s offset=%llu length=%llu part_bytes=%llu\n",
		    partition, off, (uint64_t)*length, size);
		return ERANGE;
	}
	if (whole) *length = (size_t)size;
	if (!data || !*length) return 0;
	if (*length > INT_MAX) return EFBIG;
	error = hakios_ufs_read_partition(partition, off, data, *length, NULL);
	if (error) MD_LOG("Sahara UFS read failed partition=%s offset=%llu length=%llu errno=%d\n",
	    partition, off, (uint64_t)*length, error);
	return error;
}

static int md_image_index(uint64_t id)
{
	for (unsigned i = 0; i < md_image_count; i++) if (md_images[i].id == id) return (int)i;
	return -1;
}
static int md_image_prepare(struct md_image *image)
{
	const char *partition;
	int error;
	if (!image) return EINVAL;
	if (image->source != MD_SOURCE_NONE) return 0;
	partition = md_ufs_partition_for_id(image->id);
	if (partition) {
		image->partition = partition;
		error = hakios_ufs_read_partition(partition, 0, NULL, 0, &image->size);
		if (error || !image->size) return error ? error : ENOENT;
		image->source = MD_SOURCE_UFS;
		MD_LOG("Sahara image id=%u source=UFS partition=%s size=%llu\n",
		    image->id, partition, image->size);
		return 0;
	}
	char path[96];
	snprintf(path, sizeof(path), "/fw/sdx55/%s", image->name);
	vfs_context_t context = vfs_context_kernel();
	error = vnode_lookup(path, 0, &image->vnode, context);
	if (!error) {
		struct vnode_attr attr;
		VATTR_INIT(&attr); VATTR_WANTED(&attr, va_data_size);
		error = vnode_getattr(image->vnode, &attr, context);
		if (!error && (vnode_vtype(image->vnode) != VREG || !attr.va_data_size ||
		    attr.va_data_size > 512ull * 1024 * 1024)) error = EINVAL;
		if (error) { vnode_put(image->vnode); image->vnode = NULLVP; return error; }
		image->size = attr.va_data_size; image->source = MD_SOURCE_PAYLOAD;
		MD_LOG("Sahara image id=%u source=payload path=%s size=%llu\n",image->id,path,image->size);
		return 0;
	}
	if (error != ENOENT) return error;
	error = md_fat_open(image->name, &image->fat);
	if (error) return error;
	image->size = image->fat.size; image->source = MD_SOURCE_FAT;
	MD_LOG("Sahara image id=%u source=UFS modem FAT path=image/sdx55m/%s size=%llu\n",
	    image->id, image->name, image->size);
	return 0;
}
static int md_image_check(const struct md_image *image, uint64_t offset, uint64_t length, bool whole)
{
	if (!image || (whole && length > image->size) || offset > image->size ||
	    (!whole && length > image->size - offset)) return whole ? EFBIG : ERANGE;
	return 0;
}
static int md_image_read_chunk(const struct md_image *image, uint64_t offset, void *data, size_t length)
{
	if (!image || !data || !length) return EINVAL;
	if (image->source == MD_SOURCE_FAT)
		return md_fat_read_prepared(&image->fat, offset, data, length);
	if (image->source == MD_SOURCE_UFS)
		return hakios_ufs_read_partition(image->partition, offset, data, length, NULL);
	if (image->source == MD_SOURCE_PAYLOAD) {
		int left = 0;
		int error = vn_rdwr(UIO_READ, image->vnode, (caddr_t)data, (int)length,
		    (off_t)offset, UIO_SYSSPACE, 0, vfs_context_ucred(vfs_context_kernel()),
		    &left, vfs_context_proc(vfs_context_kernel()));
		if (!error && left) error = EIO;
		return error;
	}
	return ENOENT;
}
static int md_image_map(void)
{
	char map[4096];size_t size=sizeof(map)-1;
	int map_error=md_file("sahara.map",0,map,&size,true);
	if (map_error) {
		/* Keep only public filename/ID metadata in the kernel. */
		static const char builtin[]=
		    "6 apps.mbn\n8 qdsp6sw.mbn\n16 mdm1m9kefs1.img\n"
		    "17 mdm1m9kefs2.img\n20 mdm1m9kefs3.img\n21 sbl1.mbn\n"
		    "23 aop.mbn\n25 tz.mbn\n29 acdb.mbn\n33 hyp.mbn\n"
		    "34 mdmddr.img\n36 multi_image_qti.mbn\n37 multi_image.mbn\n"
		    "38 xbl_cfg.elf\n40 apdp.mbn\n41 devcfg.mbn\n42 sec.elf\n";
		if (map_error != ENOENT) return map_error;
		if (sizeof(builtin) > sizeof(map)) return E2BIG;
		memcpy(map, builtin, sizeof(builtin)); size = sizeof(builtin) - 1;
		MD_LOG("sahara.map source=builtin metadata\n");
	}
	md_image_count=0;memset(md_image_bytes,0,sizeof(md_image_bytes));map[size]=0;
	char *p=map;
	while (*p) {
		while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') p++;
		if (!*p) break;
		if (*p=='#') { while (*p && *p!='\n') p++;continue; }
		if (md_image_count==32) return E2BIG;
		unsigned id=0,digits=0;
		while (*p>='0' && *p<='9') { if (id>6553) return ERANGE;id=id*10+(*p++-'0');digits++; }
		if (!digits || !id || id>65535 || (*p!=' ' && *p!='\t')) return EINVAL;
		while (*p==' ' || *p=='\t') p++;
		char *begin=p;
		while (*p && *p!='\n' && *p!='\r' && *p!=' ' && *p!='\t') p++;
		size_t len=(size_t)(p-begin);
		if (!len || len>=64) return EINVAL;
		for (unsigned i=0;i<md_image_count;i++) if (md_images[i].id==id) return EEXIST;
		struct md_image *image=&md_images[md_image_count++];memset(image,0,sizeof(*image));image->id=id;
		memcpy(image->name,begin,len);image->name[len]=0;
		while (*p==' ' || *p=='\t' || *p=='\r') p++;
		if (*p && *p!='\n') return EINVAL;
	}
	return md_image_count?0:EINVAL;
}
static int md_sahara_read(uint64_t id,uint64_t offset,uint64_t length)
{
	int image_index=md_image_index(id);
	struct md_image *image;
	if (image_index < 0) { MD_LOG("Sahara missing image id=%llu; check sahara.map\n",id);return ENOENT; }
	image=&md_images[image_index];
	MD_TRY(md_image_prepare(image));
	if (!length || offset>512ull*1024*1024 || length>512ull*1024*1024-offset) return ERANGE;
	MD_TRY(md_image_check(image, offset, length, false));
	MD_LOG("Sahara READ_DATA id=%llu offset=%llu length=%llu chunk=%u\n",id,offset,length,MD_PACKET);
	uint64_t total=length, sent=0, started=md_now_us(), next_report=8ull*1024*1024;
	while (length) {
		size_t count=length>MD_PACKET?MD_PACKET:(size_t)length;
		MD_TRY(md_image_read_chunk(image,offset,md_sahara_packet,count));
		MD_TRY(md_send(&md_channels[0],md_sahara_packet,count));
		offset+=count;length-=count;
		sent+=count;
		if (sent >= next_report || !length) {
			uint64_t elapsed=md_now_us()-started;
			uint64_t mbps=elapsed ? sent*1000000ull/(elapsed*1024ull*1024ull) : 0;
			MD_LOG("Sahara progress id=%llu bytes=%llu/%llu elapsed_us=%llu MBps=%llu\n",
			    id,sent,total,elapsed,mbps);
			while (next_report <= sent) next_report += 8ull*1024*1024;
		}
	}
	md_image_bytes[image_index] += total;
	return 0;
}
static int md_sahara(void)
{
	md_stage="Sahara";
	MD_TRY(md_start_channel(0,2,1));MD_TRY(md_start_channel(1,3,1));
	uint64_t transferred=0;
	for (unsigned requests=0;requests<16384;requests++) {
		size_t length=256;
		MD_TRY(md_sahara_message(md_sahara_packet,&length));
		uint32_t command=md_u32(md_sahara_packet);
		if (command==1) {
			uint32_t hello[12];
			MD_TRY(md_sahara_hello(md_sahara_packet,length,hello));
			MD_LOG("Sahara HELLO image-mode=%u\n",hello[5]);
			MD_TRY(md_send(&md_channels[0],hello,sizeof(hello)));
		} else if (command==3 || command==0x12) {
			if (length!=(command==3?20:32)) return EPROTO;
			uint64_t id=command==3?md_u32(md_sahara_packet+8):md_u64(md_sahara_packet+8);
			uint64_t offset=command==3?md_u32(md_sahara_packet+12):md_u64(md_sahara_packet+16);
			uint64_t count=command==3?md_u32(md_sahara_packet+16):md_u64(md_sahara_packet+24);
			if (count>512ull*1024*1024-transferred) return EFBIG;
			MD_TRY(md_sahara_read(id,offset,count));
			transferred+=count;
		} else if (command==4) {
			uint32_t image_id,status;
			if (length!=16) return EPROTO;
			image_id=md_u32(md_sahara_packet+8);status=md_u32(md_sahara_packet+12);
			MD_LOG("Sahara image complete id=%u status=%u bytes=%llu\n",image_id,status,
			    md_image_index(image_id)>=0 ? md_image_bytes[md_image_index(image_id)] : 0);
			if (status) return EIO;
			uint32_t done[2]={5,8};MD_TRY(md_send(&md_channels[0],done,sizeof(done)));
		} else if (command==6) {
			if (length!=12 || md_u32(md_sahara_packet+8)>1) return EPROTO;
			if (md_u32(md_sahara_packet+8)==1) {
				MD_LOG("Sahara complete bytes=%llu\n",transferred);return 0;
			}
		} else {
			/* Includes memory-debug/RDDM and all command/write modes.  The modem
			 * path deliberately has no implementation that could expose or alter
			 * calibration storage. */
			MD_LOG("Sahara rejected command=%#x length=%u (read-only policy)\n",command,(unsigned)length);
			return ENOTSUP;
		}
	}
	return ELOOP;
}
#include "umi_modem_qmi.h"
#include "umi_modem_tftp.h"
static uint32_t md_nas_node,md_nas_port,md_uim_node,md_uim_port,md_dms_node,md_dms_port;
static uint32_t md_wda_node,md_wda_port,md_wds_node,md_wds_port;
static bool md_registered;
static unsigned md_services;
/* Host QRTR node 1. Control traffic uses the control port; the one QMI
 * client uses the first Linux ephemeral port. */
#define MD_QRTR_CTRL 0xfffffffeu
#define MD_QMI_PORT 0x4000u
#define MD_WDS_CLIENT 0x4004u
#define MD_WDA_CLIENT 0x4005u
/* Host servreg locator (QMI 64 v1 instance 1), as Linux pd-mapper port 16385. */
#define MD_PDM_PORT 0x4001u
static uint8_t md_qmi_reply[512],md_pdm_reply[256];
static size_t md_qmi_reply_size;
static uint32_t md_qmi_reply_node,md_qmi_reply_port,md_qmi_reply_client;
static uint8_t md_qrtr_tx[MD_PACKET];
static int md_qrtr_send(unsigned type,unsigned src_port,unsigned node,unsigned port,const void *payload,size_t size)
{
	uint32_t header[8]={1,type,1,src_port,0,(uint32_t)size,node,port};
	if (size>sizeof(md_qrtr_tx)-32) return EMSGSIZE;
	memcpy(md_qrtr_tx,header,32);memcpy(md_qrtr_tx+32,payload,size);
	return md_send(&md_channels[2],md_qrtr_tx,(32+size+3)&~(size_t)3);
}
/* Host TFTP (QRTR service 4096) as Linux tqftpserv: published at versions 1
 * and 3 (the SDX55 looks up v3), one host port per transfer. The modem reads
 * its carrier configuration (modem_pr/mcfg) here and stays offline
 * (DMS DeviceNotReady) without it. Reads come only from the read-only modem
 * FAT partition: /readonly/firmware/image/X is image/X there. Writes
 * (/readwrite/...) are acknowledged and discarded; nothing is stored. */
#define MD_TFTP_V1_PORT 0x4002u
#define MD_TFTP_V3_PORT 0x4003u
#define MD_TFTP_PORT 0x4010u
#define MD_TFTP_SLOTS 4u
#define MD_TFTP_PREFIX "/readonly/firmware/image/"
struct md_tftp {
	bool used,writer;
	uint32_t node,port,blksize,wsize,blocks;
	uint64_t seek,limit;
	uint16_t expected;
	struct md_fat_file file;
};
static struct md_tftp md_tftp[MD_TFTP_SLOTS];
static bool md_tftp_published;
/* The SDX55 router's node (the HELLO sender, 2). As Linux qrtr_node_enqueue,
 * every control packet names it as destination; v16's NEW_SERVER to
 * 0xffffffff made the router re-HELLO and drop our TFTP registration. */
static uint32_t md_qrtr_peer;
static unsigned md_tftp_reads,md_tftp_writes,md_tftp_missing;
static uint64_t md_tftp_bytes;
static uint8_t md_tftp_tx[MD_PACKET];
static void md_tftp_close(struct md_tftp *t)
{
	if (t->file.chain) kfree_data(t->file.chain,(size_t)t->file.chain_count*sizeof(*t->file.chain));
	memset(t,0,sizeof(*t));
}
static int md_tftp_error(unsigned src_port,uint32_t node,uint32_t port,uint8_t code,const char *text)
{
	uint8_t packet[32]={0,UMI_TFTP_ERROR,0,code};
	size_t n=strlen(text)+1;
	memcpy(packet+4,text,n);
	return md_qrtr_send(1,src_port,node,port,packet,4+n);
}
static int md_tftp_data(unsigned slot,uint32_t block)
{
	struct md_tftp *t=&md_tftp[slot];
	uint32_t n=umi_tftp_block_length(t->limit,t->blksize,block);
	md_tftp_tx[0]=0;md_tftp_tx[1]=UMI_TFTP_DATA;md_tftp_tx[2]=(uint8_t)(block>>8);md_tftp_tx[3]=(uint8_t)block;
	if (n) MD_TRY(md_fat_read_prepared(&t->file,t->seek+(uint64_t)(block-1)*t->blksize,md_tftp_tx+4,n));
	md_tftp_bytes+=n;
	return md_qrtr_send(1,MD_TFTP_PORT+slot,t->node,t->port,md_tftp_tx,4+n);
}
static int md_tftp_announce(uint32_t node)
{
	uint32_t v1[5]={4,4096,1,1,MD_TFTP_V1_PORT},v3[5]={4,4096,3,1,MD_TFTP_V3_PORT};
	MD_TRY(md_qrtr_send(4,MD_QRTR_CTRL,node,MD_QRTR_CTRL,v1,sizeof(v1)));
	MD_TRY(md_qrtr_send(4,MD_QRTR_CTRL,node,MD_QRTR_CTRL,v3,sizeof(v3)));
	MD_LOG("QRTR announced TFTP service=4096 v1 port=%#x v3 port=%#x to node=%u\n",MD_TFTP_V1_PORT,MD_TFTP_V3_PORT,node);
	return 0;
}
/* Route B probe: a host-side service the modem may address (Linux qrtr-ns NEW_SERVER), same type-4
 * packet as md_tftp_announce. The NEW_SERVER words are {cmd, service, version | instance << 8,
 * host node, port}; the host node is 1, as in the TFTP registration. The bridge names the service
 * (IMSDCM 0x302 v1 instance 0, per the Android qrtr-lookup evidence) and when; a request that
 * arrives before the router's HELLO is kept and sent from the HELLO handler, so a re-HELLO or a
 * modem restart re-publishes it exactly as Linux announce_servers does. Logged once per boot. */
#define MD_HOST_NODE 1u
static uint32_t md_host_service[5];
static bool md_host_known,md_host_logged;
static int md_host_announce(uint32_t node,uint32_t port)
{
	if (!md_host_known) return EINVAL;
	MD_TRY(md_qrtr_send(4,MD_QRTR_CTRL,node,port,md_host_service,sizeof(md_host_service)));
	if (!md_host_logged) {
		md_host_logged=true;
		MD_LOG("dcm: announced svc %#x v%u node %u port %u\n",md_host_service[1],md_host_service[2]&255u,node,md_host_service[4]);
	}
	return 0;
}
int umi_bb_announce(uint32_t service,uint32_t version,uint32_t instance,uint32_t port)
{
	md_host_service[0]=4;md_host_service[1]=service;
	md_host_service[2]=(version&255u)|(instance<<8);md_host_service[3]=MD_HOST_NODE;md_host_service[4]=port;
	md_host_known=true;
	if (!md_bridge || !md_qrtr_peer) return 0;
	return md_host_announce(md_qrtr_peer,MD_QRTR_CTRL);
}
/* Empty files (e.g. oem_hw_null.txt) are valid TFTP reads but not images. */
static int md_tftp_open(const char *path,struct md_fat_file *file)
{
	md_fat_quiet=true;
	int error=md_fat_open_path("tftp",path,file);
	if (error==ERANGE && !file->directory && !file->size) error=0;
	md_fat_quiet=false;
	return error;
}
static int md_tftp_request(unsigned service_port,const uint8_t *d,size_t size,uint32_t node,uint32_t port)
{
	struct umi_tftp_request r;
	unsigned slot=0;
	if (umi_tftp_parse(d,size,&r)) return md_tftp_error(service_port,node,port,4,"bad request");
	while (slot<MD_TFTP_SLOTS && md_tftp[slot].used) slot++;
	if (slot==MD_TFTP_SLOTS) return md_tftp_error(service_port,node,port,0,"busy");
	struct md_tftp *t=&md_tftp[slot];
	unsigned src=MD_TFTP_PORT+slot;
	t->used=true;t->node=node;t->port=port;t->wsize=r.wsize;
	t->blksize=r.blksize<MD_PACKET-64?r.blksize:MD_PACKET-64;
	if (r.op==UMI_TFTP_WRQ) {
		t->writer=true;t->expected=1;md_tftp_writes++;
		MD_LOG("TFTP write %s acknowledged and discarded\n",r.path);
		if (!r.options) { uint8_t ack[4]={0,UMI_TFTP_ACK,0,0}; return md_qrtr_send(1,src,node,port,ack,4); }
		size_t n=umi_tftp_oack(md_tftp_tx,sizeof(md_tftp_tx),&r,t->blksize,0);
		return n?md_qrtr_send(1,src,node,port,md_tftp_tx,n):EMSGSIZE;
	}
	char path[160];
	size_t prefix=strlen(MD_TFTP_PREFIX);
	int error=strncmp(r.path,MD_TFTP_PREFIX,prefix) || strlen(r.path+prefix)+7>sizeof(path) ? ENOENT : 0;
	if (!error) {
		snprintf(path,sizeof(path),"image/%s",r.path+prefix);
		error=md_tftp_open(path,&t->file);
	}
	if (!error && r.seek>t->file.size) error=ERANGE;
	if (!error) {
		t->seek=r.seek;t->limit=t->file.size-r.seek;
		if (r.rsize && r.rsize<t->limit) t->limit=r.rsize;
		t->blocks=umi_tftp_blocks(t->limit,t->blksize);
		if (t->blocks>65535) error=EFBIG;
	}
	if (error) {
		if (md_tftp_missing++<16) MD_LOG("TFTP read %s errno=%d\n",r.path,error);
		md_tftp_close(t);
		return md_tftp_error(src,node,port,1,"file not found");
	}
	md_tftp_reads++;
	if (!r.options) return md_tftp_data(slot,1);
	size_t n=umi_tftp_oack(md_tftp_tx,sizeof(md_tftp_tx),&r,t->blksize,t->file.size);
	return n?md_qrtr_send(1,src,node,port,md_tftp_tx,n):EMSGSIZE;
}
/* ACK n sends blocks n+1..n+wsize; the ACK of the last block or a modem
 * ERROR (including its end-of-transfer) ends the transfer. */
static int md_tftp_client(unsigned slot,const uint8_t *d,size_t size,uint32_t node,uint32_t port)
{
	struct md_tftp *t=&md_tftp[slot];
	if (!t->used || t->node!=node || t->port!=port || size<4) return 0;
	uint32_t block=(uint32_t)d[2]<<8|d[3];
	if (d[1]==UMI_TFTP_ERROR) { md_tftp_close(t);return 0; }
	if (t->writer) {
		bool last=size-4<t->blksize;
		if (d[1]!=UMI_TFTP_DATA) { md_tftp_close(t);return 0; }
		if (block!=t->expected) return 0;
		t->expected++;
		if (block%t->wsize==0 || last) {
			uint8_t ack[4]={0,UMI_TFTP_ACK,d[2],d[3]};
			MD_TRY(md_qrtr_send(1,MD_TFTP_PORT+slot,node,port,ack,4));
		}
		if (last) md_tftp_close(t);
		return 0;
	}
	if (d[1]!=UMI_TFTP_ACK || block>=t->blocks) { md_tftp_close(t);return 0; }
	for (uint32_t b=block+1;b<=t->blocks && b<=block+t->wsize;b++) MD_TRY(md_tftp_data(slot,b));
	return 0;
}
/* As Linux qrtr_endpoint_post: the first byte selects the 32-byte v1 header
 * or the 16-byte v2 header (version byte 3, u8 type/flags/optlen, le32 size,
 * le16 src node/port) used by the SDX55 IPC router (xprt-version 3). */
static int md_qrtr_packet(const uint8_t *p,size_t length)
{
	unsigned ver=length?p[0]:0,type=0,size=0,node=0,port=0,dst_port=0,hdr=0;
	bool confirm=false;
	int domains=-1;size_t reply=0;
	if (ver==1 && length>=32) {
		type=md_u32(p+4);node=md_u32(p+8);port=md_u32(p+12);
		confirm=md_u32(p+16)!=0;size=md_u32(p+20);dst_port=md_u32(p+28);hdr=32;
	} else if (ver==3 && length>=16) {
		type=p[1];confirm=p[2]&1;size=md_u32(p+4);hdr=16+p[3];
		node=p[8]|(unsigned)p[9]<<8;port=p[10]|(unsigned)p[11]<<8;
		dst_port=p[14]|(unsigned)p[15]<<8;
		if (port==0xfffe) port=MD_QRTR_CTRL;
		if (dst_port==0xfffe) dst_port=MD_QRTR_CTRL;
	}
	if (!hdr || hdr>length || size>length-hdr) {
		MD_LOG("QRTR bad header ver=%u length=%u word0=%#x\n",ver,(unsigned)length,length>=4?md_u32(p):0);
		return EPROTO;
	}
	const uint8_t *d=p+hdr;
	if (confirm) {
		/* As Linux qrtr_send_resume_tx: from and naming the receiving port. */
		uint32_t resume[5]={7,1,dst_port,0,0};
		MD_TRY(md_qrtr_send(7,dst_port,node,port,resume,sizeof(resume)));
	}
	if (type==4 && size>=20 && md_u32(d)==4) {
		unsigned service=md_u32(d+4),instance=md_u32(d+8),sn=md_u32(d+12),sp=md_u32(d+16);
		MD_LOG("QRTR service=%u instance=%#x node=%u port=%u\n",service,instance,sn,sp);
		md_services++;
		if (md_bridge) umi_bb_new_server(service,instance,sn,sp);
		if (service==3 && !md_nas_port) { md_nas_node=sn;md_nas_port=sp; }
		if (service==11 && !md_uim_port) { md_uim_node=sn;md_uim_port=sp; }
		if (service==2 && !md_dms_port) { md_dms_node=sn;md_dms_port=sp; }
		if (service==1 && !md_wds_port) { md_wds_node=sn;md_wds_port=sp; }
		if (service==26 && !md_wda_port) { md_wda_node=sn;md_wda_port=sp; }
		/* As umi-linux: TFTP goes up once MPSS EFS (node-3 service 21) is
		 * announced. Published minutes later, the modem never read it. */
		if (service==21 && sn==3 && !md_tftp_published) {
			md_tftp_published=true;
			MD_TRY(md_tftp_announce(md_qrtr_peer));
		}
	} else if (type==2 && size>=4) {
		MD_LOG("QRTR HELLO received node=%u ver=%u\n",node,ver);
		md_qrtr_hello=true;md_qrtr_peer=node;
		/* As Linux qrtr-ns ctrl_cmd_hello: say_hello() back to the sender, then
		 * announce all the host's services (announce_servers). The SDX55 router
		 * re-sends HELLO when it (re)opens its link and sends no control
		 * messages (NEW_SERVER) on it until our HELLO answers; data packets
		 * still flow. */
		uint32_t hello[5]={2,0,0,0,0},server[5]={4,64,0x101,1,MD_PDM_PORT};
		MD_TRY(md_qrtr_send(2,MD_QRTR_CTRL,node,MD_QRTR_CTRL,hello,sizeof(hello)));
		MD_LOG("QRTR HELLO replied to node=%u\n",node);
		MD_TRY(md_qrtr_send(4,MD_QRTR_CTRL,node,MD_QRTR_CTRL,server,sizeof(server)));
		MD_LOG("QRTR announced servreg locator service=64 port=%#x to node=%u\n",MD_PDM_PORT,node);
		if (md_tftp_published) MD_TRY(md_tftp_announce(node));
		if (md_host_known) MD_TRY(md_host_announce(node,MD_QRTR_CTRL));
	} else if (type==1 && dst_port==MD_PDM_PORT &&
	    (domains=umi3_servreg_domains(d,size,md_pdm_reply,sizeof(md_pdm_reply),&reply))>=0) {
		const uint8_t *name=NULL;size_t name_length=0;
		(void)umi3_find_tlv(d,size,1,&name,&name_length);
		MD_LOG("QRTR servreg locator request=%.*s domains=%d node=%u port=%u\n",(int)name_length,name,domains,node,port);
		MD_TRY(md_qrtr_send(1,MD_PDM_PORT,node,port,md_pdm_reply,reply));
	} else if (type==1 && (dst_port==MD_TFTP_V1_PORT || dst_port==MD_TFTP_V3_PORT)) {
		MD_TRY(md_tftp_request(dst_port,d,size,node,port));
	} else if (type==1 && dst_port>=MD_TFTP_PORT && dst_port<MD_TFTP_PORT+MD_TFTP_SLOTS) {
		MD_TRY(md_tftp_client(dst_port-MD_TFTP_PORT,d,size,node,port));
	} else if (type==10 && size>=20 && md_u32(d)==10) {
		/* NEW_LOOKUP as qrtr-ns ctrl_cmd_new_lookup: a node asks where a service lives. Log the
		 * first few (a modem-side probe for 770/40 is the route-B question); answer only the host
		 * service this boot announced. TODO(route B, report INVESTIGATION-routeB-x55-noreg.md §1.2):
		 * Android also hosts IMSRTP 40 (0x28) on the AP, the modem as its client -- a later phase. */
		static unsigned lookups;
		if (lookups++<8)
			MD_LOG("QRTR lookup service=%u instance=%#x from node=%u port=%u\n",md_u32(d+4),md_u32(d+8),node,port);
		if (md_host_known && md_host_service[1]==md_u32(d+4)) MD_TRY(md_host_announce(node,port));
	} else if (type==6 && size>=12) {
		/* DEL_CLIENT: a modem port closed; drop its transfers (tqftpserv). */
		for (unsigned i=0;i<MD_TFTP_SLOTS;i++)
			if (md_tftp[i].used && md_tftp[i].node==md_u32(d+4) && md_tftp[i].port==md_u32(d+8)) md_tftp_close(&md_tftp[i]);
	} else if (type==1 && (dst_port==MD_QMI_PORT || dst_port==MD_WDS_CLIENT || dst_port==MD_WDA_CLIENT) && size<=sizeof(md_qmi_reply)) {
		if (dst_port==MD_WDS_CLIENT && node==md_wds_node && port==md_wds_port && size>=7 &&
		    d[0]==4 && umi3_le16(d+3)==0x22) {
			const uint8_t *v;size_t n;
			if (!umi3_find_tlv(d,size,1,&v,&n) && n>=1 && v[0]==1) {
				md_net_down();MD_LOG("WDS disconnected pdp_ip0\n");
			}
		}
		memcpy(md_qmi_reply,d,size);md_qmi_reply_size=size;
		md_qmi_reply_node=node;md_qmi_reply_port=port;md_qmi_reply_client=dst_port;
	} else if (md_bridge && type==5 && size>=20 && md_u32(d)==5) {
		umi_bb_del_server(md_u32(d+4),md_u32(d+12),md_u32(d+16));
	} else if (md_bridge && type==1 && umi_bb_qrtr(dst_port,node,port,d,(uint32_t)size)) {
		md_active_at=md_now_us();
	} else {
		MD_LOG("QRTR type=%u bytes=%u node=%u port=%u dst_port=%#x\n",type,size,node,port,dst_port);
	}
	return 0;
}
/* Mission-mode EFS sync (MHI 10/11) as umi-linux umi-efs-ram: the modem
 * opens a Sahara memory-debug session and offers its EFS image; read it,
 * discard every byte, RESET, and count the RESET_RESP as the acknowledgement.
 * RAM only: nothing is stored and no partition is ever opened for writing.
 * The initial EFS is the read-only Sahara images 16/17/20 at boot. */
static uint8_t md_efs_stream[MD_PACKET],md_efs_table[4096];
static size_t md_efs_pos,md_efs_size;
static unsigned md_efs_regions;
static uint64_t md_efs_bytes;
/* data NULL discards. Mid-message silence is a protocol error, not the
 * idle ETIMEDOUT the IPCR loops ignore. */
static int md_efs_exact(void *data,size_t length)
{
	uint8_t *p=data;
	while (length) {
		if (md_efs_pos==md_efs_size) {
			md_efs_size=sizeof(md_efs_stream);md_efs_pos=0;
			int error=md_receive(&md_channels[5],md_efs_stream,&md_efs_size,5000);
			if (error) { md_efs_size=0;return error==ETIMEDOUT?EIO:error; }
			if (!md_efs_size) return EIO;
		}
		size_t n=md_efs_size-md_efs_pos;
		if (n>length) n=length;
		if (p) { memcpy(p,md_efs_stream+md_efs_pos,n);p+=n; }
		md_efs_pos+=n;length-=n;
	}
	return 0;
}
static int md_efs_read(uint64_t address,uint64_t length,bool wide,void *data)
{
	uint32_t read[6]={10,16,(uint32_t)address,(uint32_t)length};
	if (wide) { read[0]=17;read[1]=24;md_set64(read,2,address);md_set64(read,4,length); }
	else if (address+length>1ull<<32) return EPROTO;
	MD_TRY(md_send(&md_channels[4],read,read[1]));
	return md_efs_exact(data,(size_t)length);
}
static int md_efs_message(void)
{
	uint32_t m[64];
	MD_TRY(md_efs_exact(m,8));
	if (m[1]<8 || m[1]>sizeof(m)) return EPROTO;
	MD_TRY(md_efs_exact(m+2,m[1]-8));
	if (m[0]==1 && m[1]==48) {
		if (m[2]<1 || m[3]>2 || m[5]!=2) return ENOTSUP;
		uint32_t hello[12]={2,48,m[2]<2?m[2]:2,m[3],0,2};
		md_efs_regions=0;md_efs_bytes=0;
		return md_send(&md_channels[4],hello,sizeof(hello));
	}
	if ((m[0]==9 && m[1]==16) || (m[0]==16 && m[1]==24)) {
		bool wide=m[0]==16;
		uint64_t address=wide?md_u64(m+2):m[2],size=wide?md_u64(m+4):m[3],total=0;
		size_t stride=wide?64:52;
		if (!size || size>sizeof(md_efs_table) || size%stride) return EPROTO;
		MD_TRY(md_efs_read(address,size,wide,md_efs_table));
		for (size_t at=0;at<size;at+=stride) {
			const uint8_t *e=md_efs_table+at;
			uint64_t base=wide?md_u64(e+8):md_u32(e+4),n=wide?md_u64(e+16):md_u32(e+8);
			if (n>64ull<<20 || (total+=n)>128ull<<20) return EPROTO;
			for (uint64_t off=0;off<n;off+=MD_PACKET)
				MD_TRY(md_efs_read(base+off,n-off<MD_PACKET?n-off:MD_PACKET,wide,NULL));
		}
		md_efs_regions=(unsigned)(size/stride);md_efs_bytes=total;
		uint32_t reset[2]={7,8};
		return md_send(&md_channels[4],reset,sizeof(reset));
	}
	if (m[0]==8 && m[1]==8) {
		MD_LOG("EFS sync regions=%u acknowledged in RAM (discarded) bytes=%llu\n",md_efs_regions,(unsigned long long)md_efs_bytes);
		return 0;
	}
	MD_LOG("EFS sync unexpected command=%u length=%u\n",m[0],m[1]);
	return EPROTO;
}
static int md_ipcr_receive(void)
{
	if (md_channels[5].rx_ready || md_efs_pos<md_efs_size) MD_TRY(md_efs_message());
	size_t length=sizeof(md_sahara_packet);
	int error=md_receive(&md_channels[3],md_sahara_packet,&length,100);
	if (error) return error;
	return md_qrtr_packet(md_sahara_packet,length);
}
/* Keep DL21 serviced (TFTP, NEW_SERVER) for ms milliseconds. */
static int md_pump(unsigned ms)
{
	uint64_t until=md_now_us()+(uint64_t)ms*1000;
	while (md_now_us()<until) {
		int error=md_ipcr_receive();
		if (error && error!=ETIMEDOUT) return error;
	}
	return 0;
}
/* One QMI request/response with a node-3 service over QRTR. Bounded by time,
 * not packet count: TFTP traffic shares DL21. */
static int md_qmi_call(uint32_t client,uint32_t node,uint32_t port,const uint8_t *request,size_t size,unsigned timeout_ms)
{
	md_qmi_reply_size=0;
	MD_TRY(md_qrtr_send(1,client,node,port,request,size));
	for (uint64_t until=md_now_us()+(uint64_t)timeout_ms*1000;md_now_us()<until;) {
		int error=md_ipcr_receive();
		if (error && error!=ETIMEDOUT) return error;
		if (md_qmi_reply_size>=7 && md_qmi_reply_client==client && md_qmi_reply_node==node && md_qmi_reply_port==port &&
		    md_qmi_reply[0]==2 && !memcmp(md_qmi_reply+1,request+1,4)) return 0;
		md_qmi_reply_size=0;
	}
	return ETIMEDOUT;
}
static int md_qmi(uint32_t node,uint32_t port,const uint8_t *request,size_t size)
{
	return md_qmi_call(MD_QMI_PORT,node,port,request,size,5000);
}
#include "umi_modem_data.inc"
/* One NAS serving-system query (own transaction counter; replies are matched
 * by port too). Logs status codes only, and only when they change. */
static uint16_t md_nas_transaction=0x8000;
static unsigned md_nas_last=UINT32_MAX;
static uint64_t md_nas_start;
static int md_nas_poll(bool *registered)
{
	struct umi3_state state={0};
	uint8_t request[16];size_t size;uint16_t result=0,code=0;
	if (umi3_nas_serving_system(request,sizeof(request),&md_nas_transaction,&size)) return EINVAL;
	MD_TRY(md_qmi(md_nas_node,md_nas_port,request,size));
	if (umi3_result(md_qmi_reply,md_qmi_reply_size,&result,&code)) return EPROTO;
	(void)umi3_apply_nas(&state,md_qmi_reply,md_qmi_reply_size);
	unsigned now=(unsigned)result<<24|(unsigned)(code&0xff)<<16|(unsigned)state.nas_reg<<8|state.nas_rat;
	if (now!=md_nas_last)
		MD_LOG("QMI NAS serving system result=%u error=%u reg=%u cs=%u ps=%u rat=%u after_ms=%llu (reg 1=registered 2=searching; rat 8=LTE 12=NR5G)\n",
		    result,code,state.nas_reg,state.nas_cs,state.nas_ps,state.nas_rat,(md_now_us()-md_nas_start)/1000);
	md_nas_last=now;*registered=state.nas_reg==1;
	md_registered=*registered && state.nas_ps==1;
	return 0;
}
/* Logs status codes only; never the ICCID, IMSI, IMEI, number or PLMN.
 * As ModemManager on umi-linux: wait for the SIM application, set DMS online
 * (the only write: operating mode), then follow NAS registration. v15 asked NAS
 * the moment it appeared and got InformationUnavailable (74) with the SIM app
 * still detecting (1). The radio goes online only after the modem has read its
 * carrier configuration over TFTP; until then DMS answers DeviceNotReady (52). */
static int md_qmi_status(void)
{
	struct umi3_state state={0};
	uint8_t request[16],mode=0xff;size_t size;uint16_t result=0,code=0;unsigned tries=0;
	md_stage="QMI-UIM";
	for (uint64_t until=md_now_us()+30000000;;) {
		if (umi3_uim_card_status(request,sizeof(request),&state.transaction,&size)) return EINVAL;
		MD_TRY(md_qmi(md_uim_node,md_uim_port,request,size));
		if (umi3_result(md_qmi_reply,md_qmi_reply_size,&result,&code)) return EPROTO;
		(void)umi3_apply_uim(&state,md_qmi_reply,md_qmi_reply_size);
		if (state.uim_app==7 || md_now_us()>=until) break;
		MD_TRY(md_pump(1000));
	}
	MD_LOG("QMI UIM card status result=%u error=%u card=%u app=%u (card 1=present, app 7=ready)\n",
	    result,code,state.uim_card,state.uim_app);
	md_stage="QMI-DMS";
	for (uint64_t until=md_now_us()+30000000;!md_dms_port && md_now_us()<until;) MD_TRY(md_pump(100));
	if (!md_dms_port) return ENOENT;
	if (umi3_dms_get_operating_mode(request,sizeof(request),&state.transaction,&size)) return EINVAL;
	MD_TRY(md_qmi(md_dms_node,md_dms_port,request,size));
	(void)umi3_dms_operating_mode(md_qmi_reply,md_qmi_reply_size,&mode);
	MD_LOG("QMI DMS operating mode=%u (0=online 1=low-power 3=offline 5=shutting-down)\n",mode);
	for (uint64_t until=md_now_us()+60000000;;) {
		if (umi3_dms_set_operating_mode(request,sizeof(request),&state.transaction,0,&size)) return EINVAL;
		MD_TRY(md_qmi(md_dms_node,md_dms_port,request,size));
		if (umi3_result(md_qmi_reply,md_qmi_reply_size,&result,&code)) return EPROTO;
		tries++;
		if (!result || code!=52 || md_now_us()>=until) break;
		MD_TRY(md_pump(2000));
	}
	MD_LOG("QMI DMS set online result=%u error=%u tries=%u TFTP reads=%u writes=%u missing=%u bytes=%llu (error 52=DeviceNotReady)\n",
	    result,code,tries,md_tftp_reads,md_tftp_writes,md_tftp_missing,(unsigned long long)md_tftp_bytes);
	md_stage="QMI-NAS";
	md_nas_start=md_now_us();
	for (uint64_t until=md_nas_start+90000000;;) {
		bool registered;
		MD_TRY(md_nas_poll(&registered));
		if (registered || md_now_us()>=until) break;
		MD_TRY(md_pump(2000));
	}
	MD_LOG("TFTP served reads=%u writes=%u missing=%u bytes=%llu\n",
	    md_tftp_reads,md_tftp_writes,md_tftp_missing,(unsigned long long)md_tftp_bytes);
	return 0;
}
static int md_mhi_boot(void)
{
	md_stage="firmware";
	/* Give the normal root mount a bounded opportunity to finish. */
	for (unsigned i=0;i<300 && rootvnode==NULLVP;i++) delay_for_interval(100,NSEC_PER_MSEC);
	if (rootvnode==NULLVP) return ENOENT;
	int error=md_image_map();
	if (error) { MD_LOG("need /fw/sdx55/sahara.map and original signed images; errno=%d\n",error);return error; }
	MD_TRY(md_dma_install());
	/* One 4 MiB BHI buffer for the life of the driver: the pool is never wound
	 * back (md_link_reset), so a restart must not allocate another one. */
	static struct md_buf sbl;
	if (!sbl.p) sbl=md_alloc(4u*1024u*1024u);
	if (!sbl.p) return ENOMEM;
	md_stage="firmware-sbl";
	size_t size=sbl.size;
	int sbl_index=md_image_index(21);
	int sbl_error=sbl_index < 0 ? ENOENT : md_image_prepare(&md_images[sbl_index]);
	if (!sbl_error) {
		if (md_images[sbl_index].size > size) sbl_error=EFBIG;
		else { size=(size_t)md_images[sbl_index].size; sbl_error=md_image_check(&md_images[sbl_index],0,size,true); }
	}
	if (!sbl_error) sbl_error=md_image_read_chunk(&md_images[sbl_index],0,sbl.p,size);
	if (sbl_error) { MD_LOG("firmware sbl1.mbn lookup errno=%d\n",sbl_error); return sbl_error; }
	MD_TRY(md_enable_dma());
	md_stage="BHI-SBL";
	md_write(MD_BAR+md_bhi+0x20,0);md_write(MD_BAR+md_bhi+0x2c,0);
	md_address(MD_BAR+md_bhi+8,sbl.a);md_write(MD_BAR+md_bhi+0x10,(uint32_t)size);
	md_publish();md_write(MD_BAR+md_bhi+0x18,1);
	MD_TRY(md_wait(MD_BAR+md_bhi+0x2c,0xc0000000,0x80000000,20000000));
	MD_LOG("BHI SBL accepted bytes=%lu\n",(unsigned long)size);
	MD_TRY(md_contexts());
	MD_TRY(md_wait(MD_BAR+md_bhi+0x28,UINT32_MAX,1,20000000));
	error=md_sahara();
	/* The verified loader also sees SBL disappear without DONE. Only a
	 * transport termination with live AMSS permits mission commands;
	 * malformed packets, file errors and SMMU faults remain fatal. */
	if (error) {
		if ((error!=EAGAIN && error!=ETIMEDOUT) || md_read(MD_BAR+md_bhi+0x28)!=2) return error;
		MD_TRY(md_fault());
		MD_LOG("Sahara transport ended errno=%d; verifying AMSS/IPCR\n",error);
	}
	md_stage="AMSS";
	bool amss=false;
	for (unsigned ms=0;ms<20000;ms++) {
		MD_TRY(md_poll());
		if (md_read(MD_BAR+md_bhi+0x28)==2) { amss=true;break; }
		delay(1000);
	}
	if (!amss) return ETIMEDOUT;
	MD_LOG("EE=2 AMSS\n");
	MD_LOG("mission power held on; runtime suspend disabled\n");
	/* Sahara completion and EE=2 can precede the controller's M0 transition
	 * and its EE change event.  Linux drains those control events before the
	 * QRTR client starts; do the same before issuing START_CHAN. */
	for (unsigned ms=0;ms<20000;ms++) {
		MD_TRY(md_poll());
		uint32_t state=md_read(MD_BAR+0x48);
		if (((state>>8)&255)==2 && md_ee_event==2) {
			MD_LOG("MHI M0 after AMSS EE event\n");break;
		}
		if (ms==19999) return ETIMEDOUT;
		delay(1000);
	}
	MD_TRY(md_enable_hw_events());
	md_stage="IPCR";
	MD_TRY(md_start_channel(2,20,0));MD_TRY(md_start_channel(3,21,0));
	/* As umi-linux: EFS sync channels open with IPCR in mission mode. */
	MD_TRY(md_start_channel(4,10,1));MD_TRY(md_start_channel(5,11,1));
	uint32_t hello[5]={2,0,0,0,0},lookup[5]={10,0,0,0,0};
	MD_TRY(md_qrtr_send(2,MD_QRTR_CTRL,UINT32_MAX,MD_QRTR_CTRL,hello,sizeof(hello)));
	MD_TRY(md_qrtr_send(10,MD_QRTR_CTRL,UINT32_MAX,MD_QRTR_CTRL,lookup,sizeof(lookup)));
	md_stage="QRTR-discovery";
	/* MPSS registers NAS/UIM/DMS/WDS about 32 s after mission mode (umi-linux
	 * modem-data logs 20/47: ModemManager's wait for them ends at AMSS+32 s).
	 * Their NEW_SERVER broadcasts arrive only while DL21 stays serviced. */
	uint64_t start=md_now_us(),until=start+120000000ull;
	if (until>md_deadline) until=md_deadline;
	while (!(md_nas_port && md_uim_port) && md_now_us()<until) {
		error=md_ipcr_receive();
		if (error && error!=ETIMEDOUT) return error;
	}
	MD_LOG("QRTR discovery services=%u NAS-node=%u NAS-port=%u UIM-node=%u UIM-port=%u after_ms=%llu\n",
	    md_services,md_nas_node,md_nas_port,md_uim_node,md_uim_port,(md_now_us()-start)/1000);
	if (!md_qrtr_hello || !md_services) return ETIMEDOUT;
	MD_TRY(md_fault());
	if (md_read(MD_BAR+md_bhi+0x28)!=2) return EIO;
	if (md_bridge) umi_bb_modem_ready();
	else if (md_nas_port && md_uim_port) MD_TRY(md_qmi_status());
	return 0;
}
/* After bring-up: keep IPCR (QRTR control, servreg, TFTP) and EFS sync
 * serviced for the whole run, and look at NAS once per 30 s. MSI stays
 * masked, so the thread sleeps between event-ring checks: 1 ms while traffic
 * flows (TFTP block round trips), 100 ms once quiet for 2 s. Returns 0 when
 * umi_modem_quiesce asked it to stop, else the link error. */
static bool md_service_stop;
/* The bridge's QMI messages leave from its own client ports (umi_bb_poll). */
static void md_bb_send(uint32_t local_port,uint32_t node,uint32_t port,const void *message,uint32_t length)
{
	int error=md_qrtr_send(1,local_port,node,port,message,length);
	if (error) MD_LOG("bridge send port=%#x bytes=%u errno=%d\n",local_port,length,error);
}
/* As Linux qrtr_port_remove: a released client port is announced as DEL_CLIENT {our node 1, port}. */
static void md_bb_release(uint32_t local_port)
{
	uint32_t del[5]={6,1,local_port,0,0};
	(void)md_qrtr_send(6,local_port,UINT32_MAX,MD_QRTR_CTRL,del,sizeof(del));
}
#include "umi_voice_audio_mhi.inc"

static int md_service(void)
{
	md_stage="service";md_deadline=UINT64_MAX;
	uint64_t quiet=md_now_us(),nas=quiet+30000000;
	bool data_attempted=false;
	MD_LOG("service running: IPCR/EFS serviced, %s\n",md_bridge?"QMI is CommCenter's":"NAS every 30 s");
	while (!__atomic_load_n(&md_service_stop,__ATOMIC_ACQUIRE)) {
		MD_TRY(md_poll());
		md_voice_service();
		if (md_bridge) {
			if (!data_attempted && md_wda_port) {
				data_attempted=true;
				uint32_t dl_max=0;
				int error=md_data_link(&dl_max);
				if (error) MD_LOG("data link failed step=%s errno=%d\n",md_stage,error);
				umi_bb_link(error?0:dl_max);
				md_stage="service";
			}
			if (umi_bb_poll(md_bb_send,md_bb_release)) md_active_at=md_now_us();
		} else if (!data_attempted && md_registered && md_wda_port && md_wds_port) {
			data_attempted=true;
			int error=md_data_start();
			if (error) { md_net_down();MD_LOG("data failed step=%s errno=%d\n",md_stage,error); }
			md_stage="service";
		}
		if (md_channels[3].rx_ready || md_channels[5].rx_ready || md_efs_pos<md_efs_size) {
			int error=md_ipcr_receive();
			if (error && error!=ETIMEDOUT) return error;
			quiet=md_now_us();continue;
		}
		if (!md_bridge && md_nas_port && md_now_us()>=nas) {
			bool registered;
			MD_TRY(md_nas_poll(&registered));
			nas=md_now_us()+30000000;
		}
		if (md_bridge) md_idle(md_now_us()-quiet<2000000 || md_now_us()-md_active_at<2000000?1:100);
		else delay_for_interval(md_data_open || md_now_us()-quiet<2000000?1:100,NSEC_PER_MSEC);
	}
	return 0;
}

/* One bring-up's state. A restart must not inherit ring pointers, service
 * ports, a Sahara stream position or a file reference from the link that
 * faulted: clear all of it before md_pcie() runs again. The DMA pool itself is
 * *not* reused (md_used is never wound back) because the voice mailbox maps
 * AP-owned pages into it; see umi_voice_audio_mhi.inc.
 * Called on the modem's service thread only. */
static void md_link_reset(void)
{
	md_bhi=0;md_bar_size=0;
	md_chdb=md_erdb=0;md_hw_events_ready=false;
	md_chctx=(struct md_buf){0};md_erctx=(struct md_buf){0};
	md_cmdctx=(struct md_buf){0};md_cmdring=(struct md_buf){0};
	memset(md_channels,0,sizeof(md_channels));
	for (unsigned e=0;e<MD_NER;e++) md_events[e]=(struct md_ring){0};
	md_cmdwp=0;md_command_wait=0;md_command_cc=-1;
	md_last_event_channel=md_last_event_code=md_last_event_bytes=0;md_ee_event=0;
	md_qrtr_hello=false;md_qrtr_peer=0;
	md_sahara_pos=md_sahara_size=0;
	md_nas_node=md_nas_port=md_uim_node=md_uim_port=0;
	md_dms_node=md_dms_port=md_wda_node=md_wda_port=0;
	md_wds_node=md_wds_port=0;
	md_registered=false;md_nas_last=UINT32_MAX;md_nas_start=0;
	md_qmi_reply_size=0;md_qmi_reply_node=md_qmi_reply_port=md_qmi_reply_client=0;
	md_tftp_published=false;md_tftp_reads=md_tftp_writes=md_tftp_missing=0;md_tftp_bytes=0;
	for (unsigned i=0;i<MD_TFTP_SLOTS;i++) md_tftp_close(&md_tftp[i]);
	md_efs_pos=md_efs_size=0;md_efs_regions=0;md_efs_bytes=0;
	md_data_open=false;
	md_active_at=0;
	/* Sahara images are resolved again by the next boot; drop the vnode holds
	 * and FAT chains so a restart cannot leak them. */
	for (unsigned i=0;i<md_image_count;i++) {
		struct md_image *image=&md_images[i];
		if (image->vnode) { vnode_put(image->vnode);image->vnode=NULLVP; }
		if (image->fat.chain) kfree_data(image->fat.chain,(size_t)image->fat.chain_count*sizeof(*image->fat.chain));
		image->source=MD_SOURCE_NONE;image->fat.chain=NULL;
	}
	MD_LOG("link state reset for restart (%u QRTR services of the faulted link dropped)\n",md_services);
	md_services=0;
}
