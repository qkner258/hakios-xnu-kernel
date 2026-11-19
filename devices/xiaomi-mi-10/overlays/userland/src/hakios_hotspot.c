/* Opt-in shared IPv4 router. PF ABI comes from the XNU being built, not iOS SPI.
 * No switches: no network writes, sockets, directories, or logs. IPv4 /24 only.
 * Additional downstreams: /var/db/hakios-hotspot/downstreams, one line:
 *   wifi-on en3 192.168.2.1/24
 */
#define PRIVATE 1
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/bpf.h>
#include <net/route.h>
#include <net/pfvar.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR "/var/db/hakios-hotspot"
#define CONFIG STATE_DIR "/downstreams"
#define JOURNAL STATE_DIR "/session"
#define UPSTREAM "pdp_ip0"
#define OWNER "com.hakios.hotspot"
#define MAX_DOWNSTREAMS 4
#define MAX_LEASES 64
#define LEASE_SECONDS 300u

/* Public SDK omits Darwin IPv4 alias ioctls. */
struct hotspot_alias { char name[IFNAMSIZ]; struct sockaddr_in address, broadcast, mask; };
#define HOTSPOT_ADDADDR _IOW('i', 26, struct hotspot_alias)
_Static_assert(sizeof(struct hotspot_alias) == 64, "Darwin alias ABI");

struct lease { uint8_t mac[6]; uint32_t address; time_t expires; };
struct downstream {
    char toggle[32], interface[IFNAMSIZ];
    uint32_t gateway; /* network byte order; .1 in an RFC1918 /24 */
    struct lease leases[MAX_LEASES];
};
/* These read-only configd SPI are exported on iOS; public SC headers mark
 * them unavailable. No preferences or CommCenter state are modified. */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern CFArrayRef SCDynamicStoreCopyKeyList(SCDynamicStoreRef, CFStringRef);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef);
struct cellular { uint32_t address, dns; int mtu; };
struct saved_interface { char name[IFNAMSIZ]; uint32_t address; short flags; bool added, raised; };
struct session {
    uint32_t magic;
    struct timeval boot;
    int forwarding;
    uint64_t token;
    uint32_t route_gateway;
    bool route_added, rules_added;
    struct saved_interface interfaces[MAX_DOWNSTREAMS];
};
struct dhcp_packet {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16], sname[64], file[128];
    uint32_t cookie;
    uint8_t options[312];
} __attribute__((packed));
_Static_assert(offsetof(struct dhcp_packet, options) == 240, "DHCP wire header");
struct dhcp_options { uint8_t type; uint32_t requested, server; };

static struct downstream downstreams[MAX_DOWNSTREAMS];
static unsigned downstream_count, active_mask;
static struct cellular upstream;
static struct session saved;
static int pf_fd = -1, lock_fd = -1, dhcp_fd = -1;
/* Write-only Ethernet transmitters; UDP remains the single receive socket. */
static int dhcp_tx[MAX_DOWNSTREAMS] = { -1, -1, -1, -1 };
static uint8_t dhcp_mac[MAX_DOWNSTREAMS][6];
static unsigned dhcp_rx_count, dhcp_tx_count;
static volatile sig_atomic_t stopping;

