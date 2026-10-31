/*
 * hakuX native core — the libretro core's network (native/net), the Network
 * option: off, LAN (system link with the consoles on this network) or Xbox
 * Live through Insignia (the Internet through NAT, Insignia's DNS server the
 * console's, as the app's "Xbox Live: Insignia" connection). LAN broadcasts
 * on the interface the default route leaves by, and also reaches the
 * consoles named in lan-peers.txt in the save directory (another subnet, a
 * VPN's far end). Insignia's DNS server can be another (insigniaDNS on this
 * network, where the provider blocks Insignia's) in insignia-dns.txt there.
 * A console heard with this one's identity (the hard disk copied from one
 * device to the other) is said, and this one gets its own as the game starts
 * next, as the app's "New identity" does.
 */
#include "native/libretro/libretro_int.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static bool g_twin_told;                /* this run's console with our identity, said */

/* The interface the IPv4 default route leaves by (the lowest metric's); ""
 * when there is none. */
static void default_route(char *name, size_t n)
{
    name[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        return;
    }
    char line[256], iface[IF_NAMESIZE + 1];
    unsigned dest, gw, flags, refs, use, metric, mask, best = UINT_MAX;
    if (fgets(line, sizeof(line), f)) {         /* the header */
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%16s %x %x %x %u %u %u %x", iface, &dest, &gw, &flags, &refs, &use, &metric,
                       &mask) == 8 && !dest && !mask && (flags & 1) && metric < best) {
                best = metric;
                snprintf(name, n, "%s", iface);
            }
        }
    }
    fclose(f);
}

/* The local network's broadcast address and prefix as lanbcast= takes them
 * ("a.b.c.d/bits"), and its interface's name: the default route's if it
 * broadcasts (Wi-Fi, Ethernet), else the first up that does. Never loopback
 * or a point-to-point tunnel (a VPN's, which has no broadcast). False if
 * there is none. */
static bool lan_bcast(char *out, size_t n, char *ifname, size_t ifn)
{
    char route[IF_NAMESIZE + 1];
    default_route(route, sizeof(route));
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) {
        return false;
    }
    const unsigned want = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
    const struct ifaddrs *pick = NULL;
    for (const struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask ||
            (i->ifa_flags & want) != want || (i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) {
            continue;
        }
        if (!pick || (!strcmp(i->ifa_name, route) && strcmp(pick->ifa_name, route))) {
            pick = i;
        }
    }
    if (pick) {
        const uint32_t ip = ((const struct sockaddr_in *)pick->ifa_addr)->sin_addr.s_addr;
        const uint32_t mask = ((const struct sockaddr_in *)pick->ifa_netmask)->sin_addr.s_addr;
        const struct in_addr b = { .s_addr = ip | ~mask };
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &b, a, sizeof(a));
        snprintf(out, n, "%s/%d", a, __builtin_popcount(mask));
        snprintf(ifname, ifn, "%s", pick->ifa_name);
    }
    freeifaddrs(list);
    return pick != NULL;
}

/* The words of <save directory>/<name>, joined by commas: one or more a
 * line, apart by commas or spaces, and # starts a comment; at most max of
 * them (0: any). "" without the file or a word in it. */
static void save_dir_list(const char *name, unsigned max, char *out, size_t n)
{
    out[0] = 0;
    const char *root = NULL;
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &root) || !root || !*root) {
        return;
    }
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", root, name);
    FILE *f = fopen(p, "r");
    if (!f) {
        return;
    }
    char line[256];
    size_t len = 0;
    unsigned words = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "#")] = 0;
        for (char *save = NULL, *t = strtok_r(line, ", \t\r\n", &save); t; t = strtok_r(NULL, ", \t\r\n", &save)) {
            if ((!max || words < max) && len + strlen(t) + 2 <= n) {
                len += (size_t)snprintf(out + len, n - len, "%s%s", len ? "," : "", t);
                words++;
            }
        }
    }
    fclose(f);
}

/* The consoles named in lan-peers.txt, as lanpeers= takes them
 * (IP[:PORT],...). */
static void lan_peers(char *out, size_t n)
{
    save_dir_list("lan-peers.txt", 0, out, n);
}

/* Insignia's DNS server (insignia.live/connect), the console's primary, as
 * the app's AppPrefs.INSIGNIA_DNS */
#define INSIGNIA_DNS "46.101.64.175"

/* The console's DNS servers for Insignia, as dns= takes them (IP[,IP]):
 * insignia-dns.txt's first two addresses, else Insignia's own. */
static void insignia_dns(char *out, size_t n)
{
    char list[64];
    save_dir_list("insignia-dns.txt", 2, list, sizeof(list));
    /* only addresses: a word that isn't one would leave the console none */
    struct in_addr a;
    char first[INET_ADDRSTRLEN + 1];
    snprintf(first, sizeof(first), "%.*s", (int)strcspn(list, ","), list);
    const char *second = strchr(list, ',');
    if (!*list || inet_pton(AF_INET, first, &a) != 1 || (second && inet_pton(AF_INET, second + 1, &a) != 1)) {
        if (*list) {
            nc_log("hakuX-libretro", "network: insignia-dns.txt names no IPv4 address (%s): Insignia's DNS server", list);
        }
        snprintf(out, n, "%s", INSIGNIA_DNS);
        return;
    }
    snprintf(out, n, "%s", list);
}

void nc_lr_net_apply(const char *mode)
{
    g_twin_told = false;
    if (!strcmp(mode, "insignia")) {
        char dns[64], arg[80];
        insignia_dns(dns, sizeof(dns));
        nc_net_option("net=nat");
        snprintf(arg, sizeof(arg), "dns=%s", dns);
        nc_net_option(arg);
        nc_log("hakuX-libretro", "network: Xbox Live through Insignia (NAT), DNS %s%s", dns,
               strcmp(dns, INSIGNIA_DNS) ? " (insignia-dns.txt)" : "");
        return;
    }
    if (strcmp(mode, "lan")) {
        nc_net_option("net=off");
        return;
    }
    char bcast[32], ifname[IF_NAMESIZE + 1], peers[240], arg[256];
    nc_net_option("net=lan");
    const bool found = lan_bcast(bcast, sizeof(bcast), ifname, sizeof(ifname));
    snprintf(arg, sizeof(arg), "lanbcast=%s", found ? bcast : "255.255.255.255");
    nc_net_option(arg);
    lan_peers(peers, sizeof(peers));
    snprintf(arg, sizeof(arg), "lanpeers=%s", peers);
    nc_net_option(arg);
    if (found) {
        nc_log("hakuX-libretro", "network: LAN on %s (%s), consoles named: %s", ifname, bcast, *peers ? peers : "none");
    } else {
        nc_log("hakuX-libretro", "network: LAN, no local network found (broadcast to 255.255.255.255), consoles "
               "named: %s", *peers ? peers : "none");
    }
}

void nc_lr_net_run(void)
{
    char addr[INET_ADDRSTRLEN];
    if (!nc_net_take_twin(addr, sizeof(addr)) || g_twin_told) {
        return;
    }
    g_twin_told = true;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/eeprom.renew", nc_io_hdd_root());
    FILE *f = fopen(p, "w");
    const bool renew = f && fclose(f) == 0;
    static char text[200];
    snprintf(text, sizeof(text), "The console at %s has this one's identity (a copied hard disk): %s", addr,
             renew ? "this one gets its own when the game starts next" : "they can't see each other");
    const struct retro_message msg = {text, 600};
    nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
    nc_log("hakuX-libretro", "network: %s%s", text, renew ? "" : " (no eeprom.renew)");
}
