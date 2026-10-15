/* NaOS updater: resolve host (IP or DNS), HTTP/1.0 GET over TCP, stream
 * the body in chunks. Manifests are small (one fetch); files stream to
 * FAT with an incremental SHA-256. All waits are bounded. */
#include "update.h"
#include "pkt.h"
#include "net.h"
#include "tcp.h"
#include "dns.h"
#include "version.h"
#include "../lib/kprintf.h"
#include "../lib/sha256.h"
#include "../fs/fat32.h"
#include "../../common/cstring.h"

#define UP_CHUNK 2048
#define UP_MAX_MANIFEST 4096
#define UP_MAX_FILE (1024u * 1024u)

static uint8_t g_chunk[UP_CHUNK];
static uint8_t g_hdr[2048];
static char g_manifest[UP_MAX_MANIFEST + 1];

static int resolve_host(const char *h, uint32_t *ip) {
  if (!h || !*h) return -1;
  if (net_parse_ip(h, ip) == 0) return 0;
  return dns_resolve(h, ip);
}

/* GET path; returns conn id or -1. Prints nothing. */
static int http_get(uint32_t ip, uint16_t port, const char *path) {
  static char req[160];
  int t, rl = 0, pl = 0;
  if (!path || path[0] != '/') return -1;
  while (path[pl] && pl < 64) pl++;
  if (path[pl] != '\0') return -1;
  t = tcp_connect(ip, port);
  if (t < 0) return -1;
  naos_memcpy(req + rl, "GET ", 4);
  rl += 4;
  naos_memcpy(req + rl, path, (uint32_t)pl);
  rl += pl;
  {
    const char *tail =
        " HTTP/1.0\r\nHost: naos\r\nConnection: close\r\n\r\n";
    uint32_t tl = (uint32_t)naos_strlen(tail);
    if ((uint32_t)rl + tl >= sizeof(req)) {
      tcp_close(t);
      return -1;
    }
    naos_memcpy(req + rl, tail, tl);
    rl += (int)tl;
  }
  if (tcp_send(t, req, (uint32_t)rl) < 0) {
    tcp_close(t);
    return -1;
  }
  return t;
}

/* Read one HTTP body into buf (cap). Returns bytes or -1. Skips headers
 * (accumulates until \r\n\r\n, 2048 cap). */
static int http_body(int t, uint8_t *buf, uint32_t cap) {
  uint32_t hlen = 0, total = 0;
  int in_body = 0;
  for (;;) {
    int r = tcp_recv(t, (char *)g_chunk, UP_CHUNK, 300);
    if (r < 0) break;
    if (r == 0) break;
    if (!in_body) {
      uint32_t j;
      int found = -1;
      if (hlen + (uint32_t)r > sizeof(g_hdr)) return -1;
      naos_memcpy(g_hdr + hlen, g_chunk, (uint32_t)r);
      hlen += (uint32_t)r;
      for (j = 0; j + 4 <= hlen; j++) {
        if (g_hdr[j] == '\r' && g_hdr[j + 1] == '\n' &&
            g_hdr[j + 2] == '\r' && g_hdr[j + 3] == '\n') {
          found = (int)j;
          break;
        }
      }
      if (found < 0) continue;
      {
        uint32_t b = (uint32_t)found + 4;
        while (b < hlen) {
          if (total >= cap) return -1;
          buf[total++] = g_hdr[b++];
        }
      }
      in_body = 1;
    } else {
      if (total + (uint32_t)r > cap) return -1;
      naos_memcpy(buf + total, g_chunk, (uint32_t)r);
      total += (uint32_t)r;
    }
  }
  return (int)total;
}

/* Manifest line value: "key=" prefix match, copy until CR/LF/NUL. */
static int mval(const char *m, const char *key, char *out, uint32_t cap) {
  uint32_t kl = (uint32_t)naos_strlen(key);
  const char *p = m;
  while (*p) {
    const char *line = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    /* line = [line, p) */
    {
      uint32_t j = 0;
      while (j < kl && line[j] == key[j]) j++;
      if (j == kl && line[j] == '=') {
        uint32_t v = j + 1, n = 0;
        while (line + v + n < p && n + 1 < cap) {
          out[n] = line[v + n];
          n++;
        }
        out[n] = '\0';
        return 0;
      }
    }
    while (*p == '\n' || *p == '\r') p++;
  }
  return -1;
}

