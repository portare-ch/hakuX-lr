/*
 * NAT backend using libslirp for Internet and Insignia access.
 *
 * The guest uses 10.0.2.0/24 with gateway 10.0.2.2. NAT-only mode keeps
 * the legacy 10.0.2.15 lease. Combined mode chooses 10.0.2.16–215 from
 * the console MAC and probes the LAN before leasing it, retrying taken
 * addresses. Frames are held during the probe.
 *
 * DHCP supplies the configured DNS server. UDP 3074 is forwarded through
 * natport (default 3074), with optional router forwarding via UPnP.
 * All libslirp I/O and timers run on the network thread.
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <libslirp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define INSIGNIA_DNS "46.101.64.175"
#define XBOX_LIVE_PORT 3074
#define TIMERS 4
#define PROBE_MS 300                    /* the LAN's time to answer before the first lease */
#define PROBES 6                        /* rounds at most: then the last address picked */
#define HELD 8                          /* the console's frames held meanwhile */

typedef struct Timer {
    bool used, armed;
    int64_t expire_ms;
    SlirpTimerId id;
    void *cb_opaque;
} Timer;

static struct {
    Slirp *slirp;
    Timer timer[TIMERS];
    struct pollfd *fds;                 /* this round's, while fill and poll run */
    int nfds, maxfds;
    uint64_t overflow;                  /* descriptors that didn't fit */
    uint8_t mac[6];                     /* the console's */
    uint32_t lease;                     /* its address (network order) */
    uint32_t candidate;                 /* the address probed for */
    int64_t probe_until;                /* ms: the first lease waits for the LAN; 0: none */
    unsigned probes;                    /* the rounds so far */
    uint8_t held[HELD][NC_NET_FRAME_MAX];
    size_t held_len[HELD];
    unsigned nheld;
} g_nat;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static bool g_internet_off;

void nc_net_internet(bool on)
{
    if (__atomic_exchange_n(&g_internet_off, !on, __ATOMIC_ACQ_REL) != !on && !on) {
        nc_log("hakuX-native-net", "nat: the Internet is off: only ARP and DHCP pass");
    }
}

/* Keep ARP and the DHCP exchange with the local server at 10.0.2.2
 * available while Internet access is blocked. */
static bool local_frame(const uint8_t *f, size_t len, bool out)
{
    static const uint8_t server[4] = {10, 0, 2, 2}, all[4] = {255, 255, 255, 255};
    if (len < 14) {
        return false;
    }
    const unsigned type = (unsigned)f[12] << 8 | f[13];
    if (type == 0x0806) {
        return true;                    /* ARP */
    }
    const uint8_t *ip = f + 14;
    const size_t ihl = len >= 14 + 20 ? (size_t)(ip[0] & 15) * 4 : 0;
    if (type != 0x0800 || (ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 || len < 14 + ihl + 8) {
        return false;
    }
    const unsigned from = (unsigned)ip[ihl] << 8 | ip[ihl + 1], to = (unsigned)ip[ihl + 2] << 8 | ip[ihl + 3];
    return out ? from == 68 && to == 67 && (!memcmp(ip + 16, server, 4) || !memcmp(ip + 16, all, 4))
               : from == 67 && to == 68 && !memcmp(ip + 12, server, 4);
}

/* ------------------------------------------------- libslirp's callbacks */

static slirp_ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
    if (!__atomic_load_n(&g_internet_off, __ATOMIC_ACQUIRE) || local_frame(buf, len, false)) {
        nc_net_deliver(buf, len);
    }
    return (slirp_ssize_t)len;
}

static void cb_guest_error(const char *msg, void *opaque)
{
    nc_log("hakuX-native-net", "nat: guest error: %s", msg);
}

static int64_t cb_clock_get_ns(void *opaque)
{
    return now_ns();
}

static void *cb_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    for (unsigned i = 0; i < TIMERS; i++) {
        Timer *t = &g_nat.timer[i];
        if (!t->used) {
            *t = (Timer){ .used = true, .id = id, .cb_opaque = cb_opaque };
            return t;
        }
    }
    nc_log("hakuX-native-net", "nat: no timer free");
    return NULL;
}

