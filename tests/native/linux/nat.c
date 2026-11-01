/*
 * The NAT backend (native/net/nat.c, upnp.c) with frames made here as the
 * console's, and a router made here that answers UPnP on 127.0.0.1:
 * - the console's UDP 3074 leaves from natport, and a datagram sent to
 *   natport first (from a peer the console never sent to) reaches it;
 * - UPnP: a device without a WAN service skipped, the router asked to
 *   forward natport to this device for an hour, or permanently when it
 *   takes no other lease, and the forward removed at the stop;
 * - natport taken: the backend still starts, the console's 3074 leaves
 *   from another port, no UPnP; upnp=off: no M-SEARCH.
 * The build's nat-smoke test (native/cmake/linux-tests.cmake).
 */
#include "native/net/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NATPORT 43074
#define BUSYPORT 43075

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

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------- the router */

static const char TV_XML[] =
    "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
    "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
    "<controlURL>/tv/ctl</controlURL></service></serviceList></device></root>";

static const char IGD_XML[] =
    "<?xml version=\"1.0\"?>\r\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
    "<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
    "<controlURL>/ctl/L3F</controlURL></service></serviceList><deviceList><device>"
    "<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
    "<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList><service>\r\n"
    "  <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\r\n"
    "  <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>\r\n"
    "  <controlURL> /ctl/IPConn </controlURL>\r\n"
    "</service></serviceList></device></deviceList></device></deviceList></device></root>\r\n";

static struct {
    int ssdp, http;
    uint16_t ssdp_port, http_port;
    pthread_t thread;
    volatile bool quit;
    pthread_mutex_t lock;
    unsigned searches, descriptions, adds, deletes, externals;
    bool only_permanent;
    char last_add[4096], last_delete[4096];
} g_igd = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void reply(int fd, const char *status, const char *body, bool chunked)
{
    char out[8192];
    int n;
    if (chunked) {
        const size_t half = strlen(body) / 2;
        n = snprintf(out, sizeof(out),
                     "HTTP/1.1 %s\r\nContent-Type: text/xml\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "%zx\r\n%.*s\r\n%zx\r\n%s\r\n0\r\n\r\n",
                     status, half, (int)half, body, strlen(body) - half, body + half);
    } else {
        n = snprintf(out, sizeof(out), "HTTP/1.1 %s\r\nContent-Type: text/xml\r\nContent-Length: %zu\r\n\r\n%s",
                     status, strlen(body), body);
    }
    (void)!write(fd, out, (size_t)n);
}

static void serve(int fd)
{
    char req[8192];
    size_t got = 0;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        const ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0) {
            return;
        }
        got += (size_t)n;
        req[got] = 0;
        const char *end = strstr(req, "\r\n\r\n");
        const char *cl = strstr(req, "Content-Length: ");
        if (end && (!cl || got - (size_t)(end + 4 - req) >= strtoul(cl + 16, NULL, 10))) {
            break;
        }
    }
    static const char ok[] = "<?xml version=\"1.0\"?><s:Envelope><s:Body><u:R/></s:Body></s:Envelope>";
    pthread_mutex_lock(&g_igd.lock);
    if (!strncmp(req, "GET /tv.xml ", 12)) {
        reply(fd, "200 OK", TV_XML, false);
    } else if (!strncmp(req, "GET /rootDesc.xml ", 18)) {
        g_igd.descriptions++;
        reply(fd, "200 OK", IGD_XML, true);
    } else if (!strncmp(req, "POST /ctl/IPConn ", 17) && strstr(req, "#GetExternalIPAddress\"")) {
        g_igd.externals++;
        reply(fd, "200 OK",
              "<?xml version=\"1.0\"?><s:Envelope><s:Body><u:GetExternalIPAddressResponse>"
              "<NewExternalIPAddress>100.64.1.2</NewExternalIPAddress></u:GetExternalIPAddressResponse>"
              "</s:Body></s:Envelope>", false);
    } else if (!strncmp(req, "POST /ctl/IPConn ", 17) && strstr(req, "#AddPortMapping\"")) {
        g_igd.adds++;
        snprintf(g_igd.last_add, sizeof(g_igd.last_add), "%s", req);
        if (g_igd.only_permanent && !strstr(req, "<NewLeaseDuration>0</NewLeaseDuration>")) {
            reply(fd, "500 Internal Server Error",
                  "<?xml version=\"1.0\"?><s:Envelope><s:Body><s:Fault><detail>"
                  "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>725</errorCode>"
                  "<errorDescription>OnlyPermanentLeasesSupported</errorDescription></UPnPError>"
                  "</detail></s:Fault></s:Body></s:Envelope>", false);
        } else {
            reply(fd, "200 OK", ok, false);
        }
    } else if (!strncmp(req, "POST /ctl/IPConn ", 17) && strstr(req, "#DeletePortMapping\"")) {
        g_igd.deletes++;
        snprintf(g_igd.last_delete, sizeof(g_igd.last_delete), "%s", req);
        reply(fd, "200 OK", ok, false);
    } else {
        reply(fd, "404 Not Found", "", false);
    }
    pthread_mutex_unlock(&g_igd.lock);
}