int update_check(const char *host, uint16_t port, const char *mpath) {
  uint32_t ip;
  int t, n;
  char ver[32], note[128], nf[16];
  if (resolve_host(host, &ip) != 0) {
    kprintf("update: cannot resolve '%s'\n", host);
    return -1;
  }
  if (sha256_selftest() != 0) {
    kprintf("update: sha selftest FAIL, abort\n");
    return -1;
  }
  t = http_get(ip, port, mpath);
  if (t < 0) {
    kprintf("update: fetch failed\n");
    return -1;
  }
  n = http_body(t, (uint8_t *)g_manifest, UP_MAX_MANIFEST);
  tcp_close(t);
  if (n <= 0) {
    kprintf("update: empty manifest\n");
    return -1;
  }
  g_manifest[n] = '\0';
  if (mval(g_manifest, "version", ver, sizeof(ver)) != 0) {
    kprintf("update: bad manifest (no version=)\n");
    return -1;
  }
  if (mval(g_manifest, "note", note, sizeof(note)) != 0) note[0] = '\0';
  kprintf("update: installed %s, available %s\n", NAOS_VERSION, ver);
  if (naos_strcmp(NAOS_VERSION, ver) == 0) kprintf("update: up to date\n");
  if (note[0]) kprintf("update: %s\n", note);
  if (mval(g_manifest, "nfiles", nf, sizeof(nf)) != 0) return 0;
  {
    int count = 0;
    for (const char *p = nf; *p >= '0' && *p <= '9'; p++)
      count = count * 10 + (*p - '0');
    for (int i = 0; i < count && i < 16; i++) {
      char k[16], nm[64], sz[16];
      k[0] = 'f';
      k[1] = (char)('0' + i);
      k[2] = '_';
      k[3] = 'n';
      k[4] = 'a';
      k[5] = 'm';
      k[6] = 'e';
      k[7] = '\0';
      if (mval(g_manifest, k, nm, sizeof(nm)) != 0) break;
      k[3] = 's';
      k[4] = 'i';
      k[5] = 'z';
      k[6] = 'e';
      if (mval(g_manifest, k, sz, sizeof(sz)) != 0) sz[0] = '\0';
      kprintf("update: file %s (%s bytes)\n", nm, sz[0] ? sz : "?");
    }
  }
  return 0;
}