static void cb_timer_free(void *timer, void *opaque)
{
    if (timer) {
        *(Timer *)timer = (Timer){ 0 };
    }
}

static void cb_timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
    if (timer) {
        ((Timer *)timer)->expire_ms = expire_ms;
        ((Timer *)timer)->armed = true;
    }
}

static void cb_poll_fd(int fd, void *opaque)
{
}

static void cb_notify(void *opaque)
{
    /* nothing to wake: libslirp only ever runs on the network thread */
}

static const SlirpCb g_cb = {
    .send_packet = cb_send_packet,
    .guest_error = cb_guest_error,
    .clock_get_ns = cb_clock_get_ns,
    .timer_free = cb_timer_free,
    .timer_mod = cb_timer_mod,
    .register_poll_fd = cb_poll_fd,
    .unregister_poll_fd = cb_poll_fd,
    .notify = cb_notify,
    .timer_new_opaque = cb_timer_new_opaque,
};

/* ------------------------------------------------------------ backend */

/* Bind the console's UDP 3074 to natport, then request a router forward. */
static void nat_forward(const NcNetConfig *c, struct in_addr guest)
{
    if (!c->nat_port) {
        nc_log("hakuX-native-net", "nat: no UDP port forwarded to the console (natport=0)");
        return;
    }
    const struct in_addr any = { .s_addr = htonl(INADDR_ANY) };
    if (slirp_add_hostfwd(g_nat.slirp, 1, any, c->nat_port, guest, XBOX_LIVE_PORT) < 0) {
        nc_log("hakuX-native-net", "nat: UDP %u unavailable (%s): the console's UDP %u uses any free port, "
               "unreachable until it sends", c->nat_port, strerror(errno), XBOX_LIVE_PORT);
        return;
    }
    nc_log("hakuX-native-net", "nat: UDP %u forwarded to the console's %u (Xbox Live)", c->nat_port,
           XBOX_LIVE_PORT);
    if (c->upnp) {
        nc_upnp_start(c->nat_port);
    }
}

static bool g_nat_shared;               /* beside the LAN (net=on): a MAC-derived lease */

void nc_net_nat_shared(bool shared)
{
    g_nat_shared = shared;
}

bool nc_net_nat_address(const uint8_t mac[6])
{
    return mac[0] == 0x52 && mac[1] == 0x55;   /* libslirp's: 52:55 and the address */
}

uint32_t nc_net_nat_lease(void)
{
    return __atomic_load_n(&g_nat.lease, __ATOMIC_ACQUIRE);
}

static int64_t now_ms(void)
{
    return now_ns() / 1000000;
}

/* Beside the LAN, the console's address: from its MAC's hash in
 * 10.0.2.16-215, the first one on from there no other console keeps. */
static uint32_t nat_pick(const uint8_t mac[6])
{
    const uint32_t h = ((uint32_t)mac[3] * 31u + mac[4]) * 31u + mac[5];
    for (unsigned i = 0; i < 200; i++) {
        const uint32_t ip = htonl(0x0A000200u | (16 + (h + i) % 200));
        if (!nc_net_lan_taken(mac, ip)) {
            return ip;
        }
    }
    return htonl(0x0A000200u | (16 + h % 200));     /* all taken: the hash's */
}

/* The DNS server DHCP gives the console: dns=, or Insignia's. */
static struct in_addr nat_dns(const NcNetConfig *c)
{
    struct in_addr dns = { .s_addr = c->dns[0] };
    if (!dns.s_addr) {
        inet_pton(AF_INET, INSIGNIA_DNS, &dns);
    }
    return dns;
}

/* The stack, for the console with this MAC (its frames' source), leasing
 * it ip. */
