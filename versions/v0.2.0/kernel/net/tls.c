/* NaOS TLS 1.2 client: ECDHE_RSA_AES_128_GCM_SHA256 (0xC02F) only.
 * Own record layer + handshake + GCM protect, on top of tcp_connect.
 * Verifies the server chain against the embedded TEST CA, checks SAN +
 * expiry, verifies ServerKeyExchange, then switches to GCM app data.
 * Single session at a time (like the rest of the stack). Scope notes:
 * TLS 1.2 only, one suite, direct-issued leaf (no chain walking yet),
 * no client certs, no renegotiation, no session resume. Own code. */
#include "tls.h"
#include "tls_aes.h"
#include "tls_prf.h"
#include "tls_bn.h"
#include "tls_p256.h"
#include "tls_rsa.h"
#include "tls_x509.h"
#include "tls_ca.h"
#include "tcp.h"
#include "net.h"
#include "dns.h"
#include "../drv/pit.h"
#include "../drv/rtc.h"
#include "../lib/sha256.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define TLS_CHANGE 20
#define TLS_ALERT 21
#define TLS_HANDSHAKE 22
#define TLS_APP 23

#define HS_HELLO_REQUEST 0
#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_CERTIFICATE 11
#define HS_SERVER_KEYEX 12
#define HS_CERT_REQUEST 13
#define HS_SERVER_DONE 14
#define HS_CLIENT_KEYEX 16
#define HS_FINISHED 20

#define SUITE_RSA_GCM 0xC02F
#define RX_CAP 18432

typedef struct {
  int tcp;
  int live;
  uint8_t cli_rand[32], srv_rand[32];
  uint8_t master[48];
  uint8_t wkey[16], rkey[16];   /* write (client), read (server) */
  uint8_t wsalt[4], rsalt[4];
  uint64_t wseq, rseq;
  sha256_t hs;                  /* handshake transcript */
  uint8_t epriv[32], epub[64];  /* our ECDHE pair */
  /* rx reassembly */
  uint8_t rx[RX_CAP + 5];
  uint32_t rx_len;
} tls_t;

static tls_t g_t;

/* ---- transport helpers ---- */

static int tx_raw(const uint8_t *buf, uint32_t len) {
  uint32_t off = 0;
  while (off < len) {
    uint32_t n = len - off;
    int r;
    if (n > 1460) n = 1460;
    r = tcp_send(g_t.tcp, buf + off, n);
    if (r < 0) return -1;
    off += (uint32_t)r;
    if ((uint32_t)r < n) break; /* short write: caller retries top */
  }
  return off == len ? 0 : -1;
}

/* Read exactly len bytes into dst (ticks timeout per recv). 0 ok. */
static int rx_fill(uint8_t *dst, uint32_t len) {
  uint32_t got = 0;
  while (got < len) {
    int r = tcp_recv(g_t.tcp, dst + got, len - got, 300);
    if (r <= 0) return -1;
    got += (uint32_t)r;
  }
  return 0;
}

/* One TLS record into g_t.rx (payload), type via *type_out. 0 ok. */
static int rx_record(uint8_t *type_out, uint32_t *len_out) {
  uint8_t hdr[5];
  uint32_t len;
  if (rx_fill(hdr, 5) != 0) return -1;
  if (hdr[0] < TLS_CHANGE || hdr[0] > TLS_APP) return -1;
  if (hdr[1] != 0x03 || (hdr[2] != 0x01 && hdr[2] != 0x03)) return -1;
  len = ((uint32_t)hdr[3] << 8) | hdr[4];
  if (len > RX_CAP) return -1;
  if (rx_fill(g_t.rx, len) != 0) return -1;
  *type_out = hdr[0];
  *len_out = len;
  return 0;
}

static int tx_record(uint8_t type, const uint8_t *frag, uint32_t len) {
  uint8_t hdr[5];
  hdr[0] = type;
  hdr[1] = 0x03;
  hdr[2] = 0x03;
  hdr[3] = (uint8_t)(len >> 8);
  hdr[4] = (uint8_t)len;
  if (tx_raw(hdr, 5) != 0) return -1;
  if (len > 0 && tx_raw(frag, len) != 0) return -1;
  return 0;
}