static void log_line(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    fputs("hotspot: ", stdout); vfprintf(stdout, format, ap); fputc('\n', stdout);
    fflush(stdout); va_end(ap);
}
static uint32_t ipv4(const char *text)
{
    struct in_addr a; return inet_pton(AF_INET, text, &a) == 1 ? a.s_addr : 0;
}
static const char *ip_text(uint32_t address, char text[INET_ADDRSTRLEN])
{
    struct in_addr a = { .s_addr = address }; return inet_ntop(AF_INET, &a, text, INET_ADDRSTRLEN);
}
static struct sockaddr_in sin_address(uint32_t address)
{
    struct sockaddr_in a = { .sin_len = sizeof(a), .sin_family = AF_INET, .sin_addr.s_addr = address };
    return a;
}
static bool valid_name(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (!n || n >= max) return false;
    for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
    return true;
}
static bool config_row(struct downstream *d, const char *line)
{
    char toggle[32], interface[IFNAMSIZ], cidr[32], extra;
    if (sscanf(line, "%31s %15s %31s %c", toggle, interface, cidr, &extra) != 3 ||
        !valid_name(toggle, sizeof(toggle)) || !valid_name(interface, sizeof(interface)) || !strcmp(interface, UPSTREAM)) return false;
    char *slash = strchr(cidr, '/');
    if (!slash || strcmp(slash, "/24")) return false;
    *slash = 0;
    uint32_t address = ipv4(cidr), h = ntohl(address);
    if ((h & 255) != 1 || !((h >> 24) == 10 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8)) return false;
    memset(d, 0, sizeof(*d));
    strlcpy(d->toggle, toggle, sizeof(d->toggle)); strlcpy(d->interface, interface, sizeof(d->interface));
    d->gateway = address;
    return true;
}
static bool load_config(void)
{
    downstream_count = 1;
    (void)config_row(&downstreams[0], "usb-on en2 172.16.42.1/24");
    FILE *f = fopen(CONFIG, "r");
    if (!f) return errno == ENOENT;
    char line[160]; bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strchr(line, '\n') && !feof(f)) { ok = false; break; }
        if (downstream_count == MAX_DOWNSTREAMS || !config_row(&downstreams[downstream_count], line)) { ok = false; break; }
        struct downstream *d = &downstreams[downstream_count];
        for (unsigned i = 0; i < downstream_count; ++i) {
            if (!strcmp(d->toggle, downstreams[i].toggle) || !strcmp(d->interface, downstreams[i].interface) || d->gateway == downstreams[i].gateway) ok = false;
        }
        if (!ok) break;
        ++downstream_count;
    }
    if (ferror(f)) ok = false;
    fclose(f); return ok;
}
static unsigned wanted_mask(void)
{
    unsigned mask = 0; char path[128];
    for (unsigned i = 0; i < downstream_count; ++i) {
        snprintf(path, sizeof(path), STATE_DIR "/%s", downstreams[i].toggle);
        if (!access(path, F_OK)) mask |= 1u << i;
    }
    return mask;
}
static uint32_t cf_ipv4(CFTypeRef value)
{
    char text[INET_ADDRSTRLEN];
    return value && CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString(value, text, sizeof(text), kCFStringEncodingASCII) ? ipv4(text) : 0;
}
static bool dictionary(CFTypeRef value)
{
    return value && CFGetTypeID(value) == CFDictionaryGetTypeID();
}
static uint32_t cellular_dns(uint32_t address)
{
    /* Explicit operator DNS for standalone QMI mode, which has no configd
     * service. Otherwise use the DNS of the matching pdp_ip0 IPv4 service. */
    FILE *f = fopen(STATE_DIR "/upstream-dns", "r");
    if (f) {
        char text[64], extra; uint32_t dns = 0;
        if (fgets(text, sizeof(text), f)) {
            char ip[INET_ADDRSTRLEN];
            if (sscanf(text, "%15s %c", ip, &extra) == 1) dns = ipv4(ip);
        }
        fclose(f); return dns;
    }
    if (errno != ENOENT) return 0;
    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR(OWNER), NULL, NULL);
    if (!store) return 0;
    CFArrayRef keys = SCDynamicStoreCopyKeyList(store, CFSTR("State:/Network/Service/[^/]+/IPv4"));
    uint32_t result = 0;
    for (CFIndex i = 0; keys && i < CFArrayGetCount(keys) && !result; ++i) {
        CFStringRef key = CFArrayGetValueAtIndex(keys, i);
        CFPropertyListRef v4 = SCDynamicStoreCopyValue(store, key);
        bool matches = false;
        if (dictionary(v4)) {
            CFTypeRef name = CFDictionaryGetValue(v4, CFSTR("InterfaceName"));
            CFTypeRef addresses = CFDictionaryGetValue(v4, CFSTR("Addresses"));
            if (name && CFEqual(name, CFSTR(UPSTREAM)) && addresses && CFGetTypeID(addresses) == CFArrayGetTypeID())
                for (CFIndex j = 0; j < CFArrayGetCount(addresses); ++j)
                    if (cf_ipv4(CFArrayGetValueAtIndex(addresses, j)) == address) matches = true;
        }
        if (v4) CFRelease(v4);
        if (!matches) continue;
        CFMutableStringRef dns_key = CFStringCreateMutableCopy(NULL, 0, key);
        CFStringReplace(dns_key, CFRangeMake(CFStringGetLength(dns_key) - 4, 4), CFSTR("DNS"));
        CFPropertyListRef dns = SCDynamicStoreCopyValue(store, dns_key); CFRelease(dns_key);
        if (dictionary(dns)) {
            CFTypeRef servers = CFDictionaryGetValue(dns, CFSTR("ServerAddresses"));
            if (servers && CFGetTypeID(servers) == CFArrayGetTypeID())
                for (CFIndex j = 0; j < CFArrayGetCount(servers) && !result; ++j) result = cf_ipv4(CFArrayGetValueAtIndex(servers, j));
        }
        if (dns) CFRelease(dns);
    }
    if (keys) CFRelease(keys); CFRelease(store); return result;
}
static bool cellular_state(struct cellular *s)
{
    memset(s, 0, sizeof(*s));
    struct ifaddrs *list;
    if (getifaddrs(&list)) return false;
    for (struct ifaddrs *p = list; p; p = p->ifa_next)
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, UPSTREAM) &&
            (p->ifa_flags & IFF_UP)) s->address = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
    freeifaddrs(list);
    if (!s->address) return false;
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return false;
    struct ifreq request = {0}; strlcpy(request.ifr_name, UPSTREAM, sizeof(request.ifr_name));
    bool ok = !ioctl(fd, SIOCGIFMTU, &request); close(fd);
    s->mtu = request.ifr_mtu;
    if (!ok || s->mtu < 576 || s->mtu > 32764) return false;
    s->dns = cellular_dns(s->address);
    return s->dns != 0;
}
static bool save_session(void)
{
    int fd = open(JOURNAL ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, &saved, sizeof(saved)) == sizeof(saved) && fsync(fd) == 0;
    int error = errno; close(fd);
    if (ok) return rename(JOURNAL ".new", JOURNAL) == 0;
    unlink(JOURNAL ".new"); errno = error; return false;
}
static bool pf_call(unsigned long command, void *argument, const char *operation)
{
    if (!ioctl(pf_fd, command, argument)) return true;
    log_line("PF failed op=%s errno=%d", operation, errno); return false;
}
static bool pf_probe(void)
{
    pf_fd = open("/dev/pf", O_RDWR);
    if (pf_fd < 0) { log_line("PF unavailable open=/dev/pf errno=%d", errno); return false; }
    struct pf_status status = {0};
    if (!pf_call(DIOCGETSTATUS, &status, "status")) return false;
    log_line("PF ready running=%u states=%u rule-size=%zu", status.running, status.states, sizeof(struct pf_rule));
    return true;
}
static bool pf_status(void)
{
    unsigned rules = 0, states = 0;
    const uint8_t actions[] = { PF_SCRUB, PF_PASS, PF_NAT };
    for (unsigned a = 0; a < sizeof(actions); ++a) {
        struct pfioc_rule list = {0}; list.rule.action = actions[a];
        if (!pf_call(DIOCGETRULES, &list, "get-rules")) return false;
        for (uint32_t i = 0; i < list.nr; ++i) {
            struct pfioc_rule r = { .ticket = list.ticket, .nr = i }; r.rule.action = actions[a];
            if (!pf_call(DIOCGETRULE, &r, "get-rule")) return false;
            if (strcmp(r.rule.owner, OWNER)) continue;
            ++rules; if (r.rule.action == PF_PASS) states += r.rule.states;
            log_line("rule interface=%s action=%u packets-in=%llu packets-out=%llu states=%u", r.rule.label, r.rule.action,
                (unsigned long long)r.rule.packets[0], (unsigned long long)r.rule.packets[1], r.rule.states);
        }
    }
    log_line("status owned-rules=%u owned-states=%u", rules, states);
    return true;
}
static bool pf_remove(void)
{
    struct pfioc_rule rule = {0}; strlcpy(rule.rule.owner, OWNER, sizeof(rule.rule.owner));
    /* Apple owner deletion expires states for each owned FILTER rule, including
     * its associated NAT states. Never flush the main ruleset or unrelated states. */
    return pf_call(DIOCDELETERULE, &rule, "delete-owner");
}
static void pf_rule_base(struct pf_rule *r, const struct downstream *d, int action, int direction)
{
    memset(r, 0, sizeof(*r));
    r->action = action; r->direction = direction; r->af = AF_INET; r->rpool.af = AF_INET; r->quick = 1;
    strlcpy(r->owner, OWNER, sizeof(r->owner));
    strlcpy(r->label, d->interface, sizeof(r->label));
    strlcpy(r->ifname, d->interface, sizeof(r->ifname));
}
static void pf_subnet(struct pf_rule_addr *a, const struct downstream *d)
{
    a->addr.type = PF_ADDR_ADDRMASK;
    a->addr.v.a.addr.addr32[0] = d->gateway & htonl(0xffffff00u);
    a->addr.v.a.mask.addr32[0] = htonl(0xffffff00u);
}
static bool pf_insert(struct pf_rule *r, uint32_t pool_address)
{
    struct pfioc_pooladdr pool = {0};
    if (!pf_call(DIOCBEGINADDRS, &pool, "begin-pool")) return false;
    if (pool_address) {
        pool.af = AF_INET; pool.addr.addr.type = PF_ADDR_ADDRMASK;
        pool.addr.addr.v.a.addr.addr32[0] = pool_address;
        pool.addr.addr.v.a.mask.addr32[0] = 0xffffffffu;
        if (!pf_call(DIOCADDADDR, &pool, "add-pool")) return false;
    }
    struct pfioc_rule request = { .action = PF_CHANGE_GET_TICKET, .pool_ticket = pool.ticket, .rule = *r };
    if (!pf_call(DIOCCHANGERULE, &request, "rule-ticket")) return false;
    /* Scoped rules go first, so every translated flow uses our filter/state
     * owner even when another client has a broad PASS rule at priority zero. */
    request.action = PF_CHANGE_ADD_HEAD;
    return pf_call(DIOCCHANGERULE, &request, "insert-rule");
}
static bool pf_load(unsigned mask)
{
    for (unsigned i = 0; i < downstream_count; ++i) if (mask & (1u << i)) {
        struct downstream *d = &downstreams[i]; struct pf_rule r;
        /* Tag only packets received on this downstream, with its source subnet.
         * NAT requires both that tag and the source; spoofing on en0 cannot NAT. */
        pf_rule_base(&r, d, PF_PASS, PF_IN); pf_subnet(&r.src, d);
        snprintf(r.tagname, sizeof(r.tagname), OWNER ".%s", d->interface);
        if (!pf_insert(&r, 0)) return false;
        pf_rule_base(&r, d, PF_NAT, PF_OUT); pf_subnet(&r.src, d);
        strlcpy(r.ifname, UPSTREAM, sizeof(r.ifname));
        snprintf(r.match_tagname, sizeof(r.match_tagname), OWNER ".%s", d->interface);
        r.rpool.proxy_port[0] = 1024; r.rpool.proxy_port[1] = 65535; /* host order, as PF expects */
        if (!pf_insert(&r, upstream.address)) return false;
        pf_rule_base(&r, d, PF_PASS, PF_OUT); strlcpy(r.ifname, UPSTREAM, sizeof(r.ifname));
        snprintf(r.match_tagname, sizeof(r.match_tagname), OWNER ".%s", d->interface);
        r.keep_state = PF_STATE_NORMAL;
        if (!pf_insert(&r, 0)) return false;
        for (int direction = PF_IN; direction <= PF_OUT; ++direction) {
            pf_rule_base(&r, d, PF_SCRUB, direction); r.proto = IPPROTO_TCP;
            pf_subnet(direction == PF_IN ? &r.src : &r.dst, d);
            r.max_mss = (uint16_t)((upstream.mtu < 1500 ? upstream.mtu : 1500) - 40);
            if (!pf_insert(&r, 0)) return false;
        }
        char net[INET_ADDRSTRLEN]; ip_text(d->gateway & htonl(0xffffff00u), net);
        log_line("NAT rule loaded downstream=%s subnet=%s/24 upstream=%s mss=%d", d->interface, net, UPSTREAM, (upstream.mtu < 1500 ? upstream.mtu : 1500) - 40);
    }
    return true;
}

