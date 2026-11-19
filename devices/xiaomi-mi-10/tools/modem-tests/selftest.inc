/* Fake IP_HW0 peer: exercise the production data poller and packet codec. */
static uint16_t probe_checksum(const uint8_t *p,size_t n)
{
    uint32_t sum=0;
    for (size_t i=0;i<n;i++) sum+=(uint32_t)p[i]<<(i%2?0:8);
    return (uint16_t)~((sum&65535)+(sum>>16)+(((sum&65535)+(sum>>16))>>16));
}
static uint16_t probe_udp_checksum(const uint8_t *ip,size_t n)
{
    uint8_t pseudo[512]={0};size_t h=(ip[0]&15)*4,len=n-h;
    memcpy(pseudo,ip+12,8);pseudo[9]=17;pseudo[10]=(uint8_t)(len>>8);pseudo[11]=(uint8_t)len;
    memcpy(pseudo+12,ip+h,len);return probe_checksum(pseudo,12+len);
}
static void probe_fix(uint8_t *ip,size_t n)
{
    size_t h=(ip[0]&15)*4;ip[2]=(uint8_t)(n>>8);ip[3]=(uint8_t)n;ip[10]=ip[11]=0;
    uint16_t sum=probe_checksum(ip,h);ip[10]=(uint8_t)(sum>>8);ip[11]=(uint8_t)sum;
    uint8_t *p=ip+h;
    if (ip[9]==1) {p[2]=p[3]=0;sum=probe_checksum(p,n-h);p[2]=(uint8_t)(sum>>8);p[3]=(uint8_t)sum;}
    else {
        p[4]=(uint8_t)((n-h)>>8);p[5]=(uint8_t)(n-h);p[6]=p[7]=0;
        sum=probe_udp_checksum(ip,n);if (!sum) sum=0xffff;p[6]=(uint8_t)(sum>>8);p[7]=(uint8_t)sum;
    }
}
static size_t probe_request(unsigned seq,uint8_t *copy)
{
    struct md_channel *tx=&md_channels[6];const uint8_t *q=tx->buffer.p,*ip=q+4;
    size_t n=seq?36:59;
    assert(tx->tx_pending&&q[0]==0&&q[1]==1&&q[2]==0&&q[3]==n);
    assert(ip[0]==0x45&&ip[2]==0&&ip[3]==n&&probe_checksum(ip,20)==0);
    assert(!memcmp(ip+12,"\x0a\x01\x02\x03",4));
    if (seq) {
        assert(ip[9]==1&&!memcmp(ip+16,"\x01\x01\x01\x01",4));
        assert(ip[20]==8&&ip[21]==0&&ip[24]==0x55&&ip[25]==0x4d&&ip[26]==0&&ip[27]==seq);
        assert(probe_checksum(ip+20,n-20)==0&&!memcmp(ip+28,"UMI-test",8));
    } else {
        static const uint8_t query[]={0x55,0x4e,1,0,0,1,0,0,0,0,0,0,
            3,'w','w','w',5,'b','a','i','d','u',3,'c','o','m',0,0,1,0,1};
        assert(ip[9]==17&&!memcmp(ip+16,"\x08\x08\x08\x08",4));
        assert(ip[20]==0xc0&&ip[21]==3&&ip[22]==0&&ip[23]==53&&ip[24]==0&&ip[25]==39);
        assert((ip[26]||ip[27])&&probe_udp_checksum(ip,n)==0&&!memcmp(ip+28,query,sizeof(query)));
    }
    memcpy(copy,ip,n);return n;
}
static void probe_complete(void)
{
    struct md_channel *tx=&md_channels[6];assert(tx->tx_pending);
    event(4,tx->ring.a+((tx->wp+MD_TRES-1)%MD_TRES)*16,100,2,0);assert(md_poll()==0);
}
static size_t probe_reply(uint8_t *ip,size_t n)
{
    uint8_t src[4];memcpy(src,ip+12,4);memcpy(ip+12,ip+16,4);memcpy(ip+16,src,4);
    if (ip[9]==1) ip[20]=0;
    else {
        ip[20]=0;ip[21]=53;ip[22]=0xc0;ip[23]=3;ip[30]=0x81;ip[31]=0x80;ip[35]=2;
        /* Compressed CNAME followed by the first A answer. */
        static const uint8_t answers[]={0xc0,12,0,5,0,1,0,0,0,60,0,2,0xc0,12,
            0xc0,12,0,1,0,1,0,0,0,60,0,4,203,0,113,7};
        memcpy(ip+n,answers,sizeof(answers));n+=sizeof(answers);
    }
    probe_fix(ip,n);return n;
}
static void probe_receive(const uint8_t *ip,size_t n)
{
    struct md_channel *rx=&md_channels[7];assert(rx->rx_posted);
    uint8_t *q=rx->buffer.p;q[0]=0;q[1]=1;q[2]=(uint8_t)(n>>8);q[3]=(uint8_t)n;memcpy(q+4,ip,n);
    unsigned delivered=net_rx;uint64_t received=md_data_rx;
    event(5,rx->ring.a+((rx->wp+MD_TRES-1)%MD_TRES)*16,101,4,(unsigned)n+4);
    assert(md_poll()==0&&rx->rx_posted&&net_rx==delivered&&md_data_rx==received+1);
}
static void test_selftest(void)
{
    uint8_t ip[512],bad[512],late[512];size_t n;
    struct md_channel *tx=&md_channels[6];
    assert(md_selftest.started&&md_selftest.next==0&&probe_log[0]==0);
    assert(md_poll()==0);n=probe_request(1,ip);uint64_t start=now;
    now+=1000;assert(md_poll()==0&&md_selftest.next==1); /* TX busy: no overwrite. */
    assert(!memcmp(ip,tx->buffer.p+4,n));probe_complete();
    n=probe_reply(ip,n);
    /* Wrong ID/sequence/source/destination/type/code/fragments and corrupt checksums. */
    const unsigned offsets[]={24,27,12,16,20,21,6};
    for (unsigned j=0;j<sizeof(offsets)/sizeof(*offsets);j++) {
        memcpy(bad,ip,n);bad[offsets[j]]^=1;probe_fix(bad,n);assert(!md_selftest_input(bad,n));
    }
    memcpy(bad,ip,n);bad[10]^=1;assert(!md_selftest_input(bad,n));
    memcpy(bad,ip,n);bad[22]^=1;assert(!md_selftest_input(bad,n));
    now=start+25000;probe_receive(ip,n);
    assert(strstr(probe_log,"icmp 1.1.1.1 seq=1 reply rtt_ms=25\n"));
    size_t logged=strlen(probe_log);probe_receive(ip,n);assert(strlen(probe_log)==logged);
    now=start+999999;assert(md_poll()==0&&!tx->tx_pending);
    now++;assert(md_poll()==0);n=probe_request(2,late);probe_complete();n=probe_reply(late,n);
    now=start+1999999;assert(md_poll()==0&&!tx->tx_pending);
    now++;assert(md_poll()==0);n=probe_request(3,ip);probe_complete();n=probe_reply(ip,n);
    now+=10000;probe_receive(ip,n);assert(strstr(probe_log,"icmp 1.1.1.1 seq=3 reply rtt_ms=10\n"));
    n=probe_request(0,ip);probe_complete();n=probe_reply(ip,n);
    /* Wrong DNS txid/port/peer/QR, malformed name/RDLENGTH and bad UDP checksum. */
    const unsigned dns_offsets[]={28,20,23,12,16,30,59,69};
    for (unsigned j=0;j<sizeof(dns_offsets)/sizeof(*dns_offsets);j++) {
        memcpy(bad,ip,n);bad[dns_offsets[j]]^=dns_offsets[j]==30?0x80:1;
        probe_fix(bad,n);assert(!md_selftest_input(bad,n));
    }
    memcpy(bad,ip,n);bad[26]^=1;assert(!md_selftest_input(bad,n));
    for (size_t short_n=40;short_n<n;short_n++) {
        memcpy(bad,ip,short_n);probe_fix(bad,short_n);assert(!md_selftest_input(bad,short_n));
    }
    now=start+2040000;probe_receive(ip,n);
    assert(strstr(probe_log,"dns reply answers=2 first=203.0.113.7 rtt_ms=40\n"));
    /* IPv4 permits a zero UDP checksum; duplicate is consumed without logging. */
    logged=strlen(probe_log);ip[26]=ip[27]=0;probe_receive(ip,n);assert(strlen(probe_log)==logged);
    now=start+5999999;assert(md_poll()==0&&md_selftest.status[1]==1);
    now++;assert(md_poll()==0&&md_selftest.status[1]==3);
    assert(strstr(probe_log,"icmp 1.1.1.1 seq=2 no reply\n"));
    logged=strlen(probe_log);probe_receive(late,36);assert(strlen(probe_log)==logged);
    md_selftest_start(&net_settings);assert(md_selftest.next==4); /* Once per boot. */
    now=md_data_log_at-1;assert(md_poll()==0&&!tx->tx_pending&&strlen(probe_log)==logged);
    now++;assert(md_poll()==0&&strstr(probe_log,"data packets pdp_ip0 tx=4 rx=6"));
    assert(md_data_log_at==now+60000000);logged=strlen(probe_log);
    now+=59999999;assert(md_poll()==0&&!tx->tx_pending&&strlen(probe_log)==logged);
    now++;assert(md_poll()==0&&strlen(probe_log)>logged);

    /* A separate simulated boot with no replies: all four expire once at 5 s. */
    memset(&md_selftest,0,sizeof(md_selftest));probe_log[0]=0;md_selftest_start(&net_settings);
    for (unsigned seq=1;seq<=3;seq++) {
        assert(md_poll()==0);probe_request(seq,ip);probe_complete();
        if (seq<3) now+=1000000;
    }
    probe_request(0,ip);probe_complete();
    now=md_selftest.sent[0]+4999999;assert(md_poll()==0&&probe_log[0]==0);
    for (unsigned i=0;i<3;i++) {
        now=md_selftest.sent[i]+5000000;assert(md_poll()==0&&md_selftest.status[i]==3);
    }
    assert(md_selftest.status[3]==3&&strstr(probe_log,"icmp 1.1.1.1 seq=1 no reply\n")&&
        strstr(probe_log,"icmp 1.1.1.1 seq=2 no reply\n")&&strstr(probe_log,"icmp 1.1.1.1 seq=3 no reply\n")&&
        strstr(probe_log,"dns no reply\n"));
    logged=strlen(probe_log);now+=5000000;assert(md_poll()==0&&!tx->tx_pending&&strlen(probe_log)==logged);
    puts("PASS selftest: IP_HW0 100/101 QMAP mux1, IPv4/ICMP/UDP checksums, 3 echoes at 1 s, WDS DNS A query, compressed CNAME/A reply, ID/seq/txid and malformed rejection, RTT, duplicate/late consumption, 5 s timeouts, once-only, 60 s counters");
}
