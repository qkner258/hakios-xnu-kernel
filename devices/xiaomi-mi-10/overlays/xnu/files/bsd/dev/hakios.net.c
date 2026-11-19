/*
 * en0: a network interface that is Ethernet as far as the rest of the kernel and Darwin can tell.
 *
 * Darwin expects its interfaces from IONetworkingFamily, which this kernel does not have; the BSD
 * half of the kernel does not need it. The interface is made with the kernel's own interface
 * KPI, the way bsd/net/if_fake.c makes its "feth": family Ethernet, the stock Ethernet framing
 * and demultiplexing, one function that is given a frame to send and one call to hand a frame
 * that has arrived to the stack. What Darwin's user space makes of an interface that has no
 * IOKit object behind it is another matter: see userland/src/net_test.m.
 *
 * What carries the frames is a link driver's business; sys/hakios.net.h is all that the two
 * know of each other. The first link is QEMU's virtio network device (hakios.virtio_net.c).
 * There can be two interfaces: en0 for the Wi-Fi (or virtio), en1 for the modem's packets
 * through the IPA (hakios.cell.c); each has its link, its state and its thread.
 */
#include <kern/clock.h>
#include <sys/hakios.lx.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/hakios.net.h>
#include <sys/sockio.h>
#include <sys/systm.h>

extern void HakiosWLANStart(const char *interface, const uint8_t *address);  /* drivers/wifi/hakios.wlan.cpp */
extern void HakiosWLANAPStart(const uint8_t *address);
extern void HakiosWLANAPStop(void);

static LCK_GRP_DECLARE(net_lck_grp, "hakios-net");

/* One interface: enN on its link. */
struct net_if {
	unsigned        unit;
	const struct hakios_net_link *link;
	bool            up;             /* the link says frames can pass */
	bool            stop, thread_running, detaching;
	ifnet_t         ifp;
	lck_mtx_t       tx_lock;        /* one frame at a time to the link */
	/* What has gone through: said once a minute while anything moves. */
	struct {
		uint64_t sent, received, sent_bytes, received_bytes, dropped_out, dropped_in;
	} count;
};
static struct net_if net_ifs[HAKIOS_NET_UNITS];

static inline struct net_if *
net_if_of(ifnet_t ifp)
{
	return ifnet_softc(ifp);
}

/* The interface on unit `unit`, once attached; NULL before. */
static struct net_if *
net_if_unit(unsigned unit)
{
	return unit < HAKIOS_NET_UNITS && net_ifs[unit].ifp != NULL ? &net_ifs[unit] : NULL;
}

/*
 * Boot-arg net_trace=N: a line in the log for each of the first N frames, either way.
 * Boot-arg net_names=1: a line for every name asked of a DNS server and for every TCP connection
 * begun from here -- which is how to see what a system that has just been given a network does
 * with it (up to NAMES_MAX lines).
 */
#define NAMES_MAX       3000
static uint32_t net_trace, net_names;

