/* Copyright (C) 2026-present PortareOS */
/* LAN duplicate filtering, combined-backend cleanup and the consoles'
 * addresses on the shared network (one each). */
#include "native/net/net.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LANPORT 43090

static int g_failed;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (cond) {                                                                                          \
            printf("ok: ");                                                                                  \
        } else {                                                                                             \
            printf("FAIL: ");                                                                                \
            g_failed = 1;                                                                                    \
        }                                                                                                    \
        printf(__VA_ARGS__);                                                                                 \
        printf("\n");                                                                                        \
    } while (0)

static void option(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void option(const char *fmt, ...)
{
    char arg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(arg, sizeof(arg), fmt, ap);
    va_end(ap);
    if (!nc_net_option(arg)) {
        printf("FAIL: option %s not taken\n", arg);
        g_failed = 1;
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        return -1;
    }
    while (readdir(d)) {
        n++;
    }
    closedir(d);
    return n;
}

/* Use a separate source port to simulate another LAN path. */
static int peer(void)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    return fd;
}

/* Send a tagged broadcast frame. */
static void peer_send(int fd, char tag)
{
    uint8_t f[60] = { 0 };
    memset(f, 0xff, 6);
    const uint8_t src[6] = { 0x00, 0x50, 0xf2, 0xaa, 0xbb, 0xcc };
    memcpy(f + 6, src, 6);
    f[12] = 0x88, f[13] = 0xb5;                 /* a local experimental EtherType */
    f[14] = (uint8_t)tag;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(LANPORT),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
}

/* A frame from mac that says nothing of its address. */
static void peer_frame(int fd, const uint8_t mac[6])
{
    uint8_t f[60] = { 0 };
    memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6);
    f[12] = 0x88, f[13] = 0xb5;
    f[14] = 'x';
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(LANPORT),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
}

/* An ARP from mac saying it is ip (op 2: a reply), to the console's port. */
static void peer_arp(int fd, const uint8_t mac[6], uint32_t ip)
{
    uint8_t f[60] = { 0 };
    memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6);
    static const uint8_t arp[10] = { 0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x02 };
    memcpy(f + 12, arp, sizeof(arp));
    memcpy(f + 22, mac, 6);
    memcpy(f + 28, &ip, 4);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(LANPORT),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
}

/* A hakuX console's hello claiming ip, leased or about to be. */
static void peer_hello(int fd, const uint8_t mac[6], uint32_t ip, bool leased)
{
    uint8_t f[60] = { 0 };
    memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6);
    f[12] = 0x88, f[13] = 0xb5;
    memcpy(f + 14, "hakuX hello", 11);
    memcpy(f + 25, &ip, 4);
    f[29] = leased;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(LANPORT),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
}

/* The console's first frame (a broadcast ARP for the gateway): the NAT
 * probes the LAN, then leases. */
static void console_first_frame(const uint8_t mac[6])
{
    uint8_t f[60] = { 0 };
    memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6);
    static const uint8_t arp[10] = { 0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01 };
    memcpy(f + 12, arp, sizeof(arp));
    memcpy(f + 22, mac, 6);
    const uint32_t gw = htonl(0x0A000202u);
    memcpy(f + 38, &gw, 4);
    nc_net_send(f, sizeof(f));
}

/* The lease once it differs from old (0: any), or 0 after ms. */
static uint32_t lease_other_than(uint32_t old, int ms)
{
    const int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        const uint32_t l = nc_net_nat_lease();
        if (l && l != old) {
            return l;
        }
        usleep(20000);
    }
    return 0;
}

static unsigned last_octet(uint32_t ip)
{
    return ntohl(ip) & 0xFF;
}

/* The address after ip in the NAT's order (10.0.2.16-215, round). */
static uint32_t next_address(uint32_t ip)
{
    return htonl(0x0A000200u | (16 + (last_octet(ip) - 16 + 1) % 200));
}

/* Count matching frames received within ms. */
static int console_got(char tag, int ms)
{
    int n = 0;
    const int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        nc_net_wait_rx(20 * 1000000ull);
        uint8_t f[NC_NET_FRAME_MAX];
        size_t len;
        while ((len = nc_net_recv(f, sizeof(f)))) {
            if (len >= 15 && f[12] == 0x88 && f[13] == 0xb5 && f[14] == (uint8_t)tag) {
                n++;
            }
        }
    }
    return n;
}

