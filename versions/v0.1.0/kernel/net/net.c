/* NaOS network stack. Static QEMU user-net config (10.0.2.15/24 via
 * 10.0.2.2); ARP cache, ICMP echo in/out. Polled from the timer tick. */
#include "net.h"
#include "pkt.h"
#include "udp.h"
#include "tcp.h"
#include "../drv/e1000.h"
#include "../drv/pit.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

/* Defaults: QEMU user-net. DHCP overwrites at boot when available. */
static uint32_t g_ip = 0x0A00020Fu;   /* 10.0.2.15 */
static uint32_t g_mask = 0xFFFFFF00u;
static uint32_t g_gw = 0x0A000202u;   /* 10.0.2.2 */
static uint32_t g_dns = 0x0A000203u;

#define ETH_IP 0x0800u
#define ETH_ARP 0x0806u
#define IP_ICMP 1u
#define IP_TCP 6u
#define IP_UDP 17u
#define ARP_REQ 1u
#define ARP_REP 2u
#define ICMP_REQ 8u
#define ICMP_REP 0u

static int g_ok;
static uint8_t g_mac[6];

typedef struct {
  uint32_t ip; /* 0 = free */
  uint8_t mac[6];
} arp_ent_t;

static arp_ent_t g_arp[8];

/* Pending ping reply match. */
static uint16_t g_ping_ident;
static uint16_t g_ping_seq;
static int g_ping_got;
static uint64_t g_ping_t0;