/* ---- GCM protect ---- */

static void gcm_iv(const uint8_t salt[4], uint64_t seq, uint8_t iv[12]) {
  int i;
  for (i = 0; i < 4; i++) iv[i] = salt[i];
  for (i = 0; i < 8; i++) iv[4 + i] = (uint8_t)(seq >> (56 - 8 * i));
}

/* Encrypt one app/handshake fragment -> record. seq++, 0 ok. */
static int tx_prot(uint8_t type, const uint8_t *pt, uint32_t len) {
  static uint8_t out[RX_CAP + 32];
  uint8_t iv[12], aad[13], tag[16];
  uint32_t clen;
  if (len + 8 + 16 > sizeof(out)) return -1;
  gcm_iv(g_t.wsalt, g_t.wseq, iv);
  /* Explicit nonce first (TLS 1.2 GCM). */
  {
    int i;
    for (i = 0; i < 8; i++)
      out[i] = (uint8_t)(g_t.wseq >> (56 - 8 * i));
  }
  aad[0] = (uint8_t)(g_t.wseq >> 56);
  aad[1] = (uint8_t)(g_t.wseq >> 48);
  aad[2] = (uint8_t)(g_t.wseq >> 40);
  aad[3] = (uint8_t)(g_t.wseq >> 32);
  aad[4] = (uint8_t)(g_t.wseq >> 24);
  aad[5] = (uint8_t)(g_t.wseq >> 16);
  aad[6] = (uint8_t)(g_t.wseq >> 8);
  aad[7] = (uint8_t)g_t.wseq;
  aad[8] = type;
  aad[9] = 0x03;
  aad[10] = 0x03;
  aad[11] = (uint8_t)(len >> 8);
  aad[12] = (uint8_t)len;
  gcm_encrypt(g_t.wkey, iv, aad, 13, pt, out + 8, len, tag);
  for (clen = 0; clen < 16; clen++) out[8 + len + clen] = tag[clen];
  g_t.wseq++;
  return tx_record(type, out, 8 + len + 16);
}

/* Decrypt one received record payload in place. Returns plaintext
 * length, <0 on auth failure. The peer's explicit nonce is taken as
 * sent (GCM only needs it unique, not predictable). seq++. */
static int rx_open(uint8_t type, uint8_t *rec, uint32_t len) {
  uint8_t iv[12], aad[13];
  int r, i;
  if (len < 8 + 16) return -1;
  for (i = 0; i < 4; i++) iv[i] = g_t.rsalt[i];
  for (i = 0; i < 8; i++) iv[4 + i] = rec[i];
  aad[0] = (uint8_t)(g_t.rseq >> 56);
  aad[1] = (uint8_t)(g_t.rseq >> 48);
  aad[2] = (uint8_t)(g_t.rseq >> 40);
  aad[3] = (uint8_t)(g_t.rseq >> 32);
  aad[4] = (uint8_t)(g_t.rseq >> 24);
  aad[5] = (uint8_t)(g_t.rseq >> 16);
  aad[6] = (uint8_t)(g_t.rseq >> 8);
  aad[7] = (uint8_t)g_t.rseq;
  aad[8] = type;
  aad[9] = 0x03;
  aad[10] = 0x03;
  {
    uint32_t plen = len - 8 - 16;
    aad[11] = (uint8_t)(plen >> 8);
    aad[12] = (uint8_t)plen;
    r = gcm_decrypt(g_t.rkey, iv, aad, 13, rec + 8, rec, plen,
                    rec + 8 + plen);
    if (r != 0) return -1;
    g_t.rseq++;
    return (int)plen;
  }
}

/* ---- handshake messages ---- */

static void hs_hash(const uint8_t *msg, uint32_t len) {
  sha256_update(&g_t.hs, msg, len);
}

