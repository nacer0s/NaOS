/* NaOS UDP sockets. Polled RX runs in tick context; sends and queue
 * takes are IRQ-guarded so a tick can never corrupt a send in flight. */
#include "udp.h"
#include "pkt.h"
#include "net.h"
#include "../drv/pit.h"
#include "../../common/cstring.h"

#define UDP_N 4
#define UDP_Q 2
#define UDP_CAP 1536
#define UDP_MAXDATA 1472

typedef struct {
  int used;
  uint16_t port;
  uint8_t pkt[UDP_Q][UDP_CAP];
  uint32_t plen[UDP_Q];
  uint32_t sip[UDP_Q];
  uint16_t sport[UDP_Q];
  int head, count;
} usock_t;

static usock_t g_u[UDP_N];
static uint16_t g_eph = 49152;
static uint8_t g_tx[8 + UDP_MAXDATA];

int udp_open(uint16_t port) {
  for (int i = 0; i < UDP_N; i++) {
    if (!g_u[i].used) {
      if (port == 0) {
        port = g_eph++;
        if (g_eph < 49152) g_eph = 49152;
      }
      g_u[i].used = 1;
      g_u[i].port = port;
      g_u[i].head = 0;
      g_u[i].count = 0;
      return i;
    }
  }
  return -1;
}

void udp_close(int s) {
  if (s < 0 || s >= UDP_N) return;
  g_u[s].used = 0;
}

int udp_sendto(int s, uint32_t srcip, uint32_t dstip, uint16_t dstport,
               const void *data, uint32_t len) {
  uint64_t f;
  uint32_t ulen;
  uint8_t pseudo[12];
  uint32_t sum;
  int r;
  if (s < 0 || s >= UDP_N || !g_u[s].used) return -1;
  if (len > UDP_MAXDATA) return -1;
  if (dstip != 0xFFFFFFFFu &&
      net_resolve(net_nexthop(dstip), pseudo + 0) != 0 &&
      net_resolve(net_nexthop(dstip), pseudo + 0) != 0)
    return -1; /* unreachable (ARP failed) */
  f = pkt_irq_save();
  ulen = 8 + len;
  pkt_put16(g_tx + 0, g_u[s].port);
  pkt_put16(g_tx + 2, dstport);
  pkt_put16(g_tx + 4, (uint16_t)ulen);
  pkt_put16(g_tx + 6, 0);
  if (len) naos_memcpy(g_tx + 8, data, len);
  /* Checksum over pseudo-header + segment. */
  pkt_put32(pseudo + 0, srcip ? srcip : net_ip());
  pkt_put32(pseudo + 4, dstip);
  pseudo[8] = 0;
  pseudo[9] = 17;
  pkt_put16(pseudo + 10, (uint16_t)ulen);
  sum = pkt_onesum(pseudo, 12) + pkt_onesum(g_tx, ulen);
  {
    uint16_t c = pkt_fold(sum);
    if (c == 0) c = 0xFFFF; /* 0 means "no checksum" */
    pkt_put16(g_tx + 6, c);
  }
  r = net_send_ip(srcip ? srcip : net_ip(), dstip, 17, g_tx, ulen);
  pkt_irq_restore(f);
  return r;
}

void udp_on_dgram(uint32_t srcip, const uint8_t *d, uint32_t len) {
  uint16_t sport, dport, ulen;
  int slot;
  if (len < 8) return;
  sport = pkt_get16(d + 0);
  dport = pkt_get16(d + 2);
  ulen = pkt_get16(d + 4);
  if (ulen < 8 || ulen > len) return;
  slot = -1;
  for (int i = 0; i < UDP_N; i++) {
    if (g_u[i].used && g_u[i].port == dport) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return;
  if (g_u[slot].count >= UDP_Q) return; /* full: drop */
  {
    int w = (g_u[slot].head + g_u[slot].count) % UDP_Q;
    uint32_t n = ulen;
    if (n > UDP_CAP) n = UDP_CAP;
    naos_memcpy(g_u[slot].pkt[w], d, n);
    g_u[slot].plen[w] = n;
    g_u[slot].sip[w] = srcip;
    g_u[slot].sport[w] = sport;
    g_u[slot].count++;
  }
}

/* Strip the 8B UDP header: recvfrom yields payload only. */
static uint32_t udp_payload_len(const uint8_t *d, uint32_t total) {
  uint32_t ulen;
  if (total < 8) return 0;
  ulen = (uint32_t)pkt_get16(d + 4);
  if (ulen < 8 || ulen > total) ulen = total;
  return ulen - 8;
}

int udp_recvfrom(int s, void *buf, uint32_t cap, uint32_t *srcip,
                 uint16_t *srcport, uint64_t timeout_ticks) {
  uint64_t t0;
  if (s < 0 || s >= UDP_N || !g_u[s].used) return -1;
  t0 = pit_ticks();
  for (;;) {
    uint64_t f = pkt_irq_save();
    int has = g_u[s].count > 0;
    pkt_irq_restore(f);
    if (has) break;
    if (pit_ticks() - t0 >= timeout_ticks) return -1;
    net_poll();
  }
  {
    uint64_t f = pkt_irq_save();
    int r = g_u[s].head;
    uint32_t total = g_u[s].plen[r];
    uint32_t off = 8; /* skip UDP header */
    uint32_t n = udp_payload_len(g_u[s].pkt[r], total);
    if (n > cap) n = cap;
    naos_memcpy(buf, g_u[s].pkt[r] + off, n);
    if (srcip) *srcip = g_u[s].sip[r];
    if (srcport) *srcport = g_u[s].sport[r];
    g_u[s].head = (r + 1) % UDP_Q;
    g_u[s].count--;
    pkt_irq_restore(f);
    return (int)n;
  }
}
