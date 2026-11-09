/*
 * Emulated Xbox Ethernet network.
 *
 *   off  Link down.
 *   on   NAT for Xbox Live/Insignia and LAN for system link together.
 *   nat  NAT only (testing).
 *   lan  LAN only (testing).
 *
 * The network thread exchanges whole Ethernet frames with the NIC through
 * queues; it does not access guest memory.
 */
#ifndef NATIVE_NET_NET_H
#define NATIVE_NET_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NcNetMode { NC_NET_OFF, NC_NET_ON, NC_NET_NAT, NC_NET_LAN } NcNetMode;

#define NC_NET_FRAME_MAX 1536           /* an Ethernet frame, header included, no FCS */

/* Options (native/core/options.c), before the boot:
 *   net=off|on              the cable (nat or lan: that half alone, for tests)
 *   dns=A.B.C.D[,A.B.C.D]   nat: the DNS servers DHCP gives the guest
 *   natport=N               nat: host UDP port for the console's 3074,
 *                           used for outgoing and unsolicited incoming
 *                           traffic; default 3074, 0 disables forwarding
 *   upnp=on|off|A.B.C.D[:PORT]
 *                           nat: request a router forward; default on.
 *                           An address selects unicast SSDP discovery
 *                           instead of multicast; default port 1900
 *   upnplease=S             nat: lease in seconds (0-604800), renewed
 *                           halfway through; default 3600, 0 is permanent
 *   lanport=N               lan: the UDP port (all consoles the same)
 *   lanbcast=A.B.C.D[/BITS][,...] lan: the local networks' broadcast
 *                           addresses (and prefixes: who is outside them),
 *                           up to NC_NET_BCASTS (Wi-Fi and Wi-Fi Direct)
 *   lanpeers=IP[:PORT],...  lan: consoles also reached directly (another
 *                           subnet, a tunnel's far end)
 *   pcap=PATH               debug: every frame sent and received, to a
 *                           capture file (Wireshark's)
 * True if arg is one of them. */
bool nc_net_option(const char *arg);
NcNetMode nc_net_mode(void);
/* The boot: the backend's thread starts (nothing in mode off). */
void nc_net_start(void);
/* After the NIC stops: join the backend and release this run's queues,
 * sockets and capture file. Configuration is kept for the next boot. */
void nc_net_stop(void);
/* Whether the NIC's link is up (a backend running). */
bool nc_net_link(void);

/* The NIC: a frame to send (copied; dropped when the queue is full). */
void nc_net_send(const uint8_t *frame, size_t len);
/* The NIC: the station address the guest gave it (its frames' source).
 * nc_net_mac: it, copied to mac; false while the guest hasn't set one. */
void nc_net_set_mac(const uint8_t mac[6]);
bool nc_net_mac(uint8_t mac[6]);
/* The NIC's thread: the oldest frame received, copied to buf (cap bytes);
 * its length, 0 when none. nc_net_peek gives the next one's length without
 * taking it (a frame the guest has no buffer for yet stays queued). */
size_t nc_net_recv(uint8_t *buf, size_t cap);
size_t nc_net_peek(void);
/* The NIC's thread waits for a received frame, at most timeout_ns. */
void nc_net_wait_rx(uint64_t timeout_ns);

/* A console with this one's MAC (its EEPROM a copy of this one's, the app's
 * hard disk copied from device to device), heard at ip (network order): the
 * two can't see each other. nc_net_take_twin, for the app: its address, once
 * each console; false when no new one. */
void nc_net_twin(uint32_t ip);
bool nc_net_take_twin(char *addr, size_t len);

/* Toggle NAT Internet access. ARP and local DHCP remain available. */
void nc_net_internet(bool on);

/* ---- backends (native/net/nat.c, lan.c, all.c) ---- */

struct pollfd;
typedef struct NcNetBackend {
    const char *name;
    bool (*init)(void);
    void (*close)(void);                /* after its thread has joined; partial init too */
    /* a guest frame, on the network thread */
    void (*input)(const uint8_t *frame, size_t len);
    /* the descriptors to poll (up to max) and the longest wait (ms);
     * returns how many */
    int (*fill)(struct pollfd *fds, int max, int *timeout_ms);
    /* after the poll (the same descriptors, their revents) */
    void (*poll)(struct pollfd *fds, int n);
} NcNetBackend;

extern const NcNetBackend nc_net_nat, nc_net_lan, nc_net_all;

/* nat.c: whether a frame's destination is the NAT's own (its gateway's and
 * DNS's addresses, 52:55:...): all.c sends those to it alone. */
bool nc_net_nat_address(const uint8_t mac[6]);
/* nat.c: run beside the LAN (all.c), before its init: each console leases
 * a MAC-derived address, 10.0.2.16-215; alone, NAT keeps 10.0.2.15, where
 * the UDP 3074 forward and consoles set up by hand expect it. */
void nc_net_nat_shared(bool shared);
/* nat.c: the address the console is leased (network order), 0 before. */
uint32_t nc_net_nat_lease(void);

/* lan.c, beside the NAT (net=on), mac the console's (if the NIC hasn't
 * said yet): the address the NAT is about to lease or has leased (leased),
 * which the hellos claim; whether another console keeps ip against this
 * one (heard within the stations' TTL: it uses ip, by its frames or a
 * leased claim; it isn't hakuX's, so can't move; or it is about to lease
 * ip too with a lower MAC); and an ARP probe for ip. */
void nc_net_lan_claim(const uint8_t mac[6], uint32_t ip, bool leased);
bool nc_net_lan_taken(const uint8_t mac[6], uint32_t ip);
void nc_net_lan_probe(uint32_t ip);

/* lan.c: another console uses the console's leased address (the front end
 * tells the user, as with a twin); taken once. */
void nc_net_conflict(uint32_t ip);
bool nc_net_take_conflict(char *addr, size_t len);

/* A backend, on the network thread: a frame for the guest. */
void nc_net_deliver(const uint8_t *frame, size_t len);

#define NC_NET_BCASTS 4

/* Backends' configuration (the options). */
typedef struct NcNetConfig {
    uint32_t dns[2];                    /* network order, 0: none */
    uint16_t nat_port;                  /* 0: no forward */
    bool upnp;
    uint32_t upnp_ip;                   /* network order, 0: the SSDP multicast group */
    uint16_t upnp_port;
    unsigned upnp_lease;                /* seconds, 0: permanent */
    uint16_t lan_port;
    unsigned nbcast;                    /* lan's networks, 0: 255.255.255.255 */
    uint32_t lan_bcast[NC_NET_BCASTS];  /* network order */
    uint32_t lan_mask[NC_NET_BCASTS];   /* network order, 0: not known */
    unsigned npeers;
    uint32_t peer_ip[8];                /* network order */
    uint16_t peer_port[8];
} NcNetConfig;
const NcNetConfig *nc_net_config(void);

/* ---- UPnP (native/net/upnp.c), for the NAT backend ----
 * nc_upnp_start requests a router forward to the same port on this device
 * and renews its lease on a worker thread. nc_upnp_stop checks ownership,
 * attempts cleanup and joins the thread; safe to call when not started. */
void nc_upnp_start(uint16_t port);
void nc_upnp_stop(void);

#ifdef __cplusplus
}
#endif

#endif
