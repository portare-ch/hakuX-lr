/*
 * hakuX native core — UPnP for the NAT backend (nat.c): the router (an
 * Internet Gateway Device) forwards a UDP port to this device, as a console
 * on the router's network would ask it to, so Xbox Live peers reach the
 * console first (Insignia's Open NAT). The console behind libslirp can't ask
 * itself: its 10.0.2.15 isn't on the router's network.
 *
 * One thread, every wait bounded and watching the stop:
 *   SSDP      M-SEARCH to 239.255.255.250:1900 (or upnp=ADDRESS[:PORT],
 *             asked directly); each answer's LOCATION, until one is a router
 *   describe  its device description: the WANIPConnection (or
 *             WANPPPConnection) service's control URL
 *   map       AddPortMapping, the port to this device's same port, for
 *             upnplease= seconds (permanent if the router takes no other),
 *             renewed at half the lease, a failed renewal retried with
 *             backoff; GetExternalIPAddress logged
 *   stop      DeletePortMapping, also after an AddPortMapping the stop cut
 *             short (the router may have applied it); unless the last
 *             AddPortMapping was answered yes, only once
 *             GetSpecificPortMappingEntry shows the forward is this device's
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SSDP_GROUP "239.255.255.250"
#define SSDP_PORT 1900
#define SEARCH_MS 3000                  /* SSDP: the answers awaited */
#define SEARCH_AGAIN_MS 1000            /* SSDP: the M-SEARCHes sent again (UDP) */
#define HTTP_MS 3000                    /* one HTTP exchange */
#define DELETE_MS 1000                  /* the forward removed, at the stop */
#define RETRY_MS 5000                   /* a failed renewal's first retry, doubling */
#define RETRY_MAX_MS 300000
#define RESPONSE_MAX 65536
#define TAG "hakuX-native-net"

typedef struct Url {
    struct sockaddr_in addr;
    char path[256];
} Url;

static struct {
    pthread_t thread;
    bool started;
    int stop_fd;                        /* eventfd: written once at the stop */
    uint16_t port;
    /* the thread's */
    bool leaving;                       /* the stop seen: the last exchange ignores it */
    Url control;
    char service[96];
    char local[INET_ADDRSTRLEN];
    bool mapped;                        /* the router may have the forward: removed at the stop */
    char buf[RESPONSE_MAX];
} g_upnp = { .stop_fd = -1 };

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 1: fd is ready for events; 0: the deadline (monotonic ms) passed; -1: the
 * stop (unless leaving) or an error. fd < 0 waits for the stop only. */
static int wait_fd(int fd, short events, int64_t deadline)
{
    for (;;) {
        const int64_t left = deadline - now_ms();
        if (left <= 0) {
            return 0;
        }
        struct pollfd p[2] = {
            { .fd = fd, .events = events },
            { .fd = g_upnp.leaving ? -1 : g_upnp.stop_fd, .events = POLLIN },
        };
        const int r = poll(p, 2, left > INT_MAX ? INT_MAX : (int)left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (p[1].revents) {
            return -1;
        }
        if (p[0].revents) {
            return 1;
        }
    }
}

static bool stopped(void)
{
    struct pollfd p = { .fd = g_upnp.stop_fd, .events = POLLIN };
    return poll(&p, 1, 0) > 0;
}

/* --------------------------------------------------------------- parsing */

/* The value of header name in [s, end) (one header per line): its start,
 * its length in *len, spaces trimmed; NULL if none. */
static const char *header(const char *s, const char *end, const char *name, size_t *len)
{
    const size_t nl = strlen(name);
    for (const char *line = s; line < end;) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol) {
            eol = end;
        }
        if ((size_t)(eol - line) > nl && !strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) {
                v++;
            }
            const char *ve = eol;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) {
                ve--;
            }
            *len = (size_t)(ve - v);
            return v;
        }
        line = eol + 1;
    }
    return NULL;
}

/* "http://A.B.C.D[:PORT][/PATH]" (n bytes) to u. Routers name themselves
 * by address. */
