/* Device side of Apple's usbmux protocol on /dev/hakios-usbmux (USB interface
 * FF/FE/02, drivers/umi/hakios.usb.c). Every mux TCP connection from the Mac's
 * usbmuxd is forwarded to 127.0.0.1:<port>: lockdownd on 62078 and the services
 * it starts. Wire format as Apple's usbmuxd (AppleUSBDeviceMux-574) and
 * libimobiledevice/usbmuxd expect: all fields big-endian; VERSION carries an
 * 8-byte header {proto, length}; after version 2 every packet has a 16-byte
 * header {proto, length, 0xfeedface, tx_seq, rx_seq}, our tx_seq counting from
 * 0; TCP packets carry a 20-byte BSD tcphdr whose window is th_win << 8.
 * The kernel gives the OUT direction as a byte stream and sends each write()
 * as one USB transfer. read() fails with ENXIO once when the host session ended.
 * lockdownd only accepts loopback peers whose TCP_PEER_PID is 0 (kernel-made
 * sockets), so each local socket is marked by the device before connect().
 * Host test: "usbmux FD" uses an inherited stream socket instead of the device. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PROTO_VERSION = 0, PROTO_CONTROL = 1, PROTO_SETUP = 2, PROTO_TCP = 6 };
enum { FIN = 1, SYN = 2, RST = 4, ACK = 0x10 };
#define HDR 16
#define TCPHDR 20
#define PACKET_MAX 16384                 /* kernel MUX_CHUNK: one write, one transfer */
#define SEGMENT (PACKET_MAX - HDR - TCPHDR)
#define WINDOW (256 * 1024)              /* host->device bytes buffered per connection */
#define RX_MAX (WINDOW + HDR + 64)       /* largest packet the host may send */
#define CONNS 64
#define HAKIOS_USBMUX_KERNEL_PEER _IOW('u', 1, int) /* drivers/umi/hakios.usb.c */