static uint16_t cksum(const void *data, uint32_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t s = 0;
  while (len > 1) {
    s += ((uint32_t)p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if (len) s += (uint32_t)p[0] << 8;
  while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
  return (uint16_t)~s;
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
  uint32_t i, free = 8;
  if (ip == 0) return;
  for (i = 0; i < 8; i++) {
    if (g_arp[i].ip == ip) break;
    if (g_arp[i].ip == 0 && free == 8) free = i;
  }
  if (i < 8) {
    for (int k = 0; k < 6; k++) g_arp[i].mac[k] = mac[k];
    return;
  }
  if (free < 8) {
    g_arp[free].ip = ip;
    for (int k = 0; k < 6; k++) g_arp[free].mac[k] = mac[k];
  }
}

static int arp_lookup(uint32_t ip, uint8_t mac[6]) {
  for (uint32_t i = 0; i < 8; i++) {
    if (g_arp[i].ip == ip) {
      for (int k = 0; k < 6; k++) mac[k] = g_arp[i].mac[k];
      return 0;
    }
  }
  return -1;
}

/* Ethernet header (14B) + payload. dst NULL = broadcast. */
static int eth_send(const uint8_t *dst, uint16_t type, const void *pl,
                    uint32_t plen) {
  static uint8_t frame[2048];
  if (!g_ok || plen + 14 > sizeof(frame)) return -1;
  if (dst) {
    for (int i = 0; i < 6; i++) frame[i] = dst[i];
  } else {
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
  }
  for (int i = 0; i < 6; i++) frame[6 + i] = g_mac[i];
  put16(frame + 12, type);
  if (plen) naos_memcpy(frame + 14, pl, plen);
  return e1000_send(frame, plen + 14);
}

static int arp_send(uint16_t oper, const uint8_t *tha, uint32_t tpa) {
  uint8_t p[28];
  const uint8_t *dst = (oper == ARP_REQ) ? 0 : tha;
  uint64_t f = pkt_irq_save();
  int r;
  put16(p + 0, 1);          /* ethernet */
  put16(p + 2, ETH_IP);
  p[4] = 6;
  p[5] = 4;
  put16(p + 6, oper);
  for (int i = 0; i < 6; i++) p[8 + i] = g_mac[i];
  put32(p + 14, g_ip);
  for (int i = 0; i < 6; i++) p[18 + i] = (oper == ARP_REQ) ? 0 : tha[i];
  put32(p + 24, tpa);
  r = eth_send(dst, ETH_ARP, p, sizeof(p));
  pkt_irq_restore(f);
  return r;
}

/* Resolve an IP to MAC (ARP request ×3, ~2s). 0 ok. */
static int arp_resolve(uint32_t ip, uint8_t mac[6]) {
  if (arp_lookup(ip, mac) == 0) return 0;
  for (int tries = 0; tries < 3; tries++) {
    uint64_t t0;
    if (arp_send(ARP_REQ, 0, ip) != 0) return -1;
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 70) { /* ~0.7s */
      net_poll();
      if (arp_lookup(ip, mac) == 0) return 0;
    }
  }
  return -1;
}

/* Build + send an ICMP echo (request or reply). */
static int icmp_send(const uint8_t *dstmac, uint32_t dstip, uint8_t type,
                     uint16_t ident, uint16_t seq, const void *data,
                     uint32_t dlen) {
  static uint8_t pkt[128];
  uint32_t ilen = 8 + dlen;
  uint32_t iplen = 20 + ilen;
  uint64_t f;
  int r;
  if (iplen + 14 > sizeof(pkt)) return -1;
  f = pkt_irq_save();
  /* IP header. */
  pkt[0] = 0x45;
  pkt[1] = 0;
  put16(pkt + 2, (uint16_t)iplen);
  put16(pkt + 4, 0);
  put16(pkt + 6, 0x4000u); /* don't fragment */
  pkt[8] = 64;             /* TTL */
  pkt[9] = IP_ICMP;
  put16(pkt + 10, 0);
  put32(pkt + 12, g_ip);
  put32(pkt + 16, dstip);
  put16(pkt + 10, cksum(pkt, 20));
  /* ICMP. */
  pkt[20] = type;
  pkt[21] = 0;
  put16(pkt + 22, 0);
  put16(pkt + 24, ident);
  put16(pkt + 26, seq);
  if (dlen) naos_memcpy(pkt + 28, data, dlen);
  put16(pkt + 22, cksum(pkt + 20, ilen));
  r = eth_send(dstmac, ETH_IP, pkt, iplen);
  pkt_irq_restore(f);
  return r;
}

static void on_arp(const uint8_t *frame, uint32_t len) {
  const uint8_t *p;
  uint16_t oper;
  uint32_t spa, tpa;
  if (len < 14 + 28) return;
  p = frame + 14;
  if (get16(p + 0) != 1 || get16(p + 2) != ETH_IP) return;
  if (p[4] != 6 || p[5] != 4) return;
  oper = get16(p + 6);
  spa = get32(p + 14);
  tpa = get32(p + 24);
  arp_learn(spa, p + 8); /* every ARP teaches us something */
  if (oper == ARP_REQ && tpa == g_ip)
    arp_send(ARP_REP, p + 8, spa); /* that's us: answer */
}

static void on_ip(const uint8_t *frame, uint32_t len) {
  const uint8_t *p;
  uint32_t iplen, src, dst, hlen;
  uint8_t proto;
  if (len < 14 + 20) return;
  p = frame + 14;
  if ((p[0] >> 4) != 4 || (p[0] & 0x0F) < 5) return;
  hlen = (uint32_t)(p[0] & 0x0F) * 4;
  proto = p[9];
  iplen = get16(p + 2);
  if (iplen + 14 > len || iplen < hlen + 8) return;
  src = get32(p + 12);
  dst = get32(p + 16);
  arp_learn(src, frame + 6); /* every IP packet teaches us something */
  if (dst != g_ip && dst != 0xFFFFFFFFu) return; /* not for us */
  if (proto == IP_TCP) {
    tcp_on_seg(src, p + hlen, iplen - hlen);
    return;
  }
  if (proto == IP_UDP) {
    udp_on_dgram(src, p + hlen, iplen - hlen);
    return;
  }
  if (proto != IP_ICMP) return;
  if (iplen < 28) return;
  {
    const uint8_t *ic = p + 20;
    uint32_t ilen = iplen - 20;
    if (ic[0] == ICMP_REQ) {
      /* Echo reply with identical payload. */
      icmp_send(frame + 6, src, ICMP_REP, get16(ic + 4), get16(ic + 6),
                ic + 8, ilen > 8 ? ilen - 8 : 0);
    } else if (ic[0] == ICMP_REP && get16(ic + 4) == g_ping_ident &&
               get16(ic + 6) == g_ping_seq) {
      g_ping_got = 1;
    }
  }
}

void net_poll(void) {
  static uint8_t buf[2048];
  uint32_t len;
  if (!g_ok) return;
  for (;;) {
    len = e1000_recv(buf, sizeof(buf));
    if (len == 0) break;
    if (len < 14) continue;
    {
      uint16_t type = get16(buf + 12);
      if (type == ETH_ARP) on_arp(buf, len);
      else if (type == ETH_IP) on_ip(buf, len);
    }
  }
}

static void ip_str(uint32_t ip, char *out) {
  int n = 0;
  for (int i = 3; i >= 0; i--) {
    unsigned int b = (ip >> (i * 8)) & 0xFFu;
    char tmp[4];
    int m = 0;
    if (b >= 100) tmp[m++] = (char)('0' + b / 100);
    if (b >= 10) tmp[m++] = (char)('0' + (b / 10) % 10);
    tmp[m++] = (char)('0' + b % 10);
    for (int k = 0; k < m; k++) out[n++] = tmp[k];
    if (i > 0) out[n++] = '.';
  }
  out[n] = '\0';
}

int net_ping(uint32_t ip) {
  uint8_t mac[6];
  uint8_t data[32];
  int got = 0;
  char ips[20];
  if (!g_ok) {
    kprintf("ping: no network\n");
    return 0;
  }
  ip_str(ip, ips);
  if (arp_resolve(ip, mac) != 0) {
    kprintf("ping %s: host unreachable (no ARP)\n", ips);
    return 0;
  }
  g_ping_ident = (uint16_t)(pit_ticks() & 0xFFFFu);
  for (int i = 0; i < 4; i++) {
    uint64_t t0;
    for (uint32_t k = 0; k < sizeof(data); k++) data[k] = (uint8_t)k;
    g_ping_seq = (uint16_t)i;
    g_ping_got = 0;
    g_ping_t0 = pit_ticks();
    if (icmp_send(mac, ip, ICMP_REQ, g_ping_ident, (uint16_t)i, data,
                  sizeof(data)) != 0) {
      kprintf("ping %s: send failed\n", ips);
      break;
    }
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 100 && !g_ping_got) net_poll();
    if (g_ping_got) {
      got++;
      kprintf("ping %s: seq=%d time=%ums\n", ips, i,
              (unsigned)((pit_ticks() - g_ping_t0) * 10u));
    } else {
      kprintf("ping %s: seq=%d timeout\n", ips, i);
    }
  }
  kprintf("ping %s: %d/4 replies\n", ips, got);
  return got;
}