static bool parse_url(const char *s, size_t n, Url *u)
{
    if (n < 7 || strncasecmp(s, "http://", 7)) {
        return false;
    }
    s += 7;
    n -= 7;
    size_t i = 0;
    while (i < n && s[i] != ':' && s[i] != '/') {
        i++;
    }
    char host[INET_ADDRSTRLEN];
    if (!i || i >= sizeof(host)) {
        return false;
    }
    memcpy(host, s, i);
    host[i] = 0;
    unsigned port = 80;
    if (i < n && s[i] == ':') {
        port = 0;
        const size_t start = ++i;
        while (i < n && i - start < 6 && isdigit((unsigned char)s[i])) {
            port = port * 10 + (unsigned)(s[i++] - '0');
        }
        if (i == start || !port || port > 65535) {
            return false;
        }
    }
    *u = (Url){ .addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) } };
    if (inet_pton(AF_INET, host, &u->addr.sin_addr) != 1) {
        return false;
    }
    if (i < n && s[i] != '/') {
        return false;
    }
    const size_t plen = n - i;
    if (plen >= sizeof(u->path)) {
        return false;
    }
    if (plen) {
        memcpy(u->path, s + i, plen);
        u->path[plen] = 0;
    } else {
        snprintf(u->path, sizeof(u->path), "/");
    }
    return true;
}

/* path's "." and ".." segments removed, in place (RFC 3986 5.2.4, for an
 * absolute path); a query or fragment kept as it is. */
static bool path_end(char c)
{
    return !c || c == '?' || c == '#';
}

static void remove_dots(char *path)
{
    char out[sizeof(((Url *)0)->path)];
    size_t o = 0;
    const char *in = path;
    while (!path_end(*in)) {
        if (!strncmp(in, "/./", 3)) {
            in += 2;
        } else if (in[0] == '/' && in[1] == '.' && path_end(in[2])) {
            out[o++] = '/';
            in += 2;
        } else if (!strncmp(in, "/../", 4) || (!strncmp(in, "/..", 3) && path_end(in[3]))) {
            while (o && out[--o] != '/') {
            }
            if (in[3] == '/') {
                in += 3;
            } else {
                out[o++] = '/';
                in += 3;
            }
        } else {
            do {
                out[o++] = *in++;
            } while (!path_end(*in) && *in != '/');
        }
    }
    snprintf(out + o, sizeof(out) - o, "%s", in);         /* the query, verbatim */
    memcpy(path, out, strlen(out) + 1);
}

/* ref resolved against base (RFC 3986 5.2): absolute, an absolute path on
 * base's server, or a path relative to base's directory. */
static bool resolve(const Url *base, const char *ref, Url *out)
{
    if (!strncasecmp(ref, "http://", 7)) {
        return parse_url(ref, strlen(ref), out);
    }
    *out = *base;
    int n;
    if (ref[0] == '/') {
        n = snprintf(out->path, sizeof(out->path), "%s", ref);
    } else {
        size_t dir = strcspn(base->path, "?#");
        while (dir && base->path[dir - 1] != '/') {
            dir--;
        }
        n = snprintf(out->path, sizeof(out->path), "%.*s%s%s", (int)dir, base->path, dir ? "" : "/", ref);
    }
    if (n < 0 || n >= (int)sizeof(out->path)) {
        return false;
    }
    remove_dots(out->path);
    return true;
}

/* The text of the first <tag>...</tag> in [s, end), trimmed, to out (n
 * bytes); false if none or too long. */
static bool xml_text(const char *s, const char *end, const char *tag, char *out, size_t n)
{
    char open[48], close[48];
    const int ol = snprintf(open, sizeof(open), "<%s>", tag);
    const int cl = snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = memmem(s, (size_t)(end - s), open, (size_t)ol);
    if (!a) {
        return false;
    }
    a += ol;
    const char *b = memmem(a, (size_t)(end - a), close, (size_t)cl);
    if (!b) {
        return false;
    }
    while (a < b && isspace((unsigned char)*a)) {
        a++;
    }
    while (b > a && isspace((unsigned char)b[-1])) {
        b--;
    }
    if ((size_t)(b - a) >= n) {
        return false;
    }
    memcpy(out, a, (size_t)(b - a));
    out[b - a] = 0;
    return true;
}

/* A chunked body (n bytes) decoded in place; its length. */
static size_t dechunk(char *s, size_t n)
{
    size_t in = 0, out = 0;
    while (in < n) {
        char *e;
        const unsigned long size = strtoul(s + in, &e, 16);
        const char *lf = memchr(s + in, '\n', n - in);
        if (e == s + in || !lf || !size) {
            break;
        }
        in = (size_t)(lf - s) + 1;
        const size_t take = size < n - in ? size : n - in;
        memmove(s + out, s + in, take);
        out += take;
        in += take;
        if (in < n && s[in] == '\r') {
            in++;
        }
        if (in < n && s[in] == '\n') {
            in++;
        }
    }
    return out;
}