/* Send one handshake message (hashes header+body, plaintext record). */
static int tx_hs(uint8_t type, const uint8_t *body, uint32_t len) {
  static uint8_t msg[2048];
  if (len + 4 > sizeof(msg)) return -1;
  msg[0] = type;
  msg[1] = (uint8_t)(len >> 16);
  msg[2] = (uint8_t)(len >> 8);
  msg[3] = (uint8_t)len;
  naos_memcpy(msg + 4, body, len);
  hs_hash(msg, len + 4);
  return tx_record(TLS_HANDSHAKE, msg, len + 4);
}

/* ClientHello body into buf. Returns length. host NULL = no SNI. */
static uint32_t build_hello(uint8_t *buf, const char *host) {
  uint8_t *p = buf;
  uint32_t i;
  int hlen = 0;
  *p++ = 0x03;
  *p++ = 0x03; /* client_version */
  tls_rand(g_t.cli_rand, 32);
  for (i = 0; i < 32; i++) *p++ = g_t.cli_rand[i];
  *p++ = 0; /* session id: none */
  *p++ = 0x00;
  *p++ = 0x02; /* one suite */
  *p++ = (SUITE_RSA_GCM >> 8) & 0xFF;
  *p++ = SUITE_RSA_GCM & 0xFF;
  *p++ = 0x01;
  *p++ = 0x00; /* null compression */
  {
    uint8_t *ext_len_at;
    uint8_t *ext_start;
    p += 2; /* extensions length, backpatched */
    ext_start = p;
    if (host) {
      while (host[hlen] && hlen < 64) hlen++;
      if (hlen > 0) {
        /* server_name (0): list len + entry(type 0 + len + name). */
        *p++ = 0x00;
        *p++ = 0x00;
        *p++ = (uint8_t)((hlen + 5) >> 8);
        *p++ = (uint8_t)(hlen + 5);
        *p++ = (uint8_t)((hlen + 3) >> 8);
        *p++ = (uint8_t)(hlen + 3);
        *p++ = 0x00;
        *p++ = (uint8_t)(hlen >> 8);
        *p++ = (uint8_t)hlen;
        for (i = 0; (int)i < hlen; i++) *p++ = (uint8_t)host[i];
      }
    }
    /* ec_point_formats (11): uncompressed only. */
    *p++ = 0x00;
    *p++ = 0x0B;
    *p++ = 0x00;
    *p++ = 0x02;
    *p++ = 0x01;
    *p++ = 0x00;
    /* supported_groups (10): secp256r1. */
    *p++ = 0x00;
    *p++ = 0x0A;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x00;
    *p++ = 0x02;
    *p++ = 0x00;
    *p++ = 0x17;
    /* signature_algorithms (13): rsa_pkcs1_sha256. */
    *p++ = 0x00;
    *p++ = 0x0D;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x00;
    *p++ = 0x02;
    *p++ = 0x04;
    *p++ = 0x01;
    {
      uint32_t elen = (uint32_t)(p - ext_start);
      ext_len_at = buf + (ext_start - buf) - 2;
      ext_len_at[0] = (uint8_t)(elen >> 8);
      ext_len_at[1] = (uint8_t)elen;
    }
  }
  return (uint32_t)(p - buf);
}

/* Receive handshake messages until `want` type arrives; hashes every
 * message; dispatches via out (up to cap). Returns message length. */
static int hs_until(uint8_t want, uint8_t *out, uint32_t cap) {
  static uint8_t pending[RX_CAP];
  static uint32_t plen;
  for (;;) {
    uint8_t rtype;
    uint32_t rlen, off = 0;
    if (rx_record(&rtype, &rlen) != 0) return -1;
    if (rtype == TLS_ALERT) return -2; /* peer says no */
    if (rtype != TLS_HANDSHAKE) return -1;
    if (rlen > RX_CAP) return -1;
    naos_memcpy(pending, g_t.rx, rlen);
    plen = rlen;
    while (off + 4 <= plen) {
      uint8_t htype = pending[off];
      uint32_t hlen = ((uint32_t)pending[off + 1] << 16) |
                      ((uint32_t)pending[off + 2] << 8) | pending[off + 3];
      if (off + 4 + hlen > plen) break; /* spans records: refill below */
      hs_hash(pending + off, hlen + 4);
      if (htype == HS_CERT_REQUEST) return -3; /* no client certs */
      if (htype == want) {
        if (hlen > cap) return -1;
        naos_memcpy(out, pending + off + 4, hlen);
        /* leftover bytes in this record belong to later messages;
         * our flow never pipelines, so demand exact end. */
        if (off + 4 + hlen != plen) return -1;
        return (int)hlen;
      }
      off += 4 + hlen;
    }
    /* Incomplete trailing message: not expected in our flow. */
    if (off != plen) return -1;
  }
}

