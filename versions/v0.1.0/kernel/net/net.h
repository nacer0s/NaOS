/* NaOS network stack: ARP + IPv4 + ICMP ping + UDP + TCP client.
 * Default config is QEMU user-net; DHCP overwrites at boot. Own code. */
#ifndef NAOS_NET_H
#define NAOS_NET_H

#include <stdint.h>

void net_init(void); /* e1000 + stack (static IP until DHCP). */
void net_poll(void); /* drain RX each tick; answers ARP + ping in */
/* Ping an IPv4 address (host order). Prints results. Returns replies. */
int net_ping(uint32_t ip);
void net_status(void); /* `net` command */
/* Parse "a.b.c.d" into host-order u32. 0 ok. */
int net_parse_ip(const char *s, uint32_t *out);

/* Shared IPv4 layer for udp.c/tcp.c/dhcp.c. */
void net_set_addrs(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
uint32_t net_ip(void);
uint32_t net_mask(void);
uint32_t net_gw(void);
uint32_t net_dns(void);
const uint8_t *net_mac(void); /* 6 bytes */
/* Render host-order IP into >=16B buffer. */
void net_ip_str(uint32_t ip, char *out);
/* Next hop for dst (same subnet ? dst : gateway; broadcast passthrough). */
uint32_t net_nexthop(uint32_t dst);
/* Blocking ARP resolve (command context only). 0 ok. */
int net_resolve(uint32_t ip, uint8_t mac[6]);
/* Send L4 packet (caller holds IRQ guard; broadcast needs no ARP). */
int net_send_ip(uint32_t src, uint32_t dst, uint8_t proto, const void *l4,
                uint32_t l4len);
/* Send with explicit dst MAC (NULL = broadcast). Caller holds IRQ guard. */
int net_send_ip_mac(const uint8_t *mac, uint32_t src, uint32_t dst,
                    uint8_t proto, const void *l4, uint32_t l4len);

#endif /* NAOS_NET_H */
