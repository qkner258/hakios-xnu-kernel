#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "coachz_bb_qmux.h"
#include "coachz_bb_channels.h"
#include "coachz_bb_pdp.h"

struct socket {};
struct ifnet {};
struct proc {};
struct ifreq { uint32_t ifr_flags; };
typedef ifnet *ifnet_t;
typedef struct ifaddr *ifaddr_t;
typedef int errno_t;
typedef void *vnode_t;
typedef void *thread_call_t;
typedef void *thread_call_param_t;
enum { VREG = 1, SIOCSIFADDR = 1, SIOCSIFDSTADDR, SIOCAIFADDR, SIOCSIFADDR_IN6,
       SIOCSIFDSTADDR_IN6, SIOCSIFNETMASK_IN6, SIOCAIFADDR_IN6, SIOCSIFFLAGS,
       IFF_UP = 0x100, IFF_RUNNING = 0x200 };
static proc process;
static proc *kernproc = &process;
static bool switch_file;
static bool modem_switch;
static int switch_type = VREG;
static int vfs_context_kernel() { return 0; }
static int vnode_lookup(const char *path, int, vnode_t *v, int)
{
    const bool present = std::strstr(path, "modem-ims") ? modem_switch : switch_file;
    *v = &process;
    return present ? 0 : 2;
}
static int vnode_vtype(vnode_t) { return switch_type; }
static void vnode_put(vnode_t) {}
static void IOLockLock(void *) {}
static void IOLockUnlock(void *) {}
static void thread_call_enter(thread_call_t) {}
static bool thread_call_enter_delayed(thread_call_t, uint64_t) { return true; }
static void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *deadline) { *deadline = 0; }
static const uint32_t kMillisecondScale = 1000000;
static uint32_t if_flags;
static void ifnet_set_flags(ifnet_t, uint32_t value, uint32_t) { if_flags = value; }
static unsigned addr_calls;
extern "C" int in_control(struct socket *, u_long, caddr_t, ifnet *, proc *) { addr_calls++; return 17; }
extern "C" int in6_control(struct socket *, u_long, caddr_t, ifnet *, proc *) { addr_calls++; return 17; }
static void *bb_lock;
static cbq_bridge bb_qmux;
static cbp_client bb_pdp_clients[CBQ_MAX_CLIENTS];
static uint32_t bb_pdp_owners[CBQ_MAX_CLIENTS];
/* umi_ims.inc reads each pdp_ip's ifnet for its DAD interface-address candidates. By default the
 * host bridge has no addresses (the KPI stub returns ENXIO, so the snapshot stays empty, exactly
 * the pre-v29i behaviour); dad_ifnet()/dad_ifaddr_set() attach an interface and give it addresses
 * for the retry tests. */
struct bb_pdp_if { ifnet_t ifp; };
struct ifaddr { uint8_t addr[16]; uint8_t prefix; };
static bb_pdp_if bb_pdp_ifaces[CBP_INTERFACES];
static ifnet dad_ifnets[CBP_INTERFACES];
static ifaddr dad_ifaddrs[CBP_INTERFACES][2];   /* = IMS_DAD_MAX_IFADDR, checked below the include */
static uint32_t dad_ifaddr_n[CBP_INTERFACES];
static errno_t ifnet_get_address_list_family(ifnet_t ifp, ifaddr_t **list, int)
{
    static ifaddr_t lists[CBP_INTERFACES][3];
    *list = nullptr;
    for (uint32_t unit = 0; unit < CBP_INTERFACES; unit++) {
        if (ifp != &dad_ifnets[unit] || !dad_ifaddr_n[unit]) continue;
        for (uint32_t i = 0; i < dad_ifaddr_n[unit]; i++) lists[unit][i] = &dad_ifaddrs[unit][i];
        lists[unit][dad_ifaddr_n[unit]] = nullptr;
        *list = lists[unit];
        return 0;
    }
    return 2;   /* ENXIO: no address on this interface */
}
static errno_t ifaddr_address(ifaddr_t ifa, struct sockaddr *sa, unsigned len)
{
    if (!ifa || len < sizeof(struct sockaddr_in6)) return 2;
    struct sockaddr_in6 sin6 = {};
    sin6.sin6_len = sizeof sin6;
    sin6.sin6_family = AF_INET6;
    memcpy(sin6.sin6_addr.s6_addr, ifa->addr, 16);
    memcpy(sa, &sin6, sizeof sin6);
    return 0;
}
static errno_t ifaddr_netmask(ifaddr_t ifa, struct sockaddr *sa, unsigned len)
{
    if (!ifa || len < sizeof(struct sockaddr_in6)) return 2;
    struct sockaddr_in6 mask = {};
    mask.sin6_len = sizeof mask;
    mask.sin6_family = AF_INET6;
    uint8_t bits = ifa->prefix;
    for (unsigned k = 0; k < 16; k++) {
        mask.sin6_addr.s6_addr[k] = bits >= 8 ? 0xff : bits ? (uint8_t)(0xff00u >> bits) : 0x00;
        bits = bits >= 8 ? (uint8_t)(bits - 8) : 0;
    }
    memcpy(sa, &mask, sizeof mask);
    return 0;
}
static void ifnet_free_address_list(ifaddr_t *) {}
static std::vector<std::pair<uint32_t, std::string>> pushed;
static void bb_push(uint32_t channel, const uint8_t *data, uint32_t len)
{
    pushed.emplace_back(channel, std::string(reinterpret_cast<const char *>(data), len));
}
static uint32_t now;
static uint32_t bb_now_ms() { return now; }
static std::string log_lines;
static void hakios_tl(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    log_lines += line;
}
#define IMS_DAD_HOLD_ENABLE 1   /* (off in the kernel since v29k; the hold logic is still tested) */
#include "umi_ims.inc"
static_assert(IMS_DAD_MAX_IFADDR == 2, "dad_ifaddrs above follows IMS_DAD_MAX_IFADDR");

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

/* A QMI SDU (flags, transaction, message, length, TLVs) for the WDS lifecycle tests. */
struct sdu {
    uint8_t b[128] = {};
    uint32_t n = 7;
    sdu(uint8_t type, uint16_t txn, uint16_t message)
    {
        b[0] = type;
        umi_ims_wr16(b + 1, txn);
        umi_ims_wr16(b + 3, message);
    }
    sdu &tlv(uint8_t type, const uint8_t *value, uint16_t len)
    {
        b[n] = type;
        umi_ims_wr16(b + n + 1, len);
        memcpy(b + n + 3, value, len);
        n += 3u + len;
        umi_ims_wr16(b + 5, n - 7u);
        return *this;
    }
    sdu &tlv8(uint8_t type, uint8_t value) { return tlv(type, &value, 1); }
    sdu &result(uint32_t error = 0)
    {
        const uint8_t v[4] = { (uint8_t)(error ? 1 : 0), 0, (uint8_t)error, (uint8_t)(error >> 8) };
        return tlv(0x02, v, sizeof v);
    }
};

static int dad_slot;
static uint8_t dad_cid;

/* Bind mux `unit + 1` and start an IPv6 PDN the way CommCenter does: no APN string or family TLV
 * that the request-side heuristics can recognize. `modem`: route B, the modem owns IMS and the
 * Apple switch stays off. */
static void dad_begin(bool enabled, uint32_t unit, bool modem = false)
{
    cbq_init(&bb_qmux);
    memset(bb_pdp_clients, 0, sizeof bb_pdp_clients);
    memset(bb_pdp_owners, 0, sizeof bb_pdp_owners);
    memset(ims_owner, 0, sizeof ims_owner);
    memset(bb_pdp_ifaces, 0, sizeof bb_pdp_ifaces);
    memset(dad_ifaddr_n, 0, sizeof dad_ifaddr_n);
    bb_lock = &process;   // IOLockLock is a no-op here; non-NULL enables ims_dad_finish()/refresh
    pushed.clear();
    modem_ims_state(false);   // both owners off first: ims_dad_switch below must really reset
    ims_state(false);
    ims_dad_switch(false);
    ims_dad_call = &process;   // the bridge allocates it in umi_bb_publish
    ims_dad_hold_call = &process;   // and its own hold timer next to it
    if (modem) {
        modem_ims_state(true);
        CHECK(!ims_enabled && modem_ims_enabled && ims_owner_on());
    } else {
        ims_state(enabled);
    }
    dad_slot = cbq_client_alloc(&bb_qmux, 1, UMI_IMS_WDS);   // qmux2, like the IMS data context
    CHECK(dad_slot >= 0);
    dad_cid = bb_qmux.client[dad_slot].cid;
    bb_pdp_owners[dad_slot] = 1u + dad_cid + (1u << 8);
    const uint8_t mux = (uint8_t)(unit + 1u);
    sdu bind(0, 1, CBD_WDS_BIND_MUX_DATA_PORT);
    bind.tlv(0x11, &mux, 1);
    ims_pdp_observe(dad_slot, bind.b, bind.n);
    sdu bind_reply(2, 1, CBD_WDS_BIND_MUX_DATA_PORT);
    bind_reply.result();
    ims_pdp_observe(dad_slot, bind_reply.b, bind_reply.n);
    CHECK(bb_pdp_clients[dad_slot].mux == unit + 1u);
    // The real Start request has no APN string TLV (CommCenter selects the profile itself); the
    // family is recorded out of band by Set IP Family, as it is on the phone.
    sdu start(0, 2, UMI_IMS_START);
    start.tlv8(0x01, 0).tlv8(0x0e, 1).tlv8(0x1f, 1);
    ims_pdp_observe(dad_slot, start.b, start.n);
    const uint8_t handle[4] = { 0x34, 0x12, 0, 0 };
    sdu started(2, 2, UMI_IMS_START);
    started.result().tlv(0x01, handle, sizeof handle);
    ims_pdp_observe(dad_slot, started.b, started.n);
    sdu set_family(0, 9, CBP_FAMILY);
    set_family.tlv8(0x01, 6);
    ims_pdp_observe(dad_slot, set_family.b, set_family.n);
    sdu family_reply(2, 9, CBP_FAMILY);
    family_reply.result();
    ims_pdp_observe(dad_slot, family_reply.b, family_reply.n);
    CHECK(bb_pdp_clients[dad_slot].active && bb_pdp_clients[dad_slot].family == 6);
    CHECK(!ims_owner[dad_slot].bearer.selected);
}

/* Route B: /var/db/hakios-voice/modem-ims, no ims-on. */
static void dad_begin_route_b(uint32_t unit) { dad_begin(false, unit, true); }

/* The runtime-settings reply that carries the IPv6 fields. `pcscf_len` < 0 omits TLV 0x2e entirely
 * (the v29g fallback path); 1 is the internet PDN (empty P-CSCF list); > 1 is IMS. */
struct dad_cfg {
    const uint8_t *a25 = nullptr;   /* 17 bytes: 16-byte address + prefix */
    const uint8_t *a27 = nullptr;   /* 16 bytes (no prefix byte) */
    const uint8_t *a28 = nullptr;   /* 17 bytes */
    int pcscf_len = -1;
    uint8_t pad = 0;                /* bytes after the last TLV, past the SDU's declared length */
};

static sdu dad_settings_sdu(const dad_cfg &cfg)
{
    const uint8_t dns_metric[4] = { 0, 0, 0, 0 };
    sdu settings(2, 3, UMI_IMS_GET_SETTINGS);
    settings.result().tlv(0x1e, dns_metric, sizeof dns_metric);
    if (cfg.a25) settings.tlv(0x25, cfg.a25, 17);
    if (cfg.a27) settings.tlv(0x27, cfg.a27, 16);
    if (cfg.a28) settings.tlv(0x28, cfg.a28, 17);
    if (cfg.pcscf_len >= 0) {
        uint8_t pcscf[33] = {};                 /* count + up to two 16-byte P-CSCF addresses */
        if (cfg.pcscf_len > 1) pcscf[0] = 2u;
        settings.tlv(0x2e, pcscf, (uint16_t)cfg.pcscf_len);
    }
    if (cfg.pad != 0) {
        /* (the declared length stays at the last TLV's end: a real reply's padding is not part of it) */
        std::memset(settings.b + settings.n, 0, cfg.pad);
        settings.n += cfg.pad;
    }
    return settings;
}

static void dad_settings(const dad_cfg &cfg)
{
    sdu settings = dad_settings_sdu(cfg);
    ims_pdp_observe(dad_slot, settings.b, settings.n);
}

/* The same reply through the proxy's delivery path (umi_bb_qrtr): observed, then held or pushed.
 * Returns true when ims_dad_hold_response() took it. */
static bool dad_settings_through_proxy(const dad_cfg &cfg, uint32_t channel = CBH_QMUX2)
{
    sdu settings = dad_settings_sdu(cfg);
    ims_pdp_observe(dad_slot, settings.b, settings.n);
    uint8_t frame[CBQ_HDR + sizeof settings.b];
    cbq_frame_header(frame, settings.n, UMI_IMS_WDS, dad_cid);
    std::memcpy(frame + CBQ_HDR, settings.b, settings.n);
    if (ims_dad_hold_response(channel, dad_slot, settings.b, settings.n, frame, CBQ_HDR + settings.n)) return true;
    pushed.emplace_back(channel, std::string(reinterpret_cast<const char *>(frame), CBQ_HDR + settings.n));
    return false;
}

/* A runtime-settings reply whose only IPv6 field is TLV 0x25. */
static void dad_settings(const uint8_t *address17)
{
    dad_cfg cfg;
    cfg.a25 = address17;
    dad_settings(cfg);
    CHECK((bb_pdp_clients[dad_slot].settings & 2u) != 0);
}

/* A successful runtime-settings reply with no TLV 0x25 at all. */
static void dad_settings_no_v6()
{
    const uint8_t dns_metric[4] = { 0, 0, 0, 0 };
    sdu settings(2, 3, UMI_IMS_GET_SETTINGS);
    settings.result().tlv(0x1e, dns_metric, sizeof dns_metric);
    ims_pdp_observe(dad_slot, settings.b, settings.n);
    CHECK((bb_pdp_clients[dad_slot].settings & 2u) == 0);
}

/* How often `needle` appears in the log from offset `from` (tests append, never clear). */
static unsigned count_from(size_t from, const std::string &needle)
{
    unsigned n = 0;
    for (size_t at = log_lines.find(needle, from); at != std::string::npos; at = log_lines.find(needle, at + 1)) n++;
    return n;
}

/* A pushed 0x5561 frame: its WDS transaction and the 16-byte address + prefix in TLV 0x01. */
static uint16_t dad_txn(const std::string &frame)
{
    return cbq_rd16(reinterpret_cast<const uint8_t *>(frame.data()) + CBQ_HDR + 1);
}
static const uint8_t *dad_addr(const std::string &frame)
{
    return reinterpret_cast<const uint8_t *>(frame.data()) + CBQ_HDR + 10;
}

/* XNU's address ioctl ran on pdp_ip `unit`: recorded for the diagnostic line, never a gate. */
static void dad_install(uint32_t unit)
{
    ims_address_installed(unit);
    ims_dad_poll();
}

/* Attach pdp_ip `unit` to the KPI stub and (re)set what its address list returns. */
static void dad_ifnet(uint32_t unit) { bb_pdp_ifaces[unit].ifp = &dad_ifnets[unit]; }
static void dad_ifaddr_set(uint32_t unit, const uint8_t *addr, uint8_t prefix)
{
    memcpy(dad_ifaddrs[unit][0].addr, addr, 16);
    dad_ifaddrs[unit][0].prefix = prefix;
    dad_ifaddr_n[unit] = 1;
}

/* The thread-call / send deadline: a forced interface snapshot, then whatever is due. */
static void dad_finish(void)
{
    ims_dad_finish();
    CHECK(bb_lock != nullptr);
}

/* Does a CTL GetVersionInfo answer list `service`? */
static bool version_lists(const uint8_t *answer, uint32_t len, uint8_t service)
{
    uint32_t size = 0;
    const uint8_t *v = cbq_tlv(answer + CBQ_HDR + CBQ_CTL_HDR, len - CBQ_HDR - CBQ_CTL_HDR, 1, &size);
    if (!v || !size) return false;
    for (uint32_t i = 0; i < v[0] && 1u + 5u * (i + 1u) <= size; i++)
        if (v[1 + 5u * i] == service) return true;
    return false;
}

/* One QMUX frame for a QMI SDU (the bridge takes one frame per write); control flags 0: from the host. */
static std::string qmux_frame(uint8_t service, uint8_t cid, const sdu &s)
{
    uint8_t f[CBQ_HDR + sizeof s.b];
    cbq_frame_header(f, s.n, service, cid);
    f[3] = 0;
    memcpy(f + CBQ_HDR, s.b, s.n);
    return std::string(reinterpret_cast<const char *>(f), CBQ_HDR + s.n);
}

