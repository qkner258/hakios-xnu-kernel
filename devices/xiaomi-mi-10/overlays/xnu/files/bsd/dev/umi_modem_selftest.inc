/* One boot-time probe, owned by the MHI polling thread. No sockets/routes. */
#define MD_SELFTEST_ID 0x554du
#define MD_SELFTEST_DNS_ID 0x554eu
#define MD_SELFTEST_PORT 49155u
static struct {
	bool started;
	uint32_t address,dns;
	unsigned next;
	uint64_t next_at,sent[4];
	uint8_t status[4]; /* 0 unsent, 1 waiting, 2 reply, 3 timeout */
} md_selftest;

static uint16_t md_be16(const uint8_t *p) { return (uint16_t)p[0]<<8|p[1]; }
static uint32_t md_be32(const uint8_t *p) { return (uint32_t)md_be16(p)<<16|md_be16(p+2); }
static void md_put_be16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }
static void md_put_be32(uint8_t *p,uint32_t v) { md_put_be16(p,(uint16_t)(v>>16));md_put_be16(p+2,(uint16_t)v); }
static uint16_t md_checksum(const uint8_t *p,size_t n,uint32_t sum)
{
	while (n>=2) { sum+=md_be16(p);p+=2;n-=2; }
	if (n) sum+=(uint16_t)*p<<8;
	while (sum>>16) sum=(sum&0xffff)+(sum>>16);
	return (uint16_t)~sum;
}
static uint32_t md_udp_sum(const uint8_t *ip,size_t n)
{
	return md_be16(ip+12)+md_be16(ip+14)+md_be16(ip+16)+md_be16(ip+18)+17+(uint32_t)n;
}