/* All route operations read the kernel acknowledgement (write success alone
 * does not mean the route was installed). Existing defaults are never replaced. */
static int route_request(int type, uint32_t gateway, unsigned *index, uint32_t *actual_gateway)
{
    struct { struct rt_msghdr h; struct sockaddr_in dst; struct sockaddr_dl gw; struct sockaddr_in mask; struct sockaddr_dl link; } m = {0};
    static int sequence;
    m.h.rtm_version = RTM_VERSION; m.h.rtm_type = type; m.h.rtm_seq = ++sequence; m.h.rtm_pid = getpid();
    (void)gateway; /* direct interface default for the raw-IP point-to-point link */
    m.h.rtm_flags = RTF_UP | RTF_STATIC;
    m.h.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK | RTA_IFP;
    m.dst = sin_address(0); m.mask = sin_address(0);
    m.gw.sdl_len = sizeof(m.gw); m.gw.sdl_family = AF_LINK; m.gw.sdl_index = if_nametoindex(UPSTREAM);
    m.link.sdl_len = sizeof(m.link); m.link.sdl_family = AF_LINK; m.link.sdl_index = if_nametoindex(UPSTREAM);
    m.h.rtm_msglen = sizeof(m);
    if (type == RTM_GET) { m.h.rtm_addrs = RTA_DST; m.h.rtm_msglen = sizeof(m.h) + sizeof(m.dst); }
    int fd = socket(PF_ROUTE, SOCK_RAW, 0);
    if (fd < 0) return errno;
    struct timeval timeout = { .tv_sec = 1 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int error = 0; char reply[2048];
    if (write(fd, &m, m.h.rtm_msglen) != m.h.rtm_msglen) error = errno ? errno : EIO;
    while (!error) {
        ssize_t n = read(fd, reply, sizeof(reply));
        if (n < (ssize_t)sizeof(struct rt_msghdr)) { error = n < 0 ? errno : EIO; break; }
        struct rt_msghdr *h = (void *)reply;
        if (h->rtm_seq != sequence || h->rtm_pid != getpid()) continue;
        error = h->rtm_errno;
        if (!error && index) *index = h->rtm_index;
        if (!error && actual_gateway) {
            *actual_gateway = 0;
            char *cursor = reply + sizeof(*h), *end = reply + n;
            for (int bit = 1; bit <= RTA_BRD && cursor + 2 <= end; bit <<= 1) if (h->rtm_addrs & bit) {
                struct sockaddr *a = (void *)cursor;
                size_t length = a->sa_len ? (a->sa_len + 3u) & ~3u : 4u;
                if (cursor + length > end) { error = EINVAL; break; }
                if (bit == RTA_GATEWAY && a->sa_family == AF_INET && a->sa_len >= sizeof(struct sockaddr_in)) *actual_gateway = ((struct sockaddr_in *)a)->sin_addr.s_addr;
                cursor += length;
            }
        }
        break;
    }
    close(fd); return error;
}
static bool ensure_default(void)
{
    unsigned index = 0; uint32_t gateway = 0;
    int error = route_request(RTM_GET, 0, &index, &gateway);
    if (!error) {
        if (index == if_nametoindex(UPSTREAM)) return true;
        log_line("enable failed default-route interface-index=%u expected=%s; disconnect STA first", index, UPSTREAM);
        return false;
    }
    if (error != ESRCH) { log_line("enable failed route-get errno=%d", error); return false; }
    saved.route_gateway = 0; saved.route_added = true;
    if (!save_session()) return false; /* record intent before mutating */
    error = route_request(RTM_ADD, 0, NULL, NULL);
    if (error) { saved.route_added = false; log_line("enable failed route-add errno=%d", error); return false; }
    error = route_request(RTM_GET, 0, &index, &gateway);
    if (error || index != if_nametoindex(UPSTREAM)) { log_line("enable failed route-verify errno=%d index=%u", error, index); return false; }
    return true;
}
static bool configure_downstream(unsigned i)
{
    struct downstream *d = &downstreams[i]; struct ifaddrs *list;
    bool found = false;
    if (getifaddrs(&list)) return false;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) if (!strcmp(p->ifa_name, d->interface) && p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
        if (((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr != d->gateway || !p->ifa_netmask ||
            ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr != htonl(0xffffff00u)) {
            freeifaddrs(list); errno = EADDRINUSE; return false;
        }
        found = true;
    }
    freeifaddrs(list);
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return false;
    struct ifreq flags = {0}; strlcpy(flags.ifr_name, d->interface, sizeof(flags.ifr_name));
    bool ok = !ioctl(fd, SIOCGIFFLAGS, &flags);
    struct saved_interface *s = &saved.interfaces[i];
    strlcpy(s->name, d->interface, sizeof(s->name)); s->address = d->gateway; s->flags = flags.ifr_flags;
    s->added = !found; s->raised = !(flags.ifr_flags & IFF_UP);
    ok = ok && save_session();
    if (ok && !found) {
        struct hotspot_alias alias = {0}; strlcpy(alias.name, d->interface, sizeof(alias.name));
        alias.address = sin_address(d->gateway); alias.broadcast = sin_address(d->gateway | htonl(255)); alias.mask = sin_address(htonl(0xffffff00u));
        ok = !ioctl(fd, HOTSPOT_ADDADDR, &alias);
    }
    if (ok && s->raised) { flags.ifr_flags |= IFF_UP; ok = !ioctl(fd, SIOCSIFFLAGS, &flags); }
    int error = errno; close(fd); errno = error; return ok;
}

static bool dhcp_parse(const struct dhcp_packet *p, size_t length, struct dhcp_options *o)
{
    memset(o, 0, sizeof(*o));
    if (length < offsetof(struct dhcp_packet, options) || length > sizeof(*p) || p->op != 1 || p->htype != 1 || p->hlen != 6 || p->hops || p->giaddr || ntohl(p->cookie) != 0x63825363u || (p->chaddr[0] & 1)) return false;
    for (size_t i = offsetof(struct dhcp_packet, options); i < length;) {
        const uint8_t *wire = (const void *)p; uint8_t code = wire[i++];
        if (code == 255) return o->type != 0;
        if (!code) continue;
        if (i == length) return false;
        uint8_t n = wire[i++]; if (n > length - i) return false;
        if (code == 53) { if (n != 1 || o->type) return false; o->type = wire[i]; }
        if (code == 50) { if (n != 4 || o->requested) return false; memcpy(&o->requested, wire + i, 4); }
        if (code == 54) { if (n != 4 || o->server) return false; memcpy(&o->server, wire + i, 4); }
        if (code == 52) return false; /* overloaded BOOTP fields are unsupported */
        i += n;
    }
    return false; /* Require END; malformed requests must never get an ACK. */
}
static bool pool_address(const struct downstream *d, uint32_t address)
{
    uint32_t h = ntohl(address), net = ntohl(d->gateway) & 0xffffff00u;
    return h >= net + 2 && h < net + 2 + MAX_LEASES;
}
static struct lease *lease_find(struct downstream *d, const uint8_t mac[6], uint32_t requested, time_t now)
{
    struct lease *free_slot = NULL;
    for (unsigned i = 0; i < MAX_LEASES; ++i) {
        struct lease *l = &d->leases[i];
        if (l->expires <= now) { l->expires = 0; if (!free_slot) free_slot = l; }
        else if (!memcmp(l->mac, mac, 6)) return !requested || requested == l->address ? l : NULL;
    }
    if (!free_slot || (requested && !pool_address(d, requested))) return NULL;
    uint32_t net = ntohl(d->gateway) & 0xffffff00u;
    for (unsigned host = 2; host < 2 + MAX_LEASES; ++host) {
        uint32_t address = requested ? requested : htonl(net + host); bool used = false;
        for (unsigned j = 0; j < MAX_LEASES; ++j) if (d->leases[j].expires && d->leases[j].address == address) used = true;
        if (!used) {
            memcpy(free_slot->mac, mac, 6); free_slot->address = address; free_slot->expires = now + 30;
            return free_slot;
        }
        if (requested) break;
    }
    return NULL;
}
static void dhcp_put(uint8_t **cursor, uint8_t code, const void *value, uint8_t length)
{
    *(*cursor)++ = code; *(*cursor)++ = length; memcpy(*cursor, value, length); *cursor += length;
}
static size_t dhcp_reply(struct dhcp_packet *reply, const struct dhcp_packet *request, const struct downstream *d, uint32_t address, uint8_t type)
{
    memset(reply, 0, sizeof(*reply)); reply->op = 2; reply->htype = 1; reply->hlen = 6;
    reply->xid = request->xid; reply->flags = request->flags; reply->yiaddr = address;
    memcpy(reply->chaddr, request->chaddr, sizeof(reply->chaddr)); reply->cookie = htonl(0x63825363u);
    uint8_t *cursor = reply->options;
    dhcp_put(&cursor, 53, &type, 1); dhcp_put(&cursor, 54, &d->gateway, 4);
    if (type != 6) {
        uint32_t mask = htonl(0xffffff00u), duration = htonl(LEASE_SECONDS), renew = htonl(LEASE_SECONDS / 2);
        uint16_t mtu = htons((uint16_t)(upstream.mtu < 1500 ? upstream.mtu : 1500));
        dhcp_put(&cursor, 1, &mask, 4); dhcp_put(&cursor, 3, &d->gateway, 4); dhcp_put(&cursor, 6, &upstream.dns, 4);
        dhcp_put(&cursor, 51, &duration, 4); dhcp_put(&cursor, 58, &renew, 4); dhcp_put(&cursor, 26, &mtu, 2);
    }
    *cursor++ = 255;
    size_t length = (size_t)(cursor - (uint8_t *)reply);
    return length < 300 ? 300 : length; /* BOOTP minimum; zero padding after END */
}
static unsigned dhcp_received_index(struct msghdr *message)
{
    if (message->msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(message); c; c = CMSG_NXTHDR(message, c))
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && c->cmsg_len == CMSG_LEN(sizeof(struct in_pktinfo)))
            return ((struct in_pktinfo *)(void *)CMSG_DATA(c))->ipi_ifindex;
    return 0;
}
static uint16_t wire_checksum(const uint8_t *p, size_t length, uint32_t sum)
{
    for (; length >= 2; length -= 2, p += 2) sum += ((unsigned)p[0] << 8) | p[1];
    if (length) sum += (unsigned)p[0] << 8;
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (uint16_t)~sum;
}
static void wire_u16(uint8_t *p, unsigned value) { p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value; }
/* Initial OFFER/ACK and every NAK use limited broadcast, including L2. A
 * renewal with ciaddr uses the requesting client's MAC, without an ARP race.
 * Build actual wire bytes: raw IP sockets on Darwin use a different ip_len ABI. */
static size_t dhcp_frame(uint8_t *out, size_t capacity, const struct downstream *d,
    const uint8_t mac[6], const struct dhcp_packet *reply, size_t size, uint8_t type,
    uint32_t ciaddr)
{
    size_t length = 14 + 20 + 8 + size;
    if (size > sizeof(*reply) || capacity < length) return 0;
    bool broadcast = type == 6 || !ciaddr;
    memset(out, 0, length);
    if (broadcast) memset(out, 255, 6); else memcpy(out, reply->chaddr, 6);
    memcpy(out + 6, mac, 6); wire_u16(out + 12, 0x0800);
    uint8_t *ip = out + 14, *udp = ip + 20;
    ip[0] = 0x45; wire_u16(ip + 2, (unsigned)(20 + 8 + size)); ip[8] = 64; ip[9] = 17;
    memcpy(ip + 12, &d->gateway, 4);
    uint32_t destination = broadcast ? htonl(INADDR_BROADCAST) : ciaddr;
    memcpy(ip + 16, &destination, 4);
    wire_u16(ip + 10, wire_checksum(ip, 20, 0));
    wire_u16(udp, 67); wire_u16(udp + 2, 68); wire_u16(udp + 4, (unsigned)(8 + size));
    memcpy(udp + 8, reply, size);
    uint32_t pseudo = 17 + (unsigned)(8 + size);
    for (unsigned i = 12; i < 20; i += 2) pseudo += ((unsigned)ip[i] << 8) | ip[i + 1];
    uint16_t checksum = wire_checksum(udp, 8 + size, pseudo);
    wire_u16(udp + 6, checksum ? checksum : 65535);
    return length;
}
static bool open_dhcp_tx(unsigned slot)
{
    struct downstream *d = &downstreams[slot];
    struct ifaddrs *all = NULL;
    bool found = false;
    if (getifaddrs(&all)) return false;
    for (struct ifaddrs *a = all; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || strcmp(a->ifa_name, d->interface)) continue;
        const struct sockaddr_dl *link = (void *)a->ifa_addr;
        if (link->sdl_alen != 6) continue;
        memcpy(dhcp_mac[slot], LLADDR(link), 6); found = true; break;
    }
    freeifaddrs(all);
    if (!found) { errno = ENXIO; return false; }
    int fd = -1;
    for (unsigned n = 0; n < 256; ++n) {
        char path[32]; snprintf(path, sizeof(path), "/dev/bpf%u", n);
        fd = open(path, O_WRONLY);
        if (fd >= 0 || errno != EBUSY) break;
    }
    if (fd < 0) return false;
    struct ifreq r = {0}; strlcpy(r.ifr_name, d->interface, sizeof(r.ifr_name));
    unsigned one = 1, dlt = 0;
    int error = 0;
    if (ioctl(fd, BIOCSETIF, &r) || ioctl(fd, BIOCGDLT, &dlt)) error = errno;
    else if (dlt != DLT_EN10MB) error = EPROTONOSUPPORT;
    else if (ioctl(fd, BIOCSHDRCMPLT, &one)) error = errno;
    if (error) {
        close(fd); errno = error; return false;
    }
    dhcp_tx[slot] = fd;
    return true;
}
static void handle_dhcp(void)
{
    struct dhcp_packet request, reply; struct sockaddr_in peer;
    struct iovec vector = { .iov_base = &request, .iov_len = sizeof(request) };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control;
    struct msghdr message = { .msg_name = &peer, .msg_namelen = sizeof(peer),
        .msg_iov = &vector, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    ssize_t n = recvmsg(dhcp_fd, &message, 0);
    if (n < 0) return;
    unsigned index = dhcp_received_index(&message);
    struct downstream *d = NULL;
    for (unsigned i = 0; index && i < downstream_count; ++i)
        if ((active_mask & (1u << i)) && if_nametoindex(downstreams[i].interface) == index) d = &downstreams[i];
    if (!d) return;
    struct dhcp_options o;
    if (n < 0 || peer.sin_port != htons(68) || !dhcp_parse(&request, (size_t)n, &o) || (o.server && o.server != d->gateway)) return;
    time_t now = time(NULL);
    if (o.type == 7 || o.type == 4) {
        for (unsigned i = 0; i < MAX_LEASES; ++i) {
            struct lease *l = &d->leases[i];
            if (l->expires > now && !memcmp(l->mac, request.chaddr, 6) && l->address == (o.type == 7 ? request.ciaddr : o.requested)) {
                if (o.type == 7) l->expires = 0;
                else { memset(l->mac, 0, sizeof(l->mac)); l->expires = now + 60; }
            }
        }
        return;
    }
    if (o.type != 1 && o.type != 3) return;
    ++dhcp_rx_count;
    if (dhcp_rx_count <= 8 || !(dhcp_rx_count & (dhcp_rx_count - 1)))
        log_line("DHCP rx interface=%s type=%u count=%u bytes=%zd", d->interface, o.type, dhcp_rx_count, n);
    uint32_t desired = o.type == 3 ? (o.requested ? o.requested : request.ciaddr) : 0;
    if (o.type == 3 && (!desired || (request.ciaddr && request.ciaddr != desired))) return;
    struct lease *l = lease_find(d, request.chaddr, desired, now);
    if (!l && o.type == 1) return;
    uint8_t type = l ? (o.type == 1 ? 2 : 5) : 6;
    size_t size = dhcp_reply(&reply, &request, d, l ? l->address : 0, type);
    unsigned slot = (unsigned)(d - downstreams);
    uint8_t frame[14 + 20 + 8 + sizeof(reply)];
    size_t length = dhcp_frame(frame, sizeof(frame), d, dhcp_mac[slot], &reply, size, type, request.ciaddr);
    if (!length || write(dhcp_tx[slot], frame, length) != (ssize_t)length) {
        log_line("DHCP send failed interface=%s errno=%d", d->interface, errno); return;
    }
    ++dhcp_tx_count;
    if (dhcp_tx_count <= 8 || !(dhcp_tx_count & (dhcp_tx_count - 1)))
        log_line("DHCP tx interface=%s type=%u count=%u bytes=%zu transport=bpf", d->interface, type, dhcp_tx_count, length);
    if (type == 5) {
        l->expires = now + LEASE_SECONDS;
        char address[INET_ADDRSTRLEN];
        log_line("DHCP lease interface=%s address=%s mac=%02x:%02x:xx:xx:xx:xx seconds=%u", d->interface, ip_text(l->address, address), request.chaddr[0], request.chaddr[1], LEASE_SECONDS);
    }
}
static bool open_dhcp(void)
{
    /* One wildcard listener avoids ambiguous unicast renewal delivery between
     * per-interface wildcard sockets. PKTINFO identifies ingress; BPF sends
     * replies without scoped limited-broadcast UDP routing. Refuse conflicts. */
    int one = 1;
    dhcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dhcp_fd < 0 || setsockopt(dhcp_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) ||
        setsockopt(dhcp_fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one))) return false;
    struct sockaddr_in local = sin_address(INADDR_ANY); local.sin_port = htons(67);
    return !bind(dhcp_fd, (void *)&local, sizeof(local));
}
static bool restore_session(void)
{
    bool ok = true;
    if (saved.rules_added || saved.token) {
        if (pf_fd < 0) pf_fd = open("/dev/pf", O_RDWR);
        if (pf_fd < 0) { log_line("cleanup failed PF-open errno=%d", errno); ok = false; }
        else if (saved.rules_added && !pf_remove()) ok = false;
    }
    if (saved.route_added) {
        unsigned index = 0; uint32_t gateway = 0;
        int error = route_request(RTM_GET, 0, &index, &gateway);
        if (!error && index == if_nametoindex(UPSTREAM) && gateway == saved.route_gateway) {
            error = route_request(RTM_DELETE, saved.route_gateway, NULL, NULL);
            if (error && error != ESRCH) { log_line("cleanup failed route errno=%d", error); ok = false; }
        } else if (error && error != ESRCH) { log_line("cleanup failed route-get errno=%d", error); ok = false; }
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    for (unsigned i = 0; i < MAX_DOWNSTREAMS; ++i) {
        struct saved_interface *s = &saved.interfaces[i]; if (!s->name[0]) continue;
        struct ifreq r = {0}; strlcpy(r.ifr_name, s->name, sizeof(r.ifr_name));
        if (fd < 0) { ok = false; continue; }
        if (s->added) {
            struct sockaddr_in address = sin_address(s->address); memcpy(&r.ifr_addr, &address, sizeof(address));
            if (ioctl(fd, SIOCDIFADDR, &r) && errno != EADDRNOTAVAIL && errno != ENXIO) ok = false;
        }
        if (s->raised && !ioctl(fd, SIOCGIFFLAGS, &r)) {
            r.ifr_flags &= ~IFF_UP;
            if (ioctl(fd, SIOCSIFFLAGS, &r)) ok = false;
        }
    }
    if (fd >= 0) close(fd);
    if (sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &saved.forwarding, sizeof(saved.forwarding))) { log_line("cleanup failed forwarding errno=%d", errno); ok = false; }
    if (saved.token && pf_fd >= 0) {
        struct pfioc_remove_token token = { .token_value = saved.token };
        if (ioctl(pf_fd, DIOCSTOPREF, &token) && errno != ENOENT && errno != EINVAL) { log_line("cleanup failed PF-stopref errno=%d", errno); ok = false; }
    }
    if (pf_fd >= 0) close(pf_fd); pf_fd = -1;
    if (ok) unlink(JOURNAL);
    else log_line("cleanup incomplete; session retained");
    return ok;
}
static bool disable_hotspot(void)
{
    if (dhcp_fd >= 0) close(dhcp_fd); dhcp_fd = -1;
    for (unsigned i = 0; i < MAX_DOWNSTREAMS; ++i) {
        if (dhcp_tx[i] >= 0) close(dhcp_tx[i]);
        dhcp_tx[i] = -1;
    }
    for (unsigned i = 0; i < downstream_count; ++i) memset(downstreams[i].leases, 0, sizeof(downstreams[i].leases));
    bool ok = restore_session();
    active_mask = 0;
    if (ok) log_line("disable rules=removed states=removed forwarding=%d", saved.forwarding);
    return ok;
}
static bool enable_hotspot(unsigned mask, const struct cellular *cell)
{
    upstream = *cell;
    memset(&saved, 0, sizeof(saved)); saved.magic = 0x48535031u;
    size_t n = sizeof(saved.boot);
    if (sysctlbyname("kern.boottime", &saved.boot, &n, NULL, 0)) { log_line("enable failed snapshot errno=%d", errno); return false; }
    n = sizeof(saved.forwarding);
    if (sysctlbyname("net.inet.ip.forwarding", &saved.forwarding, &n, NULL, 0) || !save_session()) {
        log_line("enable failed snapshot errno=%d", errno); return false;
    }
    if (!pf_probe()) goto failed;
    if (!pf_call(DIOCSTARTREF, &saved.token, "startref") || !save_session()) goto failed;
    for (unsigned i = 0; i < downstream_count; ++i) if (mask & (1u << i)) {
        if (!configure_downstream(i)) {
            log_line("enable failed downstream=%s errno=%d", downstreams[i].interface, errno); goto failed;
        }
    }
    if (!open_dhcp()) { log_line("enable failed DHCP-bind errno=%d", errno); goto failed; }
    for (unsigned i = 0; i < downstream_count; ++i) if (mask & (1u << i)) {
        if (!open_dhcp_tx(i)) { log_line("enable failed DHCP-bpf interface=%s errno=%d", downstreams[i].interface, errno); goto failed; }
    }
    dhcp_rx_count = dhcp_tx_count = 0;
    if (!ensure_default()) goto failed;
    saved.rules_added = true;
    if (!save_session() || !pf_load(mask)) goto failed;
    int one = 1;
    if (sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &one, sizeof(one))) { log_line("enable failed forwarding errno=%d", errno); goto failed; }
    active_mask = mask;
    char a[INET_ADDRSTRLEN], dns[INET_ADDRSTRLEN];
    log_line("upstream %s address=%s mtu=%d dns=%s", UPSTREAM, ip_text(upstream.address, a), upstream.mtu, ip_text(upstream.dns, dns));
    log_line("enable downstream-mask=%u forwarding=1", mask);
    return true;
