/* iOS 20F66 IO80211 hostapd XPC activation, with the kernel authenticator.
 * misd/pfd/bootpd/mDNSResponder own IP services. This service never changes
 * addresses, forwarding, PF rules, DHCP/DNS sockets or CommCenter state. */
#define PRIVATE 1
#include <dispatch/dispatch.h>
#include <arpa/inet.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/pfvar.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <errno.h>

/* The stripped iOS SDK omits XPC headers; these are the exported libSystem
 * C signatures used by stock 20F66 IO80211/hostapd, not private object layouts. */
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER 1
extern xpc_type_t xpc_get_type(xpc_object_t);
extern xpc_connection_t xpc_connection_create_mach_service(const char *,dispatch_queue_t,uint64_t);
extern void xpc_connection_set_event_handler(xpc_connection_t,void (^)(xpc_object_t));
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_connection_cancel(xpc_connection_t);
extern uid_t xpc_connection_get_euid(xpc_connection_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_connection_send_message(xpc_connection_t,xpc_object_t);
extern void xpc_release(xpc_object_t);

/* Mirrored fixed-width, credential-free kernel status ABI. */
struct ap_status {
    unsigned version, active, security, channel, ssid_length, associated, authorized;
    unsigned char ssid[32], address[6], peer[6];
    unsigned hidden;
};
_Static_assert(sizeof(struct ap_status) == 76, "kernel AP status ABI");
_Static_assert(sizeof(struct pf_rule) == 1040 && sizeof(struct pfioc_rule) == 3104, "20F66 PF ABI");
_Static_assert(DIOCDELETERULE == 0xcc20441c && DIOCCHANGERULE == 0xcc20441a,
    "stock pfd ioctl ABI (20F66)");
static bool active, nat_ready, dns_seen;
static unsigned client_count;
static int capture = -1;
static unsigned capture_length;
static unsigned char capture_bytes[65536];
static uint32_t last_lease;

static bool read_status(struct ap_status *s)
{
    size_t n = sizeof(*s);
    memset(s,0,sizeof(*s));
    return !sysctlbyname("kern.hakios.wifi_ap_status",s,&n,NULL,0) && n==sizeof(*s) && s->version==1;
}

/* Recursively inspect active NAT rules; never open PF with write access. */
static bool nat_rules(int fd, const char *path, unsigned depth)
{
    if (depth > 8) return false;
    struct pfioc_rule rules = {0};
    strlcpy(rules.anchor,path,sizeof(rules.anchor)); rules.rule.action=PF_NAT;
    if (!ioctl(fd,DIOCGETRULES,&rules)) {
        unsigned count=rules.nr;
        for (unsigned i=0;i<count && i<4096;i++) {
            rules.nr=i; rules.rule.action=PF_NAT;
            if (ioctl(fd,DIOCGETRULE,&rules)) break;
            if (rules.rule.action==PF_NAT && !strcmp(rules.rule.ifname,"pdp_ip0")) return true;
        }
    }
    struct pfioc_ruleset set={0}; strlcpy(set.path,path,sizeof(set.path));
    if (ioctl(fd,DIOCGETRULESETS,&set)) return false;
    unsigned count=set.nr;
    for(unsigned i=0;i<count && i<256;i++) {
        set.nr=i;
        if(ioctl(fd,DIOCGETRULESET,&set)) break;
        char child[sizeof(set.path)];
        int n=snprintf(child,sizeof(child),"%s%s%s",path,*path?"/":"",set.name);
        if(n>0 && (size_t)n<sizeof(child) && nat_rules(fd,child,depth+1)) return true;
    }
    return false;
}

static void pf_observe(void)
{
    struct pf_status status={0};
    int forwarding=0;size_t n=sizeof(forwarding);
    int fd=open("/dev/pf",O_RDONLY);
    bool ready=fd>=0 && !ioctl(fd,DIOCGETSTATUS,&status) && status.running &&
        !sysctlbyname("net.inet.ip.forwarding",&forwarding,&n,NULL,0) && forwarding && nat_rules(fd,"",0);
    if(fd>=0)close(fd);
    if(ready!=nat_ready) {
        nat_ready=ready;
        printf("hotspot: NAT %s owner=native-misd upstream=pdp_ip0 forwarding=%d observed=rules\n",ready?"up":"down",forwarding);
    }
}

static int capture_open(void)
{
    int fd=-1;
    for(unsigned i=0;i<256;i++) {
        char path[32];snprintf(path,sizeof(path),"/dev/bpf%u",i);
        fd=open(path,O_RDONLY|O_NONBLOCK);
        if(fd>=0 || errno!=EBUSY)break;
    }
    if(fd<0)return -1;
    struct ifreq ifr={0};strlcpy(ifr.ifr_name,"ap1",sizeof(ifr.ifr_name));
    unsigned one=1,dlt=0,buf=sizeof(capture_bytes);
    (void)ioctl(fd,BIOCSBLEN,&buf);
    /* Ethernet IPv4 UDP only, server replies from 67 or 53. No EAPOL/key data. */
    struct bpf_insn insn[]={
        BPF_STMT(BPF_LD|BPF_H|BPF_ABS,12),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x800,0,8),
        BPF_STMT(BPF_LD|BPF_B|BPF_ABS,23),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,17,0,6),
        BPF_STMT(BPF_LD|BPF_H|BPF_ABS,20),BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,0x1fff,4,0),
        BPF_STMT(BPF_LDX|BPF_B|BPF_MSH,14),BPF_STMT(BPF_LD|BPF_H|BPF_IND,14),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,67,2,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,53,1,0),
        BPF_STMT(BPF_RET|BPF_K,0),BPF_STMT(BPF_RET|BPF_K,512)
    };
    struct bpf_program filter={sizeof(insn)/sizeof(insn[0]),insn};
    if(ioctl(fd,BIOCSETIF,&ifr)||ioctl(fd,BIOCGDLT,&dlt)||dlt!=DLT_EN10MB||
        ioctl(fd,BIOCIMMEDIATE,&one)||ioctl(fd,BIOCSSEESENT,&one)||ioctl(fd,BIOCSETF,&filter)||
        ioctl(fd,BIOCGBLEN,&capture_length)||capture_length>sizeof(capture_bytes)) { close(fd);return -1; }
    return fd;
}