/* Whether the response read so far (len bytes, NUL-terminated) is all of
 * it: a Content-Length reached or a chunked body's last chunk. */
static bool response_complete(const char *s, size_t len)
{
    const char *end = strstr(s, "\r\n\r\n");
    if (!end) {
        return false;
    }
    const size_t body = len - (size_t)(end + 4 - s);
    size_t vl;
    const char *v = header(s, end, "Content-Length", &vl);
    if (v) {
        return body >= strtoul(v, NULL, 10);
    }
    v = header(s, end, "Transfer-Encoding", &vl);
    return v && vl == 7 && !strncasecmp(v, "chunked", 7) && len >= 5 && !strcmp(s + len - 5, "0\r\n\r\n");
}

/* --------------------------------------------------------------- HTTP */

/* An HTTP exchange with u's server: req (len bytes) sent, the response read
 * to its end. Its status (0: none, no answer in timeout_ms or the stop);
 * the body, decoded and NUL-terminated, in g_upnp.buf. */
static int http(const Url *u, const char *req, size_t len, int timeout_ms)
{
    const int64_t deadline = now_ms() + timeout_ms;
    char *buf = g_upnp.buf, *hend, *body;
    buf[0] = 0;
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    int status = 0, err = 0;
    socklen_t elen = sizeof(err);
    size_t got = 0, blen, vl;
    bool whole = false;
    const char *v;
    if ((connect(fd, (const struct sockaddr *)&u->addr, sizeof(u->addr)) < 0 && errno != EINPROGRESS) ||
        wait_fd(fd, POLLOUT, deadline) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
        goto out;
    }
    for (size_t off = 0; off < len;) {
        const ssize_t n = send(fd, req + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
        } else if ((n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ||
                   wait_fd(fd, POLLOUT, deadline) != 1) {
            goto out;
        }
    }
    while (got + 1 < RESPONSE_MAX) {
        const ssize_t n = recv(fd, buf + got, RESPONSE_MAX - 1 - got, 0);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = 0;
            if ((whole = response_complete(buf, got))) {
                break;
            }
        } else if (n == 0) {
            whole = true;               /* the server closed: the end */
            break;
        } else if ((errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ||
                   wait_fd(fd, POLLIN, deadline) != 1) {
            break;
        }
    }
    buf[got] = 0;
    hend = strstr(buf, "\r\n\r\n");
    if (!whole || !hend || got < 12 || strncmp(buf, "HTTP/1.", 7)) {
        goto out;
    }
    status = atoi(buf + 9);
    body = hend + 4;
    blen = got - (size_t)(body - buf);
    v = header(buf, hend, "Transfer-Encoding", &vl);
    if (v && vl == 7 && !strncasecmp(v, "chunked", 7)) {
        blen = dechunk(body, blen);
    } else if ((v = header(buf, hend, "Content-Length", &vl))) {
        const unsigned long cl = strtoul(v, NULL, 10);
        blen = cl < blen ? cl : blen;
    }
    memmove(buf, body, blen);
    buf[blen] = 0;
out:
    close(fd);
    if (!status) {
        buf[0] = 0;
    }
    return status;
}

/* A SOAP action on the control URL with its arguments (XML elements); the
 * HTTP status, the response's body in g_upnp.buf. */
static int soap(const char *action, const char *args, int timeout_ms)
{
    const Url *u = &g_upnp.control;
    char host[INET_ADDRSTRLEN], body[1024], req[2048];
    inet_ntop(AF_INET, &u->addr.sin_addr, host, sizeof(host));
    const int blen = snprintf(body, sizeof(body),
                              "<?xml version=\"1.0\"?>\r\n"
                              "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                              "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                              "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
                              action, g_upnp.service, args, action);
    const int rlen = snprintf(req, sizeof(req),
                              "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
                              "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                              "SOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                              u->path, host, ntohs(u->addr.sin_port), g_upnp.service, action, blen, body);
    if (blen >= (int)sizeof(body) || rlen >= (int)sizeof(req)) {
        return 0;
    }
    return http(u, req, (size_t)rlen, timeout_ms);
}

/* The UPnP error code of the last SOAP response, 0 if none. */
static int soap_error(void)
{
    char code[16];
    const char *b = g_upnp.buf;
    return xml_text(b, b + strlen(b), "errorCode", code, sizeof(code)) ? atoi(code) : 0;
}

/* --------------------------------------------------------------- steps */

/* The description at desc: the control URL and service type of its WAN
 * connection service, the first of services[] it has. */
static bool describe(const Url *desc)
{
    static const char *const services[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    char host[INET_ADDRSTRLEN], req[512];
    inet_ntop(AF_INET, &desc->addr.sin_addr, host, sizeof(host));
    const int rlen = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
                              desc->path, host, ntohs(desc->addr.sin_port));
    if (rlen >= (int)sizeof(req)) {
        return false;
    }
    const int status = http(desc, req, (size_t)rlen, HTTP_MS);
    if (status != 200) {
        if (!stopped()) {
            nc_log(TAG, "nat: UPnP: the description at http://%s:%u%s: %s %d", host, ntohs(desc->addr.sin_port),
                   desc->path, status ? "HTTP" : "no answer", status);
        }
        return false;
    }
    const char *xml = g_upnp.buf, *end = xml + strlen(xml);
    Url base = *desc;
    char text[256];
    if (xml_text(xml, end, "URLBase", text, sizeof(text)) && *text) {
        Url b;
        if (parse_url(text, strlen(text), &b)) {
            base = b;
        }
    }
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        for (const char *s = xml; (s = memmem(s, (size_t)(end - s), "<service>", 9));) {
            const char *e = memmem(s, (size_t)(end - s), "</service>", 10);
            if (!e) {
                break;
            }
            if (xml_text(s, e, "serviceType", text, sizeof(text)) && !strcmp(text, services[i]) &&
                xml_text(s, e, "controlURL", text, sizeof(text)) && resolve(&base, text, &g_upnp.control)) {
                snprintf(g_upnp.service, sizeof(g_upnp.service), "%s", services[i]);
                return true;
            }
            s = e;
        }
    }
    nc_log(TAG, "nat: UPnP: %s answered but has no WAN connection service", host);
    return false;
}