static void
trace_frame(const char *way, const uint8_t *frame, size_t length, size_t whole)
{
	static uint32_t said, names;
	const uint8_t *end = frame + length, *ip = frame + ETHER_HDR_LEN, *l4;
	uint16_t type;
	bool ports;

	if (length < ETHER_HDR_LEN + 28 || (said >= net_trace && (!net_names || names >= NAMES_MAX))) {
		return;
	}
	type = (uint16_t)(frame[12] << 8 | frame[13]);
	l4 = ip + 4 * (ip[0] & 0xf);
	ports = type == ETHERTYPE_IP && (ip[9] == IPPROTO_TCP || ip[9] == IPPROTO_UDP) && l4 + 4 <= end;
	if (net_names && names < NAMES_MAX && ports && way[0] == 'o') {
		if (ip[9] == IPPROTO_UDP && (l4[2] << 8 | l4[3]) == 53 && l4 + 8 + 12 < end) {
			char name[128];
			const uint8_t *label = l4 + 8 + 12;
			size_t n = 0;

			/* the question: lengths and labels, up to a zero */
			while (label < end && *label != 0 && *label < 64 && label + 1 + *label < end && n + *label + 2 < sizeof(name)) {
				memcpy(name + n, label + 1, *label);
				n += *label;
				name[n++] = '.';
				label += 1 + *label;
			}
			name[n ? n - 1 : 0] = 0;
			names++;
			printf("net: asks for the name %s (type %u)\n", name, label + 2 < end ? label[1] << 8 | label[2] : 0);
		} else if (ip[9] == IPPROTO_TCP && l4 + 14 <= end && (l4[13] & 0x12) == 0x02) {
			names++;
			printf("net: connects to %u.%u.%u.%u port %u\n", ip[16], ip[17], ip[18], ip[19], l4[2] << 8 | l4[3]);
		}
	}
	if (said >= net_trace) {
		return;
	}
	said++;
	if (type == ETHERTYPE_IP) {
		printf("net: %s %zu bytes: IPv4 %s %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u%s\n", way, whole, ip[9] == IPPROTO_TCP ? "TCP" :
		    ip[9] == IPPROTO_UDP ? "UDP" : ip[9] == IPPROTO_ICMP ? "ICMP" : "protocol?", ip[12], ip[13], ip[14], ip[15],
		    ports ? l4[0] << 8 | l4[1] : 0, ip[16], ip[17], ip[18], ip[19], ports ? l4[2] << 8 | l4[3] : 0,
		    ip[9] == IPPROTO_TCP && ports && l4 + 14 <= end ? (l4[13] & 2 ? " SYN" : l4[13] & 1 ? " FIN" : l4[13] & 4 ? " RST" : "") : "");
	} else {
		printf("net: %s %zu bytes: %s (type %#x) to %02x:%02x:%02x:%02x:%02x:%02x\n", way, whole, type == ETHERTYPE_ARP ? "ARP" :
		    type == ETHERTYPE_IPV6 ? "IPv6" : "?", type, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
	}
}

/* ... of a frame on its way out: the start of it is enough */
static void
trace_packet(mbuf_t packet, size_t length)
{
	uint8_t start[192];
	size_t have = length < sizeof(start) ? length : sizeof(start);

	if ((net_trace || net_names) && mbuf_copydata(packet, 0, have, start) == 0) {
		trace_frame("out", start, have, length);
	}
}

/* ---- the interface -------------------------------------------------------------------------------- */

enum { AP_BSD_RX, AP_BSD_NOIF, AP_BSD_LENGTH, AP_BSD_ALLOC, AP_BSD_COPY,
    AP_BSD_SETDATA, AP_BSD_INPUT, AP_BSD_REJECT, AP_BSD_TX, AP_BSD_TX_DOWN,
    AP_BSD_TX_LENGTH, AP_BSD_TX_QUEUE, AP_BSD_TX_DROP, AP_BSD_COUNT };
static uint64_t ap_bsd_count[AP_BSD_COUNT];
static void
ap_bsd_event(unsigned event, size_t length, int error)
{
	static const char *names[] = { "rx", "no-interface", "rx-length", "rx-alloc", "rx-copy",
	    "rx-setdata", "input", "input-reject", "tx", "tx-down", "tx-length", "tx-queued", "tx-drop" };
	uint64_t count = os_atomic_inc_orig(&ap_bsd_count[event], relaxed) + 1;
	if (count <= 8 || !(count & (count - 1)))
		printf("hotspot-ap: bsd stage=%s count=%llu bytes=%zu error=%d\n", names[event], count, length, error);
}

/* The stack has a frame to send (a chain of mbufs that starts with the Ethernet header). */
static errno_t
net_output(ifnet_t ifp, mbuf_t packet)
{
	struct net_if *n = net_if_of(ifp);
	size_t length = mbuf_pkthdr_len(packet);
	bool sent = false;
	if (n->unit == 3) {
		ap_bsd_event(AP_BSD_TX, length, 0);
		if (!n->up) ap_bsd_event(AP_BSD_TX_DOWN, length, ENETDOWN);
		if (length > HAKIOS_NET_FRAME_MAX) ap_bsd_event(AP_BSD_TX_LENGTH, length, EMSGSIZE);
	}

	if (length <= HAKIOS_NET_FRAME_MAX && n->up) {
		bpf_tap_out(ifp, DLT_EN10MB, packet, NULL, 0);
		trace_packet(packet, length);
		lck_mtx_lock(&n->tx_lock);
		sent = n->link->transmit(packet, length);
		lck_mtx_unlock(&n->tx_lock);
	}
	if (sent) {
		n->count.sent++;
		n->count.sent_bytes += length;
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		n->count.dropped_out++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	if (n->unit == 3) {
		ap_bsd_event(sent ? AP_BSD_TX_QUEUE : AP_BSD_TX_DROP, length, sent ? 0 : ENOBUFS);
		return sent ? 0 : ENOBUFS;
	}
	return 0;
}

int
hakios_net_address_unit(unsigned unit, uint8_t address[6])
{
	struct net_if *n = net_if_unit(unit);
	return n ? ifnet_lladdr_copy_bytes(n->ifp, address, 6) : ENXIO;
}

/* Called after the stack has drained its references, including the DELETE ioctl.
 * No radio producer remains at this point; the AP worker is stopped first. */
static void
net_ap_detached(ifnet_t ifp)
{
	struct net_if *n = net_if_of(ifp);
	if (n->unit != 3) return;
	n->ifp = NULL;
	ifnet_release(ifp);
	lck_mtx_destroy(&n->tx_lock, &net_lck_grp);
	n->link = NULL;
	os_atomic_store(&n->detaching, false, release);
}

int
hakios_net_ap_detach(void)
{
	struct net_if *n = net_if_unit(3);
	if (!n || os_atomic_load(&n->detaching, acquire)) return 0;
	n->up = false;
	os_atomic_store(&n->stop, true, release);
	while (os_atomic_load(&n->thread_running, acquire))
		delay_for_interval(1, NSEC_PER_MSEC);
	lx_os_ap_trace("net:unpublish-enter", 0, 0);
	HakiosWLANAPStop();
	lx_os_ap_trace("net:unpublish-exit", 0, 0);
	os_atomic_store(&n->detaching, true, release);
	lx_os_ap_trace("net:detach-enter", 0, 0);
	int error = ifnet_detach(n->ifp);
	lx_os_ap_trace("net:detach-exit", 0, error);
	if (error) os_atomic_store(&n->detaching, false, release);
	return error;
}

/* UMI USB diagnostics only; interfaces 0/1 keep the common driver path.
 * ifnet_input takes ownership even on error. Never inspect its mbuf afterward. */
extern void hakios_tl(const char *, ...) __printflike(1, 2);
static struct {
    uint64_t calls, no_if, invalid, alloc, copy, setdata, accepted, rejected;
    uint64_t demux_ok, demux_error, arp, ipv4, ipv6, other_dst;
    uint64_t arp_error, ipv4_error, ipv6_error;
    unsigned dumps;
    int last_input, last_demux;
} usb_rx_diag;

static errno_t
usb_net_demux(ifnet_t ifp, mbuf_t packet, char *header, protocol_family_t *family)
{
    const uint8_t *h = (const uint8_t *)header;
    unsigned type = (unsigned)h[12] << 8 | h[13];
    usb_rx_diag.arp += type == ETHERTYPE_ARP;
    usb_rx_diag.ipv4 += type == ETHERTYPE_IP;
    usb_rx_diag.ipv6 += type == ETHERTYPE_IPV6;
    uint8_t own[6];
    if (!(h[0] & 1) && !ifnet_lladdr_copy_bytes(ifp, own, sizeof(own)) && memcmp(h, own, 6))
        ++usb_rx_diag.other_dst; /* ether_demux marks these M_PROMISC */
    errno_t error = ether_demux(ifp, packet, header, family);
    usb_rx_diag.last_demux = error;
    if (error) ++usb_rx_diag.demux_error; else ++usb_rx_diag.demux_ok;
    if (error) {
        usb_rx_diag.arp_error += type == ETHERTYPE_ARP;
        usb_rx_diag.ipv4_error += type == ETHERTYPE_IP;
        usb_rx_diag.ipv6_error += type == ETHERTYPE_IPV6;
    }
    if (type == ETHERTYPE_ARP && usb_rx_diag.arp <= 8) {
        uint8_t a[28];
        int rc = mbuf_copydata(packet, 0, sizeof(a), a);
        if (!rc) hakios_tl("[usb-net] ARP #%llu demux=%d hrd/pro=%02x%02x/%02x%02x hln/pln=%u/%u op=%u sha=%02x:%02x:%02x:%02x:%02x:%02x spa=%u.%u.%u.%u tpa=%u.%u.%u.%u\n",
            usb_rx_diag.arp, error, a[0], a[1], a[2], a[3], a[4], a[5], a[6] << 8 | a[7],
            a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15], a[16], a[17], a[24], a[25], a[26], a[27]);
        else hakios_tl("[usb-net] ARP #%llu demux=%d copy=%d len=%zu\n",
            usb_rx_diag.arp, error, rc, mbuf_pkthdr_len(packet));
    }
    return error;
}

void
hakios_net_usb_diagnostic(void)
{
    struct net_if *n = net_if_unit(2);
    if (!n) { hakios_tl("[usb-net] en2 absent calls=%llu no-if=%llu\n", usb_rx_diag.calls, usb_rx_diag.no_if); return; }
    uint8_t mac[6] = {0};
    int mac_error = ifnet_lladdr_copy_bytes(n->ifp, mac, sizeof(mac));
    hakios_tl("[usb-net] flags=%#x UP=%u RUNNING=%u link=%u mac=%02x:%02x:%02x:%02x:%02x:%02x mac-rc=%d\n",
        ifnet_flags(n->ifp), !!(ifnet_flags(n->ifp) & IFF_UP), !!(ifnet_flags(n->ifp) & IFF_RUNNING), n->up,
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],mac_error);
    hakios_tl("[usb-net] input calls=%llu accepted=%llu rejected=%llu last=%d no-if=%llu invalid=%llu alloc=%llu copy=%llu setdata=%llu\n",
        usb_rx_diag.calls, usb_rx_diag.accepted, usb_rx_diag.rejected, usb_rx_diag.last_input,
        usb_rx_diag.no_if, usb_rx_diag.invalid, usb_rx_diag.alloc, usb_rx_diag.copy, usb_rx_diag.setdata);
    hakios_tl("[usb-net] demux ok=%llu error=%llu last=%d arp=%llu ipv4=%llu ipv6=%llu other-dst=%llu\n",
        usb_rx_diag.demux_ok, usb_rx_diag.demux_error, usb_rx_diag.last_demux,
        usb_rx_diag.arp, usb_rx_diag.ipv4, usb_rx_diag.ipv6, usb_rx_diag.other_dst);
    hakios_tl("[usb-net] demux-errors arp=%llu ipv4=%llu ipv6=%llu; output sent=%llu dropped=%llu\n",
        usb_rx_diag.arp_error, usb_rx_diag.ipv4_error, usb_rx_diag.ipv6_error,
        n->count.sent, n->count.dropped_out);
    ifaddr_t *addresses = NULL;
    int error = ifnet_get_address_list_family(n->ifp, &addresses, AF_INET);
    unsigned count = 0;
    if (!error) {
        for (unsigned i = 0; addresses[i]; ++i) {
            struct sockaddr_in address;
            if (!ifaddr_address(addresses[i], (struct sockaddr *)&address, sizeof(address))) {
                const uint8_t *a = (const uint8_t *)&address.sin_addr;
                hakios_tl("[usb-net] IPv4=%u.%u.%u.%u\n", a[0],a[1],a[2],a[3]); ++count;
            }
        }
        ifnet_free_address_list(addresses);
    }
    if (!count) hakios_tl("[usb-net] no IPv4 address list-rc=%d\n", error);
}

