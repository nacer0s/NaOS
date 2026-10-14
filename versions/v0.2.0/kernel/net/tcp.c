/* NaOS TCP client. All segment sends are IRQ-guarded (tick RX can fire
 * mid-send from command context). RX only appends in-order bytes and
 * sends ACKs; waits/retransmits happen in caller context. */
#include "tcp.h"
#include "pkt.h"
#include "net.h"
#include "../drv/pit.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define TCP_N 2
#define TCP_RXB 4096
#define TCP_MSS 1460
#define TCP_RTO 100 /* 1s @100Hz */
#define TCP_TRIES 4

#define TF_FIN 0x01u
#define TF_SYN 0x02u
#define TF_RST 0x04u
#define TF_PSH 0x08u
#define TF_ACK 0x10u

#define TS_SYN_SENT 1
#define TS_EST 2
#define TS_FIN_WAIT 3
#define TS_CLOSE_WAIT 4
#define TS_LAST_ACK 5

typedef struct {
  int used;
  int state;
  uint32_t lip, rip;
  uint16_t lport, rport;
  uint8_t rmac[6];
  uint32_t snd_una, snd_nxt, rcv_nxt;
  uint16_t mss;
  uint8_t rx[TCP_RXB];
  uint32_t rx_len;
  uint8_t tx[64 + TCP_MSS];
  uint8_t oseg[TCP_MSS];
  uint32_t oseg_seq, oseg_len;
  uint64_t oseg_t0;
  int oseg_fin;
} tcp_t;

static tcp_t g_t[TCP_N];
static uint16_t g_next_port = 50000;

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

/* Build + send one segment (seq given). Caller holds IRQ guard. */
static int seg_send(tcp_t *c, uint32_t seq, uint8_t flags, const void *data,
                    uint32_t dlen) {
  uint8_t pseudo[12];
  uint32_t tlen = 20 + dlen;
  uint32_t sum;
  uint32_t win = TCP_RXB - c->rx_len;
  if (dlen > TCP_MSS || tlen + 20 + 14 > sizeof(c->tx)) return -1;
  pkt_put16(c->tx + 0, c->lport);
  pkt_put16(c->tx + 2, c->rport);
  pkt_put32(c->tx + 4, seq);
  pkt_put32(c->tx + 8, c->rcv_nxt);
  c->tx[12] = (5 << 4);
  c->tx[13] = flags;
  pkt_put16(c->tx + 14, (uint16_t)win);
  pkt_put16(c->tx + 16, 0);
  pkt_put16(c->tx + 18, 0);
  if (dlen) naos_memcpy(c->tx + 20, data, dlen);
  pkt_put32(pseudo + 0, c->lip);
  pkt_put32(pseudo + 4, c->rip);
  pseudo[8] = 0;
  pseudo[9] = 6;
  pkt_put16(pseudo + 10, (uint16_t)tlen);
  sum = pkt_onesum(pseudo, 12) + pkt_onesum(c->tx, tlen);
  pkt_put16(c->tx + 16, pkt_fold(sum));
  return net_send_ip_mac(c->rmac, c->lip, c->rip, 6, c->tx, tlen);
}

static int tcb_alloc(void) {
  for (int i = 0; i < TCP_N; i++) {
    if (!g_t[i].used) {
      naos_memset(&g_t[i], 0, sizeof(g_t[i]));
      g_t[i].used = 1;
      g_t[i].mss = TCP_MSS;
      return i;
    }
  }
  return -1;
}

int tcp_connect(uint32_t ip, uint16_t port) {
  int t = tcb_alloc();
  tcp_t *c;
  uint64_t f;
  if (t < 0) return -1;
  c = &g_t[t];
  c->lip = net_ip();
  c->rip = ip;
  c->rport = port;
  c->lport = g_next_port++;
  if (g_next_port < 50000) g_next_port = 50000;
  if (net_resolve(net_nexthop(ip), c->rmac) != 0) {
    c->used = 0;
    return -1;
  }
  {
    uint32_t iss =
        (uint32_t)((pit_ticks() * 2654435761u) ^ 0x7C9E4501u ^ (t * 97u));
    /* SYN ×TRIES. */
    for (int i = 0; i < TCP_TRIES; i++) {
      uint64_t t0;
      c->state = TS_SYN_SENT;
      c->snd_una = iss;
      c->snd_nxt = iss + 1;
      c->rcv_nxt = 0;
      f = pkt_irq_save();
      seg_send(c, iss, TF_SYN, 0, 0);
      pkt_irq_restore(f);
      t0 = pit_ticks();
      while (pit_ticks() - t0 < TCP_RTO) {
        net_poll();
        if (!c->used) return -1;
        if (c->state == TS_EST) return t;
      }
      if (!c->used) return -1;
    }
    c->used = 0;
    return -1;
  }
}