static bool nat_start(const uint8_t mac[6], uint32_t ip)
{
    const NcNetConfig *c = nc_net_config();
    const struct in_addr dns = nat_dns(c);
    SlirpConfig cfg = {
        .version = 4,
        .in_enabled = true,
        .vhostname = "hakuX",
        .vnameserver = dns,
        .disable_host_loopback = true,
        .disable_dns = true,            /* dns is a real server: no redirect */
    };
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    /* the console's own address: libslirp leases from vdhcp_start */
    cfg.vdhcp_start.s_addr = ip;
    char start[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg.vdhcp_start, start, sizeof(start));
    g_nat.slirp = slirp_new(&cfg, &g_cb, NULL);
    if (!g_nat.slirp) {
        nc_log("hakuX-native-net", "nat: libslirp didn't start");
        return false;
    }
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dns, b, sizeof(b));
    nc_log("hakuX-native-net", "nat: libslirp %s, 10.0.2.0/24, the console's address %s, DNS %s",
           slirp_version_string(), start, b);
    memcpy(g_nat.mac, mac, 6);
    __atomic_store_n(&g_nat.lease, ip, __ATOMIC_RELEASE);
    if (g_nat_shared) {
        nc_net_lan_claim(mac, ip, true);
    }
    nat_forward(c, cfg.vdhcp_start);
    return true;
}

/* A probe round: claim ip, ask the LAN who has it, wait PROBE_MS. */
static void nat_probe_round(uint32_t ip)
{
    g_nat.candidate = ip;
    g_nat.probes++;
    char a[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, a, sizeof(a));
    nc_log("hakuX-native-net", "nat: asking the LAN who has %s (round %u)", a, g_nat.probes);
    nc_net_lan_claim(g_nat.mac, ip, false);
    nc_net_lan_probe(ip);
    g_nat.probe_until = now_ms() + PROBE_MS;
}

/* Beside the LAN, the first frame: a probe round for the address the MAC
 * picks; the console's frames held meanwhile. */
static void nat_probe(const uint8_t *frame, size_t len)
{
    if (!g_nat.probe_until) {
        memcpy(g_nat.mac, frame + 6, 6);
        g_nat.probes = 0;
        nat_probe_round(nat_pick(g_nat.mac));
    }
    if (g_nat.nheld < HELD && len <= NC_NET_FRAME_MAX) {
        memcpy(g_nat.held[g_nat.nheld], frame, len);
        g_nat.held_len[g_nat.nheld++] = len;
    }
}

/* A round is up: another console keeps the candidate, so a round for the
 * next free one (PROBES at most); else lease it and give the stack the
 * frames held. */
static void nat_probed(void)
{
    uint32_t ip = g_nat.candidate;
    if (nc_net_lan_taken(g_nat.mac, ip)) {
        ip = nat_pick(g_nat.mac);
        if (g_nat.probes < PROBES && ip != g_nat.candidate) {
            nat_probe_round(ip);
            return;
        }
    }
    g_nat.probe_until = 0;
    if (!nat_start(g_nat.mac, ip)) {
        g_nat.nheld = 0;
        return;
    }
    /* Recheck access before replaying held frames. */
    for (unsigned i = 0; i < g_nat.nheld; i++) {
        if (!__atomic_load_n(&g_internet_off, __ATOMIC_ACQUIRE) ||
            local_frame(g_nat.held[i], g_nat.held_len[i], true)) {
            slirp_input(g_nat.slirp, g_nat.held[i], (int)g_nat.held_len[i]);
        }
    }
    g_nat.nheld = 0;
}

static bool nat_init(void)
{
    /* the stack starts with the console's first frame, which carries its MAC */
    const struct in_addr dns = nat_dns(nc_net_config());
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dns, b, sizeof(b));
    nc_log("hakuX-native-net", "nat: DNS %s, the stack starts with the console's first frame", b);
    return true;
}

static void nat_input(const uint8_t *frame, size_t len)
{
    if (len < 14) {
        return;
    }
    if (__atomic_load_n(&g_internet_off, __ATOMIC_ACQUIRE) && !local_frame(frame, len, true)) {
        return;
    }
    if (!g_nat.slirp && g_nat_shared) {
        nat_probe(frame, len);
        return;
    }
    if (!g_nat.slirp && !nat_start(frame + 6, htonl(0x0A00020Fu))) {     /* alone: 10.0.2.15 */
        return;
    }
    slirp_input(g_nat.slirp, frame, (int)len);
}

