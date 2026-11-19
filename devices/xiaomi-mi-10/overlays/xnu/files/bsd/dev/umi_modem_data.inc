/* Single IPv4 PDN: QRTR WDA/WDS -> QMAP mux 1 -> MHI IP_HW0. All MHI
 * rings belong to md_service; ifnet output only queues packets for it. */
static bool md_data_open;
static uint64_t md_data_tx,md_data_rx,md_data_drop,md_data_log_at;
#include "umi_modem_selftest.inc"

static int md_data_poll(void)
{
	if (!md_data_open) return 0;
	struct md_channel *tx=&md_channels[6],*rx=&md_channels[7];
	if (tx->tx_done) { tx->tx_done=false;md_data_tx++; }
	md_selftest_timeouts();
	if (rx->rx_ready) {
		size_t at=0;
		while (at<rx->rx_length) {
			const uint8_t *ip;size_t n,start=at;uint8_t mux;
			if (umi3_qmap_next(rx->buffer.p,rx->rx_length,&at,&mux,&ip,&n)) { md_data_drop++;break; }
			/* Bridge: every mux and IP version goes to its pdp_ip (umi_bb_data_input checks both). */
			if (md_bridge && !(rx->buffer.p[start]&0xc0)) {
				if (umi_bb_data_input(mux,ip,(uint32_t)n)) md_data_rx++;else md_data_drop++;
				md_active_at=md_now_us();continue;
			}
			/* QMAP command bit and next-header bit are not IP packets. v1
			 * has no checksum header; do not feed those bytes to IPv4. */
			if ((rx->buffer.p[start]&0xc0) || mux!=1 || n<20 || (ip[0]>>4)!=4 ||
			    (ip[0]&15)<5 || (size_t)(ip[0]&15)*4>n || ((size_t)ip[2]<<8|ip[3])!=n) {
				md_data_drop++;continue;
			}
			if (md_selftest_input(ip,n) || !md_net_input(ip,n)) md_data_rx++;else md_data_drop++;
		}
		rx->rx_ready=false;
		MD_TRY(md_post(rx,MD_PACKET));
	}
	if (!tx->tx_pending) {
		uint8_t mux=1;size_t n=0;bool selftest=false;
		if (md_bridge) n=umi_bb_data_take(&mux,tx->buffer.p+4,MD_PACKET-4);
		else {
			n=md_selftest_packet(tx->buffer.p+4);selftest=n!=0;
			if (!n) n=md_net_take(tx->buffer.p+4,MD_PACKET-4);
		}
		if (n) {
			if (md_bridge) md_active_at=md_now_us();
			tx->buffer.p[0]=0;tx->buffer.p[1]=mux;
			tx->buffer.p[2]=(uint8_t)(n>>8);tx->buffer.p[3]=(uint8_t)n;
			MD_TRY(md_post(tx,n+4));
			if (selftest) md_selftest_sent();
		}
	}
	if (md_now_us()>=md_data_log_at) {
		MD_LOG("data packets pdp_ip0 tx=%llu rx=%llu drop=%llu tx_drop=%llu\n",
		    (unsigned long long)md_data_tx,(unsigned long long)md_data_rx,
		    (unsigned long long)md_data_drop,(unsigned long long)md_net_drops());
		if (md_bridge) umi_bb_report((unsigned)(md_now_us()/1000000));
		md_data_log_at=md_now_us()+60000000;
	}
	return 0;
}

static int md_data_request(uint32_t client,uint32_t node,uint32_t port,const uint8_t *request,size_t n,unsigned ms)
{
	uint16_t result=0,code=0;
	MD_TRY(md_qmi_call(client,node,port,request,n,ms));
	if (umi3_result(md_qmi_reply,md_qmi_reply_size,&result,&code)) return EPROTO;
	MD_LOG("%s result=%u error=%u\n",md_stage,result,code);
	return result || code ? EIO : 0;
}