void net_status(void) {
  char ips[20], msk[20], gw[20], dns[20];
  if (!g_ok) {
    kprintf("net: down (no NIC)\n");
    return;
  }
  ip_str(g_ip, ips);
  ip_str(g_mask, msk);
  ip_str(g_gw, gw);
  ip_str(g_dns, dns);
  kprintf("net: mac %x:%x:%x:%x:%x:%x ip %s mask %s gw %s dns %s link=%d tx=%u rx=%u\n",
          (unsigned)g_mac[0], (unsigned)g_mac[1], (unsigned)g_mac[2],
          (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5], ips,
          msk, gw, dns, e1000_link(), (unsigned)e1000_tx_count(),
          (unsigned)e1000_rx_count());
}

void net_init(void) {
  char ips[20];
  g_ok = 0;
  for (int i = 0; i < 8; i++) g_arp[i].ip = 0;
  if (!e1000_init()) return;
  e1000_mac(g_mac);
  g_ok = 1;
  ip_str(g_ip, ips);
  kprintf("net: up (static %s/24)\n", ips);
}

/* ---- shared layer for UDP/TCP/DHCP ---- */

void net_set_addrs(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
  g_ip = ip;
  g_mask = mask;
  g_gw = gw;
  g_dns = dns;
}

uint32_t net_ip(void) { return g_ip; }
uint32_t net_mask(void) { return g_mask; }
uint32_t net_gw(void) { return g_gw; }
uint32_t net_dns(void) { return g_dns; }
const uint8_t *net_mac(void) { return g_mac; }

void net_ip_str(uint32_t ip, char *out) { ip_str(ip, out); }

uint32_t net_nexthop(uint32_t dst) {
  if (dst == 0xFFFFFFFFu) return dst;
  if (((dst ^ g_ip) & g_mask) == 0) return dst; /* same subnet */
  return g_gw;
}

int net_resolve(uint32_t ip, uint8_t mac[6]) {
  return arp_resolve(ip, mac);
}

/* Send with explicit dst MAC (NULL = broadcast). Caller IRQ-guards. */
int net_send_ip_mac(const uint8_t *mac, uint32_t src, uint32_t dst,
                    uint8_t proto, const void *l4, uint32_t l4len) {
  static uint8_t pkt[1600];
  uint32_t iplen = 20 + l4len;
  if (!g_ok || iplen + 14 > sizeof(pkt)) return -1;
  pkt[0] = 0x45;
  pkt[1] = 0;
  put16(pkt + 2, (uint16_t)iplen);
  put16(pkt + 4, 0);
  put16(pkt + 6, 0x4000u);
  pkt[8] = 64;
  pkt[9] = proto;
  put16(pkt + 10, 0);
  put32(pkt + 12, src);
  put32(pkt + 16, dst);
  put16(pkt + 10, cksum(pkt, 20));
  if (l4len) naos_memcpy(pkt + 20, l4, l4len);
  return eth_send(mac, ETH_IP, pkt, iplen);
}

/* Send with ARP lookup of the next hop (broadcast needs no ARP). */
int net_send_ip(uint32_t src, uint32_t dst, uint8_t proto, const void *l4,
                uint32_t l4len) {
  uint8_t mac[6];
  if (dst == 0xFFFFFFFFu)
    return net_send_ip_mac(0, src, dst, proto, l4, l4len);
  if (arp_lookup(net_nexthop(dst), mac) != 0) return -1;
  return net_send_ip_mac(mac, src, dst, proto, l4, l4len);
}

int net_parse_ip(const char *s, uint32_t *out) {
  uint32_t parts[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; i++) {
    int digits = 0;
    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') {
      parts[i] = parts[i] * 10 + (uint32_t)(*s - '0');
      if (parts[i] > 255) return -1;
      s++;
      digits++;
    }
    if (digits == 0) return -1;
    if (i < 3) {
      if (*s != '.') return -1;
      s++;
    }
  }
  if (*s != '\0') return -1;
  *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  return 0;
}