/* ---- public API ---- */

static uint64_t rtc_stamp(void) {
  rtc_time_t t;
  if (!rtc_read(&t)) return 0;
  return (uint64_t)t.year * 10000000000ULL + (uint64_t)t.month * 100000000ULL +
         (uint64_t)t.day * 1000000ULL + (uint64_t)t.hour * 10000ULL +
         (uint64_t)t.minute * 100ULL + (uint64_t)t.second;
}

static void seed_rand(void) {
  uint8_t seed[32];
  uint64_t t = pit_ticks();
  const uint8_t *mac = net_mac();
  rtc_time_t rt;
  int i;
  for (i = 0; i < 8; i++) seed[i] = (uint8_t)(t >> (8 * i));
  if (rtc_read(&rt)) {
    seed[8] = (uint8_t)rt.year;
    seed[9] = (uint8_t)(rt.year >> 8);
    seed[10] = rt.month;
    seed[11] = rt.day;
    seed[12] = rt.hour;
    seed[13] = rt.minute;
    seed[14] = rt.second;
    seed[15] = 0;
  } else {
    for (i = 8; i < 16; i++) seed[i] = (uint8_t)(t >> (8 * (i - 8)));
  }
  for (i = 0; i < 6; i++) seed[16 + i] = mac ? mac[i] : 0;
  for (i = 22; i < 32; i++) seed[i] = (uint8_t)(t >> (8 * (i - 22)));
  tls_rand_seed(seed, 32);
}

/* Connect + handshake. host NULL when dialing by IP (ip set); ip NULL
 * when dialing by name (resolved here, SNI sent). 0 ok. */