static int add_poll(int fd, int events, void *opaque)
{
    if (g_nat.nfds >= g_nat.maxfds) {
        g_nat.overflow++;
        return -1;
    }
    struct pollfd *p = &g_nat.fds[g_nat.nfds];
    p->fd = fd;
    p->events = (short)((events & SLIRP_POLL_IN ? POLLIN : 0) | (events & SLIRP_POLL_OUT ? POLLOUT : 0) |
                        (events & SLIRP_POLL_PRI ? POLLPRI : 0));
    p->revents = 0;
    return g_nat.nfds++;
}

static int get_revents(int idx, void *opaque)
{
    if (idx < 0 || idx >= g_nat.nfds) {
        return 0;
    }
    const short r = g_nat.fds[idx].revents;
    return (r & POLLIN ? SLIRP_POLL_IN : 0) | (r & POLLOUT ? SLIRP_POLL_OUT : 0) |
           (r & POLLPRI ? SLIRP_POLL_PRI : 0) | (r & POLLERR ? SLIRP_POLL_ERR : 0) |
           (r & POLLHUP ? SLIRP_POLL_HUP : 0);
}

static int nat_fill(struct pollfd *fds, int max, int *timeout_ms)
{
    if (g_nat.probe_until) {
        const int64_t left = g_nat.probe_until - now_ms();
        *timeout_ms = left <= 0 ? 0 : left < *timeout_ms ? (int)left : *timeout_ms;
    }
    if (!g_nat.slirp) {
        return 0;
    }
    g_nat.fds = fds;
    g_nat.maxfds = max;
    g_nat.nfds = 0;
    uint32_t timeout = (uint32_t)*timeout_ms;
    slirp_pollfds_fill(g_nat.slirp, &timeout, add_poll, NULL);
    const int64_t now = now_ns() / 1000000;
    for (unsigned i = 0; i < TIMERS; i++) {
        const Timer *t = &g_nat.timer[i];
        if (t->used && t->armed) {
            const int64_t left = t->expire_ms > now ? t->expire_ms - now : 0;
            timeout = left < timeout ? (uint32_t)left : timeout;
        }
    }
    *timeout_ms = (int)timeout;
    return g_nat.nfds;
}

static void nat_poll(struct pollfd *fds, int n)
{
    if (g_nat.probe_until && now_ms() >= g_nat.probe_until) {
        nat_probed();
        return;                         /* its descriptors join the next round */
    }
    if (!g_nat.slirp) {
        return;
    }
    g_nat.fds = fds;
    g_nat.nfds = n;
    slirp_pollfds_poll(g_nat.slirp, 0, get_revents, NULL);
    const int64_t now = now_ns() / 1000000;
    for (unsigned i = 0; i < TIMERS; i++) {
        Timer *t = &g_nat.timer[i];
        if (t->used && t->armed && t->expire_ms <= now) {
            t->armed = false;
            slirp_handle_timer(g_nat.slirp, t->id, t->cb_opaque);
        }
    }
    static unsigned s_logged;
    if (g_nat.overflow && s_logged++ < 8) {
        nc_log("hakuX-native-net", "nat: %llu descriptors over the poll's %d", (unsigned long long)g_nat.overflow,
               g_nat.maxfds);
    }
    g_nat.overflow = 0;
}

/* After the network thread joins, clean up UPnP, the stack, sockets and
 * timers so the next boot can initialize them again. */
static void nat_close(void)
{
    nc_upnp_stop();
    if (g_nat.slirp) {
        slirp_cleanup(g_nat.slirp);
    }
    memset(&g_nat, 0, sizeof(g_nat));
}

const NcNetBackend nc_net_nat = {
    .name = "nat",
    .init = nat_init,
    .close = nat_close,
    .input = nat_input,
    .fill = nat_fill,
    .poll = nat_poll,
};