static void packet(const unsigned char *p,unsigned n)
{
    if(n<42 || p[12]!=8 || p[13] || (p[14]>>4)!=4 || p[23]!=17) return;
    unsigned h=(p[14]&15)*4;
    if(h<20 || n<14+h+8) return;
    const unsigned char *u=p+14+h;unsigned port=u[0]*256+u[1];
    if(port==53 && !dns_seen) { dns_seen=true;puts("hotspot: DNS reply interface=ap1 owner=native-mrc-proxy observed=UDP"); }
    if(port!=67 || u[2]!=0 || u[3]!=68 || n<14+h+8+240) return;
    const unsigned char *b=u+8;unsigned len=n-(unsigned)(b-p),type=0;
    if(b[0]!=2 || b[1]!=1 || b[2]!=6 || memcmp(b+236,"\x63\x82\x53\x63",4))return;
    for(unsigned at=240;at<len;) {
        unsigned op=b[at++];if(op==255)break;if(!op)continue;
        if(at==len)break;unsigned count=b[at++];if(count>len-at)break;
        if(op==53 && count==1)type=b[at];at+=count;
    }
    uint32_t address;memcpy(&address,b+16,4);
    if(type==5 && address && address!=last_lease) {
        char text[INET_ADDRSTRLEN];inet_ntop(AF_INET,&address,text,sizeof(text));last_lease=address;
        printf("hotspot: DHCP lease interface=ap1 address=%s mac=%02x:%02x:xx:xx:xx:xx owner=native-bootpd observed=ACK\n",text,b[28],b[29]);
    }
}

static void observe(void)
{
    struct ap_status s;bool now=read_status(&s) && s.active && s.security==1;
    if(now!=active) {
        active=now;
        printf("hotspot: native AP %s interface=ap1 IP-owner=misd/bootpd/pfd/mDNSResponder\n",now?"up":"teardown");
        if(!now) { if(capture>=0)close(capture);capture=-1;nat_ready=dns_seen=false;last_lease=0; }
    }
    if(!now) { client_count=0;return; }
    if(s.authorized!=client_count) {
        client_count=s.authorized;
        printf("hotspot: authorized clients=%u interface=ap1\n",client_count);
    }
    pf_observe();
    if(capture<0)capture=capture_open();
    if(capture<0)return;
    for(unsigned batch=0;batch<4;batch++) {
        ssize_t n=read(capture,capture_bytes,capture_length);if(n<=0)break;
        for(size_t at=0;at+sizeof(struct bpf_hdr)<=(size_t)n;) {
            struct bpf_hdr h;memcpy(&h,capture_bytes+at,sizeof(h));
            if(h.bh_hdrlen<sizeof(h)||h.bh_hdrlen>(size_t)n-at||h.bh_caplen>(size_t)n-at-h.bh_hdrlen)break;
            packet(capture_bytes+at+h.bh_hdrlen,h.bh_caplen);
            at+=BPF_WORDALIGN(h.bh_hdrlen+h.bh_caplen);
        }
    }
}

int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    dispatch_queue_t q=dispatch_get_main_queue();
    xpc_connection_t listener=xpc_connection_create_mach_service("com.apple.wifi.hostapd",q,XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener,^(xpc_object_t peer){
        if(xpc_get_type(peer)!=XPC_TYPE_CONNECTION)return;
        xpc_connection_set_event_handler(peer,^(xpc_object_t request){
            if(xpc_get_type(request)!=XPC_TYPE_DICTIONARY)return;
            /* IO80211's empty activation dictionary contains no credentials.
             * Only root's wifid may acknowledge an already running WPA2 AP. */
            struct ap_status s;
            if(xpc_connection_get_euid(peer)!=0 || !read_status(&s) || !s.active || s.security!=1) {
                puts("hotspot-ap: native authenticator activation rejected inactive-or-unauthorized");
                xpc_connection_cancel(peer);return;
            }
            /* IO80211 has already installed these settings in the kernel. */
            (void)unlink("/var/run/hostapd.conf");
            xpc_object_t reply=xpc_dictionary_create_reply(request);
            if(reply){ xpc_connection_send_message(peer,reply);xpc_release(reply); }
            puts("hotspot-ap: native authenticator ready security=WPA2-PSK engine=kernel");
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
    dispatch_source_t timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,q);
    dispatch_source_set_timer(timer,DISPATCH_TIME_NOW,NSEC_PER_SEC,NSEC_PER_SEC/10);
    dispatch_source_set_event_handler(timer,^{observe();});dispatch_resume(timer);
    dispatch_main();
}
