/* NaOS DHCP client: broadcast DISCOVER, unicast REQUEST, parse OFFER/ACK.
 * Options needed: 53 (type), 54 (server), 51 (lease, ignored), 1 (mask),
 * 3 (router), 6 (DNS). Retries with ~1.5s waits; failure keeps static IP. */
#include "dhcp.h"
#include "pkt.h"
#include "net.h"
#include "udp.h"
#include "../drv/pit.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define DHCP_OP_REQ 1
#define DHCP_MAGIC 0x63825363u

static void dhcp_base(uint8_t *m, uint32_t xid, const uint8_t mac[6]) {
  naos_memset(m, 0, 244);
  m[0] = DHCP_OP_REQ;
  m[1] = 1;
  m[2] = 6;
  m[3] = 0;
  pkt_put32(m + 4, xid);
  pkt_put16(m + 10, 0);
  for (int i = 0; i < 6; i++) m[28 + i] = mac[i];
  pkt_put32(m + 236, DHCP_MAGIC);
}

/* Returns message type (53) or -1; fills yiaddr + options. */
static int dhcp_parse(const uint8_t *m, uint32_t len, uint32_t xid,
                      uint32_t *yiaddr, uint32_t *server, uint32_t *mask,
                      uint32_t *gw, uint32_t *dns) {
  uint32_t o;
  int type = -1;
  if (len < 244) return -1;
  if (m[0] != 2) return -1; /* not a reply */
  if (pkt_get32(m + 4) != xid) return -1;
  if (pkt_get32(m + 236) != DHCP_MAGIC) return -1;
  *yiaddr = pkt_get32(m + 16);
  o = 240;
  while (o < len) {
    uint8_t code = m[o];
    uint8_t n;
    if (code == 255) break;
    if (code == 0) {
      o++;
      continue;
    }
    if (o + 1 >= len) break;
    n = m[o + 1];
    if (o + 2 + n > len) break;
    if (code == 53 && n >= 1) type = m[o + 2];
    else if (code == 54 && n >= 4 && server) *server = pkt_get32(m + o + 2);
    else if (code == 1 && n >= 4 && mask) *mask = pkt_get32(m + o + 2);
    else if (code == 3 && n >= 4 && gw) *gw = pkt_get32(m + o + 2);
    else if (code == 6 && n >= 4 && dns) *dns = pkt_get32(m + o + 2);
    o += 2u + n;
  }
  return type;
}

int net_dhcp(void) {
  static uint8_t msg[300];
  static uint8_t rep[1536];
  const uint8_t *mac = net_mac();
  uint32_t xid = (pit_ticks() * 2654435761u) ^ 0xD0C00000u ^ pit_ticks();
  uint32_t yiaddr = 0, server = 0;
  uint32_t sip;
  uint16_t sport;
  int s, r, o;
  char ips[20];
  kprintf("dhcp: discover...\n");
  s = udp_open(68);
  if (s < 0) {
    kprintf("dhcp: no socket\n");
    return -1;
  }
  /* DISCOVER ×2. */
  for (int t = 0; t < 2 && yiaddr == 0; t++) {
    dhcp_base(msg, xid, mac);
    o = 240;
    msg[o++] = 53;
    msg[o++] = 1;
    msg[o++] = 1; /* discover */
    msg[o++] = 55;
    msg[o++] = 3;
    msg[o++] = 1;
    msg[o++] = 3;
    msg[o++] = 6;
    msg[o++] = 255;
    if (udp_sendto(s, 0, 0xFFFFFFFFu, 67, msg, (uint32_t)o) != 0) break;
    {
      uint64_t t0 = pit_ticks();
      while (pit_ticks() - t0 < 150) {
        uint32_t y = 0, sv = 0;
        r = udp_recvfrom(s, rep, sizeof(rep), &sip, &sport, 10);
        if (r < 0) continue;
        if (dhcp_parse(rep, (uint32_t)r, xid, &y, &sv, 0, 0, 0) == 2 &&
            y != 0) {
          yiaddr = y;
          server = sv;
          break;
        }
      }
    }
  }
  if (yiaddr == 0 || server == 0) {
    kprintf("dhcp: no offer (keeping static)\n");
    udp_close(s);
    return -1;
  }
  net_ip_str(yiaddr, ips);
  kprintf("dhcp: offer %s, requesting...\n", ips);
  /* REQUEST ×2 (unicast to server). */
  for (int t = 0; t < 2; t++) {
    dhcp_base(msg, xid, mac);
    o = 240;
    msg[o++] = 53;
    msg[o++] = 1;
    msg[o++] = 3; /* request */
    msg[o++] = 50;
    msg[o++] = 4;
    pkt_put32(msg + o, yiaddr);
    o += 4;
    msg[o++] = 54;
    msg[o++] = 4;
    pkt_put32(msg + o, server);
    o += 4;
    msg[o++] = 255;
    if (udp_sendto(s, 0, server, 67, msg, (uint32_t)o) != 0) break;
    {
      uint64_t t0 = pit_ticks();
      while (pit_ticks() - t0 < 150) {
        uint32_t y = 0, sv = 0, mk = 0, g = 0, dn = 0;
        r = udp_recvfrom(s, rep, sizeof(rep), &sip, &sport, 10);
        if (r < 0) continue;
        if (dhcp_parse(rep, (uint32_t)r, xid, &y, &sv, &mk, &g, &dn) == 5 &&
            y != 0) {
          if (mk == 0) mk = 0xFFFFFF00u;
          if (g == 0) g = server;
          net_set_addrs(y, mk, g, dn);
          udp_close(s);
          net_ip_str(y, ips);
          kprintf("dhcp: ack %s\n", ips);
          return 0;
        }
      }
    }
  }
  kprintf("dhcp: no ack (keeping static)\n");
  udp_close(s);
  return -1;
}