int main()
{
    uint8_t packet[16];
    uint32_t node = 0, port = 0;
    cbq_init(&bb_qmux);
    CHECK(!ims_enabled);
    CHECK(cbq_server_new(&bb_qmux, UMI_IMS_IMSA, 1, 3, 73));
    CHECK(!ims_poll(packet, &node, &port));
    switch_file = true;
    switch_type = 2;
    ims_check_switch(nullptr, nullptr);
    CHECK(!ims_switch_wanted);
    switch_type = VREG;
    ims_check_switch(nullptr, nullptr);
    ims_state(ims_switch_wanted);
    CHECK(ims_enabled && ims_audit.registration == -1);
    now = 1;
    CHECK(ims_poll(packet, &node, &port) == 14 && node == 3 && port == 73);
    CHECK(packet[0] == 0 && umi_ims_rd16(packet + 3) == 0x33 && umi_ims_rd16(packet + 5) == 7);
    CHECK(packet[7] == 0x10 && umi_ims_rd16(packet + 8) == 4);
    CHECK(!packet[10] && !packet[11] && !packet[12] && !packet[13]);
    const uint16_t txn = umi_ims_rd16(packet + 1);
    uint8_t bind[] = {2, 0, 0, 0x33, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
    uint8_t response[] = {2, 0, 0, 0x20, 0, 11, 0, 2, 4, 0, 0, 0, 0, 0, 0x10, 1, 0, 0};
    // A query reply cannot satisfy the preceding bind, even with the right txn.
    umi_ims_wr16(response + 1, txn);
    ims_receive(3, 73, response, sizeof response);
    CHECK(ims_audit.pending && !ims_audit.bound && !ims_bearer_allowed());
    umi_ims_wr16(bind + 1, txn + 1);
    ims_receive(3, 73, bind, sizeof bind);
    CHECK(ims_audit.pending);
    umi_ims_wr16(bind + 1, txn);
    ims_receive(4, 73, bind, sizeof bind);
    CHECK(ims_audit.pending);
    bind[10] = 1;
    bind[12] = 31;
    ims_receive(3, 73, bind, sizeof bind);
    CHECK(!ims_audit.pending && !ims_audit.bound && !ims_bearer_allowed());
    now += 4999;
    CHECK(!ims_poll(packet, &node, &port));
    now++;
    CHECK(ims_poll(packet, &node, &port) == 14);
    umi_ims_wr16(bind + 1, ims_audit.txn);
    bind[10] = bind[12] = 0;
    ims_receive(3, 73, bind, sizeof bind);
    CHECK(ims_audit.bound && !ims_audit.pending && !ims_bearer_allowed());
    CHECK(ims_poll(packet, &node, &port) == 7);
    CHECK(umi_ims_rd16(packet + 3) == 0x20 && !umi_ims_rd16(packet + 5));
    umi_ims_wr16(response + 1, txn + 1);
    ims_receive(3, 73, response, sizeof response);
    CHECK(ims_audit.pending && ims_audit.registration == -1);
    umi_ims_wr16(response + 1, ims_audit.txn);
    ims_receive(4, 73, response, sizeof response);
    CHECK(ims_audit.pending);
    ims_receive(3, 73, response, sizeof response);
    CHECK(!ims_audit.pending && ims_audit.registration == 0 && ims_bearer_allowed());
    now += 9999;
    CHECK(!ims_poll(packet, &node, &port));
    now++;
    CHECK(ims_poll(packet, &node, &port) == 7);
    now += 5000;
    CHECK(!ims_poll(packet, &node, &port));
    CHECK(!ims_audit.pending && !ims_audit.bound && ims_audit.registration == -1 && !ims_bearer_allowed());
    now += 5000;
    CHECK(ims_poll(packet, &node, &port) == 14);
    umi_ims_wr16(bind + 1, ims_audit.txn);
    ims_receive(3, 73, bind, sizeof bind);
    CHECK(ims_poll(packet, &node, &port) == 7);
    umi_ims_wr16(response + 1, ims_audit.txn);
    response[17] = 1;
    ims_receive(3, 73, response, sizeof response);
    CHECK(ims_audit.registration == 1 && !ims_bearer_allowed());
    response[17] = 0;
    now += 10000;
    CHECK(ims_poll(packet, &node, &port) == 7);
    umi_ims_wr16(response + 1, ims_audit.txn);
    ims_receive(3, 73, response, sizeof response);
    CHECK(ims_bearer_allowed());
    now += 14999;
    CHECK(ims_poll(packet, &node, &port) == 7 && ims_bearer_allowed());
    // Sending a new query must not refresh the age of the last successful one.
    now++;
    CHECK(!ims_bearer_allowed());
    ims_state(false);
    CHECK(!ims_poll(packet, &node, &port) && ims_audit.release && ims_audit.registration == -1);

    // DLIL sends this notification after protocol attachment with no payload.
    // It has already updated the interface flags before calling the driver.
    ifnet interface;
    if_flags = IFF_UP;
    CHECK(!ims_pdp_ioctl(&interface, SIOCSIFFLAGS, nullptr));
    CHECK(if_flags == IFF_UP);

    // Address notifications carry an ifaddr, not a userspace ifreq. The
    // protocol owner already processed the ioctl; never reenter it here.
    uint8_t opaque_address = 0;
    for (unsigned long cmd : {SIOCSIFADDR, SIOCSIFDSTADDR, SIOCAIFADDR,
            SIOCSIFADDR_IN6, SIOCSIFDSTADDR_IN6, SIOCSIFNETMASK_IN6, SIOCAIFADDR_IN6})
        CHECK(!ims_pdp_ioctl(&interface, cmd, &opaque_address));
    CHECK(addr_calls == 0);
    ifreq flags = {IFF_UP | IFF_RUNNING};
    CHECK(!ims_pdp_ioctl(&interface, SIOCSIFFLAGS, &flags));
    CHECK(if_flags == IFF_UP);
    CHECK(ims_pdp_ioctl(&interface, 0, nullptr) == -1);

    // Even an unrecognized response extension must never reach a log.
    const char secret[] = "PRIVATE-PAYLOAD-CANARY";
    uint8_t extended[64];
    memcpy(extended, response, sizeof response);
    extended[18] = 0x70;
    umi_ims_wr16(extended + 19, sizeof secret);
    memcpy(extended + 21, secret, sizeof secret);
    umi_ims_wr16(extended + 5, 14 + sizeof secret);
    ims_state(true);
    now += 10000;
    CHECK(ims_poll(packet, &node, &port) == 14);
    umi_ims_wr16(bind + 1, ims_audit.txn);
    ims_receive(3, 73, bind, sizeof bind);
    CHECK(ims_poll(packet, &node, &port) == 7);
    umi_ims_wr16(extended + 1, ims_audit.txn);
    ims_receive(3, 73, extended, 21 + sizeof secret);
    CHECK(log_lines.find(secret) == std::string::npos);
    CHECK(log_lines.find("audit timeout") != std::string::npos);
    CHECK(ims_bearer_allowed());
    cbq_server_del(&bb_qmux, UMI_IMS_IMSA, 3, 73);
    CHECK(cbq_server_new(&bb_qmux, UMI_IMS_IMSA, 1, 3, 74));
    CHECK(!ims_poll(packet, &node, &port));
    CHECK(!ims_audit.bound && !ims_bearer_allowed() && ims_audit.release);

    // IMSRTP (0x28): with the stub off (IMS off) the allocation is rejected exactly as before;
    // with it on the bridge allocates the client itself and never forwards it to the modem.
    uint8_t ctl[] = {1, 0, 0, 0, 0, 0, 0, 9, 0x22, 0, 4, 0, 1, 1, 0, 0x28};
    uint8_t answer[64];
    bb_qmux.imsrtp = 0;
    const cbq_result missing = cbq_ctl(&bb_qmux, 0, ctl + CBQ_HDR, sizeof ctl - CBQ_HDR, answer, sizeof answer);
    ims_note_ctl(ctl, sizeof ctl, answer, missing.len);
    CHECK(missing.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == CBQ_ERR_INVALID_SERVICE_TYPE);
    CHECK(log_lines.find("media client svc 0x28 allocation result 1 error 31") != std::string::npos);

    // The version list carries 0x28 only while the stub is on.
    uint8_t vctl[] = {1, 0, 0, 0, 0, 0, 0, 0x0b, 0x21, 0, 0, 0};
    cbq_result v = cbq_ctl(&bb_qmux, 0, vctl + CBQ_HDR, sizeof vctl - CBQ_HDR, answer, sizeof answer);
    CHECK(v.kind == CBQ_TO_HOST && !version_lists(answer, v.len, 0x28));
    bb_qmux.imsrtp = 1;
    v = cbq_ctl(&bb_qmux, 0, vctl + CBQ_HDR, sizeof vctl - CBQ_HDR, answer, sizeof answer);
    CHECK(v.kind == CBQ_TO_HOST && version_lists(answer, v.len, 0x28));
    // A modem that does announce 0x28 is listed once, not twice.
    CHECK(cbq_server_new(&bb_qmux, 0x28, 1, 3, 90));
    v = cbq_ctl(&bb_qmux, 0, vctl + CBQ_HDR, sizeof vctl - CBQ_HDR, answer, sizeof answer);
    {
        uint32_t size = 0, n28 = 0;
        const uint8_t *list = cbq_tlv(answer + CBQ_HDR + CBQ_CTL_HDR, v.len - CBQ_HDR - CBQ_CTL_HDR, 1, &size);
        for (uint32_t i = 0; list && i < list[0] && 1u + 5u * (i + 1u) <= size; i++) n28 += list[1 + 5u * i] == 0x28;
        CHECK(v.kind == CBQ_TO_HOST && n28 == 1);
    }
    cbq_server_del(&bb_qmux, 0x28, 3, 90);

    // GetClientId(0x28) is answered here with a local id (result 0, TLV 0x01 = service, cid), and
    // the lifecycle's one allocation line goes out; the same answer noted twice does not repeat it.
    const cbq_result local = cbq_ctl(&bb_qmux, 0, ctl + CBQ_HDR, sizeof ctl - CBQ_HDR, answer, sizeof answer);
    CHECK(local.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
    CHECK(answer[12 + 7] == 1 && cbq_rd16(answer + 12 + 8) == 2 && answer[12 + 10] == 0x28);
    const uint8_t rtp_cid = answer[12 + 11];
    ims_note_ctl(ctl, sizeof ctl, answer, local.len);
    CHECK(count_from(0, "imsrtp stub: client " + std::to_string(rtp_cid) + " allocated\n") == 1);
    CHECK(log_lines.find("media client svc 0x28 allocation result 0 error 0") != std::string::npos);
    ims_note_ctl(ctl, sizeof ctl, answer, local.len);
    CHECK(count_from(0, "imsrtp stub: client ") == 1);

    // INITIALIZE_REQ (0x0020): answered locally, same transaction, with only the response's
    // mandatory result TLV (rtp-wire.h: imsrtp_initialize_resp_msg_v01 = resp). That is what the
    // QMI RTP command driver needs to report its stack controller initialized.
    {
        sdu init(CBQ_QMI_REQUEST, 0x1234, 0x0020);
        const std::string frame = qmux_frame(0x28, rtp_cid, init);
        const cbq_result r = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r.kind == CBQ_PRIVATE && r.service == 0x28 && r.cid == rtp_cid);
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, init.b));
        CHECK(pushed.size() == 1 && pushed[0].first == 0);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(pushed[0].second.size() == CBQ_HDR + CBQ_SDU_HDR + 7u);
        CHECK(p[0] == CBQ_IF_TYPE && p[3] == CBQ_FLAG_SERVICE && p[4] == 0x28 && p[5] == rtp_cid);
        CHECK(p[CBQ_HDR] == CBQ_QMI_RESPONSE);
        CHECK(cbq_rd16(p + CBQ_HDR + 1) == 0x1234);            /* the request's transaction */
        CHECK(cbq_rd16(p + CBQ_HDR + 3) == 0x0020);
        CHECK(cbq_rd16(p + CBQ_HDR + 5) == 7);
        CHECK(p[CBQ_HDR + 7] == 0x02 && cbq_rd16(p + CBQ_HDR + 8) == 4);
        CHECK(cbq_rd16(p + CBQ_HDR + 10) == 0 && cbq_rd16(p + CBQ_HDR + 12) == 0);   /* result 0, error 0 */
        CHECK(count_from(0, "imsrtp stub: INITIALIZE ok\n") == 1);
        // A second INITIALIZE is answered again but logged once per client.
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, init.b));
        CHECK(pushed.size() == 1 && count_from(0, "imsrtp stub: INITIALIZE ok\n") == 1);
    }

    // Session messages are out of scope tonight: a well-formed QMI_ERR_NOT_SUPPORTED, once per
    // message id (0x0021 UNINITIALIZE, the INITIALIZE counterpart, succeeds instead).
    const uint16_t unsupported[] = { 0x0022, 0x0025, 0x0022 };
    for (uint16_t message : unsupported) {
        sdu sess(CBQ_QMI_REQUEST, 0x0077, message);
        const std::string frame = qmux_frame(0x28, rtp_cid, sess);
        const cbq_result r = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r.kind == CBQ_PRIVATE);
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, sess.b));
        CHECK(pushed.size() == 1);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(cbq_rd16(p + CBQ_HDR + 1) == 0x0077 && cbq_rd16(p + CBQ_HDR + 3) == message);
        CHECK(cbq_rd16(p + CBQ_HDR + 5) == 7);
        CHECK(cbq_rd16(p + CBQ_HDR + 10) == 1 && cbq_rd16(p + CBQ_HDR + 12) == CBQ_ERR_NOT_SUPPORTED);
        char want[64];
        snprintf(want, sizeof want, "imsrtp stub: msg 0x%04x answered unsupported\n", message);
        CHECK(count_from(0, want) == 1);
    }

    // umi_rtpok=1 (dev-only boot argument): the session lifecycle is answered result 0 and followed
    // by its indication, the event the driver's session state machine advances on, so SipStack can
    // send the INVITE (v29t INVESTIGATION-volte-call-1402 §6-B). No media: the bridge never touches
    // RTP, so nothing here reaches the modem. umi_rtpok=0 (the default) is the block above.
    {
        const size_t mark = log_lines.size();
        ims_rtpok_on = true;
        const uint8_t app_id = 0x77;
        const uint8_t cookie[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
        sdu sess(CBQ_QMI_REQUEST, 0x0090, 0x0022);
        sess.tlv(0x01, &app_id, 1).tlv8(0x02, 0x08).tlv8(0x03, 0x00).tlv(0x18, cookie, sizeof cookie);
        const std::string frame = qmux_frame(0x28, rtp_cid, sess);
        const cbq_result r = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r.kind == CBQ_PRIVATE && r.service == 0x28 && r.cid == rtp_cid);
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, sess.b));
        // Response first (result only), then the SESSION_INITIALIZE indication.
        CHECK(pushed.size() == 2 && pushed[0].first == 0 && pushed[1].first == 0);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(p[CBQ_HDR] == CBQ_QMI_RESPONSE);
        CHECK(cbq_rd16(p + CBQ_HDR + 1) == 0x0090 && cbq_rd16(p + CBQ_HDR + 3) == 0x0022);
        CHECK(cbq_rd16(p + CBQ_HDR + 5) == 7 && cbq_rd16(p + CBQ_HDR + 10) == 0 && cbq_rd16(p + CBQ_HDR + 12) == 0);
        const std::string &ind = pushed[1].second;
        const uint8_t *q = reinterpret_cast<const uint8_t *>(ind.data());
        CHECK(ind.size() == CBQ_HDR + CBQ_SDU_HDR + 22u);            /* status 7 + app 4 + session 4 + cookie 7 */
        CHECK(q[0] == CBQ_IF_TYPE && q[4] == 0x28 && q[5] == rtp_cid && q[CBQ_HDR] == CBQ_QMI_INDICATION);
        CHECK(cbq_rd16(q + CBQ_HDR + 1) == 0 && cbq_rd16(q + CBQ_HDR + 3) == 0x0022 && cbq_rd16(q + CBQ_HDR + 5) == 22);
        const uint8_t *t = q + CBQ_HDR + 7;
        CHECK(t[0] == 0x01 && cbq_rd16(t + 1) == 4 && cbq_rd16(t + 3) == 0 && cbq_rd16(t + 5) == 0);   /* status */
        CHECK(t[7] == 0x02 && cbq_rd16(t + 8) == 1 && t[10] == app_id);                                /* app_id */
        CHECK(t[11] == 0x10 && cbq_rd16(t + 12) == 1 && t[14] == 1);                                   /* first session id */
        CHECK(t[15] == 0x11 && cbq_rd16(t + 16) == 4 && memcmp(t + 18, cookie, sizeof cookie) == 0);   /* cookie echoed */
        CHECK(count_from(mark, "imsrtp stub: session 0x0022 answered ok (umi_rtpok)\n") == 1);
        const uint8_t session = t[14];

        // Configure (0x0024): session id echoed, app id from the request's TLV 0x40 into 0x12, cookie
        // from 0x23 into 0x11.
        {
            sdu conf(CBQ_QMI_REQUEST, 0x0091, 0x0024);
            conf.tlv8(0x01, session).tlv8(0x40, 0x05).tlv(0x23, cookie, sizeof cookie);
            const std::string f = qmux_frame(0x28, rtp_cid, conf);
            const cbq_result cr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(cr.kind == CBQ_PRIVATE);
            pushed.clear();
            CHECK(ims_rtp_answer(0, cr, conf.b));
            CHECK(pushed.size() == 2);
            const uint8_t *a = reinterpret_cast<const uint8_t *>(pushed[1].second.data());
            CHECK(cbq_rd16(a + CBQ_HDR + 3) == 0x0024 && cbq_rd16(a + CBQ_HDR + 5) == 22);
            const uint8_t *u = a + CBQ_HDR + 7;
            CHECK(u[0] == 0x01 && cbq_rd16(u + 3) == 0);
            CHECK(u[7] == 0x02 && cbq_rd16(u + 8) == 1 && u[10] == session);
            CHECK(u[11] == 0x12 && cbq_rd16(u + 12) == 1 && u[14] == 0x05);
            CHECK(u[15] == 0x11 && cbq_rd16(u + 16) == 4 && memcmp(u + 18, cookie, sizeof cookie) == 0);
            CHECK(count_from(mark, "imsrtp stub: session 0x0024 answered ok (umi_rtpok)\n") == 1);
        }
        // Start (0x0025) and stop (0x0026): status, session id, app id (TLV 0x11 in the request and
        // in the indication).
        const uint16_t calls[] = { 0x0025, 0x0026 };
        for (uint16_t message : calls) {
            sdu m(CBQ_QMI_REQUEST, 0x0092, message);
            m.tlv8(0x01, session).tlv8(0x11, 0x05);
            const std::string f = qmux_frame(0x28, rtp_cid, m);
            const cbq_result mr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(mr.kind == CBQ_PRIVATE);
            pushed.clear();
            CHECK(ims_rtp_answer(0, mr, m.b));
            CHECK(pushed.size() == 2);
            const uint8_t *a = reinterpret_cast<const uint8_t *>(pushed[1].second.data());
            CHECK(cbq_rd16(a + CBQ_HDR + 3) == message && cbq_rd16(a + CBQ_HDR + 5) == 15);
            const uint8_t *u = a + CBQ_HDR + 7;
            CHECK(u[0] == 0x01 && cbq_rd16(u + 3) == 0);
            CHECK(u[7] == 0x02 && cbq_rd16(u + 8) == 1 && u[10] == session);
            CHECK(u[11] == 0x11 && cbq_rd16(u + 12) == 1 && u[14] == 0x05);
            char want[64];
            snprintf(want, sizeof want, "imsrtp stub: session 0x%04x answered ok (umi_rtpok)\n", message);
            CHECK(count_from(mark, want) == 1);
        }
        // Uninitialize (0x0023): app id moves to TLV 0x10 in that indication (and in the request).
        {
            sdu m(CBQ_QMI_REQUEST, 0x0094, 0x0023);
            m.tlv8(0x01, session).tlv8(0x10, 0x05);
            const std::string f = qmux_frame(0x28, rtp_cid, m);
            const cbq_result mr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(mr.kind == CBQ_PRIVATE);
            pushed.clear();
            CHECK(ims_rtp_answer(0, mr, m.b));
            CHECK(pushed.size() == 2);
            const uint8_t *a = reinterpret_cast<const uint8_t *>(pushed[1].second.data());
            CHECK(cbq_rd16(a + CBQ_HDR + 3) == 0x0023 && cbq_rd16(a + CBQ_HDR + 5) == 15);
            const uint8_t *u = a + CBQ_HDR + 7;
            CHECK(u[7] == 0x02 && cbq_rd16(u + 8) == 1 && u[10] == session);
            CHECK(u[11] == 0x10 && cbq_rd16(u + 12) == 1 && u[14] == 0x05);
            CHECK(count_from(mark, "imsrtp stub: session 0x0023 answered ok (umi_rtpok)\n") == 1);
        }
        // A second initialize is a new session: the next id, no cookie TLV (optional), one log line
        // for the message id in total.
        {
            sdu again(CBQ_QMI_REQUEST, 0x0095, 0x0022);
            again.tlv(0x01, &app_id, 1);
            const std::string f = qmux_frame(0x28, rtp_cid, again);
            const cbq_result ar = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(ar.kind == CBQ_PRIVATE);
            pushed.clear();
            CHECK(ims_rtp_answer(0, ar, again.b));
            CHECK(pushed.size() == 2 && pushed[1].second.size() == CBQ_HDR + CBQ_SDU_HDR + 15u);
            const uint8_t *a = reinterpret_cast<const uint8_t *>(pushed[1].second.data());
            CHECK(cbq_rd16(a + CBQ_HDR + 5) == 15);
            CHECK(a[CBQ_HDR + 7 + 11] == 0x10 && a[CBQ_HDR + 7 + 14] == (uint8_t)(session + 1u));
            CHECK(count_from(mark, "imsrtp stub: session 0x0022 answered ok (umi_rtpok)\n") == 1);
        }
        // The per-session link settings that follow SESSION_START.
        // 0x0029 has an indication in the IDL and the driver's request timer is cancelled from it
        // (v29v 81187 kept a CONFIGURE_RTCP_REPORTS timer because only a response came back):
        // status + session id, app id from the request (0x12) into TLV 0x10.
        {
            const uint16_t message = 0x0029;
            sdu m(CBQ_QMI_REQUEST, 0x0096, message);
            const uint8_t enable = 1;
            m.tlv8(0x01, session).tlv8(0x02, enable).tlv8(0x12, 0x04);
            const std::string f = qmux_frame(0x28, rtp_cid, m);
            const cbq_result mr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(mr.kind == CBQ_PRIVATE);
            pushed.clear();
            CHECK(ims_rtp_answer(0, mr, m.b));
            CHECK(pushed.size() == 2);
            const uint8_t *a0 = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
            CHECK(a0[CBQ_HDR] == CBQ_QMI_RESPONSE && cbq_rd16(a0 + CBQ_HDR + 3) == message);
            CHECK(cbq_rd16(a0 + CBQ_HDR + 10) == 0 && cbq_rd16(a0 + CBQ_HDR + 12) == 0);
            const uint8_t *a = reinterpret_cast<const uint8_t *>(pushed[1].second.data());
            CHECK(a[CBQ_HDR] == CBQ_QMI_INDICATION && cbq_rd16(a + CBQ_HDR + 3) == message);
            CHECK(cbq_rd16(a + CBQ_HDR + 5) == 15);           /* status 7 + session 4 + app 4 */
            const uint8_t *u = a + CBQ_HDR + 7;
            CHECK(u[0] == 0x01 && cbq_rd16(u + 3) == 0);
            CHECK(u[7] == 0x02 && cbq_rd16(u + 8) == 1 && u[10] == session);
            CHECK(u[11] == 0x10 && cbq_rd16(u + 12) == 1 && u[14] == 0x04);
            CHECK(count_from(mark, "imsrtp stub: session 0x0029 answered ok (umi_rtpok)\n") == 1);
        }
        // 0x002A ConfigureRTPLinkMonitor / 0x002B ConfigureRTCPLinkMonitor: their IDL indication is a
        // link monitor EVENT (status enum carries 0x07 TX only / 0x08 RX only), not a config ack, and
        // the live v32 call turned each synthesized one into handleMediaSessionMediaError error=5/6
        // (kCallDisconnectionReasonMediaTimeout). Response alone, success, once per id, no indication.
        {
            const uint16_t monitors[] = { 0x002A, 0x002B };
            for (uint16_t message : monitors) {
                sdu m(CBQ_QMI_REQUEST, 0x0096, message);
                const uint8_t enable = message == 0x002A ? 1 : 0;
                m.tlv8(0x01, session).tlv8(0x02, enable).tlv8(0x11, 0x04);
                const std::string f = qmux_frame(0x28, rtp_cid, m);
                const cbq_result mr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
                CHECK(mr.kind == CBQ_PRIVATE);
                pushed.clear();
                CHECK(ims_rtp_answer(0, mr, m.b));
                CHECK(pushed.size() == 1);                        /* response only */
                const uint8_t *a0 = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
                CHECK(a0[CBQ_HDR] == CBQ_QMI_RESPONSE && cbq_rd16(a0 + CBQ_HDR + 1) == 0x0096);
                CHECK(cbq_rd16(a0 + CBQ_HDR + 3) == message);
                CHECK(cbq_rd16(a0 + CBQ_HDR + 5) == 7);           /* result TLV only */
                CHECK(cbq_rd16(a0 + CBQ_HDR + 10) == 0 && cbq_rd16(a0 + CBQ_HDR + 12) == 0);
                char want[80];
                snprintf(want, sizeof want, "imsrtp stub: 0x%04x response only (link monitor)\n", message);
                CHECK(count_from(mark, want) == 1);
                // A second one is answered again but not logged twice.
                pushed.clear();
                CHECK(ims_rtp_answer(0, mr, m.b));
                CHECK(pushed.size() == 1 && count_from(mark, want) == 1);
                // No indication at all for this message id, ever.
                char ind[80];
                snprintf(ind, sizeof ind, "imsrtp stub: session 0x%04x answered ok (umi_rtpok)\n", message);
                CHECK(count_from(mark, ind) == 0);
            }
        }
        {
            // 0x002C..0x002F have no indication in the IDL: the response alone, still success.
            const uint16_t dtmfs[] = { 0x002C, 0x002D, 0x002E, 0x002F };
            for (uint16_t message : dtmfs) {
                sdu dtmf(CBQ_QMI_REQUEST, 0x0097, message);
                dtmf.tlv8(0x01, session);
                const std::string f = qmux_frame(0x28, rtp_cid, dtmf);
                const cbq_result dr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
                CHECK(dr.kind == CBQ_PRIVATE);
                pushed.clear();
                CHECK(ims_rtp_answer(0, dr, dtmf.b));
                CHECK(pushed.size() == 1 && cbq_rd16(reinterpret_cast<const uint8_t *>(pushed[0].second.data()) + CBQ_HDR + 10) == 0);
            }
        }
        // The round trip: with the worker present the answer is held and only reaches CommCenter
        // from the flush, in request order (response, then indication). This is what keeps the
        // stack from processing an answer before it has armed its request timer.
        {
            const uint8_t app_id2 = 0x33;
            sdu m(CBQ_QMI_REQUEST, 0x0098, 0x0022);
            m.tlv(0x01, &app_id2, 1);
            const std::string f = qmux_frame(0x28, rtp_cid, m);
            const cbq_result mr = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(mr.kind == CBQ_PRIVATE);
            pushed.clear();
            ims_rtp_trip_call = (thread_call_t)(uintptr_t)1;   /* the host has no thread call to run */
            CHECK(ims_rtp_answer(0, mr, m.b));
            CHECK(pushed.empty());                             /* held, not delivered */
            CHECK(ims_rtp_trip_n == 2);
            ims_rtp_trip_flush();
            CHECK(pushed.size() == 2 && pushed[0].first == 0 && pushed[1].first == 0);
            CHECK(reinterpret_cast<const uint8_t *>(pushed[0].second.data())[CBQ_HDR] == CBQ_QMI_RESPONSE);
            CHECK(reinterpret_cast<const uint8_t *>(pushed[1].second.data())[CBQ_HDR] == CBQ_QMI_INDICATION);
            CHECK(ims_rtp_trip_n == 0);
            // IMS off drains what is still queued (an answer already promised is still delivered).
            pushed.clear();
            CHECK(ims_rtp_answer(0, mr, m.b));
            CHECK(pushed.empty() && ims_rtp_trip_n == 2);
            ims_state(false);
            CHECK(pushed.size() == 2 && ims_rtp_trip_n == 0);
            ims_state(true);
            ims_rtp_trip_call = nullptr;                       /* back to the immediate push */
        }
        // Gate off again: the same request is today's well-formed NOT_SUPPORTED, one frame.
        ims_rtpok_on = false;
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, sess.b));
        CHECK(pushed.size() == 1);
        p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(cbq_rd16(p + CBQ_HDR + 10) == 1 && cbq_rd16(p + CBQ_HDR + 12) == CBQ_ERR_NOT_SUPPORTED);
    }

    {
        sdu uninit(CBQ_QMI_REQUEST, 0x0078, 0x0021);
        const std::string frame = qmux_frame(0x28, rtp_cid, uninit);
        const cbq_result r = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r.kind == CBQ_PRIVATE);
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, uninit.b));
        CHECK(pushed.size() == 1);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(cbq_rd16(p + CBQ_HDR + 10) == 0 && cbq_rd16(p + CBQ_HDR + 12) == 0);
        CHECK(count_from(0, "imsrtp stub: msg 0x0021 answered ok\n") == 1);
    }

    // ReleaseClientId(0x28) frees the local client; the next allocation is a new lifecycle event
    // and the client id is logged again.
    {
        uint8_t release[] = {1, 0, 0, 0, 0, 0, 0, 0x0a, 0x23, 0, 5, 0, 1, 2, 0, 0x28, rtp_cid};
        const cbq_result r = cbq_ctl(&bb_qmux, 0, release + CBQ_HDR, sizeof release - CBQ_HDR, answer, sizeof answer);
        CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
        ims_note_ctl(release, sizeof release, answer, r.len);
        const cbq_result again = cbq_ctl(&bb_qmux, 0, ctl + CBQ_HDR, sizeof ctl - CBQ_HDR, answer, sizeof answer);
        CHECK(again.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
        ims_note_ctl(ctl, sizeof ctl, answer, again.len);
        CHECK(count_from(0, "imsrtp stub: client ") == 2);
        // The new client has not initialized yet: another INITIALIZE is a new client's event.
        const uint8_t next_cid = answer[12 + 11];
        sdu init2(CBQ_QMI_REQUEST, 0x2222, 0x0020);
        const std::string frame = qmux_frame(0x28, next_cid, init2);
        const cbq_result r2 = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r2.kind == CBQ_PRIVATE);
        pushed.clear();
        CHECK(ims_rtp_answer(0, r2, init2.b));
        CHECK(pushed.size() == 1 && count_from(0, "imsrtp stub: INITIALIZE ok\n") == 2);
    }

    // AUDIO 0xe1 msg 0x33 is the legacy RTP controller's first gate. TLV 0x01 is the ARI payload
    // {event, call type, session id} (ipt.dis:849990): the boot message is {8, 2, 0}, a live call's
    // CallEvent carries its own event/session. Any valid event 0..8 with call type 2 (kVoIP) must
    // succeed or the chain never sends a 0x28 message (v29l INVESTIGATION-rtp-stack-init; v29v
    // 79504-79598 refused a live call's triple). Call type 9 (kEmergency) and everything else stays
    // NOT_SUPPORTED: no media here, so an emergency call must keep failing fast.
    uint8_t e1ctl[] = {1, 0, 0, 0, 0, 0, 0, 0x0c, 0x22, 0, 4, 0, 1, 1, 0, 0xe1};
    const cbq_result e1alloc = cbq_ctl(&bb_qmux, 1, e1ctl + CBQ_HDR, sizeof e1ctl - CBQ_HDR, answer, sizeof answer);
    CHECK(e1alloc.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
    const uint8_t audio_cid = answer[12 + 11];
    {
        const uint8_t boot[3] = { 0x08, 0x02, 0x00 };   /* event 8, kVoIP, session 0 */
        sdu call(CBQ_QMI_REQUEST, 0x0042, 0x0033);
        call.tlv(0x01, boot, sizeof boot);
        const std::string frame = qmux_frame(0xe1, audio_cid, call);
        const cbq_result r = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r.kind == CBQ_PRIVATE && r.service == 0xe1 && r.cid == audio_cid);
        pushed.clear();
        CHECK(ims_audio_answer(1, r, call.b));
        CHECK(pushed.size() == 1 && pushed[0].first == 1);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(pushed[0].second.size() == CBQ_HDR + CBQ_SDU_HDR + 7u);
        CHECK(p[0] == CBQ_IF_TYPE && p[4] == 0xe1 && p[5] == audio_cid && p[CBQ_HDR] == CBQ_QMI_RESPONSE);
        CHECK(cbq_rd16(p + CBQ_HDR + 1) == 0x0042);            /* the request's transaction */
        CHECK(cbq_rd16(p + CBQ_HDR + 3) == 0x0033);
        CHECK(cbq_rd16(p + CBQ_HDR + 5) == 7);
        CHECK(p[CBQ_HDR + 7] == 0x02 && cbq_rd16(p + CBQ_HDR + 8) == 4);
        CHECK(cbq_rd16(p + CBQ_HDR + 10) == 0 && cbq_rd16(p + CBQ_HDR + 12) == 0);
        CHECK(count_from(0, "audio 0xe1 0x33 call event 8 (voip, session 0) answered ok\n") == 1);
        pushed.clear();
        CHECK(ims_audio_answer(1, r, call.b));
        CHECK(pushed.size() == 1 && count_from(0, "audio 0xe1 0x33 call event ") == 1);

        // A live call's CallEvent: another event index and its own session id, still VoIP.
        const uint8_t live[3] = { 0x03, 0x02, 0x07 };
        sdu lc(CBQ_QMI_REQUEST, 0x0045, 0x0033);
        lc.tlv(0x01, live, sizeof live);
        const std::string lf = qmux_frame(0xe1, audio_cid, lc);
        const cbq_result lr = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(lf.data()), lf.size(), answer, sizeof answer);
        CHECK(lr.kind == CBQ_PRIVATE);
        pushed.clear();
        CHECK(ims_audio_answer(1, lr, lc.b));
        CHECK(pushed.size() == 1);
        CHECK(count_from(0, "audio 0xe1 0x33 call event 3 (voip, session 7) answered ok\n") == 1);
        // Logged once per event index: a second one is answered, no second line.
        pushed.clear();
        CHECK(ims_audio_answer(1, lr, lc.b));
        CHECK(pushed.size() == 1 && count_from(0, "audio 0xe1 0x33 call event 3 ") == 1);

        // kEmergency (type 9) and any other call type keep today's NOT_SUPPORTED path, as do an
        // out-of-range event index, another ARI length and another AUDIO message.
        const uint8_t emer[3] = { 0x02, 0x09, 0x01 };
        const uint8_t other_type[3] = { 0x02, 0x03, 0x01 };
        const uint8_t big_event[3] = { 0x09, 0x02, 0x01 };
        const uint8_t short_ari[2] = { 0x02, 0x02 };
        const uint8_t *reject[] = { emer, other_type, big_event };
        for (const uint8_t *ari : reject) {
            sdu m(CBQ_QMI_REQUEST, 0x0043, 0x0033);
            m.tlv(0x01, ari, 3);
            const std::string f = qmux_frame(0xe1, audio_cid, m);
            const cbq_result mr = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(mr.kind == CBQ_PRIVATE && !ims_audio_answer(1, mr, m.b) && pushed.size() == 1);
        }
        {
            sdu m(CBQ_QMI_REQUEST, 0x0043, 0x0033);
            m.tlv(0x01, short_ari, sizeof short_ari);
            const std::string f = qmux_frame(0xe1, audio_cid, m);
            const cbq_result mr = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(f.data()), f.size(), answer, sizeof answer);
            CHECK(mr.kind == CBQ_PRIVATE && !ims_audio_answer(1, mr, m.b) && pushed.size() == 1);
        }
        sdu msg34(CBQ_QMI_REQUEST, 0x0044, 0x0034);
        msg34.tlv(0x01, live, sizeof live);
        const std::string f3 = qmux_frame(0xe1, audio_cid, msg34);
        const cbq_result r3 = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(f3.data()), f3.size(), answer, sizeof answer);
        CHECK(r3.kind == CBQ_PRIVATE && !ims_audio_answer(1, r3, msg34.b) && pushed.size() == 1);
        // IMS off: unchanged, nothing answered.
        ims_state(false);
        CHECK(!ims_audio_answer(1, r, call.b) && pushed.size() == 1);
        ims_state(true);
    }

    // 0x28 UninitializeAll (0x003D) and RegisterServices (0x0030) are the next two gates after
    // the CallEvent: success, one "answered ok" line per message id.
    {
        uint8_t rctl[] = {1, 0, 0, 0, 0, 0, 0, 0x0d, 0x22, 0, 4, 0, 1, 1, 0, 0x28};
        const cbq_result rc = cbq_ctl(&bb_qmux, 0, rctl + CBQ_HDR, sizeof rctl - CBQ_HDR, answer, sizeof answer);
        CHECK(rc.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
        const uint8_t cid2 = answer[12 + 11];
        const uint16_t gates[] = { 0x003D, 0x0030, 0x003D };
        for (uint16_t message : gates) {
            sdu m(CBQ_QMI_REQUEST, 0x0050, message);
            const uint8_t app_id = 0x05;      /* the request's TLV 0x01 app id, echoed in the indication */
            if (message == 0x003D) m.tlv(0x01, &app_id, 1);
            const std::string frame = qmux_frame(0x28, cid2, m);
            const cbq_result r = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
            CHECK(r.kind == CBQ_PRIVATE);
            pushed.clear();
            CHECK(ims_rtp_answer(0, r, m.b));
            // 0x003D: the response first, then the UNINITIALIZE_ALL indication the controller waits
            // for (v29m: its 10 s timer only stops when this indication arrives).
            CHECK(pushed.size() == (message == 0x003D ? 2u : 1u));
            const uint8_t *p = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
            CHECK(p[CBQ_HDR] == CBQ_QMI_RESPONSE);
            CHECK(cbq_rd16(p + CBQ_HDR + 1) == 0x0050 && cbq_rd16(p + CBQ_HDR + 3) == message);
            CHECK(cbq_rd16(p + CBQ_HDR + 10) == 0 && cbq_rd16(p + CBQ_HDR + 12) == 0);
            if (message == 0x003D) {
                const std::string &ind = pushed[1].second;
                const uint8_t *q = reinterpret_cast<const uint8_t *>(ind.data());
                CHECK(pushed[1].first == 0 && ind.size() == CBQ_HDR + CBQ_SDU_HDR + 15u);
                CHECK(q[0] == CBQ_IF_TYPE && q[1] == (uint8_t)(ind.size() - 1u) && q[3] == CBQ_FLAG_SERVICE);
                CHECK(q[4] == 0x28 && q[5] == cid2);
                CHECK(q[CBQ_HDR] == CBQ_QMI_INDICATION);
                CHECK(cbq_rd16(q + CBQ_HDR + 1) == 0);                 /* indications carry txn 0 */
                CHECK(cbq_rd16(q + CBQ_HDR + 3) == 0x003D);
                CHECK(cbq_rd16(q + CBQ_HDR + 5) == 15);
                const uint8_t *t = q + CBQ_HDR + 7;
                /* TLV 0x01 app_id: the byte the driver reports as the stack controller's appId,
                 * matched against SipStack's (0xff) - it is NOT TLV 0x10, which is only the
                 * "<N> sessions dropped" count (v29q IMS log). */
                CHECK(t[0] == 0x01 && cbq_rd16(t + 1) == 1 && t[3] == app_id);
                /* TLV 0x02 status: 32-bit enum, 0 = IMSRTP_SUCCESS (mandatory in the indication). */
                CHECK(t[4] == 0x02 && cbq_rd16(t + 5) == 4);
                CHECK(cbq_rd16(t + 7) == 0 && cbq_rd16(t + 9) == 0);
                /* TLV 0x10 sessions dropped from this request: the bridge holds no sessions. */
                CHECK(t[11] == 0x10 && cbq_rd16(t + 12) == 1 && t[14] == 0);
            }
            char want[64];
            snprintf(want, sizeof want, "imsrtp stub: msg 0x%04x answered ok\n", message);
            CHECK(count_from(0, want) == 1);
        }
        CHECK(count_from(0, "imsrtp stub: 0x003d indication sent (app 0x05 dropped 0)\n") == 1);   /* once per client */
    }

    // A 0x3D request without TLV 0x01 (never sent by Apple's builder: imsrtp_uninitialize_all_
    // rtp_session_req_msg_v01 makes app_id mandatory) still gets a well-formed indication; the app
    // id falls back to 0xff, SipStack's configured default, and the one line says so.
    {
        uint8_t rctl2[] = {1, 0, 0, 0, 0, 0, 0, 0x0d, 0x22, 0, 4, 0, 1, 1, 0, 0x28};
        const cbq_result rc2 = cbq_ctl(&bb_qmux, 0, rctl2 + CBQ_HDR, sizeof rctl2 - CBQ_HDR, answer, sizeof answer);
        CHECK(rc2.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
        const uint8_t cid3 = answer[12 + 11];
        sdu m(CBQ_QMI_REQUEST, 0x0060, 0x003D);
        const std::string frame = qmux_frame(0x28, cid3, m);
        const cbq_result r = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), answer, sizeof answer);
        CHECK(r.kind == CBQ_PRIVATE);
        pushed.clear();
        CHECK(ims_rtp_answer(0, r, m.b));
        CHECK(pushed.size() == 2 && pushed[1].second.size() == CBQ_HDR + CBQ_SDU_HDR + 15u);
        const uint8_t *q = reinterpret_cast<const uint8_t *>(pushed[1].second.data());
        CHECK(cbq_rd16(q + CBQ_HDR + 5) == 15);
        CHECK(q[CBQ_HDR + 7] == 0x01 && cbq_rd16(q + CBQ_HDR + 8) == 1 && q[CBQ_HDR + 10] == 0xff);
        CHECK(q[CBQ_HDR + 11] == 0x02 && cbq_rd16(q + CBQ_HDR + 12) == 4);
        CHECK(q[CBQ_HDR + 18] == 0x10 && cbq_rd16(q + CBQ_HDR + 19) == 1 && q[CBQ_HDR + 21] == 0);
        CHECK(count_from(0, "imsrtp stub: 0x003d indication sent (app 0xff default dropped 0)\n") == 1);
    }

    // ---- route B (modem-ims): the same media answers, never from an older switch sample -------
    // v31 §5.2: the poll samples the switch files at most once a second, while CommCenter's 0x28
    // GetClientId is answered on the CTL thread; the driver samples again for that frame and applies
    // the owners before cbq_from_host (ims_ctl_wants_switch -> ims_sample_switch -> ims_apply_switch).
    // Here the file is set and the poll's sample is still the old one, which is that race.
    {
        ims_state(false);
        modem_ims_state(false);
        switch_file = false;
        modem_switch = true;   /* /var/db/hakios-voice/modem-ims only: route B */
        const size_t mark = log_lines.size();
        // Stale sample: 0x28 is not private yet and the allocation is refused, as v29i saw it.
        const cbq_result stale = cbq_ctl(&bb_qmux, 0, ctl + CBQ_HDR, sizeof ctl - CBQ_HDR, answer, sizeof answer);
        CHECK(stale.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == CBQ_ERR_INVALID_SERVICE_TYPE);
        CHECK(!modem_ims_enabled && !ims_owner_on());
        // The CTL thread's refresh: the file is read now, and the owners follow before the answer.
        // (The arrays above hold the CTL SDU only, so a real QMUX frame is built for this check.)
        uint8_t cframe[CBQ_HDR + CBQ_CTL_HDR + 4u];
        cbq_frame_header(cframe, CBQ_CTL_HDR + 4u, 0, 0);
        cframe[3] = 0;
        uint8_t *cs = cframe + CBQ_HDR;
        cs[0] = 0; cs[1] = 3; cs[2] = 0x22; cs[3] = 0; cs[4] = 4; cs[5] = 0;   /* txn 3, GetClientId */
        cs[6] = 0x01; cs[7] = 1; cs[8] = 0; cs[9] = 0x28;                       /* TLV 0x01 = service 0x28 */
        CHECK(ims_ctl_wants_switch(cframe, sizeof cframe));
        ims_sample_switch();
        ims_apply_switch();
        CHECK(modem_ims_wanted && modem_ims_enabled && !ims_enabled && ims_owner_on());
        CHECK(bb_qmux.imsrtp);   /* 0x28 is private while either owner is on */
        const cbq_result alloc = cbq_ctl(&bb_qmux, 0, ctl + CBQ_HDR, sizeof ctl - CBQ_HDR, answer, sizeof answer);
        CHECK(alloc.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
        const uint8_t b_cid = answer[12 + 11];
        ims_note_ctl(ctl, sizeof ctl, answer, alloc.len);
        CHECK(count_from(mark, "media client svc 0x28 allocation result 0 error 0\n") == 1);
        CHECK(count_from(mark, "imsrtp stub: client " + std::to_string(b_cid) + " allocated\n") == 1);
        // The version list the QMI layer gates on carries 0x28 in route B too.
        const cbq_result vb = cbq_ctl(&bb_qmux, 0, vctl + CBQ_HDR, sizeof vctl - CBQ_HDR, answer, sizeof answer);
        CHECK(vb.kind == CBQ_TO_HOST && version_lists(answer, vb.len, 0x28));

        // 0x28 INITIALIZE: the same success answer (result TLV only) as under ims-on.
        sdu init(CBQ_QMI_REQUEST, 0x0b01, 0x0020);
        const std::string bf = qmux_frame(0x28, b_cid, init);
        const cbq_result br = cbq_from_host(&bb_qmux, 0, reinterpret_cast<const uint8_t *>(bf.data()), bf.size(), answer, sizeof answer);
        CHECK(br.kind == CBQ_PRIVATE && br.service == 0x28 && br.cid == b_cid);
        pushed.clear();
        CHECK(ims_rtp_answer(0, br, init.b));
        CHECK(pushed.size() == 1);
        const uint8_t *bp = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
        CHECK(bp[CBQ_HDR] == CBQ_QMI_RESPONSE && cbq_rd16(bp + CBQ_HDR + 1) == 0x0b01);
        CHECK(cbq_rd16(bp + CBQ_HDR + 10) == 0 && cbq_rd16(bp + CBQ_HDR + 12) == 0);
        CHECK(count_from(mark, "imsrtp stub: INITIALIZE ok\n") == 1);

        // AUDIO 0xe1 0x33: the VoIP CallEvent gate answers in route B as well; kEmergency does not.
        uint8_t be1[] = {1, 0, 0, 0, 0, 0, 0, 0x0c, 0x22, 0, 4, 0, 1, 1, 0, 0xe1};
        const cbq_result ea = cbq_ctl(&bb_qmux, 1, be1 + CBQ_HDR, sizeof be1 - CBQ_HDR, answer, sizeof answer);
        CHECK(ea.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == 0);
        const uint8_t b_audio = answer[12 + 11];
        {
            const uint8_t live[3] = { 0x05, 0x02, 0x03 };   /* event 5, kVoIP, session 3 */
            sdu call(CBQ_QMI_REQUEST, 0x0b02, 0x0033);
            call.tlv(0x01, live, sizeof live);
            const std::string af = qmux_frame(0xe1, b_audio, call);
            const cbq_result ar = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(af.data()), af.size(), answer, sizeof answer);
            CHECK(ar.kind == CBQ_PRIVATE && ar.service == 0xe1 && ar.cid == b_audio);
            pushed.clear();
            CHECK(ims_audio_answer(1, ar, call.b));
            CHECK(pushed.size() == 1 && pushed[0].first == 1);
            const uint8_t *ap = reinterpret_cast<const uint8_t *>(pushed[0].second.data());
            CHECK(cbq_rd16(ap + CBQ_HDR + 1) == 0x0b02 && cbq_rd16(ap + CBQ_HDR + 3) == 0x0033);
            CHECK(cbq_rd16(ap + CBQ_HDR + 10) == 0 && cbq_rd16(ap + CBQ_HDR + 12) == 0);
            CHECK(count_from(mark, "audio 0xe1 0x33 call event 5 (voip, session 3) answered ok\n") == 1);
            const uint8_t emer[3] = { 0x02, 0x09, 0x01 };
            sdu em(CBQ_QMI_REQUEST, 0x0b03, 0x0033);
            em.tlv(0x01, emer, sizeof emer);
            const std::string ef = qmux_frame(0xe1, b_audio, em);
            const cbq_result er = cbq_from_host(&bb_qmux, 1, reinterpret_cast<const uint8_t *>(ef.data()), ef.size(), answer, sizeof answer);
            CHECK(er.kind == CBQ_PRIVATE && !ims_audio_answer(1, er, em.b) && pushed.size() == 1);
        }

        // Both switches off: 0x28 is not private, the list drops it, and a request on the client
        // left over from before is answered NOT_SUPPORTED exactly as the generic path would.
        modem_switch = false;
        ims_sample_switch();
        ims_apply_switch();
        CHECK(!ims_switch_wanted && !modem_ims_wanted && !ims_owner_on() && !bb_qmux.imsrtp);
        const cbq_result off = cbq_ctl(&bb_qmux, 0, ctl + CBQ_HDR, sizeof ctl - CBQ_HDR, answer, sizeof answer);
        CHECK(off.kind == CBQ_TO_HOST && cbq_rd16(answer + 12 + 5) == CBQ_ERR_INVALID_SERVICE_TYPE);
        const cbq_result vo = cbq_ctl(&bb_qmux, 0, vctl + CBQ_HDR, sizeof vctl - CBQ_HDR, answer, sizeof answer);
        CHECK(vo.kind == CBQ_TO_HOST && !version_lists(answer, vo.len, 0x28));
        pushed.clear();
        CHECK(!ims_rtp_answer(0, br, init.b) && !ims_audio_answer(1, ea, init.b) && pushed.empty());
        const uint32_t un = cbq_private_unsupported(init.b, br.service, br.cid, answer, sizeof answer);
        CHECK(un && cbq_rd16(answer + CBQ_HDR + 10) == 1 && cbq_rd16(answer + CBQ_HDR + 12) == CBQ_ERR_NOT_SUPPORTED);
        // Only the CTL client and version requests resample; a service request never does.
        cs[2] = 0x21;                                                            /* GetVersionInfo */
        CHECK(ims_ctl_wants_switch(cframe, sizeof cframe));
        CHECK(!ims_ctl_wants_switch(cframe, sizeof cframe - 1u));                /* a truncated frame */
        CHECK(!ims_ctl_wants_switch(reinterpret_cast<const uint8_t *>(bf.data()), bf.size()));
        switch_file = false;
        modem_switch = false;
    }

    // ---- synthesized WDS "DAD complete" indication (0x5561) ----
    // 1. IMS on and a 0x2d reply carrying a global TLV 0x25 but no TLV 0x2e at all: the fallback
    //    still sends, exactly one indication for this client with a nonzero X55-style txn. No
    //    address ioctl (XNU never calls one for the IMS unit) and no request-side APN/bearer
    //    (CommCenter sends none).
    const uint8_t global[17] = { 0x24, 0x01, 0x0d, 0xb0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 64 };
    dad_begin(true, 1);
    CHECK(!ims_owner[dad_slot].bearer.selected);
    CHECK(!ims_dad_ioctl_ok[1]);
    CHECK(log_lines.find("WDS start cid " + std::to_string(dad_cid) +
                         " fields 0x01/0x0001 0x0e/0x0001 0x1f/0x0001") != std::string::npos);
    dad_settings(global);
    CHECK(ims_dad[dad_slot].armed && !ims_dad[dad_slot].sent);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 1);
    uint16_t first_txn;
    {
        const std::string &frame = pushed[0].second;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(frame.data());
        CHECK(pushed[0].first == CBH_QMUX2);
        CHECK(frame.size() == CBQ_HDR + CBQ_SDU_HDR + 3u + 17u);
        CHECK(p[0] == CBQ_IF_TYPE && p[3] == CBQ_FLAG_SERVICE && p[4] == UMI_IMS_WDS && p[5] == dad_cid);
        CHECK(p[CBQ_HDR] == CBQ_QMI_INDICATION);
        first_txn = cbq_rd16(p + CBQ_HDR + 1);
        CHECK(first_txn != 0);
        CHECK(cbq_rd16(p + CBQ_HDR + 3) == IMS_DAD_MESSAGE);
        CHECK(cbq_rd16(p + CBQ_HDR + 5) == 3u + 17u);
        CHECK(p[CBQ_HDR + 7] == IMS_DAD_TLV && cbq_rd16(p + CBQ_HDR + 8) == 17);
        CHECK(!memcmp(p + CBQ_HDR + 10, global, 17));
    }
    CHECK(log_lines.find("DAD complete indication cid " + std::to_string(dad_cid) +
                         " mux 2 candidates 1 (tlv 0x25/0x27/0x28 present: 1/0/0) txn " + std::to_string(first_txn) +
                         ".." + std::to_string(first_txn) + " sent (no 0x2e field: fallback send)\n") != std::string::npos);
    // One per activation: a repeated timer and another 0x2d reply do not repeat it.
    now += 10000;
    ims_dad_run_locked();
    dad_settings(global);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 1);
    // A Stop, then a new Start and its 0x2d reply, is a new activation: it may send again, with the
    // next transaction.
    sdu stop(0, 4, UMI_IMS_STOP);
    ims_pdp_observe(dad_slot, stop.b, stop.n);
    sdu restart(0, 5, UMI_IMS_START);
    restart.tlv8(0x01, 0).tlv8(0x0e, 1);
    ims_pdp_observe(dad_slot, restart.b, restart.n);
    const uint8_t restart_handle[4] = { 0x35, 0x12, 0, 0 };
    sdu restart_reply(2, 5, UMI_IMS_START);
    restart_reply.result().tlv(0x01, restart_handle, sizeof restart_handle);
    ims_pdp_observe(dad_slot, restart_reply.b, restart_reply.n);
    dad_settings(global);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 2);
    CHECK(dad_txn(pushed[1].second) == first_txn + 1);
    CHECK(!memcmp(dad_addr(pushed[1].second), global, 17));

    // 1b. The same 0x2d reply with padding past its last TLV: the SDU's declared length is a floor
    //     (as cbd_sdu_ok() reads it), so the address fields are still found and the activation arms.
    dad_begin(true, 1);
    dad_cfg padded;
    padded.a25 = global;
    padded.pad = 5;
    dad_settings(padded);
    CHECK(ims_dad[dad_slot].armed && ims_dad[dad_slot].cand_n == 1);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 1);
    CHECK(!memcmp(dad_addr(pushed[0].second), global, 17));

    // 2. IMS off: nothing synthesized, even with the same settings and address ioctl.
    dad_begin(false, 1);
    dad_settings(global);
    dad_install(1);
    now += IMS_DAD_DELAY_MS;
    ims_dad_poll();
    ims_dad_run_locked();
    CHECK(pushed.empty());

    // 2b. Route B (modem-ims, no ims-on): the 0x5561 keepalive belongs to the IMS bearer, not to
    //     Apple's stack, so the same activation arms and sends. With both switches off it is inert.
    dad_begin_route_b(1);
    CHECK(!ims_enabled && modem_ims_enabled && ims_owner_on());
    dad_settings(global);
    CHECK(ims_dad[dad_slot].armed && !ims_dad[dad_slot].sent);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 1);
    CHECK(cbq_rd16(reinterpret_cast<const uint8_t *>(pushed[0].second.data()) + CBQ_HDR + 3) == IMS_DAD_MESSAGE);
    CHECK(log_lines.find("DAD complete indication cid " + std::to_string(dad_cid) + " mux 2 candidates 1") !=
          std::string::npos);
    dad_begin(false, 1);
    CHECK(!ims_owner_on());
    dad_settings(global);
    CHECK(!ims_dad[dad_slot].armed);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.empty());

    // 2c. One owner's switch-off never tears down what the other still holds, and a repeated
    //     on-edge is a no-op: the armed timer and its candidates survive.
    dad_begin_route_b(1);
    dad_settings(global);
    CHECK(ims_dad[dad_slot].armed);
    ims_state(true);                     // ims-on arrives: the AP takes over
    CHECK(ims_enabled && modem_ims_enabled && ims_dad[dad_slot].armed);
    modem_ims_state(false);              // route B leaves: the AP keeps the DAD state
    CHECK(ims_enabled && !modem_ims_enabled && ims_owner_on() && ims_dad[dad_slot].armed);
    const uint32_t dad_gen = ims_dad_ifaddr_gen;
    ims_dad_switch(true);                // idempotent
    CHECK(ims_dad_ifaddr_gen == dad_gen && ims_dad[dad_slot].armed);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 1);
    ims_state(false);

    // 3. A link-local TLV 0x25 is ignored, as CommCenter ignores a link-local DAD; one deduped
    //    "not armed" line says why.
    const uint8_t link_local[17] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 64 };
    dad_begin(true, 1);
    size_t mark = log_lines.size();
    dad_settings(link_local);
    CHECK(!ims_dad[dad_slot].armed);
    dad_settings(link_local);
    CHECK(count_from(mark, "DAD not armed cid ") == 1);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.empty());

    // 4. Teardown before the timer: nothing is sent and the activation is closed.
    dad_begin(true, 1);
    dad_settings(global);
    CHECK(ims_dad[dad_slot].armed);
    sdu teardown(0, 6, UMI_IMS_STOP);
    ims_pdp_observe(dad_slot, teardown.b, teardown.n);
    CHECK(!ims_dad[dad_slot].armed && !ims_dad[dad_slot].sent && ims_dad[dad_slot].closed);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.empty());

    // 5. A 0x2d reply without any IPv6 field cannot arm; the address ioctl is only a diagnostic, and
    //    the reason line is still emitted once.
    dad_begin(true, 1);
    mark = log_lines.size();
    dad_install(1);
    dad_settings_no_v6();
    CHECK(!ims_dad[dad_slot].armed);
    CHECK(count_from(mark, "DAD not armed cid ") == 1);
    CHECK(log_lines.find("ioctl 1 closed 0 worker 1") != std::string::npos);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.empty());

    // 6. The runtime-settings schema line is no longer hidden behind the request-side APN match.
    dad_begin(true, 1);
    const uint8_t dns_metric[4] = { 0, 0, 0, 0 };
    sdu schema(2, 3, UMI_IMS_GET_SETTINGS);
    schema.result().tlv(0x1e, dns_metric, sizeof dns_metric).tlv(0x25, global, 17);
    cbq_result note = { CBQ_TO_HOST, 0, 0, 0, 0, 1, UMI_IMS_WDS, dad_cid };
    mark = log_lines.size();
    ims_note("rx", note, schema.b, schema.n);
    CHECK(!ims_owner[dad_slot].bearer.selected);
    CHECK(count_from(mark, "WDS settings field 0x25 bytes 17") == 1);
    CHECK(count_from(mark, "WDS settings field 0x1e bytes 4") == 1);

    // 7. IMS reply (0x2e length 33) with three IPv6-shaped fields: one indication per DISTINCT
    //    17-byte value (address + prefix, what CommCenter compares), each with a nonzero incrementing
    //    txn. 0x27 has no prefix byte and borrows 0x25's 64; 0x28 has the same address with prefix
    //    48, a different value to CommCenter, so it is NOT deduped.
    const uint8_t g25[17] = { 0x2a, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xaa, 0xbb, 0xcc, 0xdd, 64 };
    const uint8_t g27[16] = { 0x2b, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44 };
    const uint8_t g28[17] = { 0x2b, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 48 };
    dad_begin(true, 1);
    mark = log_lines.size();
    dad_cfg full;
    full.a25 = g25;
    full.a27 = g27;
    full.a28 = g28;
    full.pcscf_len = 33;
    dad_settings(full);
    CHECK(ims_dad[dad_slot].armed && ims_dad[dad_slot].cand_n == 3);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 3);
    {
        const uint16_t t1 = dad_txn(pushed[0].second), t2 = dad_txn(pushed[1].second), t3 = dad_txn(pushed[2].second);
        CHECK(t1 != 0 && t2 == t1 + 1 && t3 == t2 + 1);
        CHECK(pushed[0].first == CBH_QMUX2 && pushed[1].first == CBH_QMUX2 && pushed[2].first == CBH_QMUX2);
        CHECK(!memcmp(dad_addr(pushed[0].second), g25, 17));          /* 0x25, prefix 64 */
        CHECK(!memcmp(dad_addr(pushed[1].second), g27, 16));          /* 0x27, borrowed prefix 64 */
        CHECK(dad_addr(pushed[1].second)[16] == 64);
        CHECK(!memcmp(dad_addr(pushed[2].second), g28, 17));          /* 0x28, prefix 48 */
        CHECK(count_from(mark, "DAD complete indication cid " + std::to_string(dad_cid) +
                         " mux 2 candidates 3 (tlv 0x25/0x27/0x28 present: 1/1/1) txn " + std::to_string(t1) +
                         ".." + std::to_string(t3) + " sent\n") == 1);
    }
    // A repeated 0x2d reply in the same activation neither re-sends nor logs another line.
    dad_settings(full);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.size() == 3);
    CHECK(count_from(mark, "DAD complete indication ") == 1);

    // 8. The internet PDN reply has the same address fields but an empty P-CSCF list (0x2e length 1):
    //    nothing is sent, and the per-activation line says exactly why.
    dad_begin(true, 1);
    mark = log_lines.size();
    dad_cfg internet;
    internet.a25 = g25;
    internet.a27 = g27;
    internet.pcscf_len = 1;
    dad_settings(internet);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.empty());
    CHECK(count_from(mark, "DAD complete indication cid " + std::to_string(dad_cid) +
                           " mux 2 candidates 2 (tlv 0x25/0x27/0x28 present: 1/1/0) txn 0..0 not sent: "
                           "internet context (0x2e len <= 1)\n") == 1);
    dad_settings(internet);
    now += IMS_DAD_DELAY_MS;
    ims_dad_run_locked();
    CHECK(pushed.empty());
    CHECK(count_from(mark, "DAD complete indication ") == 1);

    // 9. Interface-address retries: pdp_ip1 has no address at the first send, the snapshot is taken
    //    again ~1 s later, and an address equal to the TLV candidate already sent is not repeated.
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_settings(global);
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 1);
    CHECK(ims_dad[dad_slot].ifretry && ims_dad[dad_slot].ifretries == 1);
    mark = log_lines.size();
    dad_ifaddr_set(1, global, 64);
    now += IMS_DAD_IFADDR_MS;
    dad_finish();
    CHECK(pushed.size() == 1);                       /* deduped against the TLV set */
    CHECK(!ims_dad[dad_slot].ifretry);               /* an address exists: the retries stop */
    CHECK(count_from(mark, "DAD addr hashes ") == 1);   /* one hash line per activation */

    // 10. An interface address that appears on a retry and differs from every TLV is sent then,
    //     with the prefix from the ifaddr netmask, and nothing is sent afterwards.
    const uint8_t if_new[16] = { 0x2a, 0x01, 0x02, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0x55, 0x66, 0x77, 0x88 };
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_settings(global);
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 1);
    dad_ifaddr_set(1, if_new, 48);
    now += IMS_DAD_IFADDR_MS;
    dad_finish();
    CHECK(pushed.size() == 2);
    CHECK(!memcmp(dad_addr(pushed[1].second), if_new, 16));
    CHECK(dad_addr(pushed[1].second)[16] == 48);
    now += IMS_DAD_IFADDR_MS;
    dad_finish();
    CHECK(pushed.size() == 2);

    // 11. No interface address for the whole window: exactly IMS_DAD_IFRETRY_MAX retries, then one
    //     line per activation says so (besides the one hash line) and the wire stays quiet.
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_settings(global);
    mark = log_lines.size();
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 1);
    for (unsigned i = 0; i < IMS_DAD_IFRETRY_MAX; i++) {
        now += IMS_DAD_IFADDR_MS;
        dad_finish();
    }
    CHECK(pushed.size() == 1);
    CHECK(!ims_dad[dad_slot].ifretry && ims_dad[dad_slot].ifretries == IMS_DAD_IFRETRY_MAX);
    CHECK(count_from(mark, "DAD no interface address on pdp_ip1 after 5 tries\n") == 1);
    CHECK(count_from(mark, "DAD addr hashes ") == 1);
    now += 10000;
    dad_finish();
    CHECK(count_from(mark, "DAD no interface address ") == 1);

    // 12. The optional per-poll refresh runs only while an activation is waiting for its send, and
    //     the first send then already carries the interface address next to the TLV one.
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_ifaddr_set(1, if_new, 48);
    ims_dad_ifaddr_refresh(false);
    CHECK(ims_dad_ifaddr_at == 0 && ims_dad_ifaddrs[1].n == 0);   /* nothing armed: no lookup */
    dad_settings(global);
    CHECK(ims_dad_ifaddr_wanted());
    now += 1;
    ims_dad_ifaddr_refresh(false);
    CHECK(ims_dad_ifaddr_at == now && ims_dad_ifaddrs[1].n == 1);
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 2);
    CHECK(!memcmp(dad_addr(pushed[1].second), if_new, 16));

    // 13. A switch clears the snapshot and bumps the generation, so a walk started before it can
    //     never store its (now stale) result.
    const uint32_t gen_before = ims_dad_ifaddr_gen;
    ims_dad_switch(false);
    CHECK(ims_dad_ifaddr_gen == gen_before + 1);
    CHECK(ims_dad_ifaddr_at == 0 && ims_dad_ifaddrs[1].n == 0);

    // 14. A 0x2d reply for the IMS context carried through the proxy's delivery path (umi_bb_qrtr)
    //     is HELD while pdp_ipN has no global IPv6 address; the next poll releases it as soon as one
    //     appears, before the 0x5561 round. The indication then carries the address that was on the
    //     interface at delivery, prefix included: exactly what CommCenter's getInterfaceAddress read.
    const uint8_t deliv[17] = { 0x2a, 0x0a, 0x0b, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0x77, 0x88, 0x99, 0xaa, 64 };
    dad_begin(true, 1);
    dad_ifnet(1);
    mark = log_lines.size();
    dad_cfg imscfg;
    imscfg.a25 = g25;
    imscfg.pcscf_len = 33;
    CHECK(dad_settings_through_proxy(imscfg));
    CHECK(ims_dad_hold[0].used && ims_dad[dad_slot].held);
    CHECK(pushed.empty());                       /* not delivered while the interface has no address */
    dad_ifaddr_set(1, deliv, 64);
    now += IMS_DAD_HOLD_POLL_MS;
    dad_finish();                                /* refresh takes the address, the poll releases */
    CHECK(!ims_dad_hold[0].used && !ims_dad[dad_slot].held);
    CHECK(pushed.size() == 1);
    CHECK(pushed[0].first == CBH_QMUX2);
    CHECK(reinterpret_cast<const uint8_t *>(pushed[0].second.data())[CBQ_HDR] == 2);   /* the 0x2d reply */
    CHECK(cbq_rd16(reinterpret_cast<const uint8_t *>(pushed[0].second.data()) + CBQ_HDR + 3) == UMI_IMS_GET_SETTINGS);
    CHECK(ims_dad[dad_slot].t0_n == 1 && !memcmp(ims_dad[dad_slot].t0[0], deliv, 17));
    CHECK(count_from(mark, "0x2d held 20 ms (ifaddr=yes) cid " + std::to_string(dad_cid) + "\n") == 1);
    now += IMS_DAD_DELAY_MS;
    dad_finish();                                /* the 0x5561 round */
    CHECK(pushed.size() == 3);                   /* the reply + the TLV candidate + the delivery one */
    CHECK(!memcmp(dad_addr(pushed[1].second), g25, 17));
    CHECK(!memcmp(dad_addr(pushed[2].second), deliv, 17));   /* 16 bytes + prefix 64 */
    CHECK(count_from(mark, " t0sent=1\n") == 1);

    // 15. No address within the bound: the reply is delivered anyway at IMS_DAD_HOLD_MS, the line
    //     says ifaddr=timeout, and the 0x5561 carries only the TLV candidate (t0 empty).
    dad_begin(true, 1);
    dad_ifnet(1);
    mark = log_lines.size();
    CHECK(dad_settings_through_proxy(imscfg));
    now += IMS_DAD_HOLD_MS - 1;
    dad_finish();
    CHECK(pushed.empty());                       /* still held just before the bound */
    now++;
    dad_finish();
    CHECK(!ims_dad_hold[0].used && pushed.size() == 1);
    CHECK(ims_dad[dad_slot].t0_n == 0);
    CHECK(count_from(mark, "0x2d held 500 ms (ifaddr=timeout) cid " + std::to_string(dad_cid) + "\n") == 1);
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 2);
    CHECK(!memcmp(dad_addr(pushed[1].second), g25, 17));
    for (unsigned i = 0; i < IMS_DAD_IFRETRY_MAX; i++) {   /* the retries end: the hash line goes out */
        now += IMS_DAD_IFADDR_MS;
        dad_finish();
    }
    CHECK(pushed.size() == 2);
    CHECK(count_from(mark, "t0=[] t0sent=0") == 1);

    // 16. Only the IMS context is held: an internet reply (0x2e length 1) and a reply without 0x2e
    //     are pushed immediately, even with no interface address.
    dad_begin(true, 1);
    dad_ifnet(1);
    mark = log_lines.size();
    dad_cfg internet2;
    internet2.a25 = g25;
    internet2.pcscf_len = 1;
    CHECK(!dad_settings_through_proxy(internet2));
    CHECK(pushed.size() == 1 && !ims_dad_hold_pending());
    CHECK(count_from(mark, "0x2d held ") == 0);
    dad_begin(true, 1);
    dad_ifnet(1);
    mark = log_lines.size();
    dad_cfg nofield;
    nofield.a25 = g25;
    CHECK(!dad_settings_through_proxy(nofield));
    CHECK(pushed.size() == 1 && !ims_dad_hold_pending());
    CHECK(count_from(mark, "0x2d held ") == 0);

    // 17. Same address, different 17th byte: the TLV says prefix 60, the interface 64. Both are sent,
    //     because CommCenter compares address AND prefix.
    const uint8_t same60[17] = { 0x2a, 0x0a, 0x0b, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0x77, 0x88, 0x99, 0xaa, 60 };
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_cfg pfx;
    pfx.a25 = same60;
    pfx.pcscf_len = 33;
    CHECK(dad_settings_through_proxy(pfx));
    dad_ifaddr_set(1, deliv, 64);
    now += IMS_DAD_HOLD_POLL_MS;
    dad_finish();
    CHECK(pushed.size() == 1);
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 3);
    CHECK(!memcmp(dad_addr(pushed[1].second), same60, 17));
    CHECK(!memcmp(dad_addr(pushed[2].second), deliv, 17));

    // 18. A teardown while the reply is held drops it: nothing is delivered late and nothing is sent.
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_cfg td;
    td.a25 = g25;
    td.pcscf_len = 33;
    CHECK(dad_settings_through_proxy(td));
    sdu stop2(0, 7, UMI_IMS_STOP);
    ims_pdp_observe(dad_slot, stop2.b, stop2.n);
    CHECK(!ims_dad_hold[0].used && !ims_dad[dad_slot].held);
    now += IMS_DAD_HOLD_MS + IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.empty());

    // 19. The address is already on the interface when the reply arrives (the poll's snapshot
    //     already has it): no hold, but it is still captured as the delivery-time candidate and sent
    //     next to the reply's TLV one.
    dad_begin(true, 1);
    dad_ifnet(1);
    dad_ifaddr_set(1, deliv, 64);
    ims_dad_ifaddr_refresh(true);                /* the snapshot the poll took before the reply */
    CHECK(ims_dad_ifaddrs[1].n == 1);
    mark = log_lines.size();
    CHECK(!dad_settings_through_proxy(imscfg));
    CHECK(pushed.size() == 1 && !ims_dad_hold_pending());
    CHECK(ims_dad[dad_slot].t0_n == 1 && !memcmp(ims_dad[dad_slot].t0[0], deliv, 17));
    CHECK(count_from(mark, "0x2d not held (ifaddr present) cid " + std::to_string(dad_cid) + "\n") == 1);
    now += IMS_DAD_DELAY_MS;
    dad_finish();
    CHECK(pushed.size() == 3);
    CHECK(!memcmp(dad_addr(pushed[1].second), g25, 17));
    CHECK(!memcmp(dad_addr(pushed[2].second), deliv, 17));

    // 20. The internet PDN is never held, even when its runtime settings carry a non-empty P-CSCF
    //     list (0x2e length 33) that looks like IMS: pdp_ip0 is the fail-safe (v29i review S1).
    dad_begin(true, 0);                          /* mux 1 / pdp_ip0: the internet PDN */
    dad_ifnet(0);
    mark = log_lines.size();
    dad_cfg inet_like_ims;
    inet_like_ims.a25 = g25;
    inet_like_ims.pcscf_len = 33;
    CHECK(!dad_settings_through_proxy(inet_like_ims));
    CHECK(pushed.size() == 1 && !ims_dad_hold_pending());
    CHECK(count_from(mark, "0x2d ") == 0);

    // 21. Second activation on the same WDS client (no re-bind, no switch). nd6 dropped the address
    //     with the old link, so the snapshot left over from activation 1 is stale: Start must clear
    //     that unit, or the new reply looks "ifaddr present" and is delivered with an old t0
    //     (v29i review B1). Here the reply is held, the poll releases it, and t0 is the NEW address.
    {
        const uint8_t olda[17] = { 0x2a, 0x0a, 0x0b, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0xde, 0xad, 0xbe, 0xef, 64 };
        const uint8_t newa[17] = { 0x2a, 0x0d, 0x0e, 0x0f, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x02, 0x03, 0x04, 64 };
        dad_begin(true, 1);
        dad_ifnet(1);
        dad_ifaddr_set(1, olda, 64);
        CHECK(dad_settings_through_proxy(imscfg));   /* held: the snapshot starts empty */
        now += IMS_DAD_HOLD_POLL_MS;
        ims_dad_ifaddr_refresh(false);               /* the poll's snapshot: olda */
        ims_dad_poll();
        CHECK(!ims_dad_hold_pending() && ims_dad_ifaddrs[1].n == 1);
        now += IMS_DAD_DELAY_MS;
        dad_finish();                                /* activation 1's 0x5561 round */
        sdu stop(0, 31, UMI_IMS_STOP);
        ims_pdp_observe(dad_slot, stop.b, stop.n);
        dad_ifaddr_n[1] = 0;                         /* nd6 dropped the address with the link */
        sdu start(0, 32, UMI_IMS_START);
        start.tlv8(0x01, 0).tlv8(0x0e, 1).tlv8(0x1f, 1);
        ims_pdp_observe(dad_slot, start.b, start.n);
        const uint8_t handle2[4] = { 0x36, 0x12, 0, 0 };
        sdu started(2, 32, UMI_IMS_START);
        started.result().tlv(0x01, handle2, sizeof handle2);
        ims_pdp_observe(dad_slot, started.b, started.n);
        sdu setfam(0, 33, CBP_FAMILY);
        setfam.tlv8(0x01, 6);
        ims_pdp_observe(dad_slot, setfam.b, setfam.n);
        sdu famr(2, 33, CBP_FAMILY);
        famr.result();
        ims_pdp_observe(dad_slot, famr.b, famr.n);
        CHECK(bb_pdp_clients[dad_slot].active && bb_pdp_clients[dad_slot].family == 6);
        CHECK(ims_dad_ifaddrs[1].n == 0);            /* Start invalidated the stale snapshot */
        const size_t before = pushed.size();
        mark = log_lines.size();
        CHECK(dad_settings_through_proxy(imscfg));   /* held: the interface really has no address */
        CHECK(ims_dad_hold_pending() && pushed.size() == before);
        dad_ifaddr_set(1, newa, 64);
        now += IMS_DAD_HOLD_POLL_MS;
        ims_dad_ifaddr_refresh(false);               /* the poll's snapshot: newa */
        ims_dad_poll();                              /* the poll path releases it */
        CHECK(pushed.size() == before + 1 && !ims_dad_hold_pending());
        CHECK(ims_dad[dad_slot].t0_n == 1 && !memcmp(ims_dad[dad_slot].t0[0], newa, 17));
        CHECK(count_from(mark, "0x2d held 20 ms (ifaddr=yes) cid " + std::to_string(dad_cid) + "\n") == 1);
    }

    // 22. A retransmitted 0x2d inside the hold window: the older answer is handed over at once (its
    //     own line), the newer one is held; the activation's "0x2d held" line then describes the
    //     answer actually waited on.
    dad_begin(true, 1);
    dad_ifnet(1);
    mark = log_lines.size();
    CHECK(dad_settings_through_proxy(imscfg));
    CHECK(ims_dad_hold_pending() && pushed.empty());
    CHECK(dad_settings_through_proxy(imscfg));
    CHECK(pushed.size() == 1 && ims_dad_hold_pending());
    CHECK(count_from(mark, "0x2d retransmit; older answer handed over cid " + std::to_string(dad_cid) + "\n") == 1);
    CHECK(count_from(mark, "0x2d held ") == 0);
    sdu stop3(0, 41, UMI_IMS_STOP);
    ims_pdp_observe(dad_slot, stop3.b, stop3.n);
    CHECK(!ims_dad_hold_pending());

    // 23. IMS is switched off while a reply is held: deliver it, never drop a real X55 answer.
    dad_begin(true, 1);
    dad_ifnet(1);
    CHECK(dad_settings_through_proxy(imscfg));
    CHECK(pushed.empty() && ims_dad_hold_pending());
    mark = log_lines.size();
    ims_state(false);                                /* the ims-on file disappeared */
    CHECK(!ims_enabled);
    CHECK(pushed.size() == 1 && !ims_dad_hold_pending());
    CHECK(count_from(mark, "0x2d held 0 ms (ifaddr=timeout) cid " + std::to_string(dad_cid) + "\n") == 1);

    // ---- Route B P1: /var/db/hakios-voice/modem-ims (default off, ims-on wins) ----
    cbq_init(&bb_qmux);
    memset(ims_owner, 0, sizeof ims_owner);
    memset(bb_pdp_clients, 0, sizeof bb_pdp_clients);
    pushed.clear();
    ims_state(false);
    modem_ims_state(false);
    CHECK(!ims_enabled && !modem_ims_enabled);

    // Off by default: the same NAS request is not touched, nothing is logged or sent.
    sdu selection(0, 4, UMI_IMS_SYSTEM_SELECTION);
    const uint8_t domain_cs[4] = {0, 0, 0, 0};
    selection.tlv8(0x10, 0).tlv(UMI_IMS_VOICE_DOMAIN, domain_cs, sizeof domain_cs);
    uint8_t nas[64];
    mark = log_lines.size();
    memset(nas, 0xa5, sizeof nas);
    CHECK(ims_nas_voice_domain(UMI_IMS_NAS, selection.b, selection.n, nas, sizeof nas) == 0);
    CHECK(nas[0] == 0xa5 && count_from(mark, "NAS 0x33") == 0);
    uint8_t imss_packet[24];
    memset(imss_packet, 0xa5, sizeof imss_packet);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0 && imss_packet[0] == 0xa5);

    // Sampling: ims-on wins; the ignored line is emitted once per transition.
    ims_audit.bound = false;
    ims_audit.pending = false;
    ims_audit.indications = 0;
    ims_audit.port = 0;
    ims_audit.registration = -1;
    switch_file = false;
    modem_switch = false;
    ims_check_switch(nullptr, nullptr);
    CHECK(!ims_switch_wanted && !modem_ims_wanted);
    switch_file = true;
    modem_switch = true;
    ims_check_switch(nullptr, nullptr);
    CHECK(ims_switch_wanted && !modem_ims_wanted);
    ims_check_switch(nullptr, nullptr);
    CHECK(count_from(mark, "modem-ims ignored: ims-on present") == 1);
    switch_file = false;
    modem_switch = true;
    ims_check_switch(nullptr, nullptr);
    CHECK(!ims_switch_wanted && modem_ims_wanted);
    mark = log_lines.size();
    ims_state(ims_switch_wanted);
    modem_ims_state(modem_ims_wanted);
    CHECK(!ims_enabled && modem_ims_enabled);
    CHECK(count_from(mark, "modem-ims switch on; IMSS 0x8f from bridge") == 1);
    // One registration owner (ims_enabled stays off), but the AP-side media stubs are shared by
    // both owners: 0x28 is private and the 0x28/0xe1 answers are given in route B too (v31 §5.2).
    CHECK(!ims_enabled && bb_qmux.imsrtp == 1);

    // Bearer inversion: the AP ims PDN is allowed even while the modem audit is idle.
    CHECK(!ims_audit.bound && ims_audit.registration == -1 && ims_bearer_allowed());

    // The IPv6-only request rewrite is skipped; the protocol helper itself still proves the shape.
    sdu ims_start(0, 3, UMI_IMS_START);
    const uint8_t apn[3] = {'i', 'm', 's'};
    ims_start.tlv(UMI_IMS_APN_TLV, apn, sizeof apn).tlv8(UMI_IMS_FAMILY_TLV, 3);
    CHECK(umi_ims_prepare(UMI_IMS_WDS, ims_start.b, ims_start.n, ims_fix, sizeof ims_fix).family_changed);
    CHECK(!ims_prepare(UMI_IMS_WDS, ims_start.b, ims_start.n).len);

    // NAS 0x33: a copy is rewritten to PS only; the TLV type sequence is logged once.
    mark = log_lines.size();
    CHECK(ims_nas_voice_domain(UMI_IMS_NAS, selection.b, selection.n, nas, sizeof nas) == selection.n);
    uint32_t domain_len = 0;
    const uint8_t *domain = umi_ims_find(nas, selection.n, UMI_IMS_VOICE_DOMAIN, &domain_len);
    CHECK(domain && domain_len == 4 && umi_ims_rd32(domain) == 1);
    CHECK(count_from(mark, "NAS 0x33 tlvs 10 23\n") == 1);
    CHECK(count_from(mark, "NAS 0x33 voice domain 0 -> PS only\n") == 1);
    mark = log_lines.size();
    CHECK(ims_nas_voice_domain(UMI_IMS_NAS, selection.b, selection.n, nas, sizeof nas) == selection.n);
    CHECK(count_from(mark, "NAS 0x33") == 0);
    // Already PS only: the bytes iOS sent are forwarded unchanged.
    sdu ps_selection(0, 5, UMI_IMS_SYSTEM_SELECTION);
    const uint8_t domain_ps[4] = {1, 0, 0, 0};
    ps_selection.tlv(UMI_IMS_VOICE_DOMAIN, domain_ps, sizeof domain_ps);
    memset(nas, 0xa5, sizeof nas);
    CHECK(ims_nas_voice_domain(UMI_IMS_NAS, ps_selection.b, ps_selection.n, nas, sizeof nas) == 0 && nas[0] == 0xa5);
    // A new switch-on session reports a 0x33 without the voice domain, and still does not add it.
    modem_ims_state(false);
    modem_ims_state(true);
    sdu no_domain(0, 6, UMI_IMS_SYSTEM_SELECTION);
    no_domain.tlv8(0x10, 0);
    mark = log_lines.size();
    CHECK(ims_nas_voice_domain(UMI_IMS_NAS, no_domain.b, no_domain.n, nas, sizeof nas) == 0);
    CHECK(count_from(mark, "NAS 0x33 tlvs 10; no 0x23, not rewritten\n") == 1);
    CHECK(ims_nas_voice_domain(UMI_IMS_WDS, selection.b, selection.n, nas, sizeof nas) == 0);

    // IMSS: qcril's client preamble -- bind to the primary subscription when the modem advertises
    // it, Get Supported Messages/Fields, Config Ind Register, then the enable-config read -- and only
    // after every answer the one 0x8f Set IMS Services Enabled per switch-on, on the bridge's port.
    cbq_init(&bb_qmux);
    CHECK(cbq_server_new(&bb_qmux, UMI_IMS_IMSS, 1, 3, 73));
    modem_ims_state(false);
    memset(imss_packet, 0xa5, sizeof imss_packet);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0 && imss_packet[0] == 0xa5);
    modem_ims_state(true);
    mark = log_lines.size();
    node = port = 0;
    // A1: Get Supported Messages, empty payload. The answer is TLV 0x10: a u16 element count, then
    // one bit per message id (LSB first): 0x2a = byte 5 bit 2, 0x8f = byte 17 bit 7, 0x90 = byte 18
    // bit 0, 0x98 = byte 19 bit 0. 22 bytes, the X55's max message id 0x00a9.
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 7 && node == 3 && port == 73);
    CHECK(umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_SUPPORTED_MSGS && umi_ims_rd16(imss_packet + 5) == 0);
    sdu msgs(2, ims_imss.txn, UMI_IMS_IMSS_GET_SUPPORTED_MSGS);
    uint8_t msgs_bitmap[22] = {};
    msgs_bitmap[5] = 0x04;
    msgs_bitmap[17] = 0x80;
    msgs_bitmap[18] = 0x01;
    msgs_bitmap[19] = 0x01;
    std::string map_hex;
    for (uint8_t byte : msgs_bitmap) { char b[3]; snprintf(b, sizeof b, "%02x", byte); map_hex += b; }
    uint8_t msgs_map[2 + sizeof msgs_bitmap];
    umi_ims_wr16(msgs_map, sizeof msgs_bitmap);
    memcpy(msgs_map + 2, msgs_bitmap, sizeof msgs_bitmap);
    msgs.result().tlv(0x10, msgs_map, sizeof msgs_map);
    ims_imss_receive(4, 73, msgs.b, msgs.n);
    CHECK(ims_imss.pending && ims_imss.step == IMSS_STEP_MSGS);   // another node is not the answer
    umi_ims_wr16(msgs.b + 1, ims_imss.txn + 1);
    ims_imss_receive(3, 73, msgs.b, msgs.n);
    CHECK(ims_imss.pending);                                       // another transaction is not the answer
    umi_ims_wr16(msgs.b + 1, ims_imss.txn);
    ims_imss_receive(3, 73, msgs.b, msgs.n);
    CHECK(!ims_imss.pending && ims_imss.step == IMSS_STEP_BIND && ims_imss.bind_supported);
    CHECK(count_from(mark, "imss supported msgs result 0 tlvs 02:4 10:24; 8f=1 2a=1 90=1 98=1 map=" + map_hex + "\n") == 1);
    // B1: Set Bind Subscription (0x98), primary, only because 0x1e advertised it. TLV 0x01, u32.
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 14);
    CHECK(umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_BIND && umi_ims_rd16(imss_packet + 5) == 7);
    CHECK(imss_packet[7] == UMI_IMS_IMSS_BIND_TLV && umi_ims_rd16(imss_packet + 8) == 4);
    CHECK(umi_ims_rd32(imss_packet + 10) == UMI_IMS_IMSS_SUBSCRIPTION_PRIMARY);
    sdu bind_sdu(2, ims_imss.txn, UMI_IMS_IMSS_BIND);
    bind_sdu.result();
    ims_imss_receive(3, 73, bind_sdu.b, bind_sdu.n);
    CHECK(ims_imss.step == IMSS_STEP_FIELDS);
    CHECK(count_from(mark, "imss 0x98 bind primary result 0\n") == 1);
    // A2: Get Supported Fields for 0x8f. The request is TLV 0x01 carrying msg_id as a u16; the
    // answer's TLV 0x10 request-fields bitmap has a one-byte count and bit N = TLV 0x10+N.
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 12);
    CHECK(umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_SUPPORTED_FIELDS);
    CHECK(umi_ims_rd16(imss_packet + 5) == 5 && imss_packet[7] == 0x01 && umi_ims_rd16(imss_packet + 8) == 2);
    CHECK(umi_ims_rd16(imss_packet + 10) == UMI_IMS_IMS_SETTINGS_ENABLED);
    sdu fields(2, ims_imss.txn, UMI_IMS_IMSS_GET_SUPPORTED_FIELDS);
    const uint8_t field_map[3] = { 2, 0x13, 0x01 };
    fields.result().tlv(0x10, field_map, sizeof field_map);
    ims_imss_receive(3, 73, fields.b, fields.n);
    CHECK(count_from(mark, "imss supported fields result 0 tlvs 02:4 10:3; 8f 10=1 11=1 14=1 18=1\n") == 1);
    // B2: the four booleans the vendor registers for (qcril does this before the read).
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 23);
    CHECK(umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_CONFIG_IND_REG && umi_ims_rd16(imss_packet + 5) == 16);
    CHECK(imss_packet[7] == 0x11 && umi_ims_rd16(imss_packet + 8) == 1 && imss_packet[10] == 1);
    CHECK(imss_packet[11] == 0x1f && umi_ims_rd16(imss_packet + 12) == 1 && imss_packet[14] == 1);
    CHECK(imss_packet[15] == 0x2d && umi_ims_rd16(imss_packet + 16) == 1 && imss_packet[18] == 1);
    CHECK(imss_packet[19] == 0x31 && umi_ims_rd16(imss_packet + 20) == 1 && imss_packet[22] == 1);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0);
    sdu indreg(2, ims_imss.txn, UMI_IMS_IMSS_CONFIG_IND_REG);
    indreg.result();
    ims_imss_receive(3, 73, indreg.b, indreg.n);
    CHECK(ims_imss_chain && ims_imss.step == IMSS_STEP_GET);
    CHECK(count_from(mark, "imss 0x2a config ind reg result 0\n") == 1);
    // A3: Get IMS Services Enabled (0x90), empty payload; one-byte TLVs are its booleans.
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 7 && umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_ENABLED);
    sdu enabled(2, ims_imss.txn, UMI_IMS_IMSS_GET_ENABLED);
    enabled.result().tlv8(0x11, 1).tlv8(0x19, 0);
    ims_imss_receive(3, 73, enabled.b, enabled.n);
    CHECK(count_from(mark, "imss get enabled result 0 tlvs 02:4 11:1=1 19:1=0\n") == 1);
    // A4: the read-only Get IMS Policy Manager Settings (0x48), empty payload, one per boot; its
    // priorities/mask are values, the APN list only a count and byte total (never its text).
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 7 && umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_POLICY);
    CHECK(umi_ims_rd16(imss_packet + 5) == 0);
    sdu policy(2, ims_imss.txn, UMI_IMS_IMSS_GET_POLICY);
    const uint8_t policy_error[4] = {0, 0, 0, 0};
    const uint8_t apn_list[] = {3, 'i', 'm', 's', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    char service_mask[17];
    snprintf(service_mask, sizeof service_mask, "00000000%08x", 3u);
    policy.result().tlv(0x10, policy_error, sizeof policy_error)
          .tlv8(0x15, 1).tlv8(0x16, 0).tlv8(0x17, 2).tlv8(0x18, 3)
          .tlv(0x19, reinterpret_cast<const uint8_t *>("\3\0\0\0\0\0\0\0"), 8)
          .tlv(0x1a, apn_list, sizeof apn_list);
    ims_imss_receive(3, 73, policy.b, policy.n);
    CHECK(count_from(mark, "imss 0x48 policy result 0 tlvs 02:4 10:4 15:1=1 16:1=0 17:1=2 18:1=3 19:8 1a:14; error 0 prio acs 1 isim 0 nv 2 pco 3 mask " +
                           std::string(service_mask) + " apns 1/3 bytes\n") == 1);
    CHECK(log_lines.find("\"ims\"") == std::string::npos);
    // C: one 0x8f, only after every precondition answered.
    memset(imss_packet, 0xa5, sizeof imss_packet);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 15);
    CHECK(imss_packet[0] == 0 && umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMS_SETTINGS_ENABLED);
    CHECK(umi_ims_rd16(imss_packet + 5) == 8);
    CHECK(imss_packet[7] == 0x10 && umi_ims_rd16(imss_packet + 8) == 1 && imss_packet[10] == 1);
    CHECK(imss_packet[11] == 0x18 && umi_ims_rd16(imss_packet + 12) == 1 && imss_packet[14] == 1);
    CHECK(count_from(mark, "imss 0x8f sent (VoLTE 1, IMS 1); X55 may persist this enable in its own EFS\n") == 1);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0);   // never sent twice
    sdu imss_answer(2, ims_imss.txn, UMI_IMS_IMS_SETTINGS_ENABLED);
    imss_answer.result();
    ims_imss_receive(4, 73, imss_answer.b, imss_answer.n);
    CHECK(ims_imss.pending);   // another node is not the answer
    umi_ims_wr16(imss_answer.b + 1, ims_imss.txn + 1);
    ims_imss_receive(3, 73, imss_answer.b, imss_answer.n);
    CHECK(ims_imss.pending);   // another transaction is not the answer
    umi_ims_wr16(imss_answer.b + 1, ims_imss.txn);
    ims_imss_receive(3, 73, imss_answer.b, imss_answer.n);
    CHECK(!ims_imss.pending && ims_imss.result == 0 && ims_imss.step == IMSS_STEP_DONE);
    CHECK(count_from(mark, "imss 0x8f result 0\n") == 1);
    // A timeout stops the request; it is not resent. The preamble is a boot fact, so a new switch-on
    // session goes straight to 0x8f.
    modem_ims_state(false);
    modem_ims_state(true);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 15);
    now += 5000;
    mark = log_lines.size();
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0 && ims_imss.result == -1);
    CHECK(count_from(mark, "imss 0x8f timeout; not resent\n") == 1);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0);
    mark = log_lines.size();
    modem_ims_state(false);
    modem_ims_state(true);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 15);
    sdu imss_error(2, ims_imss.txn, UMI_IMS_IMS_SETTINGS_ENABLED);
    imss_error.result(3);
    ims_imss_receive(3, 73, imss_error.b, imss_error.n);
    CHECK(ims_imss.result == 3 && count_from(mark, "imss 0x8f result 3\n") == 1);
    // Switching off releases the port.
    modem_ims_state(false);
    modem_ims_state(true);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 15);
    modem_ims_state(false);
    CHECK(ims_imss.release && ims_imss_poll(imss_packet, &node, &port) == 0);
    // A modem that does not advertise 0x98 gets no bind: the chain steps from 0x1e straight to 0x1f.
    ims_imss_chain = 0;
    ims_imss_stalled = 0;
    ims_imss_logged = 0;
    ims_imss.bind_supported = 0;
    modem_ims_state(false);
    modem_ims_state(true);
    mark = log_lines.size();
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 7 && umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_SUPPORTED_MSGS);
    sdu msgs_no_bind(2, ims_imss.txn, UMI_IMS_IMSS_GET_SUPPORTED_MSGS);
    uint8_t no_bind_bitmap[22] = {};
    no_bind_bitmap[5] = 0x04;
    no_bind_bitmap[17] = 0x80;
    uint8_t no_bind_map[2 + sizeof no_bind_bitmap];
    umi_ims_wr16(no_bind_map, sizeof no_bind_bitmap);
    memcpy(no_bind_map + 2, no_bind_bitmap, sizeof no_bind_bitmap);
    msgs_no_bind.result().tlv(0x10, no_bind_map, sizeof no_bind_map);
    ims_imss_receive(3, 73, msgs_no_bind.b, msgs_no_bind.n);
    CHECK(ims_imss.step == IMSS_STEP_FIELDS && !ims_imss.bind_supported);
    CHECK(count_from(mark, "98=0 map=") == 1);
    CHECK(count_from(mark, "imss 0x98 bind") == 0);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 12 && umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_SUPPORTED_FIELDS);
    // A preamble step that times out stops the whole thing for the boot: no 0x8f, no retry even in
    // a new session, and the one-line budgets keep every answer line single.
    ims_imss_chain = 0;
    ims_imss_stalled = 0;
    modem_ims_state(false);
    modem_ims_state(true);
    mark = log_lines.size();
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 7 && umi_ims_rd16(imss_packet + 3) == UMI_IMS_IMSS_GET_SUPPORTED_MSGS);
    now += 5000;
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0 && ims_imss.result == -1 && ims_imss_stalled);
    CHECK(count_from(mark, "imss 0x001e timeout; chain stopped, not resent\n") == 1);
    modem_ims_state(false);
    modem_ims_state(true);
    CHECK(ims_imss_poll(imss_packet, &node, &port) == 0);
    CHECK(count_from(mark, "imss 0x8f sent") == 0);

    // IMSA in modem-ims mode: Bind, then Register Indications once, then the usual query; the
    // 0x23/0x24 indications change the modem state line only when it actually changes.
    cbq_init(&bb_qmux);
    CHECK(cbq_server_new(&bb_qmux, UMI_IMS_IMSA, 1, 3, 73));
    modem_ims_state(true);
    ims_audit.bound = false;
    ims_audit.pending = false;
    ims_audit.indications = 0;
    ims_audit.port = 0;
    ims_audit.registration = -1;
    now += 10;
    ims_audit.next_at = now;
    CHECK(ims_poll(packet, &node, &port) == 14 && umi_ims_rd16(packet + 3) == UMI_IMS_BIND_SUBSCRIPTION);
    sdu bind_answer(2, ims_audit.txn, UMI_IMS_BIND_SUBSCRIPTION);
    bind_answer.result();
    ims_receive(3, 73, bind_answer.b, bind_answer.n);
    CHECK(ims_audit.bound);
    CHECK(ims_poll(packet, &node, &port) == 15 && umi_ims_rd16(packet + 3) == UMI_IMS_IMSA_INDICATIONS);
    CHECK(umi_ims_rd16(packet + 5) == 8);
    CHECK(packet[7] == 0x10 && packet[10] == 1 && packet[11] == 0x11 && packet[14] == 1);
    mark = log_lines.size();
    sdu indications_answer(2, ims_audit.txn, UMI_IMS_IMSA_INDICATIONS);
    indications_answer.result();
    ims_receive(3, 73, indications_answer.b, indications_answer.n);
    CHECK(ims_audit.indications && count_from(mark, "imsa 0x22 register indications result 0\n") == 1);
    CHECK(ims_poll(packet, &node, &port) == 7 && umi_ims_rd16(packet + 3) == UMI_IMS_GET_REGISTRATION);
    mark = log_lines.size();
    sdu registration(4, 0, UMI_IMS_IMSA_REGISTRATION_IND);
    const uint8_t registered[4] = {2, 0, 0, 0};
    const uint8_t technology_lte[4] = {2, 0, 0, 0};
    registration.tlv(0x11, registered, sizeof registered).tlv(0x13, technology_lte, sizeof technology_lte);
    ims_receive(3, 73, registration.b, registration.n);
    CHECK(count_from(mark, "modem registration 2 technology 2; voice service -1; error -1 text 0 bytes\n") == 1);
    ims_receive(3, 73, registration.b, registration.n);
    CHECK(count_from(mark, "modem registration") == 1);
    // The 0x23 error TLVs (0x10 u16 code, 0x12 error text): numeric code and the text's length only,
    // because the text can carry a SIP URI or a number.
    {
        const uint8_t err_code[2] = {7, 0};
        const char err_text[] = "sip:PRIVATE-CANARY@example.invalid";
        sdu failed(4, 0, UMI_IMS_IMSA_REGISTRATION_IND);
        failed.tlv(0x11, registered, sizeof registered).tlv(0x13, technology_lte, sizeof technology_lte)
              .tlv(0x10, err_code, sizeof err_code)
              .tlv(0x12, reinterpret_cast<const uint8_t *>(err_text), sizeof err_text - 1);
        ims_receive(3, 73, failed.b, failed.n);
        CHECK(count_from(mark, "modem registration 2 technology 2; voice service -1; error 7 text " +
                               std::to_string(sizeof err_text - 1) + " bytes\n") == 1);
        CHECK(log_lines.find("PRIVATE-CANARY") == std::string::npos);
    }
    const uint8_t voice_available[4] = {1, 0, 0, 0};
    sdu services(4, 0, UMI_IMS_IMSA_SERVICES_IND);
    services.tlv(0x11, voice_available, sizeof voice_available);
    // The error state survives a services indication (it is the 0x23 side's last value).
    ims_receive(3, 73, services.b, services.n);
    CHECK(count_from(mark, "modem registration 2 technology 2; voice service 1; error 7 text 34 bytes\n") == 1);
    ims_receive(3, 73, services.b, services.n);
    CHECK(count_from(mark, "modem registration") == 3);
    ims_receive(4, 73, registration.b, registration.n);   // foreign node: ignored
    CHECK(count_from(mark, "modem registration") == 3);
    // The 10 s query keeps its line only when the registration moved.
    mark = log_lines.size();
    const uint8_t legacy_registered[1] = {1};
    sdu query_answer(2, ims_audit.txn, UMI_IMS_GET_REGISTRATION);
    query_answer.result().tlv(0x10, legacy_registered, sizeof legacy_registered);
    ims_receive(3, 73, query_answer.b, query_answer.n);
    CHECK(count_from(mark, "modem IMS audit result") == 1);
    now += 10000;
    CHECK(ims_poll(packet, &node, &port) == 7);
    mark = log_lines.size();
    sdu same_answer(2, ims_audit.txn, UMI_IMS_GET_REGISTRATION);
    same_answer.result().tlv(0x10, legacy_registered, sizeof legacy_registered);
    ims_receive(3, 73, same_answer.b, same_answer.n);
    CHECK(count_from(mark, "modem IMS audit result") == 0);
    // The 0x20 error TLVs (0x11 u16 code, 0x13 error text): one line when the code or the text
    // length moves, numeric code and text length only.
    now += 10000;
    CHECK(ims_poll(packet, &node, &port) == 7);
    mark = log_lines.size();
    const uint8_t code_denied[2] = {0x0b, 0x00};
    const char code_text[] = "tel:PRIVATE-CANARY";
    sdu err_answer(2, ims_audit.txn, UMI_IMS_GET_REGISTRATION);
    err_answer.result().tlv(0x10, legacy_registered, sizeof legacy_registered)
              .tlv(0x11, code_denied, sizeof code_denied)
              .tlv(0x13, reinterpret_cast<const uint8_t *>(code_text), sizeof code_text - 1);
    ims_receive(3, 73, err_answer.b, err_answer.n);
    CHECK(count_from(mark, "modem IMS registration error 11 text 18 bytes\n") == 1);
    CHECK(log_lines.find("PRIVATE-CANARY") == std::string::npos);
    now += 10000;
    CHECK(ims_poll(packet, &node, &port) == 7);
    mark = log_lines.size();
    sdu err_again(2, ims_audit.txn, UMI_IMS_GET_REGISTRATION);
    err_again.result().tlv(0x10, legacy_registered, sizeof legacy_registered)
             .tlv(0x11, code_denied, sizeof code_denied)
             .tlv(0x13, reinterpret_cast<const uint8_t *>(code_text), sizeof code_text - 1);
    ims_receive(3, 73, err_again.b, err_again.n);
    CHECK(count_from(mark, "modem IMS registration error") == 0);
    // ---- Route B probe: IMSDCM 770 (0x302 v1 instance 0), observe only ----------------------
    // Switch off: no announce request, no line, no answer -- the bytes of the old behaviour.
    modem_ims_state(false);
    uint32_t dcm_service = 0, dcm_version = 0, dcm_instance = 0, dcm_local = 0;
    uint8_t dcm_out[32];
    mark = log_lines.size();
    CHECK(!ims_dcm_announce(&dcm_service, &dcm_version, &dcm_instance, &dcm_local));
    sdu dcm_start(0, 11, UMI_IMS_DCM_START);
    const uint8_t dcm_conn[] = { 3, 'i', 'm', 's', 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0xff, 0xff, 0xff };
    const uint8_t dcm_seq[4] = { 7, 0, 0, 0 }, dcm_zero[4] = { 0, 0, 0, 0 };
    dcm_start.tlv(0x01, dcm_conn, sizeof dcm_conn)
             .tlv(0x10, dcm_seq, sizeof dcm_seq).tlv(0x11, dcm_zero, sizeof dcm_zero)
             .tlv(0x12, dcm_zero, sizeof dcm_zero).tlv(0x13, dcm_zero, sizeof dcm_zero);
    CHECK(!ims_dcm_receive(UMI_IMS_DCM_PORT, 3, 77, dcm_start.b, dcm_start.n));
    CHECK(!ims_dcm_reply(dcm_out, sizeof dcm_out, &node, &port, &dcm_local));
    CHECK(count_from(mark, "dcm:") == 0);
    // Switch on: exactly the Android qrtr-lookup parameters (770 v1 instance 0), once per edge.
    modem_ims_state(true);
    CHECK(ims_dcm_announce(&dcm_service, &dcm_version, &dcm_instance, &dcm_local) == 1);
    CHECK(dcm_service == 0x302 && dcm_version == 1 && dcm_instance == 0 && dcm_local == UMI_IMS_DCM_PORT);
    CHECK(!ims_dcm_announce(&dcm_service, &dcm_version, &dcm_instance, &dcm_local));
    // 0x20 PDP Activate Request: one line with the TLV ids plus the TLV 0x01 sequence (APN string,
    // APN type, RAT, IP family, WDS profile) and 0x10..0x13 values; nothing is answered, so no
    // bearer is created.
    mark = log_lines.size();
    CHECK(ims_dcm_receive(UMI_IMS_DCM_PORT, 3, 77, dcm_start.b, dcm_start.n) == 1);
    CHECK(count_from(mark, "dcm: msg 0x0020 node 3 port 77 len 58 tlvs 01:20 10:4 11:4 12:4 13:4; apn \"ims\" apn_type 0 rat 0 ip_family 2 profile 4294967295 seq 7 sub 0 slot 0 inst 0\n") == 1);
    CHECK(!ims_dcm_reply(dcm_out, sizeof dcm_out, &node, &port, &dcm_local));
    // A retry with a new transaction is the same line; a different IP family is a new one.
    umi_ims_wr16(dcm_start.b + 1, 12);
    CHECK(ims_dcm_receive(UMI_IMS_DCM_PORT, 3, 77, dcm_start.b, dcm_start.n) == 1);
    CHECK(count_from(mark, "dcm: msg 0x0020") == 1);
    dcm_start.b[22] = 6;   /* ip_family 2 -> 6 */
    CHECK(ims_dcm_receive(UMI_IMS_DCM_PORT, 3, 77, dcm_start.b, dcm_start.n) == 1);
    CHECK(count_from(mark, "dcm: msg 0x0020") == 2);
    // QMI CTL for service 770 (which does not fit the u8 Service TLV): the u16 QRTR Allocate CID
    // gets {result 0, {service, cid 1}} from the port it was addressed to.
    sdu dcm_ctl(0, 9, UMI_IMS_CTL_ALLOCATE_QRTR);
    const uint8_t ctl_service[2] = { 0x02, 0x03 };   /* 0x0302 */
    dcm_ctl.tlv(0x01, ctl_service, sizeof ctl_service);
    CHECK(ims_dcm_receive(UMI_IMS_QRTR_CTRL, 3, 78, dcm_ctl.b, dcm_ctl.n) == 1);
    CHECK(ims_dcm_reply(dcm_out, sizeof dcm_out, &node, &port, &dcm_local) == 20);
    const uint8_t want_alloc[20] = { 2, 9, 0, 0x22, 0xff, 13, 0, 0x02, 4, 0, 0, 0, 0, 0, 0x01, 3, 0, 2, 3, 1 };
    CHECK(!memcmp(dcm_out, want_alloc, sizeof want_alloc) && node == 3 && port == 78 && dcm_local == UMI_IMS_QRTR_CTRL);
    CHECK(count_from(mark, "dcm: ctl msg 0xff22 node 3 port 78 svc 0x302 -> cid 1\n") == 1);
    // Get Version Info (0x21): one service entry, 770's low byte, v1.0.
    sdu dcm_ver(0, 10, UMI_IMS_CTL_GET_VERSION);
    CHECK(ims_dcm_receive(UMI_IMS_QRTR_CTRL, 3, 78, dcm_ver.b, dcm_ver.n) == 1);
    CHECK(ims_dcm_reply(dcm_out, sizeof dcm_out, &node, &port, &dcm_local) == 23);
    const uint8_t want_ver[23] = { 2, 10, 0, 0x21, 0, 16, 0, 0x02, 4, 0, 0, 0, 0, 0, 0x01, 6, 0, 1, 2, 1, 0, 0, 0 };
    CHECK(!memcmp(dcm_out, want_ver, sizeof want_ver));
    // Release echoes {service, cid}; a non-CTL message on the control port is left to the kernel.
    sdu dcm_rel(0, 11, UMI_IMS_CTL_RELEASE);
    const uint8_t rel_pair[2] = { 0x02, 5 };
    dcm_rel.tlv(0x01, rel_pair, sizeof rel_pair);
    CHECK(ims_dcm_receive(UMI_IMS_QRTR_CTRL, 3, 78, dcm_rel.b, dcm_rel.n) == 1);
    CHECK(ims_dcm_reply(dcm_out, sizeof dcm_out, &node, &port, &dcm_local) == 19 && dcm_out[17] == 2 && dcm_out[18] == 5);
    CHECK(!ims_dcm_receive(UMI_IMS_QRTR_CTRL, 3, 78, dcm_start.b, dcm_start.n));
    // Retry flood: identical lines are deduped, and the boot budget stops at UMI_DCM_LINES.
    const uint8_t before = ims_dcm.lines;
    mark = log_lines.size();
    for (unsigned i = 1; i <= 40; i++) {
        umi_ims_wr16(dcm_start.b + 1, (uint16_t)(100 + i));
        dcm_start.b[33] = (uint8_t)i;
        CHECK(ims_dcm_receive(UMI_IMS_DCM_PORT, 3, 77, dcm_start.b, dcm_start.n) == 1);
    }
    CHECK(ims_dcm.lines == UMI_DCM_LINES);
    CHECK(count_from(mark, "dcm:") == UMI_DCM_LINES - before);
    // The report's WDS profile line: iOS's IMS Start (it has vendor 0xC2) once per switch-on,
    // TLV 0x31 and 0xC2 values only; an internet Start (no 0xC2) and the switch-off emit nothing.
    sdu ims_profile_start(0, 40, UMI_IMS_START);
    ims_profile_start.tlv8(0x31, 2).tlv8(0xc2, 1);
    mark = log_lines.size();
    ims_wds_profile_probe(5, UMI_IMS_WDS, ims_profile_start.b, ims_profile_start.n);
    CHECK(count_from(mark, "WDS start cid 5 TLV 0x31 2 TLV 0xC2 1 (iOS IMS profile probe)\n") == 1);
    ims_wds_profile_probe(5, UMI_IMS_WDS, ims_profile_start.b, ims_profile_start.n);
    CHECK(count_from(mark, "WDS start cid") == 1);
    sdu net_start(0, 41, UMI_IMS_START);
    net_start.tlv8(0x31, 1);
    ims_wds_profile_probe(6, UMI_IMS_WDS, net_start.b, net_start.n);
    CHECK(count_from(mark, "WDS start cid") == 1);
    modem_ims_state(false);
    ims_wds_profile_probe(5, UMI_IMS_WDS, ims_profile_start.b, ims_profile_start.n);
    CHECK(count_from(mark, "WDS start cid") == 1);
    CHECK(!ims_dcm_announce(&dcm_service, &dcm_version, &dcm_instance, &dcm_local));
    // With modem-ims off the same indication changes nothing and logs nothing.
    modem_ims_state(false);
    const size_t indication_mark = log_lines.size();
    ims_receive(3, 73, registration.b, registration.n);
    CHECK(count_from(indication_mark, "modem registration") == 0);
    switch_file = false;
    modem_switch = false;
    ims_check_switch(nullptr, nullptr);
    ims_state(ims_switch_wanted);
    modem_ims_state(modem_ims_wanted);
    CHECK(!ims_enabled && !modem_ims_enabled);

    puts("PASS IMS bridge: switch, subscription bind, audit age, transactions, timeout, endpoint change, NULL flag notification, address notification without protocol reentry, payload redaction, IMSRTP 0x28 stub (allocation local/absent, version list, INITIALIZE response bytes, UNINITIALIZE, unsupported session messages deduped, release), AUDIO 0xe1 CallEvent gate (the {event, call type, session} triple: kVoIP events 0..8 ok, kEmergency/other types/other messages/other lengths unchanged, ims-off unchanged), 0x28 UninitializeAll/RegisterServices and the 0x003d indication (response first, then indication with TLV 0x01 app id / TLV 0x02 status / TLV 0x10 zero sessions, once per client, 0xff fallback), route B media parity (0x28 allocated and INITIALIZE answered under modem-ims, the e1 VoIP event answered, a CTL frame resampling a stale switch, both off refused again), umi_rtpok session lifecycle (0x0022/0x0023/0x0024/0x0025/0x0026: response then indication, app id and cookie echoed, session id assigned and reused, deduped log, gate off unchanged; 0x0029 response then its indication, 0x002A/0x002B response only, never an indication, once per id; 0x002C..0x002F response only), one round trip for the answers (held, then flushed in request order, drained by the switch), synthesized 0x5561 DAD complete (no ioctl, no request-side APN, on/off, route B modem-ims, the owner guard and idempotent switch, link-local, teardown, IMS 0x2e signature, 0x25/0x27/0x28 candidates, dedupe, nonzero txns, once per activation, interface-address retries and their one no-address line, 0x2d hold until the pdp_ip address, 500 ms bound, non-IMS untouched, delivery-time candidate with prefix, pdp_ip0 never held, stale snapshot cleared at Start, retransmit handover, ims-off delivers the held reply), route B modem-ims (switch sampling and ims-on precedence, off means untouched NAS/IMSS, bearer inversion, no IPv6-only rewrite, NAS 0x33 TLV 0x23 rewrite and its one type line, IMSS preamble (0x1e supported messages with its u16-count bitmap and hex line, 0x98 subscription bind only when advertised, 0x1f supported fields for 0x8f as TLV 0x01, one 0x2a with four booleans before the 0x90 enable read, the single 0x8f only after 0x2a, one line per answer, timeout/stall), IMSA 0x22 registration and 0x23/0x24 change-only lines, 0x20/0x23 error-code TLVs as numbers with the text length only), route B IMSDCM probe (770 v1 instance 0 announce request once per edge and nothing with the switch off, 0x20 Start Connection logged from its TLV 0x01 sequence and 0x10..0x13 with no answer, dedupe and the 30-line boot cap, QMI CTL Get Version Info/Allocate CID (0xFF22)/Release with exact bytes from the addressed port, the one-shot WDS 0x31/0xC2 line and its no-0xC2/switch-off silence)");
}
