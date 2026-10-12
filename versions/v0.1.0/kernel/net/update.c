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