int main(void)
{
    option("net=lan");
    option("lanport=%u", LANPORT);
    option("lanbcast=127.0.0.1");
    nc_net_start();
    CHECK(nc_net_link(), "lan: started");
    const int a = peer(), b = peer();

    peer_send(a, 'r');
    peer_send(a, 'r');
    CHECK(console_got('r', 300) == 2, "a frame the peer sends twice reaches the console twice");

    peer_send(a, 'd');
    peer_send(b, 'd');
    CHECK(console_got('d', 300) == 1, "the same frame from another address within 100 ms reaches it once");

    peer_send(a, 'l');
    console_got('l', 150);
    peer_send(b, 'l');
    CHECK(console_got('l', 300) == 1, "the same frame from another address after 100 ms reaches it again");
    nc_net_stop();
    close(a);
    close(b);

    /* Both backends must close on stop. */
    option("net=on");
    option("upnp=off");
    option("natport=0");
    const int before = open_fds();
    for (int i = 0; i < 3; i++) {
        nc_net_start();
        CHECK(nc_net_link(), "all: started (boot %d)", i + 1);
        nc_net_stop();
    }
    const int after = open_fds();
    CHECK(after == before, "all: no descriptors left after 3 boots (%d before, %d after)", before, after);

    /* One address each on the shared network. */
    static const uint8_t me[6] = { 0x00, 0x50, 0xf2, 0x10, 0x20, 0x30 };
    static const uint8_t other[6] = { 0x00, 0x50, 0xf2, 0xaa, 0xbb, 0xcc };
    static const uint8_t higher[6] = { 0x00, 0x50, 0xf2, 0xff, 0xff, 0xff };
    static const uint8_t lower[6] = { 0x00, 0x50, 0xf2, 0x00, 0x00, 0x01 };
    char addr[16];
    nc_net_set_mac(me);
    nc_net_start();
    console_first_frame(me);
    const uint32_t first = lease_other_than(0, 1500);
    CHECK(first && last_octet(first) >= 16 && last_octet(first) <= 215,
          "on: the console leases 10.0.2.%u from its MAC", last_octet(first));
    nc_net_stop();

    const int p = peer();
    nc_net_start();
    peer_arp(p, other, first);              /* a console at it already */
    usleep(100000);
    console_first_frame(me);
    const uint32_t second = lease_other_than(0, 1500);
    CHECK(second && second != first, "on: a console already at 10.0.2.%u: this one leases 10.0.2.%u",
          last_octet(first), last_octet(second));
    nc_net_take_conflict(addr, sizeof(addr));
    peer_arp(p, other, second);             /* at ours, after the lease */
    usleep(300000);
    CHECK(nc_net_nat_lease() == second && nc_net_take_conflict(addr, sizeof(addr)),
          "on: a console at 10.0.2.%u after the lease: the lease stays, the user is told (%s)",
          last_octet(second), addr);
    nc_net_stop();

    struct { const uint8_t *mac; bool leased; bool keeps; const char *what; } claims[] = {
        { lower, false, false, "a lower MAC about to lease it" },
        { higher, false, true, "a higher MAC about to lease it" },
        { higher, true, false, "a higher MAC that leased it" },
    };
    for (unsigned i = 0; i < sizeof(claims) / sizeof(claims[0]); i++) {
        nc_net_start();
        peer_hello(p, claims[i].mac, first, claims[i].leased);
        usleep(100000);
        console_first_frame(me);
        const uint32_t got = lease_other_than(0, 1500);
        CHECK(got && (got == first) == claims[i].keeps, "on: %s (10.0.2.%u): this one leases 10.0.2.%u",
              claims[i].what, last_octet(first), last_octet(got));
        nc_net_stop();
    }

    /* Three consoles about to lease the same address: the two higher MACs
     * give it up, and settle the next one too (each claimed and probed). */
    static const uint8_t middle[6] = { 0x00, 0x50, 0xf2, 0x00, 0x00, 0x02 };
    nc_net_start();
    console_first_frame(me);                /* round 1: first */
    usleep(100000);
    peer_hello(p, lower, first, false);     /* the others start at the same time */
    peer_hello(p, middle, first, false);
    usleep(300000);                         /* round 2: the next one */
    peer_hello(p, middle, next_address(first), false);    /* the middle MAC gave first up too */
    const uint32_t settled = lease_other_than(0, 2500);
    CHECK(settled == next_address(next_address(first)),
          "on: three consoles at 10.0.2.%u: the lowest keeps it, the middle one takes 10.0.2.%u, this one "
          "10.0.2.%u", last_octet(first), last_octet(next_address(first)), last_octet(settled));
    nc_net_stop();

    /* A station's entry taken over by another (64 heard, the oldest goes)
     * forgets the address the old one had. */
    nc_net_start();
    peer_arp(p, other, first);
    for (unsigned i = 0; i < 64; i++) {
        const uint8_t mac[6] = { 0x00, 0x50, 0xf2, 0x7a, (uint8_t)(i >> 8), (uint8_t)i };
        peer_frame(p, mac);
    }
    usleep(200000);
    console_first_frame(me);
    const uint32_t reused = lease_other_than(0, 1500);
    CHECK(reused == first, "on: the entry of the console at 10.0.2.%u reused by another: this one leases "
          "10.0.2.%u", last_octet(first), last_octet(reused));
    nc_net_stop();
    close(p);

    printf(g_failed ? "FAIL: lan-smoke\n" : "PASS: lan-smoke\n");
    return g_failed;
}
