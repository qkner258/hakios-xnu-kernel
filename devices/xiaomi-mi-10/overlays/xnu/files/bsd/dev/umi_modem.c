/* UMI external SDX55 bring-up. New implementation using register/protocol
 * facts from the owner's Linux 7.3 umi tree. No MPSS, IPA or flash writes.
 * Default off; supervisor owns all hardware testing.
 *
 * Recovery (v30): a link fault no longer ends here for good. The service
 * thread holds PERST, tells the bridge the X55 is gone, and re-runs the same
 * bring-up the boot uses: PERST cycle, PCIe retrain, MHI/Sahara/AMSS, QRTR
 * discovery. Bounded to MD_RESTARTS_MAX per boot and >= MD_RESTART_GAP_MS
 * apart; umi_mdm_restart=0 (kern.hakios.umi_mdm_restart) turns it off. No NV,
 * EFS, modemst, fsg or calibration partition is written by any of this.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <sys/kauth.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <sys/sysctl.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern uint64_t kvtophys(vm_offset_t);
extern kern_return_t thread_terminate(thread_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern int umi_rpmh_modem_power(void);
extern lck_mtx_t hakios_smmu_lock;
extern lck_mtx_t hakios_pcie_refgen_lock;
#define MD_REFGEN_LOCK() lck_mtx_lock(&hakios_pcie_refgen_lock)
#define MD_REFGEN_UNLOCK() lck_mtx_unlock(&hakios_pcie_refgen_lock)
void umi_modem_init(void);
void umi_modem_quiesce(void);
#define MD_LOG(...) hakios_tl("UMI MODEM: " __VA_ARGS__)
#define MD_BIT(n) (1u << (n))
#define MD_TRY(x) do { int md_err = (x); if (md_err) return md_err; } while (0)
#define MD_GCC 0x00100000u
#define MD_GPIO 0x0f500000u
#define MD_PARF 0x01c10000u
#define MD_PHY 0x01c16000u
#define MD_DBI 0x64000000u
#define MD_ATU 0x64001000u
#define MD_CONF 0x64100000u
#define MD_BAR 0x64300000u
#define MD_SMMU 0x15000000u
#define MD_IOVA 0xb0000000ull
#define MD_DMA_SIZE (16u * 1024u * 1024u)
static const char *md_stage;
static bool md_link, md_gpio_owned, md_dma_on, md_live;
static uint32_t md_bar_size, md_bhi;
static uint64_t md_deadline;
/* Fault recovery state. md_fault_reason (declared in umi_modem_mhi.inc) names
 * what md_fault() saw, for the one restart line; md_stopped keeps the
 * loader-return stop idempotent. */
static bool md_stopped, md_thread_alive;
static int md_restart_enabled = 0;   /* v29w: off by default -- the v29v restart hung the phone (16:06:48) */
static unsigned md_restart_count, md_restart_requests;
#define MD_RESTARTS_MAX 3u
#define MD_RESTART_GAP_MS 5000u
#define MD_RESTART_GAP_MAX_MS 30000u
SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, umi_mdm_restart, CTLFLAG_RW | CTLFLAG_LOCKED, &md_restart_enabled, 0,
    "UMI external X55: full link restart after an MHI/SMMU fault (1=on, 0=off)");
static uint64_t md_now_us(void)
{
	uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
}
static struct { uint32_t pa, size; vm_offset_t va; } md_regions[] = {
	{MD_GCC,0xa0000,0}, {MD_GPIO,0x100000,0},
	{MD_PARF,0x3000,0}, {MD_PHY,0x1000,0},
	{MD_DBI,0x1000,0}, {MD_ATU,0x1000,0},
	{MD_CONF,0x1000,0}, {MD_BAR,0x100000,0},
	{MD_SMMU,0x100000,0},
};
static vm_offset_t md_reg(uint32_t pa)
{
	for (unsigned i=0; i<sizeof(md_regions)/sizeof(md_regions[0]); i++) {
		uint32_t off=pa-md_regions[i].pa;
		if (!(pa&3) && off <= md_regions[i].size-4) return md_regions[i].va+off;
	}
	return 0;
}
static uint32_t md_read(uint32_t pa)
{
	vm_offset_t va=md_reg(pa);
	if (!va) return UINT32_MAX;
	uint32_t v=*(volatile uint32_t *)va;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}