int tcp_send(int t, const void *buf, uint32_t len) {
  tcp_t *c;
  uint64_t f, t0;
  if (t < 0 || t >= TCP_N || !g_t[t].used) return -1;
  c = &g_t[t];
  if (c->state != TS_EST) return -1;
  if (len == 0) return 0;
  if (len > c->mss) len = c->mss;
  naos_memcpy(c->oseg, buf, len);
  c->oseg_seq = c->snd_nxt;
  c->oseg_len = (uint32_t)len;
  c->oseg_fin = 0;
  c->snd_nxt += (uint32_t)len; /* sent bytes are "on the wire" now */
  for (int i = 0; i < TCP_TRIES; i++) {
    f = pkt_irq_save();
    seg_send(c, c->oseg_seq, TF_PSH | TF_ACK, c->oseg, c->oseg_len);
    pkt_irq_restore(f);
    c->oseg_t0 = pit_ticks();
    t0 = pit_ticks();
    while (pit_ticks() - t0 < TCP_RTO) {
      int done;
      net_poll();
      if (!c->used || c->state != TS_EST) return -1;
      f = pkt_irq_save();
      done = (c->oseg_len == 0);
      pkt_irq_restore(f);
      if (done) return (int)len;
    }
  }
  return -1;
}

int tcp_recv(int t, void *buf, uint32_t cap, uint64_t timeout_ticks) {
  tcp_t *c;
  uint64_t t0 = pit_ticks();
  if (t < 0 || t >= TCP_N || !g_t[t].used) return -1;
  c = &g_t[t];
  for (;;) {
    uint64_t f = pkt_irq_save();
    uint32_t n = c->rx_len;
    pkt_irq_restore(f);
    if (n > 0) {
      if (n > cap) n = cap;
      f = pkt_irq_save();
      naos_memcpy(buf, c->rx, n);
      naos_memmove(c->rx, c->rx + n, c->rx_len - n);
      c->rx_len -= n;
      pkt_irq_restore(f);
      return (int)n;
    }
    if (!c->used) return -1;
    if (c->state == TS_CLOSE_WAIT || c->state == TS_LAST_ACK) return 0;
    if (pit_ticks() - t0 >= timeout_ticks) {
      /* Closed by peer with nothing left? EOF, else timeout. */
      return (c->state == TS_EST) ? -1 : 0;
    }
    net_poll();
  }
}

int tcp_close(int t) {
  tcp_t *c;
  uint64_t f, t0;
  if (t < 0 || t >= TCP_N || !g_t[t].used) return -1;
  c = &g_t[t];
  if (c->state == TS_EST) {
    f = pkt_irq_save();
    seg_send(c, c->snd_nxt, TF_FIN | TF_ACK, 0, 0);
    pkt_irq_restore(f);
    c->snd_nxt++;
    c->state = TS_FIN_WAIT;
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 150 && c->used && c->state == TS_FIN_WAIT)
      net_poll();
  } else if (c->state == TS_CLOSE_WAIT) {
    f = pkt_irq_save();
    seg_send(c, c->snd_nxt, TF_FIN | TF_ACK, 0, 0);
    pkt_irq_restore(f);
    c->snd_nxt++;
    c->state = TS_LAST_ACK;
    t0 = pit_ticks();
    /* Peer FIN already seen: done as soon as our FIN is acked. */
    while (pit_ticks() - t0 < 150 && c->used && c->state == TS_LAST_ACK &&
           c->snd_una != c->snd_nxt)
      net_poll();
  }
  c->used = 0;
  c->state = 0;
  return 0;
}

