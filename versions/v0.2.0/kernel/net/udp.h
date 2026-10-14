/* NaOS UDP: tiny socket table over the IPv4 layer in net.c. Own code. */
#ifndef NAOS_UDP_H
#define NAOS_UDP_H

#include <stdint.h>

/* Open a local port (0 = ephemeral). Returns sock id or -1. */
int udp_open(uint16_t port);
void udp_close(int s);
/* Send a datagram (src 0.0.0.0 allowed for DHCP). 0 ok. */
int udp_sendto(int s, uint32_t srcip, uint32_t dstip, uint16_t dstport,
               const void *data, uint32_t len);
/* Wait for a datagram (timeout in ticks). Returns payload bytes, -1 on timeout. */
int udp_recvfrom(int s, void *buf, uint32_t cap, uint32_t *srcip,
                 uint16_t *srcport, uint64_t timeout_ticks);
/* RX demux (called from net.c on UDP packets for us). */
void udp_on_dgram(uint32_t srcip, const uint8_t *d, uint32_t len);

#endif /* NAOS_UDP_H */