static void md_write(uint32_t pa,uint32_t v)
{
	vm_offset_t va=md_reg(pa);
	if (!va) return;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)va=v;
	__asm__ volatile("dsb osh" ::: "memory");
}
static int md_update(uint32_t pa,uint32_t clear,uint32_t set)
{
	uint32_t v=md_read(pa);
	if (v==UINT32_MAX) return EIO;
	md_write(pa,(v&~clear)|set);
	return 0;
}
static int md_wait(uint32_t pa,uint32_t mask,uint32_t wanted,unsigned us)
{
	for (unsigned n=0;n<us;n+=100) {
		uint32_t v=md_read(pa);
		if (v==UINT32_MAX) return EIO;
		if ((v&mask)==wanted) return 0;
		delay(100);
	}
	MD_LOG("timeout stage=%s address=%#x value=%#x\n",md_stage,pa,md_read(pa));
	return ETIMEDOUT;
}
static int md_output(unsigned pin,bool high,unsigned cfg)
{
	MD_TRY(md_update(MD_GPIO+pin*0x1000+4,2,high?2:0));
	return md_update(MD_GPIO+pin*0x1000,0x3ff,cfg|MD_BIT(9));
}
static void md_address(uint32_t pa,uint64_t value)
{
	md_write(pa+4,(uint32_t)(value>>32)); md_write(pa,(uint32_t)value);
}
/* The service thread's pause: msleep, so umi_modem_wake (bridge writes, pdp_ip output) cuts it short. */
static int md_wake_channel;
static void md_idle(unsigned ms)
{
	struct timespec pause={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*NSEC_PER_MSEC};
	(void)msleep(&md_wake_channel,NULL,PZERO,"umi-modem",&pause);
}
void umi_modem_wake(void)
{
	wakeup(&md_wake_channel);
}
#include "umi_modem_pcie.inc"
#include "umi_modem_dma.inc"
#include "umi_modem_net.inc"
#include "umi_modem_mhi.inc"

static void md_stop(bool faulted);
void umi_modem_quiesce(void)
{
	/* CPUs are stopped by loader_return. Stop the live MHI endpoint before
	 * PERST so no outstanding DMA can reach memory reused by the loader. */
	__atomic_store_n(&md_service_stop,true,__ATOMIC_RELEASE);
	__atomic_store_n(&md_net_ready,false,__ATOMIC_RELEASE);
	if (!md_gpio_owned) return;
	md_stop(false);
}
/* Clock/domain gating for a link that is down. GCC and GPIO are on the
 * always-on interconnect; nothing inside the (possibly frozen) PCIe block is
 * read or written here. md_pcie() re-enables every vote at the next bring-up. */
static void md_gate(void)
{
	(void)md_update(MD_GCC+0x52010,MD_BIT(15)|0x7c00u,0);
	(void)md_update(MD_GCC+0x8c00c,1,0);
	/* v29w: 0x8d058 bit0 is Wi-Fi's own PCIe0 clock branch -- umi_pcie.c:102 is the branch()
	 * that turns it on, and clearing it here (even on a clean stop) is what left ath11k's ring
	 * pointer write hanging on the SoC. Do not gate it from the modem. 0x52010 bits 10-15 and
	 * 0x8c00c bit0 stay as they are for now: work/v29i/INVESTIGATION-wifi-srng-hang.md §4.2
	 * leaves open whether they also vote for PCIe0 (no gcc-sm8250.c/gcc-kona.c here to check). */
}
static void md_stop(bool faulted)
{
	if (!md_gpio_owned) { MD_LOG("stopped before board ownership\n");return; }
	if (__atomic_exchange_n(&md_stopped,true,__ATOMIC_ACQ_REL)) {
		(void)md_output(85,false,1);
		return;
	}
	/* DMA allocations and SMMU ownership survive until reset. Never hand a
	 * possibly outstanding device transaction memory that has been freed. */
	if (md_link) {
		if (faulted) {
			/* The fault report read MHI state and BHI EE back as all ones: the
			 * endpoint is off the bus, and config space, DBI and the BARs sit
			 * behind that frozen link, where a single read can stall this
			 * thread for good. PERST and the clock votes are enough. */
			MD_LOG("link faulted: PCIe config/DBI untouched, PERST only, clocks left on\n");
		} else {
			uint32_t command=md_read(MD_CONF+4);
			if (command!=UINT32_MAX) md_write(MD_CONF+4,(command&0xffff)&~4u);
			md_write(MD_DBI+4,2);
		}
	}
	if (md_gpio_owned) (void)md_output(85,false,1);
	/* v29x: never gate on a fault. With the endpoint gone an access through
	 * a still-clocked root port completes as all ones; with its clocks gated
	 * a straggling doorbell/BAR access from another thread never completes
	 * and the whole SoC stalls (v29v 16:06:48 and v29w 19:27, both within a
	 * second of this point). md_pcie() re-votes on a restart anyway. */
	if (!faulted) md_gate();
	delay(1000);
	md_link=false;
	__atomic_store_n(&md_live,false,__ATOMIC_RELEASE);
	MD_LOG("quiesced PERST asserted; shared votes and DMA retained%s\n",
	    faulted?" (faulted link)":"");
}
/* 0 = no transport thread, 1 = live link, 2 = thread alive without a link. */
int umi_modem_state(void)
{
	if (!__atomic_load_n(&md_thread_alive,__ATOMIC_ACQUIRE)) return 0;
	return __atomic_load_n(&md_live,__ATOMIC_ACQUIRE)?1:2;
}
unsigned umi_modem_restarts(void)
{
	return __atomic_load_n(&md_restart_count,__ATOMIC_ACQUIRE);
}
/* CommCenter's ABM reset asks for a real bring-up when the link is dead: the
 * service thread is woken and its backoff cut short. Refused when the kill
 * switch is off, the per-boot cap is used up, the link is live (nothing to
 * do) or there is no thread left to do it. */