static void *igd_thread(void *arg)
{
    while (!g_igd.quit) {
        struct pollfd p[2] = { { .fd = g_igd.ssdp, .events = POLLIN }, { .fd = g_igd.http, .events = POLLIN } };
        if (poll(p, 2, 50) <= 0) {
            continue;
        }
        if (p[0].revents & POLLIN) {
            char msg[1024];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const ssize_t n = recvfrom(g_igd.ssdp, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                msg[n] = 0;
                if (!strncmp(msg, "M-SEARCH * HTTP/1.1\r\n", 21) && strstr(msg, "MAN: \"ssdp:discover\"")) {
                    pthread_mutex_lock(&g_igd.lock);
                    g_igd.searches++;
                    pthread_mutex_unlock(&g_igd.lock);
                    /* a media renderer that answers everything, then the router */
                    static const char *const paths[] = { "/tv.xml", "/rootDesc.xml" };
                    for (unsigned i = 0; i < 2; i++) {
                        char out[512];
                        const int len = snprintf(out, sizeof(out),
                                                 "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\n"
                                                 "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                                                 "USN: uuid:test-%u\r\nEXT:\r\nSERVER: test UPnP/1.1\r\n"
                                                 "Location:  http://127.0.0.1:%u%s\r\n\r\n",
                                                 i, g_igd.http_port, paths[i]);
                        sendto(g_igd.ssdp, out, (size_t)len, 0, (struct sockaddr *)&from, fl);
                    }
                }
            }
        }
        if (p[1].revents & POLLIN) {
            const int fd = accept(g_igd.http, NULL, NULL);
            if (fd >= 0) {
                serve(fd);
                close(fd);
            }
        }
    }
    return NULL;
}

static uint16_t bound_port(int fd)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    return ntohs(a.sin_port);
}

static bool igd_start(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    g_igd.ssdp = socket(AF_INET, SOCK_DGRAM, 0);
    g_igd.http = socket(AF_INET, SOCK_STREAM, 0);
    if (g_igd.ssdp < 0 || g_igd.http < 0 || bind(g_igd.ssdp, (struct sockaddr *)&a, sizeof(a)) ||
        bind(g_igd.http, (struct sockaddr *)&a, sizeof(a)) || listen(g_igd.http, 8)) {
        return false;
    }
    g_igd.ssdp_port = bound_port(g_igd.ssdp);
    g_igd.http_port = bound_port(g_igd.http);
    return pthread_create(&g_igd.thread, NULL, igd_thread, NULL) == 0;
}

static unsigned igd_count(const unsigned *counter)
{
    pthread_mutex_lock(&g_igd.lock);
    const unsigned v = *counter;
    pthread_mutex_unlock(&g_igd.lock);
    return v;
}

static bool igd_wait(const unsigned *counter, unsigned want, int ms)
{
    const int64_t end = now_ms() + ms;
    while (igd_count(counter) < want && now_ms() < end) {
        sleep_ms(10);
    }
    return igd_count(counter) >= want;
}

static void igd_reset(bool only_permanent)
{
    pthread_mutex_lock(&g_igd.lock);
    g_igd.searches = g_igd.descriptions = g_igd.adds = g_igd.deletes = g_igd.externals = 0;
    g_igd.last_add[0] = g_igd.last_delete[0] = 0;
    g_igd.only_permanent = only_permanent;
    pthread_mutex_unlock(&g_igd.lock);
}

static bool has(const char *s, const char *what)
{
    pthread_mutex_lock(&g_igd.lock);
    const bool r = strstr(s, what) != NULL;
    pthread_mutex_unlock(&g_igd.lock);
    return r;
}

/* ---------------------------------------------------- the console */