/* IP_HW0 as raw IP over basic QMAP (5): the link both modes use. *dl_max: the downlink aggregate limit. */
static int md_data_link(uint32_t *dl_max)
{
	struct umi3_state s={0};uint8_t request[80];size_t n,vn;const uint8_t *v;
	md_stage="IP_HW0";
	MD_TRY(md_enable_hw_events());
	MD_TRY(md_start_channel(6,100,4));MD_TRY(md_start_channel(7,101,5));
	md_data_open=true;md_data_log_at=md_now_us()+60000000;
	md_stage="WDA set data format raw-ip qmap=1 ep=pcie/4";
	if (umi3_wda_format(request,sizeof(request),&s.transaction,&n)) return EINVAL;
	MD_TRY(md_data_request(MD_WDA_CLIENT,md_wda_node,md_wda_port,request,n,10000));
	/* WDA can succeed while selecting a different format. Accept only the
	 * framing we implement, never silently interpret raw IP/QMAPv5 as v1. */
	for (unsigned type=0x11;type<=0x13;type++) {
		if (umi3_find_tlv(md_qmi_reply,md_qmi_reply_size,(uint8_t)type,&v,&vn) || vn!=4) return EPROTO;
		if (umi3_le32(v)!=(type==0x11?2u:5u)) {
			MD_LOG("WDA unsupported format tlv=%#x value=%u\n",type,umi3_le32(v));
			return EPROTONOSUPPORT;
		}
	}
	if (umi3_find_tlv(md_qmi_reply,md_qmi_reply_size,0x16,&v,&vn) || vn!=4 ||
	    !umi3_le32(v) || umi3_le32(v)>MD_PACKET) return EMSGSIZE;
	*dl_max=umi3_le32(v);
	return 0;
}

/* umi_modem=1: the kernel's own single IPv4 PDN on mux 1 (cmnet) to pdp_ip0. */
static int md_data_start(void)
{
	struct umi3_state s={0};uint8_t request[80];size_t n;uint32_t dl_max;
	MD_TRY(md_data_link(&dl_max));
	md_stage="WDS bind ep=pcie/4 mux=1";
	if (umi3_wds_bind(request,sizeof(request),&s.transaction,&n)) return EINVAL;
	MD_TRY(md_data_request(MD_WDS_CLIENT,md_wds_node,md_wds_port,request,n,10000));
	md_stage="WDS set IP family ipv4";
	if (umi3_wds_family(request,sizeof(request),&s.transaction,&n)) return EINVAL;
	MD_TRY(md_data_request(MD_WDS_CLIENT,md_wds_node,md_wds_port,request,n,10000));
	md_stage="WDS start network apn=cmnet";
	if (umi3_wds_start(request,sizeof(request),&s.transaction,"cmnet",&n)) return EINVAL;
	MD_TRY(md_data_request(MD_WDS_CLIENT,md_wds_node,md_wds_port,request,n,120000));
	if (umi3_apply_wds(&s,md_qmi_reply,md_qmi_reply_size) || !s.wds_up) return EPROTO;
	MD_LOG("WDS start network apn=cmnet result=0 handle=%#x\n",s.wds_handle);
	md_stage="WDS get runtime settings";
	if (umi3_wds_settings(request,sizeof(request),&s.transaction,&n)) return EINVAL;
	MD_TRY(md_data_request(MD_WDS_CLIENT,md_wds_node,md_wds_port,request,n,10000));
	if (umi3_apply_wds(&s,md_qmi_reply,md_qmi_reply_size) || !s.address || !s.gateway ||
	    !s.mask || s.mtu<576 || s.mtu>MD_PACKET-4) return EPROTO;
	uint32_t inverse=~s.mask;
	if (inverse&(inverse+1)) return EPROTO;
	unsigned prefix=0;for (uint32_t mask=s.mask;mask;mask<<=1) prefix++;
	md_stage="pdp_ip0 attach";
	MD_TRY(md_net_attach(&s));
#define MD_IP(a) (unsigned)((a)>>24),(unsigned)(((a)>>16)&255),(unsigned)(((a)>>8)&255),(unsigned)((a)&255)
	MD_LOG("data up pdp_ip0 ipv4=%u.%u.%u.%u/%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u,%u.%u.%u.%u mtu=%u default_route=manual\n",
	    MD_IP(s.address),prefix,MD_IP(s.gateway),MD_IP(s.dns1),MD_IP(s.dns2),s.mtu);
#undef MD_IP
	md_selftest_start(&s);
	return 0;
}
