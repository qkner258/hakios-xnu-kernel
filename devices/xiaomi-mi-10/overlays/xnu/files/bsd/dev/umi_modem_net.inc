/* XNU raw IPv4 interface. The NCM driver's ifnet/mbuf pattern, with no
 * Ethernet header or ARP. No default route or resolver policy is installed. */
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/proc_internal.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include "umi_modem_qmi.h"

static ifnet_t md_ifp;
static bool md_net_ready;
static LCK_GRP_DECLARE(md_net_group,"umi-modem-net");
static LCK_MTX_DECLARE(md_net_lock,&md_net_group);
#define MD_NET_QUEUE 64u
static mbuf_t md_net_queue[MD_NET_QUEUE];
static unsigned md_net_head,md_net_count;
static uint64_t md_net_tx_drop;

static uint64_t md_net_drops(void) { return __atomic_load_n(&md_net_tx_drop,__ATOMIC_RELAXED); }
static errno_t md_net_output(ifnet_t ifp,mbuf_t packet)
{
	size_t n=mbuf_pkthdr_len(packet);int error=0;
	lck_mtx_lock(&md_net_lock);
	if (!__atomic_load_n(&md_net_ready,__ATOMIC_ACQUIRE) || !(ifnet_flags(ifp)&IFF_UP)) error=ENETDOWN;
	else if (n<20 || n>ifnet_mtu(ifp)) error=EMSGSIZE;
	else if (md_net_count==MD_NET_QUEUE) error=ENOBUFS;
	else { md_net_queue[(md_net_head+md_net_count)%MD_NET_QUEUE]=packet;md_net_count++; }
	lck_mtx_unlock(&md_net_lock);
	if (error) { __atomic_fetch_add(&md_net_tx_drop,1,__ATOMIC_RELAXED);mbuf_freem(packet); }
	return error;
}
static size_t md_net_take(uint8_t *data,size_t capacity)
{
	mbuf_t packet=NULL;
	lck_mtx_lock(&md_net_lock);
	if (md_net_count) {
		packet=md_net_queue[md_net_head];md_net_head=(md_net_head+1)%MD_NET_QUEUE;md_net_count--;
	}
	lck_mtx_unlock(&md_net_lock);
	if (!packet) return 0;
	size_t n=mbuf_pkthdr_len(packet);
	if (!__atomic_load_n(&md_net_ready,__ATOMIC_ACQUIRE) || n>capacity || mbuf_copydata(packet,0,n,data)) {
		__atomic_fetch_add(&md_net_tx_drop,1,__ATOMIC_RELAXED);n=0;
	} else ifnet_stat_increment_out(md_ifp,1,(uint32_t)n,0);
	mbuf_freem(packet);return n;
}
static errno_t md_net_demux(ifnet_t ifp,mbuf_t packet,char *header,protocol_family_t *family)
{
	(void)ifp;(void)header;uint8_t v;
	if (mbuf_copydata(packet,0,1,&v) || (v>>4)!=4) return EPROTONOSUPPORT;
	*family=PF_INET;return 0;
}
static errno_t md_net_add_proto(ifnet_t ifp,protocol_family_t family,const struct ifnet_demux_desc *desc,uint32_t count)
{
	(void)ifp;(void)desc;(void)count;return family==PF_INET?0:EPROTONOSUPPORT;
}
static errno_t md_net_del_proto(ifnet_t ifp,protocol_family_t family)
{
	(void)ifp;(void)family;return 0;
}
static errno_t md_net_proto_input(ifnet_t ifp,protocol_family_t family,mbuf_t packet,char *header)
{
	(void)ifp;(void)header;
	if (proto_input(family,packet)) mbuf_freem(packet);
	return 0;
}
static errno_t md_net_pre_output(ifnet_t ifp,protocol_family_t family,mbuf_t *packet,
    const struct sockaddr *dest,void *route,char *type,char *link_dest)
{
	(void)ifp;(void)packet;(void)dest;(void)route;(void)type;(void)link_dest;
	return family==PF_INET?0:EPROTONOSUPPORT;
}
static errno_t md_net_ioctl(ifnet_t ifp,u_long command,void *data)
{
	switch (command) {
	case SIOCSIFADDR:case SIOCAIFADDR:case SIOCSIFDSTADDR:case SIOCSIFFLAGS:return 0;
	case SIOCSIFMTU: {
		int mtu=((struct ifreq *)data)->ifr_mtu;
		return mtu<576 || mtu>32764?EINVAL:ifnet_set_mtu(ifp,(uint32_t)mtu);
	}
	default:return EOPNOTSUPP;
	}
}
static int md_net_input(const uint8_t *data,size_t n)
{
	if (!__atomic_load_n(&md_net_ready,__ATOMIC_ACQUIRE)) return ENETDOWN;
	mbuf_t packet;unsigned chunks=1;
	int error=mbuf_allocpacket(MBUF_DONTWAIT,n,&chunks,&packet);
	if (error) return error;
	if ((error=mbuf_copyback(packet,0,n,data,MBUF_DONTWAIT))) { mbuf_freem(packet);return error; }
	mbuf_pkthdr_setrcvif(packet,md_ifp);
	mbuf_pkthdr_setheader(packet,NULL);
	struct ifnet_stat_increment_param stats={.packets_in=1,.bytes_in=(uint32_t)n};
	return ifnet_input(md_ifp,packet,&stats);
}
static void md_net_down(void)
{
	__atomic_store_n(&md_net_ready,false,__ATOMIC_RELEASE);
	if (md_ifp) ifnet_set_flags(md_ifp,0,IFF_UP|IFF_RUNNING);
}
static int md_net_attach(const struct umi3_state *s)
{
	struct ifnet_init_eparams init={
		.ver=IFNET_INIT_CURRENT_VERSION,.len=sizeof(init),
		.flags=IFNET_INIT_LEGACY|IFNET_INIT_NX_NOAUTO,
		.uniqueid="umi-sdx55-ip",.uniqueid_len=12,.name="pdp_ip",.unit=0,
		.family=IFNET_FAMILY_CELLULAR,.type=IFT_CELLULAR,
		.output=md_net_output,.demux=md_net_demux,.add_proto=md_net_add_proto,
		.del_proto=md_net_del_proto,.ioctl=md_net_ioctl,
	};
	MD_TRY(ifnet_allocate_extended(&init,&md_ifp));
	ifnet_set_mtu(md_ifp,s->mtu);
	ifnet_set_flags(md_ifp,IFF_POINTOPOINT|IFF_UP|IFF_RUNNING,0xffff);
	ifnet_set_hdrlen(md_ifp,0);ifnet_set_offload(md_ifp,0);
	MD_TRY(ifnet_attach(md_ifp,NULL));
	struct ifnet_attach_proto_param proto={.input=md_net_proto_input,.pre_output=md_net_pre_output};
	MD_TRY(ifnet_attach_protocol(md_ifp,PF_INET,&proto));
	struct in_aliasreq address={0};
	strlcpy(address.ifra_name,"pdp_ip0",sizeof(address.ifra_name));
	address.ifra_addr.sin_len=address.ifra_dstaddr.sin_len=address.ifra_mask.sin_len=sizeof(struct sockaddr_in);
	address.ifra_addr.sin_family=address.ifra_dstaddr.sin_family=address.ifra_mask.sin_family=AF_INET;
	address.ifra_addr.sin_addr.s_addr=htonl(s->address);
	address.ifra_dstaddr.sin_addr.s_addr=htonl(s->gateway);
	address.ifra_mask.sin_addr.s_addr=htonl(s->mask);
	MD_TRY(in_control(NULL,SIOCAIFADDR,(caddr_t)&address,md_ifp,kernproc));
	__atomic_store_n(&md_net_ready,true,__ATOMIC_RELEASE);
	return 0;
}