int umi_modem_restart(void)
{
	if (__atomic_load_n(&md_live,__ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&md_thread_alive,__ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&md_service_stop,__ATOMIC_ACQUIRE)) return 0;
	if (!__atomic_load_n(&md_restart_enabled,__ATOMIC_RELAXED) ||
	    __atomic_load_n(&md_restart_count,__ATOMIC_ACQUIRE)>=MD_RESTARTS_MAX) return 0;
	if (__atomic_exchange_n(&md_restart_requests,1,__ATOMIC_ACQ_REL)) return 1;
	MD_LOG("restart requested by the bridge\n");
	umi_modem_wake();
	return 1;
}
static bool md_restart_allowed(void)
{
	return __atomic_load_n(&md_restart_enabled,__ATOMIC_RELAXED) &&
	    __atomic_load_n(&md_restart_count,__ATOMIC_ACQUIRE)<MD_RESTARTS_MAX;
}
static void md_restart_log(unsigned n,const char *result)
{
	MD_LOG("umi_modem: restart %u/%u after fault (%s): %s\n",n,MD_RESTARTS_MAX,
	    md_fault_reason?md_fault_reason:"unknown",result);
}
/* Backoff between attempts, in 100 ms sleeps: quiesce (loader return) and a
 * bridge request cut it short, but never inside MD_RESTART_GAP_MS of the fault. */
static void md_restart_pause(unsigned n)
{
	unsigned ms=MD_RESTART_GAP_MS<<(n-1);
	if (ms>MD_RESTART_GAP_MAX_MS) ms=MD_RESTART_GAP_MAX_MS;
	uint64_t start=md_now_us(),until=start+(uint64_t)ms*1000;
	while (!__atomic_load_n(&md_service_stop,__ATOMIC_ACQUIRE) && md_now_us()<until) {
		if (__atomic_load_n(&md_restart_requests,__ATOMIC_ACQUIRE) &&
		    md_now_us()-start>=(uint64_t)MD_RESTART_GAP_MS*1000) break;
		md_idle(100);
	}
	__atomic_store_n(&md_restart_requests,0,__ATOMIC_RELEASE);
}
/* PCIe/BHI/EE checks, then the firmware the boot path loads. probe stops after
 * the EE read; a restart reuses every step unchanged. */