/* SSDP: the Internet Gateway Devices that answer, each new one described
 * as it's heard, until one has a WAN connection service (describe). */
static bool discover(void)
{
    static const char *const targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        nc_log(TAG, "nat: UPnP: no socket: %s", strerror(errno));
        return false;
    }
    const unsigned char ttl = 2;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    const NcNetConfig *c = nc_net_config();
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(c->upnp_port ? c->upnp_port : SSDP_PORT) };
    if (c->upnp_ip) {
        to.sin_addr.s_addr = c->upnp_ip;
    } else {
        inet_pton(AF_INET, SSDP_GROUP, &to.sin_addr);
    }
    char host[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &to.sin_addr, host, sizeof(host));
    Url tried[8];
    unsigned ntried = 0;
    const int64_t end = now_ms() + SEARCH_MS;
    int64_t again = 0;
    bool found = false;
    while (!found && !stopped()) {
        const int64_t now = now_ms();
        if (again && now >= end) {
            break;
        }
        if (now >= again) {
            for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
                char msg[256];
                const int n = snprintf(msg, sizeof(msg),
                                       "M-SEARCH * HTTP/1.1\r\nHOST: %s:%u\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                                       "ST: %s\r\n\r\n",
                                       host, ntohs(to.sin_port), targets[i]);
                if (sendto(fd, msg, (size_t)n, 0, (const struct sockaddr *)&to, sizeof(to)) < 0 && !again) {
                    nc_log(TAG, "nat: UPnP: M-SEARCH to %s: %s", host, strerror(errno));
                }
            }
            again = now + SEARCH_AGAIN_MS;
        }
        if (wait_fd(fd, POLLIN, again < end ? again : end) < 0) {
            break;
        }
        char answer[1536];
        ssize_t n;
        while (!found && (n = recv(fd, answer, sizeof(answer) - 1, 0)) > 0) {
            answer[n] = 0;
            size_t ll;
            const char *loc = header(answer, answer + n, "LOCATION", &ll);
            Url desc;
            if (strncmp(answer, "HTTP/1.", 7) || !strstr(answer, " 200") || !loc || !parse_url(loc, ll, &desc)) {
                continue;
            }
            bool seen = false;
            for (unsigned i = 0; i < ntried && !seen; i++) {
                seen = !memcmp(&tried[i].addr, &desc.addr, sizeof(desc.addr)) && !strcmp(tried[i].path, desc.path);
            }
            if (seen || ntried == sizeof(tried) / sizeof(tried[0])) {
                continue;
            }
            tried[ntried++] = desc;
            found = describe(&desc);
        }
    }
    close(fd);
    if (!found && !ntried && !stopped()) {
        nc_log(TAG, "nat: UPnP: no router answered: for Open NAT, forward UDP %u to this device on the router",
               g_upnp.port);
    }
    return found;
}