void tcp_on_seg(uint32_t srcip, const uint8_t *seg, uint32_t len) {
  uint16_t sport, dport, win;
  uint32_t seq, ack;
  uint8_t off, flags;
  uint32_t hlen, dlen;
  tcp_t *c = 0;
  uint64_t f;
  if (len < 20) return;
  sport = pkt_get16(seg + 0);
  dport = pkt_get16(seg + 2);
  seq = pkt_get32(seg + 4);
  ack = pkt_get32(seg + 8);
  off = seg[12] >> 4;
  flags = seg[13];
  win = pkt_get16(seg + 14);
  (void)win;
  hlen = (uint32_t)off * 4;
  if (hlen < 20 || hlen > len) return;
  dlen = len - hlen;
  for (int i = 0; i < TCP_N; i++) {
    if (g_t[i].used && g_t[i].lport == dport && g_t[i].rport == sport &&
        g_t[i].rip == srcip) {
      c = &g_t[i];
      break;
    }
  }
  if (!c) {
    /* Unknown port: RST. (Rare; keep quiet to avoid storms.) */
    return;
  }
  if (flags & TF_RST) {
    c->used = 0;
    c->state = 0;
    return;
  }
  f = pkt_irq_save();
  /* ACK processing: advance snd_una if it covers sent bytes. */
  if ((flags & TF_ACK) && c->state != TS_SYN_SENT) {
    if (!seq_lt(ack, c->snd_una) && !seq_lt(c->snd_nxt, ack)) {
      c->snd_una = ack;
      if (c->oseg_len > 0 && !seq_lt(ack, c->oseg_seq + c->oseg_len))
        c->oseg_len = 0;
    }
  }
  switch (c->state) {
  case TS_SYN_SENT:
    if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK) &&
        ack == c->snd_nxt) {
      c->rcv_nxt = seq + 1;
      c->snd_una = ack;
      /* Peer MSS option? scan for kind 2. */
      if (hlen > 20) {
        uint32_t o = 20;
        while (o < hlen) {
          uint8_t k = seg[o];
          uint8_t l;
          if (k == 0) break;
          if (k == 1) {
            o++;
            continue;
          }
          if (o + 1 >= hlen) break;
          l = seg[o + 1];
          if (l < 2 || o + l > hlen) break;
          if (k == 2 && l == 4) {
            uint16_t m = pkt_get16(seg + o + 2);
            if (m < 536) m = 536;
            if (m > TCP_MSS) m = TCP_MSS;
            c->mss = m;
          }
          o += l;
        }
      }
      c->state = TS_EST;
      seg_send(c, c->snd_nxt, TF_ACK, 0, 0);
    }
    break;
  case TS_EST:
  case TS_FIN_WAIT:
    if (dlen > 0 && c->rx_len < TCP_RXB) {
      if (seq == c->rcv_nxt) {
        uint32_t n = dlen;
        uint32_t free = TCP_RXB - c->rx_len;
        if (n > free) n = free;
        naos_memcpy(c->rx + c->rx_len, seg + hlen, n);
        c->rx_len += n;
        c->rcv_nxt += n;
      }
      /* Else out of order: drop, dup-ACK below asks for resend. */
    }
    if (flags & TF_FIN) {
      if (seq + dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        seg_send(c, c->snd_nxt, TF_ACK, 0, 0);
        if (c->state == TS_EST) c->state = TS_CLOSE_WAIT;
        else {
          c->used = 0;
          c->state = 0;
        }
        break;
      }
    }
    if (dlen > 0 || (flags & TF_FIN))
      seg_send(c, c->snd_nxt, TF_ACK, 0, 0);
    break;
  case TS_CLOSE_WAIT:
  case TS_LAST_ACK:
    if ((flags & TF_FIN) && seq == c->rcv_nxt) {
      c->rcv_nxt++;
      seg_send(c, c->snd_nxt, TF_ACK, 0, 0);
      if (c->state == TS_LAST_ACK) {
        c->used = 0;
        c->state = 0;
      }
    } else if (dlen > 0 || (flags & TF_FIN)) {
      seg_send(c, c->snd_nxt, TF_ACK, 0, 0);
    }
    break;
  default:
    break;
  }
  pkt_irq_restore(f);
}

int net_fetch(uint32_t ip, uint16_t port, const char *path) {
  static char req[160];
  static char chunk[512 + 1];
  static uint8_t hdr[2048];
  int t, rl = 0, pl = 0;
  uint32_t hlen = 0, total = 0;
  int in_body = 0;
  if (!path || path[0] != '/') {
    kprintf("usage: fetch <a.b.c.d> <port> </path>\n");
    return -1;
  }
  while (path[pl] && pl < 64) pl++;
  if (path[pl] != '\0') {
    kprintf("fetch: path too long\n");
    return -1;
  }
  t = tcp_connect(ip, port);
  if (t < 0) {
    kprintf("fetch: connect failed\n");
    return -1;
  }
  naos_memcpy(req + rl, "GET ", 4);
  rl += 4;
  naos_memcpy(req + rl, path, (uint32_t)pl);
  rl += pl;
  {
    const char *tail =
        " HTTP/1.0\r\nHost: naos\r\nConnection: close\r\n\r\n";
    uint32_t tl = (uint32_t)naos_strlen(tail);
    naos_memcpy(req + rl, tail, tl);
    rl += (int)tl;
  }
  if (tcp_send(t, req, (uint32_t)rl) < 0) {
    kprintf("fetch: send failed\n");
    tcp_close(t);
    return -1;
  }
  for (;;) {
    int r = tcp_recv(t, chunk, 512, 300);
    if (r < 0) {
      kprintf("fetch: timeout\n");
      break;
    }
    if (r == 0) break; /* server closed */
    if (!in_body) {
      uint32_t j;
      int found = -1;
      if (hlen + (uint32_t)r > sizeof(hdr)) {
        kprintf("fetch: header too big\n");
        break;
      }
      naos_memcpy(hdr + hlen, chunk, (uint32_t)r);
      hlen += (uint32_t)r;
      for (j = 0; j + 4 <= hlen; j++) {
        if (hdr[j] == '\r' && hdr[j + 1] == '\n' && hdr[j + 2] == '\r' &&
            hdr[j + 3] == '\n') {
          found = (int)j;
          break;
        }
      }
      if (found >= 0) {
        uint32_t b = (uint32_t)found + 4;
        while (b < hlen) {
          uint32_t n = hlen - b;
          if (n > 512) n = 512;
          naos_memcpy(chunk, hdr + b, n);
          chunk[n] = '\0';
          kprintf("%s", chunk);
          total += n;
          b += n;
        }
        in_body = 1;
      }
    } else {
      chunk[r] = '\0';
      kprintf("%s", chunk);
      total += (uint32_t)r;
      if (total > 65536) {
        kprintf("fetch: too big, cut\n");
        break;
      }
    }
  }
  tcp_close(t);
  kprintf("fetch: %u body bytes\n", (unsigned)total);
  return 0;
}