/* A frame has arrived on unit `unit`. */
void
hakios_net_input_unit(unsigned unit, const uint8_t *frame, size_t length)
{
	struct net_if *n = net_if_unit(unit);
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	bool usb = unit == 2;
	if (unit == 3) ap_bsd_event(AP_BSD_RX, length, 0);
	if (usb) ++usb_rx_diag.calls;
	if (n == NULL) {
		if (unit == 3) ap_bsd_event(AP_BSD_NOIF, length, ENXIO);
		if (usb) ++usb_rx_diag.no_if;
		return;
	}
	trace_frame("in ", frame, length, length);
	if (length < ETHER_HDR_LEN || length > HAKIOS_NET_FRAME_MAX) {
		if (unit == 3) ap_bsd_event(AP_BSD_LENGTH, length, EMSGSIZE);
		if (usb) ++usb_rx_diag.invalid;
		n->count.dropped_in++; return;
	}
	if (mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		if (unit == 3) ap_bsd_event(AP_BSD_ALLOC, length, ENOBUFS);
		if (usb) ++usb_rx_diag.alloc;
		n->count.dropped_in++; return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		if (unit == 3) ap_bsd_event(AP_BSD_COPY, length, ENOBUFS);
		if (usb) ++usb_rx_diag.copy;
		mbuf_freem(packet); n->count.dropped_in++; return;
	}
	/* Same framing contract as XNU if_fake.c: L2 header in headroom,
	 * m_data at the network header, packet length excludes ETHER_HDR_LEN. */
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, n->ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	errno_t error = mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	if (error) {
		if (unit == 3) ap_bsd_event(AP_BSD_SETDATA, length, error);
		if (usb) ++usb_rx_diag.setdata;
		mbuf_freem(packet); n->count.dropped_in++; return;
	}
	/* Snapshot the ACTUAL copied mbuf, not the DMA source, before ownership
	 * transfers. Print this saved copy AFTER ifnet_input, never dereference it. */
	bool dump = usb && usb_rx_diag.dumps < 3;
	char hex[64 * 3 + 1];
	size_t have = length < 64 ? length : 64, plen = 0, mlen = 0;
	bool exact = false, rcvif = false, hdr = false;
	int snapshot_error = 0;
	if (dump) {
		uint8_t saved[64]; static const char digits[] = "0123456789abcdef";
		memcpy(saved, header, ETHER_HDR_LEN);
		snapshot_error = mbuf_copydata(packet, 0, have - ETHER_HDR_LEN, saved + ETHER_HDR_LEN);
		exact = chunks == 1 && !memcmp(header, frame, length);
		plen = mbuf_pkthdr_len(packet); mlen = mbuf_len(packet);
		rcvif = mbuf_pkthdr_rcvif(packet) == n->ifp;
		hdr = mbuf_pkthdr_header(packet) == (char *)mbuf_data(packet) - ETHER_HDR_LEN;
		if (snapshot_error) have = ETHER_HDR_LEN;
		for (size_t i = 0; i < have; ++i) {
			hex[3*i] = digits[saved[i] >> 4]; hex[3*i+1] = digits[saved[i] & 15]; hex[3*i+2] = ' ';
		}
		hex[3*have] = 0;
	}
	bpf_tap_in(n->ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	n->count.received++;
	n->count.received_bytes += length;
	error = ifnet_input(n->ifp, packet, &stats);
	if (unit == 3) ap_bsd_event(error ? AP_BSD_REJECT : AP_BSD_INPUT, length, error);
	if (usb) {
		usb_rx_diag.last_input = error;
		if (error) ++usb_rx_diag.rejected; else ++usb_rx_diag.accepted;
		if (dump) {
			++usb_rx_diag.dumps;
			hakios_tl("[usb-net] delivered #%u len=%zu pkthdr=%zu mlen=%zu rcvif=%u header=%u exact=%u input-rc=%d snapshot-rc=%d first%zu=%s\n",
			    usb_rx_diag.dumps, length, plen, mlen, rcvif, hdr, exact, error, snapshot_error, have, hex);
		}
	}
}

void
hakios_net_input(const uint8_t *frame, size_t length)
{
	hakios_net_input_unit(0, frame, length);
}

static errno_t
net_ioctl(ifnet_t ifp, u_long command, void *data)
{
	struct net_if *n = net_if_of(ifp);
	/* (a radio says it is one here too: IO80211.framework lists the interfaces whose media is 802.11) */
	const int media[] = { (n->link->wireless ? IFM_IEEE80211 : IFM_ETHER) | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *media_request = data;
	user_addr_t list;
	errno_t error = 0;

	if (n->link->wireless) lx_os_ap_trace("net-ioctl-enter", (unsigned)command, n->unit);

	switch (command) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
	case SIOCSIFDSTADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCDIFADDR:
		break;
	case SIOCSIFFLAGS:
		/* running whenever it is up: the link is there or it is not, nothing is switched on */
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		/* what configd asks to learn whether the cable is in */
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)media_request)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)media_request)->ifmu_ulist);
		media_request->ifm_active = media_request->ifm_current = media[0];
		media_request->ifm_mask = 0;
		media_request->ifm_status = IFM_AVALID | (n->up ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && media_request->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		media_request->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = ETHERMTU;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;                          /* the link hands us everything anyway */
	default:
		/* what Darwin's Wi-Fi side asks of a radio's interface: drivers/wifi/hakios.apple80211.c */
		error = n->link->wireless ? hakios_apple80211_ioctl(command, data) : EOPNOTSUPP;
		break;
	}
	if (n->link->wireless) lx_os_ap_trace("net-ioctl-exit", (unsigned)command, error);
	return error;
}