static int md_bringup(bool probe)
{
	md_stage="power";
	MD_TRY(md_pcie());
	md_stage="BHI-EE";
	md_bhi=md_read(MD_BAR+0x28);
	if ((md_bhi&3) || md_bhi<0xa8 || md_bhi>md_bar_size-0x34) return EPROTO;
	if (md_read(MD_BAR+md_bhi+4)!=1) return EPROTO;
	uint32_t ee=md_read(MD_BAR+md_bhi+0x28);
	MD_LOG("EE=%u (0=PBL 1=SBL 2=AMSS 3=RDDM 4=WFW 6=EDL)\n",ee);
	if (ee>7) return EIO;
	if (probe) { MD_LOG("PASS probe PCI=17cb:0306 EE=%u DMA=off\n",ee); return 0; }
	/* Existing firmware-owned rings cannot be adopted safely. */
	if (ee!=0) {
		MD_LOG("EE=%u is not PBL: its rings are not ours; cold PBL required\n",ee);
		return EBUSY;
	}
	int error=md_mhi_boot();
	if (error) return error;
	MD_LOG("PASS stage3 AMSS IPCR=20/21 ER=0 QRTR control channel open\n");
	__atomic_store_n(&md_live,true,__ATOMIC_RELEASE);
	return 0;
}
static void md_thread(void *arg,wait_result_t result)
{
	(void)result;
	bool probe=(uintptr_t)arg==1;
	int error=0;
	bool reported=false;
	__atomic_store_n(&md_thread_alive,true,__ATOMIC_RELEASE);
	md_stage="map";
	for (unsigned i=0;i<sizeof(md_regions)/sizeof(md_regions[0]);i++) {
		md_regions[i].va=ml_io_map(md_regions[i].pa,md_regions[i].size);
		if (!md_regions[i].va) { error=ENOMEM; goto done; }
	}
	/* umi_modem=2: CommCenter must find its baseband services whatever the modem does. */
	if (md_bridge) umi_bb_publish();
	for (unsigned round=0;;round++) {
		if (round) md_link_reset();
		/* Stage 4 (SIM ready, DMS online, NAS registration) runs to about 300 s. */
		md_deadline=md_now_us()+360000000ull;
		__atomic_store_n(&md_stopped,false,__ATOMIC_RELEASE);
		error=md_bringup(probe);
		if (round) {
			char result[64];
			if (!error) snprintf(result,sizeof(result),"ok, link up");
			else snprintf(result,sizeof(result),"failed errno=%d stage=%s",error,md_stage?md_stage:"?");
			md_restart_log(round,result);
		}
		if (!error) md_fault_reason=NULL;
		if (!error && !probe) {
			/* Serviced until quiesce, which then stops the endpoint itself. */
			error=md_service();
			md_net_down();
			__atomic_store_n(&md_live,false,__ATOMIC_RELEASE);
		}
		if (error) { MD_LOG("FAIL stage=%s errno=%d\n",md_stage,error); reported=true; }
		if (probe || !error || __atomic_load_n(&md_service_stop,__ATOMIC_ACQUIRE)) break;
		/* The link is gone. Put it in a safe state, tell the bridge, restart. */
		md_stop(true);
		if (md_bridge) umi_bb_modem_lost();
		if (!md_restart_allowed()) {
			MD_LOG("no restart after fault (%s): %s\n",md_fault_reason?md_fault_reason:"unknown",
			    __atomic_load_n(&md_restart_enabled,__ATOMIC_RELAXED)?"per-boot limit reached":
			    "umi_mdm_restart=0");
			break;
		}
		unsigned n=__atomic_add_fetch(&md_restart_count,1,__ATOMIC_ACQ_REL);
		md_restart_log(n,"begin: PERST cycle, PCIe retrain, MHI/Sahara, QRTR discovery");
		md_restart_pause(n);
	}
done:
	if (error && !reported) MD_LOG("FAIL stage=%s errno=%d\n",md_stage,error);
	md_stop(!!error);
	__atomic_store_n(&md_thread_alive,false,__ATOMIC_RELEASE);
	thread_terminate(current_thread());
}
void umi_modem_init(void)
{
	uint32_t on=0,probe=0;
	PE_parse_boot_argn("umi_modem",&on,sizeof(on));
	PE_parse_boot_argn("umi_modem_probe",&probe,sizeof(probe));
	if (on!=1 && on!=2 && probe!=1) return;
	if (on && probe==1) {
		MD_LOG("FAIL stage=arguments errno=%d; choose probe or stage1\n",EBUSY);
		return;
	}
	thread_t thread;
	md_bridge=on==2;
	MD_LOG("start external-X55 mode=%s; integrated MPSS disabled\n",probe?"probe":md_bridge?"bridge (CommCenter dials)":"stage1");
	if (kernel_thread_start(md_thread,(void *)(uintptr_t)(probe?1:0),&thread)!=KERN_SUCCESS) {
		MD_LOG("FAIL stage=thread errno=%d\n",ENOMEM); return;
	}
	thread_deallocate(thread);
}