static void md_selftest_start(const struct umi3_state *s)
{
	if (md_selftest.started) return;
	md_selftest.started=true;md_selftest.address=s->address;md_selftest.dns=s->dns1;
	md_selftest.next_at=md_now_us();
}
static size_t md_selftest_packet(uint8_t *ip)
{
	unsigned i=md_selftest.next;
	if (!md_selftest.started || i==4 || md_now_us()<md_selftest.next_at) return 0;
	if (i==3 && !md_selftest.dns) {
		MD_LOG("data selftest dns no reply (no WDS DNS)\n");
		md_selftest.status[3]=3;md_selftest.next=4;return 0;
	}
	/* DNS header + www.baidu.com IN A question, with recursion desired. */
	static const uint8_t question[]={3,'w','w','w',5,'b','a','i','d','u',3,'c','o','m',0,0,1,0,1};
	size_t n=i<3?36:20+8+12+sizeof(question);
	memset(ip,0,n);ip[0]=0x45;md_put_be16(ip+2,(uint16_t)n);
	md_put_be16(ip+4,(uint16_t)(MD_SELFTEST_ID+i));ip[6]=0x40;ip[8]=64;ip[9]=i<3?1:17;
	md_put_be32(ip+12,md_selftest.address);md_put_be32(ip+16,i<3?0x01010101:md_selftest.dns);
	md_put_be16(ip+10,md_checksum(ip,20,0));
	uint8_t *p=ip+20;
	if (i<3) {
		p[0]=8;md_put_be16(p+4,MD_SELFTEST_ID);md_put_be16(p+6,(uint16_t)(i+1));
		memcpy(p+8,"UMI-test",8);md_put_be16(p+2,md_checksum(p,n-20,0));
	} else {
		md_put_be16(p,MD_SELFTEST_PORT);md_put_be16(p+2,53);md_put_be16(p+4,(uint16_t)(n-20));
		md_put_be16(p+8,MD_SELFTEST_DNS_ID);p[10]=1;p[13]=1;memcpy(p+20,question,sizeof(question));
		uint16_t sum=md_checksum(p,n-20,md_udp_sum(ip,n-20));
		md_put_be16(p+6,sum?sum:0xffff);
	}
	return n;
}
static void md_selftest_sent(void)
{
	unsigned i=md_selftest.next++;
	md_selftest.status[i]=1;md_selftest.sent[i]=md_now_us();
	md_selftest.next_at=md_selftest.sent[i]+(i<2?1000000:0);
}
static void md_selftest_timeouts(void)
{
	for (unsigned i=0;i<4;i++) {
		if (md_selftest.status[i]!=1 || md_now_us()-md_selftest.sent[i]<5000000) continue;
		md_selftest.status[i]=3;
		if (i<3) MD_LOG("data selftest icmp 1.1.1.1 seq=%u no reply\n",i+1);
		else MD_LOG("data selftest dns no reply\n");
	}
}
/* Skip a wire name, including a compressed suffix, without following pointers. */
static bool md_dns_skip_name(const uint8_t *dns,size_t n,size_t *at)
{
	while (*at<n) {
		unsigned len=dns[(*at)++];
		if (!len) return true;
		if ((len&0xc0)==0xc0) {
			if (*at>=n || (((len&63)<<8)|dns[*at])>=n) return false;
			(*at)++;return true;
		}
		if (len>63 || len>n-*at) return false;
		*at+=len;
	}
	return false;
}
static bool md_dns_answers(const uint8_t *dns,size_t n,uint32_t *first)
{
	size_t at=12;*first=0;
	for (unsigned q=0;q<md_be16(dns+4);q++) {
		if (!md_dns_skip_name(dns,n,&at) || n-at<4) return false;
		at+=4;
	}
	for (unsigned a=0;a<md_be16(dns+6);a++) {
		if (!md_dns_skip_name(dns,n,&at) || n-at<10) return false;
		size_t len=md_be16(dns+at+8);
		if (len>n-at-10) return false;
		if (!*first && md_be16(dns+at)==1 && md_be16(dns+at+2)==1 && len==4) *first=md_be32(dns+at+10);
		at+=10+len;
	}
	return true;
}
/* Called after data_poll validates the IPv4 header/total lengths. */
static bool md_selftest_input(const uint8_t *ip,size_t n)
{
	if (!md_selftest.started || md_be32(ip+16)!=md_selftest.address || (md_be16(ip+6)&0x3fff)) return false;
	size_t ihl=(ip[0]&15)*4,len=n-ihl;const uint8_t *p=ip+ihl;
	if (md_checksum(ip,ihl,0)) return false;
	unsigned i;
	uint32_t first=0;
	if (ip[9]==1) {
		if (md_be32(ip+12)!=0x01010101 || len<8 || p[0] || p[1] ||
		    md_be16(p+4)!=MD_SELFTEST_ID || md_be16(p+6)<1 || md_be16(p+6)>3 || md_checksum(p,len,0)) return false;
		i=md_be16(p+6)-1;
	} else if (ip[9]==17) {
		if (md_be32(ip+12)!=md_selftest.dns || len<20 || md_be16(p)!=53 || md_be16(p+2)!=MD_SELFTEST_PORT ||
		    md_be16(p+4)!=len || (md_be16(p+6) && md_checksum(p,len,md_udp_sum(ip,len))) ||
		    md_be16(p+8)!=MD_SELFTEST_DNS_ID || (p[10]&0xf8)!=0x80 || !md_dns_answers(p+8,len-8,&first)) return false;
		i=3;
	} else return false;
	if (!md_selftest.status[i]) return false;
	/* Consume duplicate/late replies too, but report each probe only once. */
	if (md_selftest.status[i]==1) {
		md_selftest.status[i]=2;
		unsigned long long rtt=(md_now_us()-md_selftest.sent[i])/1000;
		if (i<3) MD_LOG("data selftest icmp 1.1.1.1 seq=%u reply rtt_ms=%llu\n",i+1,rtt);
		else MD_LOG("data selftest dns reply answers=%u first=%u.%u.%u.%u rtt_ms=%llu\n",
		    md_be16(p+14),(unsigned)(first>>24),(unsigned)((first>>16)&255),
		    (unsigned)((first>>8)&255),(unsigned)(first&255),rtt);
	}
	return true;
}