/* This device's address toward the router (the forward's destination). */
static bool local_address(void)
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in me;
    socklen_t len = sizeof(me);
    const bool ok = fd >= 0 &&
                    connect(fd, (const struct sockaddr *)&g_upnp.control.addr, sizeof(g_upnp.control.addr)) == 0 &&
                    getsockname(fd, (struct sockaddr *)&me, &len) == 0 &&
                    inet_ntop(AF_INET, &me.sin_addr, g_upnp.local, sizeof(g_upnp.local));
    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/* The router's own Internet address, logged: one that is private or
 * carrier-grade NAT's (100.64.0.0/10) has another NAT before the Internet,
 * which no forward here opens. */
static void log_external(void)
{
    char ip[INET_ADDRSTRLEN];
    struct in_addr a;
    const char *b = g_upnp.buf;
    if (soap("GetExternalIPAddress", "", HTTP_MS) != 200 ||
        !xml_text(b, b + strlen(b), "NewExternalIPAddress", ip, sizeof(ip)) || inet_pton(AF_INET, ip, &a) != 1) {
        return;
    }
    const uint32_t h = ntohl(a.s_addr);
    const bool behind = (h >> 24) == 10 || (h >> 20) == (172u << 4 | 1) || (h >> 16) == (192u << 8 | 168) ||
                        (h >> 22) == (100u << 2 | 1);
    nc_log(TAG, "nat: UPnP: the router's Internet address is %s%s", ip,
           behind ? ", itself behind another NAT (the provider's?): peers may not reach the console" : "");
}

/* The HTTP status (200: forwarded); *error the UPnP error code, 0 if none. */
static int add_mapping(unsigned lease, int *error)
{
    char args[640];
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
             "<NewEnabled>1</NewEnabled><NewPortMappingDescription>hakuX Xbox Live</NewPortMappingDescription>"
             "<NewLeaseDuration>%u</NewLeaseDuration>",
             g_upnp.port, g_upnp.port, g_upnp.local, lease);
    const int status = soap("AddPortMapping", args, HTTP_MS);
    *error = status && status != 200 ? soap_error() : 0;
    return status;
}

/* Whether the router's forward of the port is this device's: 1 yes, 0 no
 * (another device's, or none: error 714), -1 unknown (no answer, another
 * error, an entry without its client). DeletePortMapping names only the
 * external port, so another console's forward must not be removed. */
static int mapping_is_ours(void)
{
    char args[256], client[INET_ADDRSTRLEN], port[8];
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
             g_upnp.port);
    const int status = soap("GetSpecificPortMappingEntry", args, DELETE_MS);
    if (status != 200) {
        return status && soap_error() == 714 ? 0 : -1;      /* NoSuchEntryInArray */
    }
    const char *b = g_upnp.buf, *end = b + strlen(b);
    if (!xml_text(b, end, "NewInternalClient", client, sizeof(client)) ||
        !xml_text(b, end, "NewInternalPort", port, sizeof(port))) {
        return -1;
    }
    return !strcmp(client, g_upnp.local) && atoi(port) == g_upnp.port;
}

/* owned: the router's last answer to AddPortMapping was yes. Otherwise the
 * forward is removed only once the router shows it is this device's. */
static void delete_mapping(bool owned)
{
    const int ours = mapping_is_ours();
    if (!ours || (ours < 0 && !owned)) {
        nc_log(TAG, "nat: UPnP: the router's UDP %u forward left alone: %s", g_upnp.port,
               ours ? "unknown whether it is this device's" : "not this device's");
        return;
    }
    char args[256];
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
             g_upnp.port);
    const int status = soap("DeletePortMapping", args, DELETE_MS);
    if (status == 200) {
        nc_log(TAG, "nat: UPnP: the router's UDP %u forward removed", g_upnp.port);
    } else if (status) {
        nc_log(TAG, "nat: UPnP: the router's UDP %u forward not removed: HTTP %d, UPnP error %d", g_upnp.port,
               status, soap_error());
    } else {
        nc_log(TAG, "nat: UPnP: the router's UDP %u forward not removed: no answer", g_upnp.port);
    }
}