struct conn {
    int fd;                              /* 0 when free */
    uint16_t host_port, port;
    uint32_t snd_nxt, snd_una, snd_wnd;  /* device -> host */
    uint32_t rcv_nxt, adv;               /* host -> device; window last advertised */
    size_t pending;
    uint8_t *buf;                        /* host bytes not yet written to fd */
};
static struct conn conns[CONNS];
static int mux = -1;
static bool device;                      /* false only for the host test's socket */
static uint16_t tx_seq, rx_seq = 0xffff;
static bool lost;                        /* host session ended: reset everything */
static uint8_t rx[RX_MAX];
static size_t rx_len;

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static void write_packet(const uint8_t *p, size_t len)
{
    if (lost) return;
    ssize_t n = write(mux, p, len);
    if (n == (ssize_t)len) return;
    if (n < 0 && errno == ENXIO) { lost = true; return; }
    fprintf(stderr, "[usbmux] write %zu: %zd %s\n", len, n, strerror(errno));
    exit(1);
}
static void send_packet(uint32_t proto, const uint8_t *body, size_t n)
{
    uint8_t p[PACKET_MAX];
    put32(p, proto); put32(p + 4, (uint32_t)(HDR + n)); put32(p + 8, 0xfeedface);
    put16(p + 12, tx_seq++); put16(p + 14, rx_seq);
    memcpy(p + HDR, body, n);
    write_packet(p, HDR + n);
}
static void send_tcp(uint16_t port, uint16_t host_port, uint32_t seq, uint32_t ack, uint8_t flags,
                     uint32_t window, const uint8_t *data, size_t n)
{
    uint8_t b[TCPHDR + SEGMENT];
    memset(b, 0, TCPHDR);
    put16(b, port); put16(b + 2, host_port); put32(b + 4, seq); put32(b + 8, ack);
    b[12] = 5 << 4; b[13] = flags; put16(b + 14, (uint16_t)(window >> 8));
    memcpy(b + TCPHDR, data, n);
    send_packet(PROTO_TCP, b, TCPHDR + n);
}
static void reply(struct conn *c, uint8_t flags, const uint8_t *data, size_t n)
{
    c->adv = (uint32_t)(WINDOW - c->pending) & ~255U;
    send_tcp(c->port, c->host_port, c->snd_nxt, c->rcv_nxt, flags, c->adv, data, n);
    c->snd_nxt += (uint32_t)n;
}
static void drop(struct conn *c, bool reset)
{
    if (reset) reply(c, RST, NULL, 0);
    fprintf(stderr, "[usbmux] close %u <- host %u%s\n", c->port, c->host_port, reset ? " (RST sent)" : "");
    close(c->fd); free(c->buf);
    memset(c, 0, sizeof(*c));
}
static void reset_all(const char *why)
{
    for (unsigned i = 0; i < CONNS; ++i) if (conns[i].fd) drop(&conns[i], false);
    tx_seq = 0; rx_seq = 0xffff;
    fprintf(stderr, "[usbmux] session reset: %s\n", why);
}
/* Write buffered host bytes to the local socket; reopen the window when it grew. */
static void flush(struct conn *c)
{
    while (c->pending) {
        ssize_t n = write(c->fd, c->buf, c->pending);
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) { drop(c, true); return; }
        memmove(c->buf, c->buf + n, c->pending -= (size_t)n);
    }
    uint32_t window = (uint32_t)(WINDOW - c->pending) & ~255U;
    if (window > c->adv && window - c->adv >= WINDOW / 4) reply(c, ACK, NULL, 0);
}
/* Local socket -> host, within the host's window. EOF or error resets. */
static void pump(struct conn *c)
{
    uint32_t inflight = c->snd_nxt - c->snd_una;
    if (inflight >= c->snd_wnd) return;
    uint8_t b[SEGMENT];
    size_t room = c->snd_wnd - inflight;
    ssize_t n = read(c->fd, b, room < sizeof(b) ? room : sizeof(b));
    if (n > 0) reply(c, ACK, b, (size_t)n);
    else if (!n || errno != EAGAIN) drop(c, true);
}
static void connect_local(const uint8_t *th, uint16_t host_port, uint16_t port)
{
    struct conn *c = NULL;
    for (unsigned i = 0; i < CONNS; ++i) {
        if (conns[i].fd && conns[i].host_port == host_port) drop(&conns[i], false); /* reused host port */
        if (!conns[i].fd && !c) c = &conns[i];
    }
    struct sockaddr_in a = { .sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int fd = c ? socket(AF_INET, SOCK_STREAM, 0) : -1;
    if (fd < 0 || (device && ioctl(mux, HAKIOS_USBMUX_KERNEL_PEER, &fd)) ||
        connect(fd, (struct sockaddr *)&a, sizeof(a)) || fcntl(fd, F_SETFL, O_NONBLOCK) ||
        !(c->buf = malloc(WINDOW))) {
        fprintf(stderr, "[usbmux] connect %u <- host %u refused: %s\n", port, host_port, c ? strerror(errno) : "no slot");
        if (fd >= 0) close(fd);
        send_tcp(port, host_port, 0, get32(th + 4) + 1, RST | ACK, 0, NULL, 0);
        return;
    }
    c->fd = fd; c->host_port = host_port; c->port = port;
    c->rcv_nxt = get32(th + 4) + 1; c->snd_wnd = (uint32_t)get16(th + 14) << 8;
    c->snd_nxt = 0; c->snd_una = 1;
    reply(c, SYN | ACK, NULL, 0);
    c->snd_nxt = 1;                      /* SYN takes one sequence number */
    fprintf(stderr, "[usbmux] connect %u <- host %u fd=%d\n", port, host_port, fd);
}
static void tcp_input(const uint8_t *th, size_t len)
{
    unsigned off = (th[12] >> 4) * 4u;
    if (len < TCPHDR || off < TCPHDR || off > len) { fprintf(stderr, "[usbmux] bad TCP header\n"); return; }
    uint16_t host_port = get16(th), port = get16(th + 2);
    uint8_t flags = th[13];
    const uint8_t *data = th + off;
    size_t n = len - off;
    struct conn *c = NULL;
    for (unsigned i = 0; i < CONNS && !c; ++i)
        if (conns[i].fd && conns[i].host_port == host_port && conns[i].port == port) c = &conns[i];
    if ((flags & (SYN | ACK)) == SYN) { connect_local(th, host_port, port); return; }
    if (!c) {
        if (!(flags & RST)) send_tcp(port, host_port, get32(th + 8), get32(th + 4) + (uint32_t)n, RST, 0, NULL, 0);
        return;
    }
    if (flags & (RST | SYN)) { drop(c, false); return; }
    c->snd_una = get32(th + 8); c->snd_wnd = (uint32_t)get16(th + 14) << 8;
    if (n) {
        if (get32(th + 4) != c->rcv_nxt || n > WINDOW - c->pending) {
            fprintf(stderr, "[usbmux] %u <- host %u: seq %u expected %u, %zu bytes, window %zu\n", port, host_port,
                    get32(th + 4), c->rcv_nxt, n, WINDOW - c->pending);
            drop(c, true); return;
        }
        memcpy(c->buf + c->pending, data, n);
        c->pending += n; c->rcv_nxt += (uint32_t)n;
        flush(c);
        if (!c->fd) return;
        reply(c, ACK, NULL, 0);
    }
    if (flags & FIN) drop(c, true);      /* the host closes with RST; treat FIN alike */
}
static void packet(const uint8_t *p, uint32_t len)
{
    uint32_t proto = get32(p);
    if (proto == PROTO_VERSION) {
        uint32_t major = len >= 20 ? get32(p + 8) : 0, features = len >= 20 ? get32(p + 12) : 0;
        reset_all("version");
        uint8_t r[20] = {0};
        put32(r + 4, sizeof(r)); put32(r + 8, 2);   /* version 2; no optional features */
        write_packet(r, sizeof(r));
        fprintf(stderr, "[usbmux] version: host %u features %#x -> device 2\n", major, features);
        return;
    }
    rx_seq = get16(p + 12);
    if (proto == PROTO_TCP) tcp_input(p + HDR, len - HDR);
    else if (proto != PROTO_SETUP && proto != PROTO_CONTROL) fprintf(stderr, "[usbmux] ignored protocol %u\n", proto);
}
static void mux_input(void)
{
    ssize_t n = read(mux, rx + rx_len, sizeof(rx) - rx_len);
    if (n < 0 && errno == ENXIO) { lost = true; return; }
    if (n < 0 && errno == EINTR) return;
    if (n <= 0) { fprintf(stderr, "[usbmux] read: %zd %s\n", n, n ? strerror(errno) : "EOF"); exit(n ? 1 : 0); }
    rx_len += (size_t)n;
    while (rx_len >= 8 && !lost) {
        uint32_t len = get32(rx + 4), header = get32(rx) == PROTO_VERSION ? 8 : HDR;
        if (len < header || len > sizeof(rx)) {
            fprintf(stderr, "[usbmux] bad packet length %u (protocol %u); stream lost\n", len, get32(rx));
            rx_len = 0; return;
        }
        if (rx_len < len) return;
        packet(rx, len);
        if (lost) return;
        memmove(rx, rx + len, rx_len -= len);
    }
}
static bool usb_mux_boot(void)
{
    char args[1024] = {0};
    size_t n = sizeof(args) - 1;
    bool usb = false, ncm = false;
    if (sysctlbyname("kern.bootargs", args, &n, NULL, 0)) return false;
    for (char *p = strtok(args, " "); p; p = strtok(NULL, " ")) {
        usb |= !strcmp(p, "usb=1"); ncm |= !strcmp(p, "usb_ncm=1");
    }
    return usb && ncm;
}
int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1) mux = atoi(argv[1]);
    else {
        if (!usb_mux_boot()) return 0;
        for (unsigned i = 0; i < 120 && (mux = open("/dev/hakios-usbmux", O_RDWR)) < 0; ++i) sleep(1);
        if (mux < 0) { perror("[usbmux] /dev/hakios-usbmux"); return 1; }
        device = true;
    }
    fprintf(stderr, "[usbmux] ready: mux TCP -> 127.0.0.1 (lockdownd 62078)\n");
    for (;;) {
        struct pollfd p[1 + CONNS];
        unsigned owner[1 + CONNS], count = 1;
        p[0] = (struct pollfd){ .fd = mux, .events = POLLIN };
        for (unsigned i = 0; i < CONNS; ++i) {
            struct conn *c = &conns[i];
            short events = (c->snd_nxt - c->snd_una < c->snd_wnd ? POLLIN : 0) | (c->pending ? POLLOUT : 0);
            if (c->fd && events) { owner[count] = i; p[count++] = (struct pollfd){ .fd = c->fd, .events = events }; }
        }
        if (poll(p, count, -1) < 0) { if (errno == EINTR) continue; perror("[usbmux] poll"); return 1; }
        if (p[0].revents) mux_input();
        for (unsigned k = 1; k < count && !lost; ++k) {
            struct conn *c = &conns[owner[k]];
            if (c->fd != p[k].fd) continue;
            if (p[k].revents & (POLLOUT | POLLERR | POLLHUP) && c->pending) flush(c);
            if (c->fd == p[k].fd && p[k].revents & (POLLIN | POLLERR | POLLHUP)) pump(c);
        }
        if (lost) { lost = false; rx_len = 0; reset_all("host session ended"); }
    }
}
