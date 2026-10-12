/* NaOS DNS: minimal recursive-query client (A records, CNAME followed).
 * One question per packet; answers parsed with compression-pointer safety
 * (bounded jumps, strict length checks — hostile servers must not crash us). */
#include "dns.h"
#include "pkt.h"
#include "net.h"
#include "udp.h"
#include "../drv/pit.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define DNS_PORT 53
#define DNS_TRIES 3
#define DNS_WAIT 100 /* ticks per try */

/* Skip a possibly-compressed name at d+*off (updates off past it).
 * Returns 0 ok. Never reads past len. */
static int skip_name(const uint8_t *d, uint32_t len, uint32_t *off) {
  uint32_t o = *off;
  int jumps = 0;
  for (;;) {
    uint8_t n;
    if (o >= len) return -1;
    n = d[o];
    if ((n & 0xC0u) == 0xC0u) {
      uint32_t ptr;
      if (o + 1 >= len) return -1;
      ptr = ((uint32_t)(n & 0x3Fu) << 8) | d[o + 1];
      if (ptr >= len) return -1;
      if (jumps == 0) *off = o + 2; /* first jump: consume pointer bytes */
      if (++jumps > 16) return -1;  /* compression loop guard */
      o = ptr;
      continue;
    }
    if (n == 0) {
      if (jumps == 0) *off = o + 1;
      return 0;
    }
    if (n > 63 || o + 1u + n > len) return -1;
    if (jumps == 0) o += 1u + n;
    else o += 1u + n; /* inside pointed data: just walk */
  }
}

/* Encode "a.b.c" into DNS labels at out. Returns label-bytes or -1. */
static int enc_name(const char *s, uint8_t *out) {
  int o = 0, dots = 0;
  const char *label = s;
  for (;;) {
    const char *dot = label;
    int n = 0;
    while (*dot && *dot != '.') {
      dot++;
      n++;
    }
    if (n == 0 || n > 63 || o + 1 + n > 253) return -1;
    out[o++] = (uint8_t)n;
    for (int i = 0; i < n; i++) out[o++] = (uint8_t)label[i];
    if (!*dot) break;
    dots++;
    label = dot + 1;
    if (dots > 16) return -1;
  }
  out[o++] = 0;
  return o;
}

/* Look for the first A record for target in one reply. Follows one CNAME
 * chain (re-query) up to depth. 0 = found (out set). */
static int dns_query(const char *name, uint32_t *out, int depth) {
  static uint8_t q[300];
  static uint8_t r[1536];
  uint16_t txid;
  int s, qlen, nlen;
  uint32_t sip;
  uint16_t sport;
  if (depth > 3) return -1;
  nlen = enc_name(name, q + 12);
  if (nlen < 0) return -1;
  if (12 + (uint32_t)nlen + 4 > sizeof(q)) return -1;
  txid = (uint16_t)((pit_ticks() * 2654435761u + (uint32_t)depth) & 0xFFFFu);
  pkt_put16(q + 0, txid);
  pkt_put16(q + 2, 0x0100u); /* RD */
  pkt_put16(q + 4, 1);
  pkt_put16(q + 6, 0);
  pkt_put16(q + 8, 0);
  pkt_put16(q + 10, 0);
  pkt_put16(q + 12 + nlen, 1); /* A */
  pkt_put16(q + 12 + nlen + 2, 1); /* IN */
  qlen = 12 + nlen + 4;
  s = udp_open(0);
  if (s < 0) return -1;
  {
    int got = -1;
    for (int t = 0; t < DNS_TRIES && got != 0; t++) {
      int rl;
      uint16_t flags, qd, an;
      uint32_t o;
      if (udp_sendto(s, net_ip(), net_dns(), DNS_PORT, q, (uint32_t)qlen) !=
          0)
        continue;
      rl = udp_recvfrom(s, r, sizeof(r), &sip, &sport, DNS_WAIT);
      if (rl < 12) continue;
      if (pkt_get16(r + 0) != txid) continue; /* not ours */
      flags = pkt_get16(r + 2);
      if (!(flags & 0x8000u)) continue;      /* not a reply */
      if ((flags & 0x000Fu) != 0) continue;  /* server says no */
      qd = pkt_get16(r + 4);
      an = pkt_get16(r + 6);
      o = 12;
      for (uint16_t i = 0; i < qd; i++) { /* skip questions */
        if (skip_name(r, (uint32_t)rl, &o) != 0 || o + 4 > (uint32_t)rl)
          break;
        o += 4;
      }
      for (uint16_t i = 0; i < an; i++) { /* scan answers */
        uint16_t type, cls, rdlen;
        if (skip_name(r, (uint32_t)rl, &o) != 0 || o + 10 > (uint32_t)rl)
          break;
        type = pkt_get16(r + o);
        cls = pkt_get16(r + o + 2);
        rdlen = pkt_get16(r + o + 8);
        o += 10;
        if (o + rdlen > (uint32_t)rl) break;
        if (type == 1 && cls == 1 && rdlen == 4) {
          *out = pkt_get32(r + o);
          got = 0;
          break;
        }
        /* CNAME (5): re-query its target (compressed names ok to skip). */
        if (type == 5 && cls == 1 && depth < 3) {
          uint32_t co = o;
          char cname[256];
          int cn = 0;
          /* Decode target into dotted text (bounded). */
          int jumps = 0;
          uint32_t p = co;
          uint32_t end = co + rdlen;
          for (;;) {
            uint8_t n;
            if (p >= end || p >= (uint32_t)rl) break;
            n = r[p];
            if ((n & 0xC0u) == 0xC0u) {
              if (p + 1 >= (uint32_t)rl) break;
              p = ((uint32_t)(n & 0x3Fu) << 8) | r[p + 1];
              end = (uint32_t)rl;
              if (++jumps > 16) break;
              continue;
            }
            if (n == 0) break;
            if (n > 63 || p + 1u + n > end || cn + (int)n + 2 >= 256) break;
            if (cn > 0) cname[cn++] = '.';
            for (uint32_t k = 0; k < n; k++) cname[cn++] = (char)r[p + 1 + k];
            p += 1u + n;
          }
          cname[cn] = '\0';
          if (cn > 0) {
            udp_close(s);
            return dns_query(cname, out, depth + 1);
          }
        }
        o += rdlen;
      }
    }
    udp_close(s);
    return got;
  }
}

int dns_resolve(const char *name, uint32_t *out) {
  char host[254];
  int n = 0;
  uint32_t ip;
  if (!name || !out) return -1;
  /* IP literal? No DNS needed. Trailing dot tolerated. */
  while (name[n] && n < 253) {
    host[n] = name[n];
    n++;
  }
  host[n] = '\0';
  if (n > 0 && host[n - 1] == '.') host[n - 1] = '\0';
  if (net_parse_ip(host, &ip) == 0) {
    *out = ip;
    return 0;
  }
  if (n == 0) return -1;
  return dns_query(host, out, 0);
}