static void *upnp_thread(void *arg)
{
    if (!discover() || !local_address()) {
        return NULL;
    }
    char router[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &g_upnp.control.addr.sin_addr, router, sizeof(router));
    log_external();
    unsigned lease = nc_net_config()->upnp_lease;
    bool forwarded = false;                     /* the router said yes once */
    int64_t retry = 0;                          /* a failed renewal's next delay, 0: none failed */
    bool owned = false;                         /* the last AddPortMapping answered yes */
    while (!stopped()) {
        int error;
        const bool before = g_upnp.mapped;
        g_upnp.mapped = true;                   /* a request the stop cuts short may still be applied */
        const int status = add_mapping(lease, &error);
        owned = status == 200;
        if (status != 200) {
            if (status && !before) {
                g_upnp.mapped = false;          /* the router answered no: nothing to remove */
            }
            if (stopped()) {
                break;
            }
            if (error == 725 && lease) {        /* OnlyPermanentLeasesSupported */
                lease = 0;
                continue;
            }
            if (!status) {
                nc_log(TAG, "nat: UPnP: the router (%s) didn't %s UDP %u: no answer", router,
                       forwarded ? "renew" : "forward", g_upnp.port);
            } else {
                nc_log(TAG, "nat: UPnP: the router (%s) didn't %s UDP %u: HTTP %d, UPnP error %d%s", router,
                       forwarded ? "renew" : "forward", g_upnp.port, status, error,
                       error == 718 ? " (another device has the port)" : "");
            }
            if (!forwarded) {
                break;
            }
            /* a renewal: the forward may still hold; asked again until the router answers */
            const int64_t cap = (int64_t)lease * 500 < RETRY_MAX_MS ? (int64_t)lease * 500 : RETRY_MAX_MS;
            retry = !retry ? ((int64_t)lease * 125 < RETRY_MS ? (int64_t)lease * 125 : RETRY_MS)
                           : (retry * 2 < cap ? retry * 2 : cap);
            if (retry < 1) {
                retry = 1;
            }
            nc_log(TAG, "nat: UPnP: asking again in %lld ms", (long long)retry);
            if (wait_fd(-1, 0, now_ms() + retry) < 0) {
                break;
            }
            continue;
        }
        if (!forwarded) {
            nc_log(TAG, "nat: UPnP: the router (%s) forwards UDP %u to %s:%u (lease %s)", router, g_upnp.port,
                   g_upnp.local, g_upnp.port, lease ? "renewed while running" : "permanent, removed at the stop");
        } else if (retry) {
            nc_log(TAG, "nat: UPnP: the router (%s) renewed UDP %u again", router, g_upnp.port);
        }
        forwarded = true;
        retry = 0;
        if (wait_fd(-1, 0, lease ? now_ms() + (int64_t)lease * 500 : INT64_MAX) < 0) {
            break;
        }
    }
    if (g_upnp.mapped) {
        g_upnp.leaving = true;
        delete_mapping(owned);
    }
    return NULL;
}

/* ------------------------------------------------------------ the API */

void nc_upnp_start(uint16_t port)
{
    nc_upnp_stop();
    g_upnp.port = port;
    g_upnp.stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_upnp.stop_fd < 0) {
        nc_log(TAG, "nat: UPnP: no eventfd: %s", strerror(errno));
        return;
    }
    if (pthread_create(&g_upnp.thread, NULL, upnp_thread, NULL) != 0) {
        nc_log(TAG, "nat: UPnP: no thread");
        close(g_upnp.stop_fd);
        g_upnp.stop_fd = -1;
        return;
    }
    g_upnp.started = true;
}

void nc_upnp_stop(void)
{
    if (g_upnp.started) {
        const uint64_t one = 1;
        (void)!write(g_upnp.stop_fd, &one, sizeof(one));
        pthread_join(g_upnp.thread, NULL);
        g_upnp.started = false;
    }
    if (g_upnp.stop_fd >= 0) {
        close(g_upnp.stop_fd);
        g_upnp.stop_fd = -1;
    }
    g_upnp.leaving = g_upnp.mapped = false;
    g_upnp.port = 0;
    g_upnp.service[0] = g_upnp.local[0] = 0;
    memset(&g_upnp.control, 0, sizeof(g_upnp.control));
}