failed:
    if (!disable_hotspot()) stopping = 1;
    return false;
}
static void signal_handler(int number) { (void)number; stopping = 1; }
static int set_switch(const char *name, bool on)
{
    if (mkdir(STATE_DIR, 0755) && errno != EEXIST) return 1;
    char path[128]; snprintf(path, sizeof(path), STATE_DIR "/%s", name);
    if (!on) return unlink(path) && errno != ENOENT ? 1 : 0;
    int fd = open(path, O_WRONLY | O_CREAT, 0600); if (fd < 0) return 1; close(fd); return 0;
}
static int set_dns(const char *text)
{
    if (!ipv4(text)) { log_line("invalid IPv4 DNS"); return 1; }
    if (mkdir(STATE_DIR, 0755) && errno != EEXIST) return 1;
    int fd = open(STATE_DIR "/upstream-dns.new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 1;
    bool ok = dprintf(fd, "%s\n", text) > 0 && fsync(fd) == 0;
    close(fd);
    return !ok || rename(STATE_DIR "/upstream-dns.new", STATE_DIR "/upstream-dns") ? 1 : 0;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--dns")) return set_dns(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--dns-auto")) return unlink(STATE_DIR "/upstream-dns") && errno != ENOENT ? 1 : 0;
    if (argc == 2 && !strcmp(argv[1], "--usb-on")) return set_switch("usb-on", true);
    if (argc == 2 && !strcmp(argv[1], "--usb-off")) return set_switch("usb-on", false);
    if (!load_config()) { log_line("invalid downstream config"); return 1; }
    if (argc == 3 && (!strcmp(argv[1], "--on") || !strcmp(argv[1], "--off"))) {
        for (unsigned i = 0; i < downstream_count; ++i) if (!strcmp(argv[2], downstreams[i].toggle))
            return set_switch(argv[2], !strcmp(argv[1], "--on"));
        log_line("unknown downstream switch"); return 1;
    }
    if (argc == 2 && (!strcmp(argv[1], "--probe") || !strcmp(argv[1], "--status"))) {
        bool ok = pf_probe();
        if (ok && !strcmp(argv[1], "--status")) {
            ok = pf_status();
            struct cellular cell; bool ready = cellular_state(&cell);
            char address[INET_ADDRSTRLEN], dns[INET_ADDRSTRLEN];
            log_line("upstream probe interface=%s address=%s mtu=%d dns=%s ready=%d", UPSTREAM,
                ip_text(cell.address, address), cell.mtu, ip_text(cell.dns, dns), ready);
            int forwarding = -1; size_t length = sizeof(forwarding);
            if (!sysctlbyname("net.inet.ip.forwarding", &forwarding, &length, NULL, 0)) log_line("forwarding=%d", forwarding);
        }
        if (pf_fd >= 0) close(pf_fd); return !ok;
    }
    if (argc == 2 && !strcmp(argv[1], "--check")) {
        for (unsigned i = 0; i < downstream_count; ++i) { char address[INET_ADDRSTRLEN]; log_line("config switch=%s interface=%s gateway=%s/24", downstreams[i].toggle, downstreams[i].interface, ip_text(downstreams[i].gateway, address)); }
        return 0;
    }
    if (argc != 1) { fprintf(stderr, "usage: hakios-hotspot [--usb-on|--usb-off|--probe|--status|--check|--dns IPv4|--dns-auto|--on switch|--off switch]\n"); return 1; }
    signal(SIGTERM, signal_handler); signal(SIGINT, signal_handler); signal(SIGPIPE, SIG_IGN);
    bool waiting = false;
    while (!stopping) {
        unsigned mask = wanted_mask();
        /* Acquire the singleton only when there is work or a crash journal. */
        if (lock_fd < 0 && (mask || !access(JOURNAL, F_OK))) {
            lock_fd = open(STATE_DIR "/lock", O_RDWR | O_CREAT, 0600);
            if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) { log_line("session lock failed errno=%d", errno); return 1; }
            int fd = open(JOURNAL, O_RDONLY);
            if (fd >= 0) {
                ssize_t n = read(fd, &saved, sizeof(saved)); close(fd);
                if (n != sizeof(saved) || saved.magic != 0x48535031u) { log_line("invalid session journal; refusing network changes"); return 1; }
                struct timeval boot; size_t size = sizeof(boot);
                if (sysctlbyname("kern.boottime", &boot, &size, NULL, 0)) return 1;
                if (boot.tv_sec == saved.boot.tv_sec && boot.tv_usec == saved.boot.tv_usec) {
                    if (!restore_session()) return 1;
                    log_line("recovered previous session");
                } else { unlink(JOURNAL); log_line("discarded previous boot session"); }
            }
        }
        struct cellular cell;
        bool ready = mask && cellular_state(&cell);
        if (active_mask && (mask != active_mask || !ready || memcmp(&cell, &upstream, sizeof(cell)))) {
            if (!ready && mask) log_line("upstream unavailable interface=%s", UPSTREAM);
            if (!disable_hotspot()) return 1;
            waiting = false;
        }
        if (mask && !active_mask && ready) {
            if (!enable_hotspot(mask, &cell)) { if (stopping) break; sleep(5); continue; }
            waiting = false;
        } else if (mask && !ready && !waiting) { log_line("upstream unavailable interface=%s", UPSTREAM); waiting = true; }
        if (!mask) { waiting = false; if (!load_config()) { log_line("invalid downstream config"); return 1; } }
        fd_set reads; FD_ZERO(&reads);
        if (dhcp_fd >= 0) FD_SET(dhcp_fd, &reads);
        struct timeval timeout = { .tv_sec = 1 };
        if (select(dhcp_fd + 1, &reads, NULL, NULL, &timeout) > 0 && dhcp_fd >= 0 && FD_ISSET(dhcp_fd, &reads)) handle_dhcp();
    }
    return active_mask && !disable_hotspot() ? 1 : 0;
}