static const uint8_t GUEST_MAC[6] = { 0x00, 0x50, 0xf2, 0x12, 0x34, 0x56 };
static const uint8_t SLIRP_MAC[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const uint8_t GUEST_IP[4] = { 10, 0, 2, 15 };
static const uint8_t GATEWAY_IP[4] = { 10, 0, 2, 2 };

static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        sum += (uint32_t)p[i] << 8 | p[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* "who has 10.0.2.2, tell 10.0.2.15": libslirp learns the console's MAC. */
static void console_arp(void)
{
    uint8_t f[42] = { 0 };
    memset(f, 0xff, 6);
    memcpy(f + 6, GUEST_MAC, 6);
    f[12] = 0x08, f[13] = 0x06;
    f[15] = 1, f[16] = 0x08, f[18] = 6, f[19] = 4, f[21] = 1;
    memcpy(f + 22, GUEST_MAC, 6);
    memcpy(f + 28, GUEST_IP, 4);
    memcpy(f + 38, GATEWAY_IP, 4);
    nc_net_send(f, sizeof(f));
}

/* A UDP datagram from the console's 3074 to ip:port (network order). */
static void console_send(uint32_t ip, uint16_t port, const char *payload)
{
    const size_t plen = strlen(payload), ulen = 8 + plen, tlen = 20 + ulen;
    uint8_t f[NC_NET_FRAME_MAX] = { 0 };
    memcpy(f, SLIRP_MAC, 6);
    memcpy(f + 6, GUEST_MAC, 6);
    f[12] = 0x08, f[13] = 0x00;
    uint8_t *ip_h = f + 14, *udp = ip_h + 20;
    ip_h[0] = 0x45;
    ip_h[2] = (uint8_t)(tlen >> 8), ip_h[3] = (uint8_t)tlen;
    ip_h[8] = 64, ip_h[9] = 17;
    memcpy(ip_h + 12, GUEST_IP, 4);
    memcpy(ip_h + 16, &ip, 4);
    const uint16_t sum = ip_checksum(ip_h, 20);
    ip_h[10] = (uint8_t)(sum >> 8), ip_h[11] = (uint8_t)sum;
    udp[0] = 3074 >> 8, udp[1] = 3074 & 0xff;
    memcpy(udp + 2, &port, 2);
    udp[4] = (uint8_t)(ulen >> 8), udp[5] = (uint8_t)ulen;
    memcpy(udp + 8, payload, plen);
    nc_net_send(f, 14 + tlen);
}

/* A UDP datagram for the console's port 3074 with payload, within ms. */
static bool console_got(const char *payload, int ms)
{
    const size_t plen = strlen(payload);
    const int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        nc_net_wait_rx(20 * 1000000ull);
        uint8_t f[NC_NET_FRAME_MAX];
        size_t n;
        while ((n = nc_net_recv(f, sizeof(f)))) {
            if (n < 42 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 17 || memcmp(f + 30, GUEST_IP, 4)) {
                continue;
            }
            const uint8_t *udp = f + 14 + (f[14] & 15) * 4;
            const size_t ulen = (size_t)(udp[4] << 8 | udp[5]);
            if ((udp[2] << 8 | udp[3]) == 3074 && ulen == 8 + plen && udp + ulen <= f + n &&
                !memcmp(udp + 8, payload, plen)) {
                return true;
            }
        }
    }
    return false;
}

/* This host's first non-loopback IPv4 address (network order), 0 if none:
 * the console's datagrams reach a socket here through it. */
static uint32_t host_ip(void)
{
    struct ifaddrs *list;
    uint32_t ip = 0;
    if (getifaddrs(&list) != 0) {
        return 0;
    }
    for (const struct ifaddrs *i = list; i && !ip; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
            !(i->ifa_flags & IFF_LOOPBACK)) {
            ip = ((const struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        }
    }
    freeifaddrs(list);
    return ip;
}

/* The source port of the console's datagram as a host peer receives it, 0
 * if it didn't arrive. */
static uint16_t console_source_port(uint32_t ip, const char *payload)
{
    const int peer = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    if (peer < 0 || bind(peer, (struct sockaddr *)&a, sizeof(a))) {
        return 0;
    }
    const uint16_t port = htons(bound_port(peer));
    uint16_t from_port = 0;
    for (int tries = 0; tries < 3 && !from_port; tries++) {
        console_send(ip, port, payload);
        struct pollfd p = { .fd = peer, .events = POLLIN };
        if (poll(&p, 1, 700) > 0) {
            char buf[256];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const ssize_t n = recvfrom(peer, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n == (ssize_t)strlen(payload) && !memcmp(buf, payload, (size_t)n)) {
                from_port = ntohs(from.sin_port);
            }
        }
    }
    close(peer);
    return from_port;
}

/* A datagram to 127.0.0.1:port from a socket the console never sent to. */
static void stranger_send(uint16_t port, const char *payload)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, payload, strlen(payload), 0, (struct sockaddr *)&to, sizeof(to));
    close(fd);
}