int tls_connect(uint32_t ip, uint16_t port, const char *host,
                const uint8_t ipb[4]) {
  static uint8_t msg[RX_CAP];
  static uint8_t hello[512];
  static uint8_t ske[1024];
  uint32_t hl;
  int mlen;
  uint8_t leaf[8192];
  uint32_t leaf_len = 0;
  uint8_t srv_pub[65];
  uint8_t sig[256];
  uint32_t sig_len = 0;
  uint8_t ln[256];
  uint32_t le = 0;
  uint8_t pms[32], verify[12], srv_fin[12];
  uint8_t hh[32];
  sha256_t hc;
  int i;
  seed_rand();
  g_t.live = 0;
  g_t.wseq = 0;
  g_t.rseq = 0;
  g_t.tcp = tcp_connect(ip, port);
  if (g_t.tcp < 0) {
    kprintf("tls: tcp failed\n");
    return -1;
  }
  /* Ephemeral ECDHE pair now (public goes out in ClientKeyExchange). */
  for (;;) {
    tls_rand(g_t.epriv, 32);
    if (p256_priv_ok(g_t.epriv)) break;
  }
  if (p256_pub(g_t.epriv, g_t.epub) != 0) {
    kprintf("tls: ecdhe keygen failed\n");
    tcp_close(g_t.tcp);
    return -1;
  }
  sha256_init(&g_t.hs);
  hl = build_hello(hello, host);
  /* ClientHello goes as a 0x0301 record (legacy), plaintext. */
  {
    uint8_t hdr[5];
    uint8_t hm[4 + 512];
    hm[0] = HS_CLIENT_HELLO;
    hm[1] = (uint8_t)(hl >> 16);
    hm[2] = (uint8_t)(hl >> 8);
    hm[3] = (uint8_t)hl;
    naos_memcpy(hm + 4, hello, hl);
    hs_hash(hm, hl + 4);
    hdr[0] = TLS_HANDSHAKE;
    hdr[1] = 0x03;
    hdr[2] = 0x01;
    hdr[3] = (uint8_t)((hl + 4) >> 8);
    hdr[4] = (uint8_t)(hl + 4);
    if (tx_raw(hdr, 5) != 0 || tx_raw(hm, hl + 4) != 0) {
      kprintf("tls: hello send failed\n");
      tcp_close(g_t.tcp);
      return -1;
    }
  }
  /* ServerHello. */
  mlen = hs_until(HS_SERVER_HELLO, msg, sizeof(msg));
  if (mlen < 38) {
    kprintf("tls: bad ServerHello\n");
    tcp_close(g_t.tcp);
    return -1;
  }
  if (msg[0] != 0x03 || msg[1] != 0x03) {
    kprintf("tls: server is not 1.2\n");
    tcp_close(g_t.tcp);
    return -1;
  }
  for (i = 0; i < 32; i++) g_t.srv_rand[i] = msg[2 + i];
  {
    /* version(2) random(32) session(1+N) cipher(2) compression(1). */
    uint32_t sl = msg[34];
    if (mlen < (int)(38 + sl)) {
      kprintf("tls: short ServerHello\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    if (msg[35 + sl] != (SUITE_RSA_GCM >> 8) ||
        msg[36 + sl] != (SUITE_RSA_GCM & 0xFF)) {
      kprintf("tls: suite refused\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    if (msg[37 + sl] != 0) {
      kprintf("tls: compression refused\n");
      tcp_close(g_t.tcp);
      return -1;
    }
  }
  /* Certificate (first = leaf). */
  mlen = hs_until(HS_CERTIFICATE, msg, sizeof(msg));
  if (mlen < 3) {
    kprintf("tls: no certificate\n");
    tcp_close(g_t.tcp);
    return -1;
  }
  {
    uint32_t total = ((uint32_t)msg[0] << 16) | ((uint32_t)msg[1] << 8) |
                     msg[2];
    uint32_t off = 3;
    if (total + 3 > (uint32_t)mlen) {
      kprintf("tls: cert length\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    if (off + 3 > (uint32_t)mlen) {
      kprintf("tls: cert empty\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    leaf_len = ((uint32_t)msg[off] << 16) | ((uint32_t)msg[off + 1] << 8) |
               msg[off + 2];
    off += 3;
    if (leaf_len == 0 || leaf_len > sizeof(leaf) ||
        off + leaf_len > (uint32_t)mlen) {
      kprintf("tls: leaf size\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    naos_memcpy(leaf, msg + off, leaf_len);
  }
  /* ServerKeyExchange (ECDHE params + RSA signature). */
  mlen = hs_until(HS_SERVER_KEYEX, ske, sizeof(ske));
  if (mlen < 70) {
    kprintf("tls: no ServerKeyExchange\n");
    tcp_close(g_t.tcp);
    return -1;
  }
  {
    uint32_t off = 0;
    uint8_t plen;
    uint8_t params[80];
    uint32_t params_len;
    sha256_t sh;
    uint8_t digest[32];
    if (ske[0] != 3 || ske[1] != 0 || ske[2] != 0x17) {
      kprintf("tls: not secp256r1\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    off = 3;
    plen = ske[off];
    if (plen != 65 || ske[off + 1] != 0x04) {
      kprintf("tls: bad server point\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    for (i = 0; i < 65; i++) srv_pub[i] = ske[off + 1 + i];
    off += 1 + 65;
    params_len = off; /* curve_type .. point end */
    for (i = 0; i < (int)params_len && i < 80; i++) params[i] = ske[i];
    if (mlen < (int)off + 2) {
      kprintf("tls: no signature\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    /* TLS 1.2: SignatureAndHashAlgorithm + u16 len + signature. */
    if (ske[off] != 0x04 || ske[off + 1] != 0x01) {
      kprintf("tls: SKE needs rsa_pkcs1_sha256\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    off += 2;
    sig_len = ((uint32_t)ske[off] << 8) | ske[off + 1];
    off += 2;
    if (sig_len != 256 || mlen < (int)off + 256) {
      kprintf("tls: bad signature size\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    for (i = 0; i < 256; i++) sig[i] = ske[off + i];
    /* Chain + signature, over client_random + server_random + params. */
    if (x509_verify_chain(leaf, leaf_len, TLS_CA_DER, TLS_CA_DER_LEN,
                          host, ipb, rtc_stamp()) != 0) {
      kprintf("tls: certificate rejected\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    if (x509_spki(leaf, leaf_len, ln, &le) != 0) {
      kprintf("tls: no RSA key\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    sha256_init(&sh);
    sha256_update(&sh, g_t.cli_rand, 32);
    sha256_update(&sh, g_t.srv_rand, 32);
    sha256_update(&sh, params, params_len);
    sha256_final(&sh, digest);
    if (rsa_verify(ln, le, sig, digest) != 1) {
      kprintf("tls: ServerKeyExchange forged\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    /* Ephemeral secret from the server point. */
    {
      uint8_t peer[64];
      for (i = 0; i < 64; i++) peer[i] = srv_pub[1 + i];
      if (p256_shared(g_t.epriv, peer, pms) != 0) {
        kprintf("tls: ECDHE failed\n");
        tcp_close(g_t.tcp);
        return -1;
      }
    }
  }
  /* ServerHelloDone. */
  mlen = hs_until(HS_SERVER_DONE, msg, sizeof(msg));
  if (mlen != 0) {
    kprintf("tls: bad ServerHelloDone\n");
    tcp_close(g_t.tcp);
    return -1;
  }
  tls_master(pms, 32, g_t.cli_rand, g_t.srv_rand, g_t.master);
  {
    /* RFC 5246 key_block for AES_128_GCM: client_key[16],
     * server_key[16], client_IV[4], server_IV[4]. */
    uint8_t kb[48];
    tls_keyblock(g_t.master, g_t.srv_rand, g_t.cli_rand, kb);
    for (i = 0; i < 16; i++) {
      g_t.wkey[i] = kb[i];
      g_t.rkey[i] = kb[16 + i];
    }
    for (i = 0; i < 4; i++) {
      g_t.wsalt[i] = kb[32 + i];
      g_t.rsalt[i] = kb[36 + i];
    }
  }
  /* ClientKeyExchange (our point), CCS, Finished. */
  {
    uint8_t cke[66];
    cke[0] = 65;
    cke[1] = 0x04;
    for (i = 0; i < 64; i++) cke[2 + i] = g_t.epub[i];
    if (tx_hs(HS_CLIENT_KEYEX, cke, 66) != 0) {
      kprintf("tls: CKE send failed\n");
      tcp_close(g_t.tcp);
      return -1;
    }
  }
  {
    uint8_t ccs = 0x01;
    if (tx_record(TLS_CHANGE, &ccs, 1) != 0) {
      kprintf("tls: CCS send failed\n");
      tcp_close(g_t.tcp);
      return -1;
    }
  }
  hc = g_t.hs;
  sha256_final(&hc, hh);
  tls_finished(g_t.master, 1, hh, verify);
  /* Finished is a handshake message (type 20) inside a GCM record.
   * It joins the transcript AFTER its verify_data is computed. */
  {
    uint8_t fin[16];
    fin[0] = HS_FINISHED;
    fin[1] = 0;
    fin[2] = 0;
    fin[3] = 12;
    for (i = 0; i < 12; i++) fin[4 + i] = verify[i];
    hs_hash(fin, 16);
    if (tx_prot(TLS_HANDSHAKE, fin, 16) != 0) {
      kprintf("tls: Finished send failed\n");
      tcp_close(g_t.tcp);
      return -1;
    }
  }
  /* Server ChangeCipherSpec (plaintext) + Finished (protected). */
  {
    uint8_t rtype;
    uint32_t rlen;
    if (rx_record(&rtype, &rlen) != 0 || rtype != TLS_CHANGE || rlen != 1 ||
        g_t.rx[0] != 0x01) {
      kprintf("tls: no server CCS\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    if (rx_record(&rtype, &rlen) != 0 || rtype != TLS_HANDSHAKE) {
      kprintf("tls: no server Finished\n");
      tcp_close(g_t.tcp);
      return -1;
    }
    {
      int plen = rx_open(TLS_HANDSHAKE, g_t.rx, rlen);
      if (plen != 16 || g_t.rx[0] != HS_FINISHED || g_t.rx[1] != 0 ||
          g_t.rx[2] != 0 || g_t.rx[3] != 12) {
        kprintf("tls: bad server Finished\n");
        tcp_close(g_t.tcp);
        return -1;
      }
      hc = g_t.hs;
      sha256_final(&hc, hh);
      tls_finished(g_t.master, 0, hh, srv_fin);
      for (i = 0; i < 12; i++) {
        if (srv_fin[i] != g_t.rx[4 + i]) {
          kprintf("tls: server Finished mismatch\n");
          tcp_close(g_t.tcp);
          return -1;
        }
      }
      hs_hash(g_t.rx, 16);
    }
  }
  kprintf("tls: handshake done (ECDHE_RSA_AES_128_GCM_SHA256)\n");
  g_t.live = 1;
  return 0;
}

/* Send application bytes (chunked to TCP_MSS records). 0 ok. */
int tls_send(const uint8_t *buf, uint32_t len) {
  uint32_t off = 0;
  static uint8_t chunk[16384];
  if (!g_t.live) return -1;
  while (off < len) {
    uint32_t n = len - off;
    uint32_t k;
    if (n > 1460) n = 1460;
    for (k = 0; k < n; k++) chunk[k] = buf[off + k];
    if (tx_prot(TLS_APP, chunk, n) != 0) return -1;
    off += n;
  }
  return 0;
}

/* Receive one application fragment (cap bytes). >0 bytes, 0 = clean
 * close, <0 on error/timeout. Alerts: fatal fails, close_notify ends. */
int tls_recv(uint8_t *buf, uint32_t cap) {
  uint8_t rtype;
  uint32_t rlen;
  int plen;
  if (!g_t.live) return -1;
  for (;;) {
    if (rx_record(&rtype, &rlen) != 0) return -1;
    if (rtype == TLS_ALERT) {
      if (rlen >= 2 && g_t.rx[0] == 2) {
        g_t.live = 0;
        return -1; /* fatal alert */
      }
      if (rlen >= 2 && g_t.rx[0] == 1 && g_t.rx[1] == 0) {
        g_t.live = 0;
        return 0; /* close_notify */
      }
      continue; /* warnings ignored */
    }
    if (rtype != TLS_APP) return -1;
    plen = rx_open(TLS_APP, g_t.rx, rlen);
    if (plen < 0) return -1;
    if ((uint32_t)plen > cap) return -1;
    for (uint32_t i = 0; i < (uint32_t)plen; i++) buf[i] = g_t.rx[i];
    return plen;
  }
}

void tls_close(void) {
  if (g_t.live) {
    /* Post-handshake alerts are encrypted records, not plaintext. */
    uint8_t bye[2] = { 1, 0 }; /* warning close_notify */
    tx_prot(TLS_ALERT, bye, 2);
    g_t.live = 0;
  }
  tcp_close(g_t.tcp);
}

/* Decimal octet into dst (no libc). */
static void put_dec(char *dst, uint32_t *np, uint32_t v) {
  char tmp[3];
  int m = 0, k;
  if (v >= 100) {
    tmp[m++] = (char)('0' + v / 100);
    v %= 100;
  }
  if (m > 0 || v >= 10) {
    tmp[m++] = (char)('0' + v / 10);
    v %= 10;
  }
  tmp[m++] = (char)('0' + v);
  for (k = 0; k < m; k++) dst[(*np)++] = tmp[k];
}

/* Shell printer callback. */
static void print_cb(const uint8_t *chunk, uint32_t len, int done) {
  uint32_t i;
  (void)done;
  if (!chunk) return;
  for (i = 0; i < len; i++) kprintf("%c", chunk[i]);
}

int tls_fetch_print(uint32_t ip, uint16_t port, const char *host,
                    const uint8_t ipb[4], const char *path) {
  int r = tls_fetch(ip, port, host, ipb, path, print_cb);
  if (r == 0) kprintf("\n");
  return r;
}

/* HTTPS GET: connect, send request, stream the body through cb.
 * cb(chunk, len, done-flag). 0 ok. */
int tls_fetch(uint32_t ip, uint16_t port, const char *host,
              const uint8_t ipb[4], const char *path,
              void (*cb)(const uint8_t *, uint32_t, int)) {
  static char req[512];
  uint32_t rl = 0;
  const char *get1 = "GET ";
  const char *get2 = " HTTP/1.0\r\nHost: ";
  const char *get3 = "\r\nConnection: close\r\n\r\n";
  uint32_t i;
  int in_body = 0;
  static uint8_t rb[2048];
  static uint8_t hb[4096];
  uint32_t hlen = 0;
  char hostbuf[16];
  const char *req_host = host;
  if (!req_host) {
    /* No name: Host header carries the dotted IP (connect still
     * goes nameless: no SNI, IP SAN check). */
    uint32_t n = 0;
    if (ipb) {
      for (i = 0; i < 4 && n + 4 < sizeof(hostbuf); i++) {
        if (i > 0) hostbuf[n++] = '.';
        put_dec(hostbuf, &n, ipb[i]);
      }
      hostbuf[n] = '\0';
      req_host = hostbuf;
    } else {
      req_host = "localhost";
    }
  }
  for (i = 0; get1[i] && rl + 1 < sizeof(req); i++) req[rl++] = get1[i];
  if (!path) path = "/";
  for (i = 0; path[i] && rl + 1 < sizeof(req); i++) req[rl++] = path[i];
  for (i = 0; get2[i] && rl + 1 < sizeof(req); i++) req[rl++] = get2[i];
  for (i = 0; req_host[i] && rl + 1 < sizeof(req); i++)
    req[rl++] = req_host[i];
  for (i = 0; get3[i] && rl + 1 < sizeof(req); i++) req[rl++] = get3[i];
  if (tls_connect(ip, port, host, ipb) != 0) return -1;
  if (tls_send((const uint8_t *)req, rl) != 0) {
    kprintf("tls: send failed\n");
    tls_close();
    return -1;
  }
  for (;;) {
    int n = tls_recv(rb, sizeof(rb));
    if (n < 0) {
      /* Bare TCP close without close_notify (HTTP/1.0 close-delimited
       * body): success if headers completed and body bytes flowed. */
      if (in_body) break;
      kprintf("tls: receive failed\n");
      tls_close();
      return -1;
    }
    if (n == 0) break;
    if (!in_body) {
      /* Stash headers until the blank line (may span records). */
      uint32_t k;
      if (hlen + (uint32_t)n > sizeof(hb)) {
        tls_close();
        return -1; /* absurd headers */
      }
      for (k = 0; k < (uint32_t)n; k++) hb[hlen + k] = rb[k];
      hlen += (uint32_t)n;
      {
        int hdr_end = -1;
        for (k = 0; k + 3 < hlen; k++) {
          if (hb[k] == '\r' && hb[k + 1] == '\n' && hb[k + 2] == '\r' &&
              hb[k + 3] == '\n') {
            hdr_end = (int)k + 4;
            break;
          }
        }
        if (hdr_end < 0) continue;
        in_body = 1;
        if (cb && (uint32_t)hdr_end < hlen)
          cb(hb + hdr_end, hlen - hdr_end, 0);
        hlen = 0;
      }
    } else {
      if (cb) cb(rb, (uint32_t)n, 0);
    }
  }
  if (cb) cb(0, 0, 1);
  tls_close();
  return 0;
}
