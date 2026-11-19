#!/usr/bin/env python3
"""Exercise the real DHCP/config/PF builders, without touching host networking."""
from pathlib import Path
import os
import plistlib
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
device = Path(__file__).resolve().parents[1]
source = device / 'overlays/userland/src/hakios_hotspot.c'
kernel = root / 'out/xiaomi-mi-10/xnu/bsd'
headers = root / 'xnu-deps/MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers'
# Macro-replace ioctl before including the implementation. All PF requests are
# captured; unexpected ioctls fail the test instead of accessing the host PF.
harness = r'''
#include <assert.h>
#include <stdarg.h>
#define ioctl hotspot_test_ioctl
#define socket hotspot_test_socket
#define close hotspot_test_close
#define sysctlbyname hotspot_test_sysctl
#define unlink hotspot_test_unlink
#define open hotspot_test_open
#define getifaddrs hotspot_test_getifaddrs
#define freeifaddrs hotspot_test_freeifaddrs
#define main hotspot_program_main
#include "hakios_hotspot.c"
#undef main
#undef open
#undef getifaddrs
#undef freeifaddrs
#undef ioctl
#undef socket
#undef close
#undef sysctlbyname
#undef unlink
static unsigned socket_count, close_count, restore_forwarding_count, unlink_count, stopref_count;
static bool delete_failure;
static unsigned bpf_opens, bpf_step, bpf_fail, bpf_dlt = DLT_EN10MB;
int hotspot_test_open(const char *path, int flags, ...) {
    assert(flags == O_WRONLY);
    assert(!strcmp(path, bpf_opens ? "/dev/bpf1" : "/dev/bpf0"));
    if (!bpf_opens++) { errno = EBUSY; return -1; }
    return 201;
}
int hotspot_test_getifaddrs(struct ifaddrs **all) {
    static struct sockaddr_dl link;
    static struct ifaddrs entry;
    memset(&link, 0, sizeof(link)); link.sdl_family = AF_LINK; link.sdl_alen = 6;
    memcpy(LLADDR(&link), "\x02\x11\x22\x33\x44\x55", 6);
    entry.ifa_name = "en3"; entry.ifa_addr = (void *)&link; *all = &entry; return 0;
}
void hotspot_test_freeifaddrs(struct ifaddrs *all) { assert(all && !all->ifa_next); }

int hotspot_test_socket(int domain, int type, int protocol) {
    assert(domain == AF_INET && type == SOCK_DGRAM && !protocol); socket_count++; return 200;
}
int hotspot_test_close(int fd) { assert(fd == 200 || fd == 99 || fd == 201); close_count++; return 0; }
int hotspot_test_sysctl(const char *name, void *old, size_t *length, void *value, size_t size) {
    assert(!strcmp(name, "net.inet.ip.forwarding") && !old && !length && value && size == sizeof(int));
    assert(*(int *)value == saved.forwarding); restore_forwarding_count++; return 0;
}
int hotspot_test_unlink(const char *name) { assert(!strcmp(name, JOURNAL)); unlink_count++; return 0; }
static struct pfioc_rule rules[32];
static unsigned rule_count, pool_count, delete_count, fail_at;
static uint32_t pool_address_seen;
int hotspot_test_ioctl(int fd, unsigned long command, ...) {
    (void)fd;
    va_list ap; va_start(ap, command); void *argument = va_arg(ap, void *); va_end(ap);
    if (command == BIOCSETIF || command == BIOCGDLT || command == BIOCSHDRCMPLT) {
        assert(fd == 201); ++bpf_step;
        if (bpf_step == bpf_fail) { errno = EIO; return -1; }
        if (command == BIOCSETIF) assert(!strcmp(((struct ifreq *)argument)->ifr_name, "en3"));
        if (command == BIOCGDLT) *(unsigned *)argument = bpf_dlt;
        if (command == BIOCSHDRCMPLT) assert(*(unsigned *)argument == 1);
        return 0;
    }
    if (command == DIOCBEGINADDRS) {
        struct pfioc_pooladdr *p = argument; p->ticket = 7; pool_count++; return 0;
    }
    if (command == DIOCADDADDR) {
        struct pfioc_pooladdr *p = argument;
        assert(p->af == AF_INET && p->ticket == 7 && p->addr.addr.v.a.mask.addr32[0] == 0xffffffffu);
        pool_address_seen = p->addr.addr.v.a.addr.addr32[0]; return 0;
    }
    if (command == DIOCCHANGERULE) {
        struct pfioc_rule *r = argument;
        if (r->action == PF_CHANGE_GET_TICKET) { r->ticket = 11; return 0; }
        assert(r->action == PF_CHANGE_ADD_HEAD && r->ticket == 11);
        if (fail_at && rule_count + 1 == fail_at) { errno = EINVAL; return -1; }
        assert(rule_count < 32 && r->pool_ticket == 7 && !r->anchor[0] && !r->anchor_call[0]);
        rules[rule_count++] = *r; return 0;
    }
    if (command == DIOCDELETERULE) {
        struct pfioc_rule *r = argument;
        assert(!strcmp(r->rule.owner, OWNER) && !r->rule.ticket);
        delete_count++;
        if (delete_failure) { errno = EIO; return -1; }
        return 0;
    }
    if (command == DIOCSTOPREF) {
        struct pfioc_remove_token *t = argument; assert(t->token_value == 123);
        stopref_count++; return 0;
    }
    assert(!"Unexpected networking ioctl in host test"); return -1;
}
static void new_request(struct dhcp_packet *p, unsigned type, unsigned client) {
    memset(p, 0, sizeof(*p)); p->op = 1; p->htype = 1; p->hlen = 6;
    p->cookie = htonl(0x63825363u); p->xid = htonl(0x01234567);
    p->chaddr[0] = 2; p->chaddr[5] = client;
    p->options[0] = 53; p->options[1] = 1; p->options[2] = type; p->options[3] = 255;
}
static const uint8_t *option(const struct dhcp_packet *p, unsigned code, unsigned expected) {
    for (unsigned i = 0; i < sizeof(p->options);) {
        unsigned tag = p->options[i++]; if (tag == 255) break; if (!tag) continue;
        unsigned n = p->options[i++]; assert(i + n <= sizeof(p->options));
        if (tag == code) { assert(n == expected); return p->options + i; } i += n;
    }
    assert(!"missing reply option"); return NULL;
}
static void assert_u32(const uint8_t *p, uint32_t value) { uint32_t got; memcpy(&got, p, 4); assert(got == value); }
/* Independent checksum validation over the complete packet, including the
 * encoded checksum; UDP pseudo-header is concatenated as actual bytes. */
static void valid_checksum(const uint8_t *bytes, size_t size) {
    uint64_t sum = 0;
    for (size_t n = 0; n < size; n++) sum += (uint64_t)bytes[n] << (n % 2 ? 0 : 8);
    while (sum > 65535) sum = (sum & 65535) + (sum >> 16);
    assert(sum == 65535);
}
static void check_frame(const struct downstream *d, const struct dhcp_packet *reply,
    size_t size, unsigned type, uint32_t ciaddr) {
    uint8_t wire[1600], check[1600], mac[6] = {2, 3, 4, 5, 6, 7};
    size_t n = dhcp_frame(wire, sizeof(wire), d, mac, reply, size, type, ciaddr);
    assert(n == 42 + size && !memcmp(wire + 6, mac, 6));
    assert(wire[12] == 8 && !wire[13]);
    bool broadcast = type == 6 || !ciaddr;
    assert(!memcmp(wire, broadcast ? "\xff\xff\xff\xff\xff\xff" : (const char *)reply->chaddr, 6));
    assert_u32(wire + 26, d->gateway);
    assert_u32(wire + 30, broadcast ? htonl(INADDR_BROADCAST) : ciaddr);
    assert(wire[34] == 0 && wire[35] == 67 && wire[36] == 0 && wire[37] == 68);
    assert(((unsigned)wire[16] << 8 | wire[17]) == 28 + size);
    assert(((unsigned)wire[38] << 8 | wire[39]) == 8 + size);
    assert(!memcmp(wire + 42, reply, size));
    valid_checksum(wire + 14, 20);
    memcpy(check, wire + 26, 8); check[8] = 0; check[9] = 17;
    memcpy(check + 10, wire + 38, 2); memcpy(check + 12, wire + 34, 8 + size);
    valid_checksum(check, 20 + size);
    assert(!dhcp_frame(wire, n - 1, d, mac, reply, size, type, ciaddr));
    assert(!dhcp_frame(wire, sizeof(wire), d, mac, reply, sizeof(*reply) + 1, type, ciaddr));
}
int main(void) {
    struct downstream d, wifi;
    assert(config_row(&d, "usb-on en2 172.16.42.1/24"));
    assert(config_row(&wifi, "wifi-on en3 192.168.2.1/24"));
    assert(!config_row(&wifi, "wifi-on pdp_ip0 192.168.2.1/24"));
    assert(!config_row(&wifi, "../on en3 192.168.2.1/24"));
    assert(!config_row(&wifi, "wifi-on en3 8.8.8.1/24"));
    assert(!config_row(&wifi, "wifi-on en3 172.32.1.1/24"));
    assert(!config_row(&wifi, "wifi-on en3 172.16.1.2/24"));
    assert(!config_row(&wifi, "wifi-on en3 192.168.2.1/16"));
    assert(!config_row(&wifi, "wifi-on en3 192.168.2.1/24 extra"));
    assert(config_row(&wifi, "wifi-on en3 192.168.2.1/24"));

    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control = {0};
    struct msghdr msg = { .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_PKTINFO;
    cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo pkt = { .ipi_ifindex = 17 }; memcpy(CMSG_DATA(cm), &pkt, sizeof(pkt));
    assert(dhcp_received_index(&msg) == 17);
    msg.msg_flags = MSG_CTRUNC; assert(!dhcp_received_index(&msg));
    msg.msg_flags = MSG_TRUNC; assert(!dhcp_received_index(&msg));
    msg.msg_flags = 0; cm->cmsg_type = IP_TTL; assert(!dhcp_received_index(&msg));
    cm->cmsg_type = IP_PKTINFO; cm->cmsg_len--; assert(!dhcp_received_index(&msg));

    struct dhcp_packet p, reply; struct dhcp_options parsed;
    new_request(&p, 1, 1);
    assert(dhcp_parse(&p, 244, &parsed) && parsed.type == 1); /* option 53 first, regression */
    assert(!dhcp_parse(&p, 239, &parsed));
    assert(!dhcp_parse(&p, 243, &parsed)); /* no END */
    p.options[3] = 50; p.options[4] = 4;
    assert(!dhcp_parse(&p, 245, &parsed)); /* truncated option body */
    new_request(&p, 3, 1); p.options[3] = 50; p.options[4] = 4;
    uint32_t requested = ipv4("172.16.42.2"); memcpy(p.options + 5, &requested, 4);
    p.options[9] = 54; p.options[10] = 4; memcpy(p.options + 11, &d.gateway, 4); p.options[15] = 255;
    assert(dhcp_parse(&p, 256, &parsed) && parsed.type == 3 && parsed.requested == requested && parsed.server == d.gateway);
    p.options[15] = 53; p.options[16] = 1; p.options[17] = 3; p.options[18] = 255;
    assert(!dhcp_parse(&p, 259, &parsed)); /* duplicate message type */
    new_request(&p, 1, 1); p.options[0] = 0; p.options[1] = 53; p.options[2] = 1; p.options[3] = 1; p.options[4] = 255;
    assert(dhcp_parse(&p, 245, &parsed)); /* PAD */
    p.giaddr = requested; assert(!dhcp_parse(&p, 245, &parsed)); p.giaddr = 0;
    p.htype = 6; assert(!dhcp_parse(&p, 245, &parsed)); p.htype = 1;
    p.chaddr[0] = 1; assert(!dhcp_parse(&p, 245, &parsed));

    /* 64 simultaneous clients: unique addresses, pool exhaustion, stable renewal,
     * expiration reuse, requested-IP conflict, subnet isolation. */
    uint32_t addresses[MAX_LEASES];
    for (unsigned i = 0; i < MAX_LEASES; ++i) {
        new_request(&p, 1, i); struct lease *l = lease_find(&d, p.chaddr, 0, 1000);
        assert(l && pool_address(&d, l->address)); l->expires = 1300; addresses[i] = l->address;
        for (unsigned j = 0; j < i; ++j) assert(addresses[i] != addresses[j]);
    }
    new_request(&p, 1, MAX_LEASES); assert(!lease_find(&d, p.chaddr, 0, 1000));
    assert(!lease_find(&d, p.chaddr, addresses[0], 1000));
    new_request(&p, 3, 0); assert(lease_find(&d, p.chaddr, addresses[0], 1100)->address == addresses[0]);
    assert(!lease_find(&d, p.chaddr, addresses[1], 1100));
    assert(!lease_find(&wifi, p.chaddr, addresses[0], 1000));
    assert(lease_find(&wifi, p.chaddr, 0, 1000)->address == ipv4("192.168.2.2"));
    new_request(&p, 3, 100); assert(lease_find(&d, p.chaddr, addresses[0], 1301)->address == addresses[0]);
    assert(!pool_address(&d, d.gateway)); assert(!pool_address(&d, ipv4("172.16.42.66")));
    assert(!pool_address(&d, ipv4("172.16.42.255")));

    upstream.address = ipv4("10.8.0.2"); upstream.dns = ipv4("10.0.0.53"); upstream.mtu = 1410;
    new_request(&p, 3, 7);
    size_t size = dhcp_reply(&reply, &p, &d, requested, 5);
    assert(size == 300 && reply.xid == p.xid && reply.op == 2 && reply.yiaddr == requested);
    assert(!memcmp(reply.chaddr, p.chaddr, 16));
    assert(*option(&reply, 53, 1) == 5); assert_u32(option(&reply, 54, 4), d.gateway);
    assert_u32(option(&reply, 1, 4), htonl(0xffffff00u)); assert_u32(option(&reply, 3, 4), d.gateway);
    assert_u32(option(&reply, 6, 4), upstream.dns); assert_u32(option(&reply, 51, 4), htonl(300));
    assert_u32(option(&reply, 58, 4), htonl(150));
    uint16_t mtu; memcpy(&mtu, option(&reply, 26, 2), 2); assert(ntohs(mtu) == 1410);
    dhcp_reply(&reply, &p, &d, 0, 6); assert(!reply.yiaddr && *option(&reply, 53, 1) == 6);
    dhcp_reply(&reply, &p, &wifi, ipv4("192.168.2.2"), 2); assert_u32(option(&reply, 3, 4), wifi.gateway);

    for (unsigned type = 2; type <= 6; ++type) {
        if (type != 2 && type != 5 && type != 6) continue;
        size = dhcp_reply(&reply, &p, &wifi, ipv4("192.168.2.2"), (uint8_t)type);
        check_frame(&wifi, &reply, size, type, 0);
        check_frame(&wifi, &reply, size, type, ipv4("192.168.2.2"));
        check_frame(&wifi, &reply, size + 1, type, 0); /* odd UDP length */
    }

    downstream_count = 2; downstreams[0] = d; downstreams[1] = wifi;
    assert(pf_load(3) && rule_count == 10 && pool_count == 10 && pool_address_seen == upstream.address);
    for (unsigned i = 0; i < rule_count; ++i) {
        struct pf_rule *r = &rules[i].rule;
        assert(!strcmp(r->owner, OWNER) && r->af == AF_INET);
        struct downstream *down = &downstreams[i / 5];
        if (i % 5 == 0) {
            assert(r->action == PF_PASS && r->direction == PF_IN && !r->keep_state);
            assert(!strcmp(r->ifname, down->interface) && r->tagname[0]);
            assert(r->src.addr.v.a.addr.addr32[0] == (down->gateway & htonl(0xffffff00u)));
        } else if (i % 5 == 1) {
            assert(r->action == PF_NAT && r->direction == PF_OUT && !strcmp(r->ifname, UPSTREAM));
            assert(r->src.addr.v.a.mask.addr32[0] == htonl(0xffffff00u));
            assert(r->src.addr.v.a.addr.addr32[0] == (down->gateway & htonl(0xffffff00u)));
            assert(!strcmp(r->match_tagname, rules[i - 1].rule.tagname));
            assert(r->rpool.af == AF_INET && r->rpool.proxy_port[0] == 1024 && r->rpool.proxy_port[1] == 65535);
        } else if (i % 5 == 2) {
            assert(r->action == PF_PASS && r->direction == PF_OUT && r->keep_state == PF_STATE_NORMAL);
            assert(!strcmp(r->ifname, UPSTREAM) && !strcmp(r->match_tagname, rules[i - 2].rule.tagname));
        } else {
            assert(r->action == PF_SCRUB && r->proto == IPPROTO_TCP && r->max_mss == 1370);
            assert(!strcmp(r->ifname, down->interface));
            assert(r->direction == (i % 5 == 3 ? PF_IN : PF_OUT));
        }
    }
    assert(pf_remove() && delete_count == 1); /* owner-scoped, no global flush */
    rule_count = pool_count = 0; fail_at = 3;
    assert(!pf_load(1) && rule_count == 2); /* fail closed to caller, no silent success */

    /* Real teardown with intercepted OS calls: a pre-PF enable failure needs
     * no PF access; a partial rule load removes only the owner and its token;
     * a failed delete retains the recovery journal. No host mutation occurs. */
    memset(&saved, 0, sizeof(saved)); saved.forwarding = 0; pf_fd = -1;
    assert(restore_session() && delete_count == 1 && !stopref_count && unlink_count == 1);
    saved.rules_added = true; saved.token = 123; saved.forwarding = 1; pf_fd = 99;
    assert(restore_session() && delete_count == 2 && stopref_count == 1 && unlink_count == 2);
    pf_fd = 99; delete_failure = true;
    assert(!restore_session() && delete_count == 3 && stopref_count == 2 && unlink_count == 2);
    assert(socket_count == 3 && restore_forwarding_count == 3 && close_count == 5);

    downstreams[0] = wifi;
    for (unsigned failure = 0; failure <= 4; ++failure) {
        bpf_opens = bpf_step = 0; bpf_fail = failure;
        bpf_dlt = failure == 4 ? DLT_NULL : DLT_EN10MB;
        dhcp_tx[0] = -1; unsigned before = close_count;
        bool ok = open_dhcp_tx(0);
        assert(ok == (failure == 0) && bpf_opens == 2);
        assert(!memcmp(dhcp_mac[0], "\x02\x11\x22\x33\x44\x55", 6));
        if (ok) { assert(dhcp_tx[0] == 201 && bpf_step == 3); hotspot_test_close(dhcp_tx[0]); }
        else assert(dhcp_tx[0] == -1 && errno == (failure == 4 ? EPROTONOSUPPORT : EIO));
        assert(close_count == before + 1); /* every failure closes the BPF fd */
    }
    dhcp_tx[0] = -1;

    uint32_t rng = 1;
    for (unsigned iteration = 0; iteration < 100000; ++iteration) {
        new_request(&p, 1, 1);
        for (unsigned i = 0; i < sizeof(p.options); ++i) { rng = rng * 1664525u + 1013904223u; p.options[i] = rng >> 24; }
        (void)dhcp_parse(&p, 240 + iteration % 313, &parsed);
    }
    printf("PASS DHCP BPF frames/checksums/setup/failure, wire/options/NAK, 64 leases/exhaustion/renewal, subnet isolation, two-interface PF NAT/tag/state/MSS/cleanup/failure, 100000 malformed packets (ASan/UBSan); PF ABI rule=%zu ioctl=%zu\n", sizeof(struct pf_rule), sizeof(struct pfioc_rule));
}
'''
with tempfile.TemporaryDirectory(prefix='umi-hotspot-test-') as tmp:
    c = Path(tmp) / 'test.c'
    executable = Path(tmp) / 'test'
    c.write_text(harness)
    subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(source.parent),
                    '-idirafter', str(headers), '-idirafter', str(kernel),
                    '-framework', 'CoreFoundation', '-framework', 'SystemConfiguration',
                    str(c), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
plist = plistlib.loads((root / 'userland/com.hakios.hotspot.plist').read_bytes())
assert plist['ProgramArguments'] == ['/redwood/bin/hakios-hotspot']
assert plist['ExitTimeOut'] == 10
print('PASS launchd plist')