static bool port_free(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    const bool ok = fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

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

int main(void)
{
    if (!igd_start()) {
        printf("FAIL: the test's router didn't start: %s\n", strerror(errno));
        return 1;
    }
    if (!port_free(NATPORT) || !port_free(BUSYPORT)) {
        printf("FAIL: UDP %u or %u is in use on this host\n", NATPORT, BUSYPORT);
        return 1;
    }
    const uint32_t ip = host_ip();

    /* natport forwarded, UPnP on through the test's router */
    option("net=nat");
    option("natport=%u", NATPORT);
    option("upnp=127.0.0.1:%u", g_igd.ssdp_port);
    nc_net_start();
    CHECK(nc_net_link(), "nat: started");
    CHECK(!port_free(NATPORT), "UDP %u held for the console", NATPORT);
    console_arp();
    if (ip) {
        const uint16_t from = console_source_port(ip, "from 3074");
        CHECK(from == NATPORT, "the console's UDP 3074 leaves from %u (got %u)", NATPORT, from);
    } else {
        printf("skip: no non-loopback IPv4 address for the console's datagrams\n");
        sleep_ms(200);
    }
    bool got = false;
    for (int tries = 0; tries < 5 && !got; tries++) {
        stranger_send(NATPORT, "unsolicited");
        got = console_got("unsolicited", 400);
    }
    CHECK(got, "a datagram to %u from a stranger reaches the console's 3074", NATPORT);
    CHECK(igd_wait(&g_igd.adds, 1, 5000), "UPnP: AddPortMapping asked");
    CHECK(igd_count(&g_igd.descriptions) >= 1, "UPnP: the router's description read (chunked)");
    CHECK(igd_count(&g_igd.externals) == 1, "UPnP: GetExternalIPAddress asked");
    CHECK(has(g_igd.last_add, "SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\""),
          "UPnP: the WANIPConnection:1 service");
    CHECK(has(g_igd.last_add, "<NewExternalPort>43074</NewExternalPort>") &&
          has(g_igd.last_add, "<NewInternalPort>43074</NewInternalPort>") &&
          has(g_igd.last_add, "<NewProtocol>UDP</NewProtocol>") &&
          has(g_igd.last_add, "<NewInternalClient>127.0.0.1</NewInternalClient>") &&
          has(g_igd.last_add, "<NewLeaseDuration>3600</NewLeaseDuration>"),
          "UPnP: UDP 43074 to 127.0.0.1:43074 for an hour");
    int64_t t = now_ms();
    nc_net_stop();
    CHECK(now_ms() - t < 2000, "stopped in %lld ms", (long long)(now_ms() - t));
    CHECK(igd_count(&g_igd.deletes) == 1 && has(g_igd.last_delete, "<NewExternalPort>43074</NewExternalPort>") &&
          has(g_igd.last_delete, "<NewProtocol>UDP</NewProtocol>"),
          "UPnP: the forward removed at the stop");
    CHECK(port_free(NATPORT), "UDP %u released", NATPORT);

    /* a router that takes permanent forwards only (error 725) */
    igd_reset(true);
    nc_net_start();
    CHECK(igd_wait(&g_igd.adds, 2, 5000) && has(g_igd.last_add, "<NewLeaseDuration>0</NewLeaseDuration>"),
          "UPnP: asked again for a permanent forward after error 725");
    nc_net_stop();
    CHECK(igd_count(&g_igd.deletes) == 1, "UPnP: the permanent forward removed at the stop");

    /* natport taken by another program */
    igd_reset(false);
    const int busy = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in b = { .sin_family = AF_INET, .sin_port = htons(BUSYPORT) };
    bind(busy, (struct sockaddr *)&b, sizeof(b));
    option("natport=%u", BUSYPORT);
    nc_net_start();
    CHECK(nc_net_link(), "nat: started with UDP %u taken", BUSYPORT);
    console_arp();
    if (ip) {
        const uint16_t from = console_source_port(ip, "from 3074 again");
        CHECK(from && from != BUSYPORT, "the console's UDP 3074 leaves from another port (%u)", from);
    }
    sleep_ms(300);
    CHECK(igd_count(&g_igd.searches) == 0, "UPnP: not asked without the forward");
    nc_net_stop();
    close(busy);

    /* upnp=off */
    option("natport=%u", NATPORT);
    option("upnp=off");
    nc_net_start();
    CHECK(!port_free(NATPORT), "UDP %u held with UPnP off", NATPORT);
    sleep_ms(300);
    CHECK(igd_count(&g_igd.searches) == 0, "UPnP off: no M-SEARCH");
    nc_net_stop();

    /* boots stopped while UPnP is still asking */
    option("upnp=127.0.0.1:%u", g_igd.ssdp_port);
    t = now_ms();
    for (int i = 0; i < 5; i++) {
        nc_net_start();
        sleep_ms(i * 3);
        nc_net_stop();
    }
    CHECK(now_ms() - t < 5000, "5 boots stopped mid-UPnP in %lld ms", (long long)(now_ms() - t));
    CHECK(port_free(NATPORT), "UDP %u released after them", NATPORT);

    g_igd.quit = true;
    pthread_join(g_igd.thread, NULL);
    printf(g_failed ? "FAIL: nat-smoke\n" : "PASS: nat-smoke\n");
    return g_failed;
}