/* Tell whoever listens for kernel events (configd does) that the link is up, or down. */
static void
net_link_event(ifnet_t ifp, uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(ifp);
	about->if_unit = ifnet_unit(ifp);
	strlcpy(about->if_name, ifnet_name(ifp), IFNAMSIZ);
	ifnet_event(ifp, header);
}

void
hakios_net_link_state_unit(unsigned unit, bool up)
{
	struct net_if *n = net_if_unit(unit);

	if (n == NULL || up == n->up) {
		return;
	}
	n->up = up;
	printf("net: en%u's link (%s) is %s\n", n->unit, n->link->name, up ? "up" : "down");
	net_link_event(n->ifp, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}

void
hakios_net_link_state(bool up)
{
	hakios_net_link_state_unit(0, up);
}

/* The interface's own thread: for a link that wants to be asked, and for the minute's line in the log. */
static void
net_thread(void *parameter, __unused wait_result_t result)
{
	struct net_if *n = parameter;
	uint64_t said = 0, total;
	unsigned quiet = 0;

	for (unsigned tick = 1; !os_atomic_load(&n->stop, acquire); tick++) {
		quiet = n->link->poll != NULL && n->link->poll() ? 0 : quiet + 1;
		/* about once a minute, if anything has moved since */
		total = n->count.sent + n->count.received + n->count.dropped_out + n->count.dropped_in;
		if (tick % 8192 == 0 && total != said) {
			said = total;
			printf("net: en%u so far %llu frames sent (%llu bytes), %llu received (%llu bytes); dropped: %llu out, %llu in\n",
			    n->unit, n->count.sent, n->count.sent_bytes, n->count.received, n->count.received_bytes,
			    n->count.dropped_out, n->count.dropped_in);
		}
		delay_for_interval(quiet < 100 ? 2 : 10, NSEC_PER_MSEC);
	}
	os_atomic_store(&n->thread_running, false, release);
}

bool
hakios_net_attach(const struct hakios_net_link *link)
{
	unsigned unit = link->unit;
	struct net_if *n = unit < HAKIOS_NET_UNITS ? &net_ifs[unit] : NULL;
	char uniqueid[16];
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		/*
		 * The simple model: every frame straight to net_output(). And no Skywalk "nexus" made for
		 * the interface by itself: sockets are all that is asked of it so far.
		 */
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = uniqueid,
		.uniqueid_len = 11,
		.name = link->hostap ? "ap" : "en",
		.unit = link->hostap ? 1 : unit,
		.softc = n,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = link->wireless ? IFNET_SUBFAMILY_WIFI : IFNET_SUBFAMILY_ANY,
		.type = IFT_ETHER,
		.output = net_output,
		.demux = unit == 2 ? usb_net_demux : ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = net_ioctl,
		.detach = link->hostap ? net_ap_detached : NULL,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	const uint8_t *address = link->address;
	ifnet_t ifp = NULL;
	thread_t thread;
	errno_t error;

	if (n == NULL) {
		printf("net: there is no unit %u for %s\n", unit, link->name);
		return false;
	}
	if (os_atomic_load(&n->detaching, acquire)) return false;
	if (n->link != NULL) {
		printf("net: en%u is on %s already; %s is not attached\n", unit, n->link->name, link->name);
		return false;
	}
	snprintf(uniqueid, sizeof(uniqueid), link->hostap ? "hakios-ap%u" : "hakios-en%u", link->hostap ? 1 : unit);
	init.uniqueid_len = (uint32_t)strlen(uniqueid) + 1;
	n->unit = unit;
	n->link = link;
	n->up = link->up;
	n->stop = false; os_atomic_store(&n->detaching, false, release);
	lck_mtx_init(&n->tx_lock, &net_lck_grp, LCK_ATTR_NULL);
	if (link->hostap) lx_os_ap_trace("net:allocate-enter", 0, 0);
	error = ifnet_allocate_extended(&init, &ifp);
	if (link->hostap) lx_os_ap_trace("net:allocate-exit", 0, error);
	if (error != 0) {
		printf("net: the interface en%u cannot be made: error %d\n", unit, error);
		n->link = NULL;
		lck_mtx_destroy(&n->tx_lock, &net_lck_grp);
		return false;
	}
	ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(ifp, 100 * 1000 * 1000);
	ifnet_set_mtu(ifp, ETHERMTU);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
	ifnet_set_offload(ifp, 0);
	if (link->hostap) lx_os_ap_trace("net:attach-enter", 0, 0);
	error = ifnet_attach(ifp, NULL);
	if (link->hostap) lx_os_ap_trace("net:attach-exit", 0, error);
	if (error != 0) {
		printf("net: the interface en%u cannot be attached: error %d\n", unit, error);
		ifnet_release(ifp);
		n->link = NULL;
		lck_mtx_destroy(&n->tx_lock, &net_lck_grp);
		return false;
	}
	ifnet_set_lladdr(ifp, address, ETHER_ADDR_LEN);
	bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
	n->ifp = ifp;
	if (n->up) {
		net_link_event(ifp, KEV_DL_LINK_ON);
	}
	if (link->wireless) {
		/* (a radio's address is the phone's own: not for the log; and Darwin's Wi-Fi side finds the interface by a service) */
		printf("net: %s%u attached on %s (a radio), link %s\n", link->hostap ? "ap" : "en", link->hostap ? 1 : unit, link->name, n->up ? "up" : "down");
		if (link->hostap) {
			lx_os_ap_trace("net:publish-enter", 0, 0);
			HakiosWLANAPStart(address);
			lx_os_ap_trace("net:publish-exit", 0, 0);
		}
		else HakiosWLANStart(uniqueid + 7, address);
	} else {
		printf("net: en%u attached on %s, address %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", unit, link->name, address[0],
		    address[1], address[2], address[3], address[4], address[5], n->up ? "up" : "down");
	}
	if (link->hostap) return true; /* AP has no poll method; radio worker logs counters. */
	os_atomic_store(&n->thread_running, true, release);
	if (kernel_thread_start(net_thread, n, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else os_atomic_store(&n->thread_running, false, release);
	return true;
}

void
hakios_net_init(void)
{
	PE_parse_boot_argn("net_trace", &net_trace, sizeof(net_trace));
	PE_parse_boot_argn("net_names", &net_names, sizeof(net_names));
	hakios_virtio_net_init();
}