/* Lowercase-hex compare helper. */
static int hexdig(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int update_get(const char *host, uint16_t port, const char *remotepath,
               const char *localpath, const char *sha_hex) {
  uint32_t ip;
  int t;
  uint32_t hlen = 0, total = 0;
  int in_body = 0;
  sha256_t sh;
  int want_sha = (sha_hex && sha_hex[0]);
  if (resolve_host(host, &ip) != 0) {
    kprintf("update: cannot resolve '%s'\n", host);
    return -1;
  }
  if (sha256_selftest() != 0) {
    kprintf("update: sha selftest FAIL, abort\n");
    return -1;
  }
  if (want_sha) {
    int n = 0;
    while (sha_hex[n]) n++;
    if (n != 64) {
      kprintf("update: bad sha (need 64 hex)\n");
      return -1;
    }
  }
  if (fat_write(localpath, "", 0) != 0) {
    kprintf("update: cannot stage '%s'\n", localpath);
    return -1;
  }
  t = http_get(ip, port, remotepath);
  if (t < 0) {
    kprintf("update: fetch failed\n");
    return -1;
  }
  sha256_init(&sh);
  for (;;) {
    int r = tcp_recv(t, (char *)g_chunk, UP_CHUNK, 300);
    if (r < 0) {
      kprintf("update: timeout\n");
      tcp_close(t);
      return -1;
    }
    if (r == 0) break;
    if (!in_body) {
      uint32_t j;
      int found = -1;
      if (hlen + (uint32_t)r > sizeof(g_hdr)) {
        tcp_close(t);
        return -1;
      }
      naos_memcpy(g_hdr + hlen, g_chunk, (uint32_t)r);
      hlen += (uint32_t)r;
      for (j = 0; j + 4 <= hlen; j++) {
        if (g_hdr[j] == '\r' && g_hdr[j + 1] == '\n' &&
            g_hdr[j + 2] == '\r' && g_hdr[j + 3] == '\n') {
          found = (int)j;
          break;
        }
      }
      if (found < 0) continue;
      in_body = 1;
      {
        uint32_t b = (uint32_t)found + 4;
        uint32_t n = hlen - b;
        if (total + n > UP_MAX_FILE) {
          tcp_close(t);
          return -1;
        }
        if (n > 0) {
          sha256_update(&sh, g_hdr + b, n);
          if (fat_append(localpath, g_hdr + b, n) != 0) {
            tcp_close(t);
            kprintf("update: disk write failed\n");
            return -1;
          }
          total += n;
        }
      }
    } else {
      if (total + (uint32_t)r > UP_MAX_FILE) {
        tcp_close(t);
        return -1;
      }
      sha256_update(&sh, g_chunk, (uint32_t)r);
      if (fat_append(localpath, g_chunk, (uint32_t)r) != 0) {
        tcp_close(t);
        kprintf("update: disk write failed\n");
        return -1;
      }
      total += (uint32_t)r;
    }
  }
  tcp_close(t);
  if (want_sha) {
    uint8_t sum[32];
    char hex[65];
    int ok = 1;
    sha256_final(&sh, sum);
    sha256_hex(sum, hex);
    for (int i = 0; i < 64; i++) {
      int a = hexdig(hex[i]), b = hexdig(sha_hex[i]);
      if (a < 0 || b < 0 || a != b) {
        ok = 0;
        break;
      }
    }
    kprintf("update: %u bytes, sha %s\n", (unsigned)total,
            ok ? "OK" : "MISMATCH");
    return ok ? 0 : -1;
  }
  kprintf("update: %u bytes (unverified)\n", (unsigned)total);
  return 0;
}

/* Decimal u32 into buf. Returns length. */
static uint32_t putu32(char *b, uint32_t v) {
  char t[10];
  uint32_t n = 0;
  if (v == 0) {
    b[0] = '0';
    return 1;
  }
  while (v > 0 && n < 10) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  for (uint32_t i = 0; i < n; i++) b[i] = t[n - 1 - i];
  return n;
}

/* Content-Type by FAT extension (either case; 8.3 names are upper). */
static const char *serve_type(const char *path) {
  uint32_t n = 0;
  while (path[n] && n < 64) n++;
  if (n >= 5) {
    const char *e = path + n - 5;
    if (e[0] == '.' && (e[1] == 'h' || e[1] == 'H') &&
        (e[2] == 't' || e[2] == 'T') && (e[3] == 'm' || e[3] == 'M') &&
        (e[4] == 'l' || e[4] == 'L'))
      return "text/html";
  }
  if (n >= 4) {
    const char *e = path + n - 4;
    if (e[0] == '.') {
      if ((e[1] == 't' || e[1] == 'T') && (e[2] == 'x' || e[2] == 'X') &&
          (e[3] == 't' || e[3] == 'T'))
        return "text/plain";
      if ((e[1] == 'h' || e[1] == 'H') && (e[2] == 't' || e[2] == 'T') &&
          (e[3] == 'm' || e[3] == 'M'))
        return "text/html";
    }
  }
  return "application/octet-stream";
}

int net_serve(uint16_t port, const char *localpath) {
  static char resp[192];
  uint32_t clus = 0, size = 0, pos = 0, rl = 0, hlen = 0;
  uint32_t pl = 0;
  int li, t;
  /* Validate: absolute FAT path, bounded, no parent escapes. */
  if (!localpath || localpath[0] != '/') {
    kprintf("usage: serve <port> </file>\n");
    return -1;
  }
  while (localpath[pl] && pl < 64) {
    if (localpath[pl] == '.' && localpath[pl + 1] == '.') {
      kprintf("serve: '..' refused\n");
      return -1;
    }
    pl++;
  }
  if (localpath[pl] != '\0') {
    kprintf("serve: path too long\n");
    return -1;
  }
  if (fat_fopen_r(localpath, &clus, &size) != 0) {
    kprintf("serve: cannot open '%s'\n", localpath);
    return -1;
  }
  li = tcp_listen(port);
  if (li < 0) {
    kprintf("serve: cannot listen on %u\n", (unsigned)port);
    return -1;
  }
  kprintf("serve: %s (%u bytes) on port %u, one connection\n", localpath,
          (unsigned)size, (unsigned)port);
  t = tcp_accept(li, 6000); /* 60s for the client to show up */
  tcp_close(li);
  if (t < 0) {
    kprintf("serve: no connection\n");
    return -1;
  }
  kprintf("serve: connection, sending\n");
  /* Read (and discard) the request head so the client isn't left hanging:
   * until the blank line, 2KB cap, ~3s. Timeout with zero bytes aborts. */
  for (;;) {
    int r = tcp_recv(t, (char *)g_hdr + hlen, sizeof(g_hdr) - hlen, 300);
    uint32_t j;
    int found = -1;
    if (r < 0) break;
    if (r == 0) break;
    hlen += (uint32_t)r;
    for (j = 0; j + 4 <= hlen; j++) {
      if (g_hdr[j] == '\r' && g_hdr[j + 1] == '\n' &&
          g_hdr[j + 2] == '\r' && g_hdr[j + 3] == '\n') {
        found = (int)j;
        break;
      }
    }
    if (found >= 0 || hlen >= sizeof(g_hdr)) break;
  }
  if (hlen == 0) {
    kprintf("serve: client went quiet\n");
    tcp_close(t);
    return -1;
  }
  /* Validate the request line: "GET /path ...". Anything else, or a path
   * that isn't the served file (case-insensitive, FAT-like), gets an
   * error status instead of the bytes. */
  {
    static const char *e400 = "HTTP/1.0 400 Bad Request\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad method\n";
    static const char *e404 = "HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot here\n";
    const char *err = (const char *)0;
    uint32_t ps = 0, pe = 0;
    if (!(hlen >= 4 && g_hdr[0] == 'G' && g_hdr[1] == 'E' &&
          g_hdr[2] == 'T' && g_hdr[3] == ' ')) {
      err = e400;
    } else {
      uint32_t a = 4, b = 0;
      int match = 1;
      ps = 4;
      while (ps < hlen && g_hdr[ps] != ' ' && g_hdr[ps] != '\r' &&
             g_hdr[ps] != '\n')
        ps++;
      pe = ps;
      for (;;) {
        char ca = (a < pe) ? (char)g_hdr[a] : '\0';
        char cb = localpath[b];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) {
          match = 0;
          break;
        }
        if (ca == '\0') break;
        a++;
        b++;
      }
      if (!match) err = e404;
    }
    if (err) {
      uint32_t el = 0, j = 0;
      while (err[el]) el++;
      while (j < el) {
        uint32_t n = el - j;
        if (n > TCP_MSS) n = TCP_MSS;
        if (tcp_send(t, err + j, n) < 0) break;
        j += n;
      }
      kprintf("serve: refused (%s)\n",
              err == e400 ? "bad method" : "not found");
      tcp_close(t);
      return -1;
    }
  }
  /* Response headers. */
  naos_memcpy(resp + rl, "HTTP/1.0 200 OK\r\nContent-Length: ", 33);
  rl += 33;
  rl += putu32(resp + rl, size);
  naos_memcpy(resp + rl, "\r\nContent-Type: ", 17);
  rl += 17;
  {
    const char *ct = serve_type(localpath);
    uint32_t cl = 0;
    while (ct[cl]) cl++;
    naos_memcpy(resp + rl, ct, cl);
    rl += cl;
  }
  naos_memcpy(resp + rl, "\r\nConnection: close\r\n\r\n", 23);
  rl += 23;
  if (tcp_send(t, resp, rl) < 0) {
    kprintf("serve: header send failed\n");
    tcp_close(t);
    return -1;
  }
  while (pos < size) {
    uint32_t n = size - pos;
    int r;
    if (n > TCP_MSS) n = TCP_MSS;
    r = fat_read_at(clus, size, pos, g_chunk, n);
    if (r <= 0) {
      kprintf("serve: file read failed\n");
      break;
    }
    if (tcp_send(t, g_chunk, (uint32_t)r) < 0) {
      kprintf("serve: send failed\n");
      break;
    }
    pos += (uint32_t)r;
  }
  kprintf("serve: sent %u bytes\n", (unsigned)pos);
  tcp_close(t);
  return (pos == size) ? 0 : -1;
}
