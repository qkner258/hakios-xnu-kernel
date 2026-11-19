#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "umi_modem_qmi.h"
#include "umi_modem_tftp.h"

static size_t response(uint8_t *m, size_t cap, uint16_t txn, uint16_t message)
{
    size_t n=7; assert(cap>=64); memset(m,0,cap);m[0]=2;m[1]=(uint8_t)txn;m[2]=(uint8_t)(txn>>8);umi3_put16(m+3,message);
    m[n++]=2;m[n++]=4;m[n++]=0;m[n++]=0;m[n++]=0;m[n++]=0;m[n++]=0;umi3_finish(m,n);return n;
}
static void add(uint8_t *m,size_t *n,uint8_t type,const void *v,size_t len)
{ assert(umi3_tlv(m,512,n,type,v,len)==0);umi3_finish(m,*n); }
int main(void)
{
    uint8_t m[512],qmap[32];const uint8_t *p;size_t n,at,payload;uint16_t txn=0;struct umi3_state s={0};uint32_t v;uint8_t mux;
    assert(umi3_uim_card_status(m,sizeof(m),&txn,&n)==0&&umi3_le16(m+3)==0x2f&&m[0]==0&&n==7);
    assert(umi3_nas_system_info(m,sizeof(m),&txn,&n)==0&&umi3_le16(m+3)==0x4d);
    assert(umi3_nas_serving_system(m,sizeof(m),&txn,&n)==0&&umi3_le16(m+3)==0x24&&m[0]==0&&n==7&&umi3_le16(m+5)==0);
    assert(umi3_nas_register(m,sizeof(m),&txn,&n)==0&&umi3_le16(m+3)==0x22&&m[7]==1&&m[10]==1);
    assert(umi3_wds_bind(m,sizeof(m),&txn,&n)==0&&umi3_le16(m+3)==0xa2);
    assert(umi3_wda_format(m,10,&txn,&n)==-1); /* Short TLV buffer must not underflow. */
    assert(umi3_wds_family(m,sizeof(m),&txn,&n)==0&&umi3_le16(m+3)==0x4d);
    assert(umi3_wds_start(m,sizeof(m),&txn,"cmnet",&n)==0&&umi3_le16(m+3)==0x20);
    assert(m[7]==0x19&&m[10]==4&&m[11]==0x14&&umi3_le16(m+12)==5&&!memcmp(m+14,"cmnet",5));
    n=response(m,sizeof(m),txn,0x2f);uint8_t card[10]={0,0,0,0,0,0,0,0,1,1};add(m,&n,0x10,card,sizeof(card));assert(umi3_apply_uim(&s,m,n)==0&&s.uim_inserted&&s.uim_card==1&&s.uim_app==0);
    /* Slot 1 present with one USIM application (type 2) in state 7 = ready. */
    n=response(m,sizeof(m),txn,0x2f);uint8_t ready[17]={0,0,0,0,0,0,0,0,1,1,0,0,0,0,1,2,7};add(m,&n,0x10,ready,sizeof(ready));
    assert(umi3_apply_uim(&s,m,n)==0&&s.uim_card==1&&s.uim_app==7);
    n=response(m,sizeof(m),txn,0x24);uint8_t serving[5]={1,0,0,0,0};add(m,&n,1,serving,sizeof(serving));assert(umi3_apply_nas(&s,m,n)==0&&s.nas_registered&&s.nas_reg==1&&s.nas_rat==0);
    /* Searching, PS attached, 3GPP, one radio interface: LTE (8). */
    n=response(m,sizeof(m),txn,0x24);uint8_t lte[6]={2,0,1,2,1,8};add(m,&n,1,lte,sizeof(lte));
    assert(umi3_apply_nas(&s,m,n)==0&&!s.nas_registered&&s.nas_reg==2&&s.nas_ps==1&&s.nas_rat==8);
    n=response(m,sizeof(m),txn,0xa2);assert(umi3_apply_wds(&s,m,n)==0&&s.wds_bound);
    n=response(m,sizeof(m),txn,0x20);v=0x78563412;add(m,&n,1,&v,4);assert(umi3_apply_wds(&s,m,n)==0&&s.wds_up&&s.wds_handle==v);
    n=response(m,sizeof(m),txn,0x2d);uint32_t ip=0xc0000202,gw=0xc0000201,mask=0xffffff00,dns=0xc0000203,mtu=1500;
    add(m,&n,0x1e,&ip,4);add(m,&n,0x20,&gw,4);add(m,&n,0x21,&mask,4);add(m,&n,0x15,&dns,4);add(m,&n,0x29,&mtu,4);
    assert(umi3_apply_wds(&s,m,n)==0&&s.address==ip&&s.gateway==gw&&s.mask==mask&&s.dns1==dns&&s.mtu==mtu);
    memset(qmap,0,sizeof(qmap));qmap[1]=1;qmap[2]=0;qmap[3]=8;memcpy(qmap+4,"IPv4OK!!",8);at=0;assert(umi3_qmap_next(qmap,12,&at,&mux,&p,&payload)==0&&mux==1&&payload==8&&!memcmp(p,"IPv4OK!!",8));
    qmap[3]=9;at=0;assert(umi3_qmap_next(qmap,12,&at,NULL,&p,&payload)!=0);
    /* servreg locator GET_DOMAIN_LIST (0x21): request txn 0x1234, TLV 1 name, optional TLV 0x10 offset. */
    uint8_t req[64],rsp[256];size_t rn,sn;const uint8_t *e;size_t en;
    rn=7;memset(req,0,7);req[1]=0x34;req[2]=0x12;umi3_put16(req+3,0x21);assert(umi3_tlv(req,sizeof(req),&rn,1,"tms/servreg",11)==0);umi3_finish(req,rn);
    assert(umi3_servreg_domains(req,rn,rsp,sizeof(rsp),&sn)==5&&rsp[0]==2&&umi3_le16(rsp+1)==0x1234&&umi3_le16(rsp+3)==0x21);
    uint16_t r=9,er=9;assert(umi3_result(rsp,sn,&r,&er)==0&&!r&&!er);
    assert(umi3_find_tlv(rsp,sn,0x10,&e,&en)==0&&en==2&&umi3_le16(e)==5);
    assert(umi3_find_tlv(rsp,sn,0x11,&e,&en)==0&&en==2&&umi3_le16(e)==1);
    assert(umi3_find_tlv(rsp,sn,0x12,&e,&en)==0&&e[0]==5&&e[1]==17&&!memcmp(e+2,"msm/adsp/audio_pd",17)&&umi3_le32(e+19)==74&&e[23]==0);
    v=4;assert(umi3_tlv(req,sizeof(req),&rn,0x10,&v,4)==0);umi3_finish(req,rn);
    assert(umi3_servreg_domains(req,rn,rsp,sizeof(rsp),&sn)==1&&umi3_find_tlv(rsp,sn,0x12,&e,&en)==0&&e[1]==18&&!memcmp(e+2,"msm/slpi/sensor_pd",18)&&umi3_le32(e+20)==90);
    rn=7;assert(umi3_tlv(req,sizeof(req),&rn,1,"tms/unknown",11)==0);umi3_finish(req,rn);
    assert(umi3_servreg_domains(req,rn,rsp,sizeof(rsp),&sn)==0&&umi3_find_tlv(rsp,sn,0x12,&e,&en)!=0&&umi3_find_tlv(rsp,sn,0x10,&e,&en)==0&&umi3_le16(e)==0);
    umi3_put16(req+3,0x24);assert(umi3_servreg_domains(req,rn,rsp,sizeof(rsp),&sn)==-1);
    /* DMS Get (0x2d) / Set (0x2e, TLV 1 u8 mode 0=online) Operating Mode. */
    uint16_t dtx=0;uint8_t dms[16],dmode=9;size_t dn;
    assert(umi3_dms_set_operating_mode(dms,sizeof(dms),&dtx,0,&dn)==0&&dn==11&&umi3_le16(dms+3)==0x2e&&umi3_le16(dms+5)==4&&dms[7]==1&&umi3_le16(dms+8)==1&&dms[10]==0);
    assert(umi3_dms_get_operating_mode(dms,sizeof(dms),&dtx,&dn)==0&&dn==7&&umi3_le16(dms+3)==0x2d&&umi3_le16(dms+5)==0);
    n=response(m,sizeof(m),1,0x2d);v=3;add(m,&n,1,&v,1);assert(umi3_dms_operating_mode(m,n,&dmode)==0&&dmode==3);
    n=response(m,sizeof(m),1,0x2e);assert(umi3_dms_operating_mode(m,n,&dmode)!=0);
    /* TFTP RRQ/WRQ as the SDX55 sends them, and tqftpserv's OACK. */
    struct umi_tftp_request tr;uint8_t oack[256];size_t on;
    static const uint8_t rrq[]="\0\1/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_sw/oem_sw.txt\0octet\0blksize\0" "1024\0tsize\0" "0\0rsize\0" "300\0seek\0" "2048\0unlink\0" "0";
    assert(umi_tftp_parse(rrq,sizeof(rrq),&tr)==0&&tr.op==1&&tr.options&&tr.tsize&&tr.blksize==1024&&tr.rsize==300&&tr.seek==2048&&tr.wsize==1&&tr.timeoutms==1000&&
           !strcmp(tr.path,"/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_sw/oem_sw.txt"));
    static const uint8_t oack_want[]="\0\6blksize\0" "1024\0timeoutms\0" "1000\0tsize\0" "5000\0wsize\0" "1\0rsize\0" "300\0seek\0" "2048";
    on=umi_tftp_oack(oack,sizeof(oack),&tr,1024,5000);assert(on==sizeof(oack_want)&&!memcmp(oack,oack_want,on));
    assert(umi_tftp_oack(oack,20,&tr,1024,5000)==0);
    static const uint8_t wrq[]="\0\2/readwrite/server_check.txt\0OCTET";
    assert(umi_tftp_parse(wrq,sizeof(wrq),&tr)==0&&tr.op==2&&!tr.options&&tr.blksize==512);
    static const uint8_t netascii[]="\0\1/a\0netascii",small[]="\0\1/a\0octet\0blksize\0" "7",open_[]={0,1,'/','a',0,'o','c','t','e','t',0,'s','e','e','k'};
    assert(umi_tftp_parse(netascii,sizeof(netascii),&tr)!=0&&umi_tftp_parse(small,sizeof(small),&tr)!=0&&umi_tftp_parse(open_,sizeof(open_),&tr)!=0);
    static const uint8_t huge[]="\0\1/a\0octet\0blksize\0" "99999999999999999999999";assert(umi_tftp_parse(huge,sizeof(huge),&tr)!=0);
    assert(umi_tftp_blocks(1000,512)==2&&umi_tftp_block_length(1000,512,1)==512&&umi_tftp_block_length(1000,512,2)==488);
    assert(umi_tftp_blocks(1024,512)==3&&umi_tftp_block_length(1024,512,3)==0&&umi_tftp_blocks(0,512)==1&&umi_tftp_block_length(0,512,1)==0);
    puts("PASS stage3 model: QMI UIM card/app state, NAS serving system reg/RAT, WDS requests, cmnet APN, response state, IPv4 settings, QMAP bounds, servreg locator domain list, DMS operating mode, TFTP RRQ/WRQ options/OACK/blocks");
}
