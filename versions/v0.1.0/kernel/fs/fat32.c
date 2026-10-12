/* NaOS FAT32 reader. Assumes 512-byte sectors (verified on our ESP image). */
#include "fat32.h"
#include "../drv/blk.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F
#define EOC_MIN 0x0FFFFFF8u

typedef struct {
  uint8_t name[11];
  uint8_t attr;
  uint8_t nt_res;
  uint8_t crt_tenth;
  uint16_t crt_time;
  uint16_t crt_date;
  uint16_t acc_date;
  uint16_t clus_hi;
  uint16_t wrt_time;
  uint16_t wrt_date;
  uint16_t clus_lo;
  uint32_t size;
} __attribute__((packed)) dirent_t;

static int g_ok;
static uint32_t g_fat_start;   /* LBA of FAT #0 */
static uint32_t g_fat_sectors; /* sectors per FAT */
static uint32_t g_fat_count;   /* number of FATs */
static uint32_t g_data_start;  /* LBA of cluster 2 */
static uint32_t g_spc;         /* sectors per cluster (1 on our image) */
static uint32_t g_root_clus;
static uint32_t g_alloc_hint = 2;

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fat_init(void) {
  static uint8_t sec[512];
  g_ok = 0;
  if (blk_read(0, 1, sec) != 0) {
    kprintf("fat: cannot read boot sector\n");
    return -1;
  }
  if (sec[510] != 0x55 || sec[511] != 0xAA) {
    kprintf("fat: bad boot signature\n");
    return -1;
  }
  uint16_t bps = rd16(sec + 11);
  if (bps != 512) {
    kprintf("fat: unsupported sector size %u\n", (unsigned)bps);
    return -1;
  }
  g_spc = sec[13];
  uint16_t rsvd = rd16(sec + 14);
  uint8_t fats = sec[16];
  uint32_t fatsz = rd32(sec + 36);
  uint32_t tot = rd32(sec + 32);
  g_root_clus = rd32(sec + 44);
  if (g_spc == 0 || fats == 0 || fatsz == 0 || tot == 0) {
    kprintf("fat: not FAT32\n");
    return -1;
  }
  g_fat_start = rsvd;
  g_fat_sectors = fatsz;
  g_fat_count = fats;
  g_data_start = rsvd + fats * fatsz;
  g_ok = 1;
  kprintf("fat32: spc=%u fat@%u data@%u root=%u tot=%u\n",
          (unsigned)g_spc, (unsigned)g_fat_start, (unsigned)g_data_start,
          (unsigned)g_root_clus, (unsigned)tot);
  return 0;
}

int fat_is_ready(void) { return g_ok; }

static uint32_t clus_to_lba(uint32_t clus) {
  return g_data_start + (clus - 2) * g_spc;
}

/* Next cluster in chain, or 0 on end/error. */
static uint32_t fat_next(uint32_t clus) {
  static uint8_t sec[512];
  uint32_t off = clus * 4u;
  uint32_t lba = g_fat_start + off / 512u;
  uint32_t idx = off % 512u;
  if (blk_read(lba, 1, sec) != 0) return 0;
  uint32_t v = rd32(sec + idx) & 0x0FFFFFFFu;
  if (v < 2 || v >= EOC_MIN) return 0;
  return v;
}

/* Uppercase a path component in place (ASCII only). */
static void str_up(char *s) {
  while (*s) {
    if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
    s++;
  }
}

/* Format "NAME.EXT" (or "NAME") into FAT 8+3 padded field. */
static void to_83(const char *in, uint8_t out[11]) {
  for (int i = 0; i < 11; i++) out[i] = ' ';
  int i = 0;
  while (*in && *in != '.' && i < 8) out[i++] = (uint8_t)*in++;
  if (*in == '.') {
    in++;
    int j = 8;
    while (*in && j < 11) out[j++] = (uint8_t)*in++;
  }
}

/* ---- Long filenames (read-only) ----
 * LFN chunks (attr 0x0F) precede their short entry, last chunk first
 * (seq | 0x40). We collect UTF-16 units, then render ASCII ('?' beyond).
 * A chain is only trusted when complete, ordered, and its checksum
 * matches the short entry. Write paths still use short names only. */
#define LFN_CHUNKS_MAX 20
static uint16_t g_lfn_u[LFN_CHUNKS_MAX * 13];
static int g_lfn_total; /* chunks in chain, 0 = none */
static int g_lfn_need;  /* next sequence number expected, -1 = idle */
static uint8_t g_lfn_sum;

static void lfn_reset(void) {
  g_lfn_total = 0;
  g_lfn_need = -1;
}

static uint8_t sum_83(const uint8_t name[11]) {
  uint8_t s = 0;
  for (int i = 0; i < 11; i++)
    s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + name[i]);
  return s;
}

/* Feed one raw 32-byte entry. Returns 1 if it was an LFN chunk. */
static int lfn_feed(const uint8_t *e) {
  static const uint8_t off[13] =
    { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
  int k, seq;
  if (e[11] != ATTR_LFN) return 0;
  if (e[0] == 0xE5 || e[0] == 0x00) { lfn_reset(); return 1; }
  seq = e[0] & 0x1F;
  if (seq == 0 || seq > LFN_CHUNKS_MAX) { lfn_reset(); return 1; }
  if (e[0] & 0x40) {
    g_lfn_total = seq;
    g_lfn_need = seq - 1;
    g_lfn_sum = e[13];
  } else {
    if (g_lfn_total == 0 || seq != g_lfn_need || e[13] != g_lfn_sum) {
      lfn_reset();
      return 1;
    }
    g_lfn_need = seq - 1;
  }
  for (k = 0; k < 13; k++)
    g_lfn_u[(seq - 1) * 13 + k] =
      (uint16_t)(e[off[k]] | (e[off[k] + 1] << 8));
  return 1;
}

/* Render the collected LFN for short name `name83` into out.
 * Returns length, or 0 when no complete valid chain is pending. */
static int lfn_get(const uint8_t name83[11], char *out, int outsz) {
  int i, n = 0;
  if (g_lfn_total == 0 || g_lfn_need != 0) return 0;
  if (sum_83(name83) != g_lfn_sum) return 0;
  for (i = 0; i < g_lfn_total * 13 && n + 1 < outsz; i++) {
    uint16_t u = g_lfn_u[i];
    if (u == 0x0000) break;
    if (u == 0xFFFF) continue;
    out[n++] = (u < 0x80) ? (char)u : '?';
  }
  out[n] = '\0';
  return n;
}

/* Find entry `name83` in directory starting at dir_clus. 0 ok.
 * Slot position (for updates) returned via slot_lba/slot_off if non-NULL. */
static int dir_find(uint32_t dir_clus, const uint8_t name83[11],
                    uint32_t *clus_out, uint32_t *size_out, uint8_t *attr_out,
                    uint32_t *slot_lba, uint32_t *slot_off) {
  static uint8_t sec[512];
  uint32_t clus = dir_clus;
  while (clus >= 2) {
    for (uint32_t s = 0; s < g_spc; s++) {
      uint32_t lba = clus_to_lba(clus) + s;
      if (blk_read(lba, 1, sec) != 0) return -1;
      for (int e = 0; e < 16; e++) {
        const dirent_t *d = (const dirent_t *)(sec + e * 32);
        if (d->name[0] == 0x00) return -1; /* end of dir */
        if (d->name[0] == 0xE5) continue;  /* deleted */
        if (d->attr == ATTR_LFN) continue;
        if (d->attr == 0x08) continue;     /* volume label */
        if (naos_memcmp(d->name, name83, 11) == 0) {
          *clus_out = ((uint32_t)d->clus_hi << 16) | d->clus_lo;
          *size_out = d->size;
          *attr_out = d->attr;
          if (slot_lba) *slot_lba = lba;
          if (slot_off) *slot_off = (uint32_t)e * 32;
          return 0;
        }
      }
    }
    uint32_t nx = fat_next(clus);
    if (nx == 0) return -1;
    clus = nx;
  }
  return -1;
}

/* Find entry by long or short name. comp_up is the uppercased path
 * component (up to 255 chars); name83 is its 8+3 form for fallback. */
static int dir_find_name(uint32_t dir_clus, const char *comp_up,
                         const uint8_t name83[11],
                         uint32_t *clus_out, uint32_t *size_out,
                         uint8_t *attr_out) {
  static uint8_t sec[512];
  uint32_t clus = dir_clus;
  lfn_reset();
  while (clus >= 2) {
    for (uint32_t s = 0; s < g_spc; s++) {
      uint32_t lba = clus_to_lba(clus) + s;
      if (blk_read(lba, 1, sec) != 0) return -1;
      for (int e = 0; e < 16; e++) {
        const dirent_t *d = (const dirent_t *)(sec + e * 32);
        const uint8_t *raw = sec + e * 32;
        int match = 0;
        if (d->name[0] == 0x00) return -1; /* end of dir */
        if (d->name[0] == 0xE5) { lfn_reset(); continue; }
        if (lfn_feed(raw)) continue;
        if (d->attr == 0x08) { lfn_reset(); continue; }
        {
          char lfn[256];
          if (lfn_get(d->name, lfn, sizeof(lfn)) > 0) {
            str_up(lfn);
            if (naos_strcmp(lfn, comp_up) == 0) match = 1;
          }
        }
        lfn_reset(); /* chain consumed either way */
        if (!match && naos_memcmp(d->name, name83, 11) == 0) match = 1;
        if (match) {
          *clus_out = ((uint32_t)d->clus_hi << 16) | d->clus_lo;
          *size_out = d->size;
          *attr_out = d->attr;
          return 0;
        }
      }
    }
    uint32_t nx = fat_next(clus);
    if (nx == 0) return -1;
    clus = nx;
  }
  return -1;
}

/* Resolve path to (cluster, size, attr). "" / "/" -> root dir. */
static int resolve(const char *path, uint32_t *clus_out, uint32_t *size_out,
                   uint8_t *attr_out) {
  while (*path == '/' || *path == '\\') path++;
  if (*path == '\0') {
    *clus_out = g_root_clus;
    *size_out = 0;
    *attr_out = ATTR_DIR;
    return 0;
  }
  uint32_t dir = g_root_clus;
  char comp[256];
  while (*path) {
    int n = 0;
    while (*path && *path != '/' && *path != '\\' && n < 255)
      comp[n++] = *path++;
    comp[n] = '\0';
    while (*path == '/' || *path == '\\') path++;
    if (n == 0) continue;
    str_up(comp);
    uint8_t f83[11];
    to_83(comp, f83);
    uint32_t c, sz;
    uint8_t at;
    if (dir_find_name(dir, comp, f83, &c, &sz, &at) != 0) return -1;
    if (*path == '\0') {
      *clus_out = c;
      *size_out = sz;
      *attr_out = at;
      return 0;
    }
    if (!(at & ATTR_DIR)) return -1; /* middle component must be a dir */
    dir = (c == 0) ? g_root_clus : c;
  }
  return -1;
}

int fat_ls(const char *path) {
  static uint8_t sec[512];
  if (!g_ok) { kprintf("fat: not ready\n"); return -1; }
  uint32_t clus, size;
  uint8_t attr;
  if (resolve(path ? path : "", &clus, &size, &attr) != 0) {
    kprintf("ls: not found: %s\n", path ? path : "");
    return -1;
  }
  if (!(attr & ATTR_DIR)) {
    kprintf("ls: not a directory: %s\n", path);
    return -1;
  }
  uint32_t c = clus;
  uint32_t count = 0;
  lfn_reset();
  while (c >= 2) {
    for (uint32_t s = 0; s < g_spc; s++) {
      if (blk_read(clus_to_lba(c) + s, 1, sec) != 0) return -1;
      for (int e = 0; e < 16; e++) {
        const dirent_t *d = (const dirent_t *)(sec + e * 32);
        if (d->name[0] == 0x00) goto done;
        if (d->name[0] == 0xE5) { lfn_reset(); continue; }
        if (lfn_feed(sec + e * 32)) continue;
        if (d->attr == 0x08) { lfn_reset(); continue; } /* volume label */
        char name[256];
        if (lfn_get(d->name, name, sizeof(name)) <= 0) {
          int n = 0;
          for (int i = 0; i < 8 && d->name[i] != ' '; i++)
            name[n++] = (char)d->name[i];
          if (!(d->attr & ATTR_DIR)) {
            int m = 0;
            char ext[4];
            for (int i = 8; i < 11 && d->name[i] != ' '; i++) ext[m++] = (char)d->name[i];
            if (m > 0) {
              name[n++] = '.';
              for (int i = 0; i < m; i++) name[n++] = ext[i];
            }
          }
          name[n] = '\0';
        }
        lfn_reset(); /* chain consumed */
        if (d->attr & ATTR_DIR)
          kprintf("  %s/ <DIR>\n", name);
        else
          kprintf("  %s %u bytes\n", name, (unsigned)d->size);
        count++;
      }
    }
    c = fat_next(c);
  }
done:
  kprintf("%u entries\n", (unsigned)count);
  return 0;
}

/* ---- write support ---- */

static uint32_t fat_get(uint32_t clus) {
  static uint8_t sec[512];
  uint32_t off = clus * 4u;
  if (blk_read(g_fat_start + off / 512u, 1, sec) != 0) return 0;
  return rd32(sec + (off % 512u)) & 0x0FFFFFFFu;
}

static int fat_set(uint32_t clus, uint32_t val) {
  static uint8_t sec[512];
  uint32_t off = clus * 4u;
  uint32_t rel = off / 512u;
  uint32_t idx = off % 512u;
  val &= 0x0FFFFFFFu;
  for (uint32_t f = 0; f < g_fat_count; f++) {
    uint32_t lba = g_fat_start + f * g_fat_sectors + rel;
    if (blk_read(lba, 1, sec) != 0) return -1;
    sec[idx] = (uint8_t)(val & 0xFF);
    sec[idx + 1] = (uint8_t)((val >> 8) & 0xFF);
    sec[idx + 2] = (uint8_t)((val >> 16) & 0xFF);
    sec[idx + 3] = (uint8_t)((sec[idx + 3] & 0xF0) | ((val >> 24) & 0x0F));
    if (blk_write(lba, 1, sec) != 0) return -1;
  }
  return 0;
}

/* Allocate one zeroed cluster (data or dir). 0 = none free. */
static uint32_t fat_alloc(void) {
  static uint8_t zero[512];
  uint32_t total = (g_fat_sectors * 512u) / 4u;
  for (uint32_t pass = 0; pass < 2; pass++) {
    uint32_t start = (pass == 0) ? g_alloc_hint : 2;
    uint32_t end = (pass == 0) ? total : g_alloc_hint;
    for (uint32_t c = start; c < end; c++) {
      if (fat_get(c) != 0) continue;
      if (fat_set(c, EOC_MIN) != 0) return 0;
      g_alloc_hint = c + 1;
      /* Zero its sectors. */
      naos_memset(zero, 0, sizeof(zero));
      for (uint32_t s = 0; s < g_spc; s++)
        if (blk_write(clus_to_lba(c) + s, 1, zero) != 0) {
          fat_set(c, 0);
          return 0;
        }
      return c;
    }
  }
  return 0;
}

static void free_chain(uint32_t clus) {
  while (clus >= 2) {
    uint32_t v = fat_get(clus);
    fat_set(clus, 0);
    if (v < 2 || v >= EOC_MIN) break;
    clus = v;
  }
}

/* Split path into parent dir cluster + 8.3 leaf. Returns 0 ok. */
static int resolve_parent(const char *path, uint32_t *parent_out,
                          uint8_t leaf83[11]) {
  while (*path == '/' || *path == '\\') path++;
  /* Find last separator. */
  const char *last = 0;
  for (const char *p = path; *p; p++)
    if (*p == '/' || *p == '\\') last = p;
  char leaf[16];
  uint32_t dir = g_root_clus;
  if (last) {
    /* Walk to the parent directory. */
    const char *p = path;
    while (p < last) {
      char comp[256];
      int n = 0;
      while (p < last && *p != '/' && *p != '\\' && n < 255)
        comp[n++] = *p++;
      comp[n] = '\0';
      while (p < last && (*p == '/' || *p == '\\')) p++;
      if (n == 0) continue;
      str_up(comp);
      uint8_t f83[11];
      to_83(comp, f83);
      uint32_t c, sz;
      uint8_t at;
      if (dir_find_name(dir, comp, f83, &c, &sz, &at) != 0) return -1;
      if (!(at & ATTR_DIR)) return -1;
      dir = (c == 0) ? g_root_clus : c;
    }
    path = last + 1;
  }
  int n = 0;
  while (*path && n < 15) leaf[n++] = *path++;
  leaf[n] = '\0';
  if (n == 0) return -1;
  if ((leaf[0] == '.' && leaf[1] == '\0') ||
      (leaf[0] == '.' && leaf[1] == '.' && leaf[2] == '\0'))
    return -2; /* refuse . and .. */
  str_up(leaf);
  to_83(leaf, leaf83);
  *parent_out = dir;
  return 0;
}

/* Find a free dir slot, extending the directory if needed. */
static int dir_alloc_slot(uint32_t dir, uint32_t *lba_out, uint32_t *off_out) {
  static uint8_t sec[512];
  uint32_t clus = dir;
  uint32_t prev = 0;
  while (clus >= 2) {
    for (uint32_t s = 0; s < g_spc; s++) {
      uint32_t lba = clus_to_lba(clus) + s;
      if (blk_read(lba, 1, sec) != 0) return -1;
      for (int e = 0; e < 16; e++) {
        if (sec[e * 32] == 0x00 || sec[e * 32] == 0xE5) {
          *lba_out = lba;
          *off_out = (uint32_t)e * 32;
          return 0;
        }
      }
    }
    prev = clus;
    uint32_t nx = fat_next(clus);
    if (nx == 0) break;
    clus = nx;
  }
  /* Full: append a cluster. */
  uint32_t nc = fat_alloc();
  if (nc == 0) return -2;
  if (fat_set(prev, nc) != 0) { fat_set(nc, 0); return -1; }
  if (fat_set(nc, EOC_MIN) != 0) return -1;
  *lba_out = clus_to_lba(nc);
  *off_out = 0;
  return 0;
}

static int write_entry(uint32_t lba, uint32_t off, const uint8_t name83[11],
                       uint8_t attr, uint32_t clus, uint32_t size) {
  static uint8_t sec[512];
  if (blk_read(lba, 1, sec) != 0) return -1;
  dirent_t *d = (dirent_t *)(sec + off);
  naos_memcpy(d->name, name83, 11);
  d->attr = attr;
  d->nt_res = 0;
  d->crt_tenth = 0;
  d->crt_time = 0;
  d->crt_date = 0;
  d->acc_date = 0;
  d->clus_hi = (uint16_t)((clus >> 16) & 0xFFFFu);
  d->wrt_time = 0;
  d->wrt_date = 0;
  d->clus_lo = (uint16_t)(clus & 0xFFFFu);
  d->size = size;
  return blk_write(lba, 1, sec);
}

int fat_write(const char *path, const void *buf, uint32_t len) {
  uint32_t parent;
  uint8_t leaf[11];
  if (!g_ok || !path || (!buf && len > 0)) return -1;
  if (resolve_parent(path, &parent, leaf) != 0) return -2;
  return fat_write_parts(parent, leaf, buf, len);
}

/* Append bytes to a file (created when missing). One call = one consistent
 * update: data lands first, the directory entry (first cluster + new size)
 * is written last. 0 ok. */
int fat_append(const char *path, const void *buf, uint32_t len) {
  static uint8_t sec[512];
  uint32_t parent, slot_lba = 0, slot_off = 0;
  uint8_t leaf[11];
  uint32_t clus = 0, size = 0;
  uint8_t attr = 0;
  uint32_t first, cur, off, left, written = 0;
  uint32_t per_clus = 512u * g_spc;
  const uint8_t *src = (const uint8_t *)buf;
  int have_old;
  if (!g_ok || !path || (!buf && len > 0)) return -1;
  if (resolve_parent(path, &parent, leaf) != 0) return -2;
  have_old = (dir_find(parent, leaf, &clus, &size, &attr, &slot_lba,
                       &slot_off) == 0);
  if (have_old && (attr & ATTR_DIR)) return -6;
  if (len == 0) {
    if (have_old) return 0; /* touch existing: nothing to do */
    if (dir_alloc_slot(parent, &slot_lba, &slot_off) != 0) return -5;
    return write_entry(slot_lba, slot_off, leaf, 0x20, 0, 0);
  }
  if (!have_old) {
    if (dir_alloc_slot(parent, &slot_lba, &slot_off) != 0) return -5;
    clus = 0;
    size = 0;
  }
  /* Walk to the last cluster of the current chain (if any). */
  first = clus;
  cur = clus;
  if (cur >= 2) {
    uint32_t nx;
    while ((nx = fat_next(cur)) >= 2 && nx < EOC_MIN) cur = nx;
  }
  off = (first >= 2) ? (size % per_clus) : 0;
  if (first < 2 || (size > 0 && off == 0)) {
    /* Empty file, or last cluster exactly full: open a new one. */
    uint32_t nc = fat_alloc();
    if (nc == 0) goto done;
    if (cur >= 2 && fat_set(cur, nc) != 0) {
      fat_set(nc, 0);
      goto done;
    }
    if (first < 2) first = nc;
    cur = nc;
    off = 0;
  }
  left = len;
  while (left > 0) {
    uint32_t room = per_clus - off;
    uint32_t fill = (left < room) ? left : room;
    uint32_t done = 0;
    while (done < fill) {
      uint32_t sno = (off + done) / 512u;
      uint32_t soff = (off + done) % 512u;
      uint32_t take = 512u - soff;
      if (take > fill - done) take = fill - done;
      if (blk_read(clus_to_lba(cur) + sno, 1, sec) != 0) goto done;
      naos_memcpy(sec + soff, src + done, take);
      if (blk_write(clus_to_lba(cur) + sno, 1, sec) != 0) goto done;
      done += take;
    }
    src += fill;
    left -= fill;
    written += fill;
    off += fill;
    if (left > 0) {
      uint32_t nc = fat_alloc();
      if (nc == 0) goto done;
      if (fat_set(cur, nc) != 0) {
        fat_set(nc, 0);
        goto done;
      }
      cur = nc;
      off = 0;
    }
  }
done:
  /* Single entry update (also repairs size after a partial failure). */
  if (write_entry(slot_lba, slot_off, leaf, 0x20, first, size + written) != 0)
    return -4;
  return (written == len) ? 0 : -3;
}

/* Write (create-or-truncate) by parent cluster + 8.3 leaf. Shared by
 * path writes and file-descriptor flushes. */
int fat_write_parts(uint32_t parent, const uint8_t leaf83[11],
                    const void *buf, uint32_t len) {
  static uint8_t sec[512];
  uint32_t old_clus = 0, old_size = 0, slot_lba = 0, slot_off = 0;
  uint8_t old_attr = 0;
  int have_old;
  uint32_t first = 0, prev = 0;
  uint32_t left = len;
  const uint8_t *src = (const uint8_t *)buf;
  uint32_t per_clus = 512u * g_spc;
  if (!g_ok || (!buf && len > 0)) return -1;
  /* Existing entry? Truncate it (dirs cannot be overwritten). */
  have_old = (dir_find(parent, leaf83, &old_clus, &old_size, &old_attr,
                       &slot_lba, &slot_off) == 0);
  if (have_old && (old_attr & ATTR_DIR)) return -6;
  if (have_old && old_clus >= 2) free_chain(old_clus);
  /* Allocate chain for the new content. */
  while (left > 0) {
    uint32_t nc = fat_alloc();
    if (nc == 0) { /* OOM: roll back partial chain */
      if (first >= 2) free_chain(first);
      return -3;
    }
    if (prev >= 2 && fat_set(prev, nc) != 0) {
      fat_set(nc, 0);
      if (first >= 2) free_chain(first);
      return -3;
    }
    if (first == 0) first = nc;
    prev = nc;
    uint32_t fill = (left > per_clus) ? per_clus : left;
    uint32_t done = 0;
    for (uint32_t s = 0; s < g_spc && done < fill; s++) {
      uint32_t take = fill - done;
      if (take > 512u) take = 512u;
      naos_memcpy(sec, src + done, take);
      if (take < 512u) naos_memset(sec + take, 0, 512u - take);
      if (blk_write(clus_to_lba(nc) + s, 1, sec) != 0) {
        if (first >= 2) free_chain(first);
        return -4;
      }
      done += take;
    }
    src += fill;
    left -= fill;
  }
  if (len > 0 && first == 0) return -3;
  if (!have_old) {
    if (dir_alloc_slot(parent, &slot_lba, &slot_off) != 0) {
      if (first >= 2) free_chain(first);
      return -5;
    }
  }
  return write_entry(slot_lba, slot_off, leaf83, 0x20, first, len);
}

int fat_mkdir(const char *path) {
  static uint8_t sec[512];
  if (!g_ok || !path) return -1;
  uint32_t parent;
  uint8_t leaf[11];
  if (resolve_parent(path, &parent, leaf) != 0) return -2;
  uint32_t dummy_c, dummy_s, slot_lba, slot_off;
  uint8_t dummy_a;
  if (dir_find(parent, leaf, &dummy_c, &dummy_s, &dummy_a, 0, 0) == 0)
    return -6; /* exists */
  uint32_t nc = fat_alloc();
  if (nc == 0) return -3;
  if (dir_alloc_slot(parent, &slot_lba, &slot_off) != 0) {
    fat_set(nc, 0);
    return -5;
  }
  /* "." and ".." entries. */
  if (blk_read(clus_to_lba(nc), 1, sec) != 0) { fat_set(nc, 0); return -4; }
  dirent_t *dot = (dirent_t *)sec;
  naos_memset(dot->name, ' ', 11);
  dot->name[0] = '.';
  dot->attr = ATTR_DIR;
  dot->clus_hi = (uint16_t)((nc >> 16) & 0xFFFFu);
  dot->clus_lo = (uint16_t)(nc & 0xFFFFu);
  dot->size = 0;
  dirent_t *dotdot = (dirent_t *)(sec + 32);
  naos_memset(dotdot->name, ' ', 11);
  dotdot->name[0] = '.';
  dotdot->name[1] = '.';
  dotdot->attr = ATTR_DIR;
  dotdot->clus_hi = (uint16_t)((parent >> 16) & 0xFFFFu);
  dotdot->clus_lo = (uint16_t)(parent & 0xFFFFu);
  dotdot->size = 0;
  if (blk_write(clus_to_lba(nc), 1, sec) != 0) { fat_set(nc, 0); return -4; }
  return write_entry(slot_lba, slot_off, leaf, ATTR_DIR, nc, 0);
}

int fat_delete(const char *path) {
  static uint8_t sec[512];
  if (!g_ok || !path) return -1;
  uint32_t parent;
  uint8_t leaf[11];
  if (resolve_parent(path, &parent, leaf) != 0) return -2;
  uint32_t clus, size, slot_lba, slot_off;
  uint8_t attr;
  if (dir_find(parent, leaf, &clus, &size, &attr, &slot_lba, &slot_off) != 0)
    return -2;
  if (attr & ATTR_DIR) {
    /* Refuse non-empty directories (allow only . and ..). */
    uint32_t c = (clus == 0) ? g_root_clus : clus;
    if (clus == 0) return -6; /* never delete the root */
    while (c >= 2) {
      for (uint32_t s = 0; s < g_spc; s++) {
        if (blk_read(clus_to_lba(c) + s, 1, sec) != 0) return -4;
        for (int e = 0; e < 16; e++) {
          const dirent_t *d = (const dirent_t *)(sec + e * 32);
          if (d->name[0] == 0x00) goto checked;
          if (d->name[0] == 0xE5 || d->attr == ATTR_LFN || d->attr == 0x08)
            continue;
          if (d->name[0] == '.' &&
              (d->name[1] == ' ' || (d->name[1] == '.' && d->name[2] == ' ')))
            continue;
          return -7; /* non-empty */
        }
      }
      c = fat_next(c);
    }
  checked:;
  }
  if (blk_read(slot_lba, 1, sec) != 0) return -4;
  sec[slot_off] = 0xE5;
  if (blk_write(slot_lba, 1, sec) != 0) return -4;
  if (clus >= 2) free_chain(clus);
  return 0;
}

#define COPY_MAX (128u * 1024u)

static uint8_t s_copybuf[COPY_MAX];

/* Copy one file by cluster+size into a new entry. */
static int copy_one_file(uint32_t src_clus, uint32_t src_size,
                         uint32_t dst_parent, const uint8_t dst83[11],
                         uint32_t *wrote_out) {
  if (src_size > COPY_MAX) return -9;
  /* Read whole source through the chain. */
  uint32_t got = 0;
  uint32_t c = src_clus;
  static uint8_t sec[512];
  while (got < src_size && c >= 2) {
    for (uint32_t s = 0; s < g_spc && got < src_size; s++) {
      if (blk_read(clus_to_lba(c) + s, 1, sec) != 0) return -4;
      uint32_t chunk = src_size - got;
      if (chunk > 512u) chunk = 512u;
      naos_memcpy(s_copybuf + got, sec, chunk);
      got += chunk;
    }
    if (got < src_size) {
      c = fat_next(c);
      if (c == 0) return -5;
    }
  }
  if (got != src_size) return -5;
  uint32_t slot_lba, slot_off;
  if (dir_alloc_slot(dst_parent, &slot_lba, &slot_off) != 0) return -5;
  /* Store via the same layout fat_write uses. */
  uint32_t first = 0, prev = 0;
  uint32_t left = src_size;
  const uint8_t *sp = s_copybuf;
  uint32_t per_clus = 512u * g_spc;
  while (left > 0) {
    uint32_t nc = fat_alloc();
    if (nc == 0) {
      if (first >= 2) free_chain(first);
      return -3;
    }
    if (prev >= 2 && fat_set(prev, nc) != 0) {
      fat_set(nc, 0);
      if (first >= 2) free_chain(first);
      return -3;
    }
    if (first == 0) first = nc;
    prev = nc;
    uint32_t fill = (left > per_clus) ? per_clus : left;
    uint32_t done = 0;
    for (uint32_t s = 0; s < g_spc && done < fill; s++) {
      uint32_t take = fill - done;
      if (take > 512u) take = 512u;
      naos_memcpy(sec, sp + done, take);
      if (take < 512u) naos_memset(sec + take, 0, 512u - take);
      if (blk_write(clus_to_lba(nc) + s, 1, sec) != 0) {
        if (first >= 2) free_chain(first);
        return -4;
      }
      done += take;
    }
    sp += fill;
    left -= fill;
  }
  if (src_size > 0 && first == 0) return -3;
  if (write_entry(slot_lba, slot_off, dst83, 0x20, first, src_size) != 0) {
    if (first >= 2) free_chain(first);
    return -4;
  }
  if (wrote_out) *wrote_out = src_size;
  return 0;
}

/* Join parent + '/' + leaf into out (bounded). Returns 0 ok. */
static int join_path(const char *parent, const char *leaf,
                     char *out, size_t outsz) {
  size_t n = 0;
  while (parent[n] && n + 1 < outsz) { out[n] = parent[n]; n++; }
  if (n == 0 || out[n - 1] != '/') {
    if (n + 1 >= outsz) return -1;
    out[n++] = '/';
  }
  size_t k = 0;
  while (leaf[k] && n + 1 < outsz) { out[n++] = leaf[k++]; }
  if (leaf[k] != '\0') return -1;
  out[n] = '\0';
  return 0;
}

/* Recursive worker. Files use exact src_size; dirs walk their own
 * entries (cluster/size/attr come straight from the dirents). */
static int copy_tree(uint32_t src_clus, uint32_t src_size, uint8_t src_attr,
                     uint32_t dst_parent, const uint8_t dst83[11],
                     const char *dst_path,
                     int depth, uint32_t *total_out) {
  if (depth > 8) return -10;
  if (!(src_attr & ATTR_DIR)) {
    uint32_t w = 0;
    int r = copy_one_file(src_clus, src_size, dst_parent, dst83, &w);
    if (r == 0 && total_out) *total_out += w;
    return r;
  }
  /* Make the destination directory (with . and ..). */
  uint32_t slot_lba, slot_off;
  if (dir_alloc_slot(dst_parent, &slot_lba, &slot_off) != 0) return -5;
  uint32_t nc = fat_alloc();
  if (nc == 0) return -3;
  {
    static uint8_t sec[512];
    if (blk_read(clus_to_lba(nc), 1, sec) != 0) { fat_set(nc, 0); return -4; }
    dirent_t *dot = (dirent_t *)sec;
    naos_memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = ATTR_DIR;
    dot->clus_hi = (uint16_t)((nc >> 16) & 0xFFFFu);
    dot->clus_lo = (uint16_t)(nc & 0xFFFFu);
    dot->size = 0;
    dirent_t *dotdot = (dirent_t *)(sec + 32);
    naos_memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = ATTR_DIR;
    dotdot->clus_hi = (uint16_t)((dst_parent >> 16) & 0xFFFFu);
    dotdot->clus_lo = (uint16_t)(dst_parent & 0xFFFFu);
    dotdot->size = 0;
    if (blk_write(clus_to_lba(nc), 1, sec) != 0) { fat_set(nc, 0); return -4; }
  }
  if (write_entry(slot_lba, slot_off, dst83, ATTR_DIR, nc, 0) != 0) {
    fat_set(nc, 0);
    return -4;
  }
  /* Walk the source directory and copy each child by dirent data. */
  {
    static uint8_t esec[512];
    char child_dst[256];
    uint32_t c = src_clus;
    while (c >= 2) {
      for (uint32_t s = 0; s < g_spc; s++) {
        if (blk_read(clus_to_lba(c) + s, 1, esec) != 0) return -4;
        for (int e = 0; e < 16; e++) {
          const dirent_t *d = (const dirent_t *)(esec + e * 32);
          if (d->name[0] == 0x00) goto walked;
          if (d->name[0] == 0xE5 || d->attr == ATTR_LFN || d->attr == 0x08)
            continue;
          if (d->name[0] == '.' &&
              (d->name[1] == ' ' || (d->name[1] == '.' && d->name[2] == ' ')))
            continue;
          char nm[13];
          int n = 0;
          for (int i = 0; i < 8 && d->name[i] != ' '; i++) nm[n++] = (char)d->name[i];
          if (!(d->attr & ATTR_DIR)) {
            int m = 0;
            char ext[4];
            for (int i = 8; i < 11 && d->name[i] != ' '; i++) ext[m++] = (char)d->name[i];
            if (m > 0) {
              nm[n++] = '.';
              for (int i = 0; i < m; i++) nm[n++] = ext[i];
            }
          }
          nm[n] = '\0';
          uint8_t child83[11];
          naos_memcpy(child83, d->name, 11);
          if (join_path(dst_path, nm, child_dst, sizeof(child_dst)) != 0)
            return -10;
          uint32_t cc = ((uint32_t)d->clus_hi << 16) | d->clus_lo;
          int r = copy_tree(cc, d->size, d->attr, nc, child83,
                            child_dst, depth + 1, total_out);
          if (r != 0) return r;
        }
      }
      c = fat_next(c);
    }
  walked:;
  }
  return 0;
}

int fat_copy(const char *src, const char *dst) {
  if (!g_ok || !src || !dst) return -1;
  uint32_t sc, ss;
  uint8_t sa;
  if (resolve(src, &sc, &ss, &sa) != 0) return -2;
  uint32_t dp;
  uint8_t leaf[11];
  if (resolve_parent(dst, &dp, leaf) != 0) return -2;
  uint32_t dc, ds;
  uint8_t da;
  if (dir_find(dp, leaf, &dc, &ds, &da, 0, 0) == 0) return -6; /* exists */
  if ((sa & ATTR_DIR)) {
    /* Refuse to copy a folder into itself: walk dst_parent's .. chain. */
    uint32_t up = dp;
    for (int i = 0; i < 32; i++) {
      if (up == sc) return -7;
      if (up == g_root_clus) break;
      uint8_t dotdot[11];
      dotdot[0] = '.';
      dotdot[1] = '.';
      for (int k = 2; k < 11; k++) dotdot[k] = ' ';
      uint32_t pc, ps;
      uint8_t pa;
      if (dir_find(up, dotdot, &pc, &ps, &pa, 0, 0) != 0) break;
      if (pc == 0) break; /* parent is root */
      up = pc;
    }
    if (up == sc) return -7;
  }
  uint32_t total = 0;
  int r = copy_tree(sc, ss, sa, dp, leaf, dst, 0, &total);
  if (r != 0) return r;
  return (total <= 0x7FFFFFFFu) ? (int)total : 0x7FFFFFFF;
}

int fat_rename(const char *oldpath, const char *newpath) {
  static uint8_t sec[512];
  if (!g_ok || !oldpath || !newpath) return -1;
  uint32_t oparent;
  uint8_t old83[11];
  if (resolve_parent(oldpath, &oparent, old83) != 0) return -2;
  uint32_t oclus, osize, oslot_lba, oslot_off;
  uint8_t oattr;
  if (dir_find(oparent, old83, &oclus, &osize, &oattr,
               &oslot_lba, &oslot_off) != 0)
    return -2;
  uint32_t nparent;
  uint8_t new83[11];
  if (resolve_parent(newpath, &nparent, new83) != 0) return -2;
  uint32_t dc, ds;
  uint8_t da;
  if (dir_find(nparent, new83, &dc, &ds, &da, 0, 0) == 0) return -6;
  if (nparent == oparent) {
    /* Same folder: rewrite the name in place (files and full folders). */
    if (blk_read(oslot_lba, 1, sec) != 0) return -4;
    naos_memcpy(sec + oslot_off, new83, 11);
    if (blk_write(oslot_lba, 1, sec) != 0) return -4;
    return 0;
  }
  /* Different folder: re-link the entry (no data moves). Folders get
   * their ".." repointed; refuse to move a folder into itself. */
  if (oattr & ATTR_DIR) {
    uint32_t up = nparent;
    for (int i = 0; i < 32; i++) {
      if (up == oclus) return -7;
      if (up == g_root_clus) break;
      uint8_t dotdot[11];
      dotdot[0] = '.';
      dotdot[1] = '.';
      for (int k = 2; k < 11; k++) dotdot[k] = ' ';
      uint32_t pc, ps;
      uint8_t pa;
      if (dir_find(up, dotdot, &pc, &ps, &pa, 0, 0) != 0) break;
      if (pc == 0) break; /* parent is root */
      up = pc;
    }
    if (up == oclus) return -7;
  }
  {
    uint32_t slot_lba, slot_off;
    if (dir_alloc_slot(nparent, &slot_lba, &slot_off) != 0) return -5;
    if (blk_read(slot_lba, 1, sec) != 0) return -4;
    dirent_t *d = (dirent_t *)(sec + slot_off);
    naos_memcpy(d->name, new83, 11);
    d->attr = oattr;
    d->nt_res = 0;
    d->crt_tenth = 0;
    d->crt_time = 0;
    d->crt_date = 0;
    d->acc_date = 0;
    d->clus_hi = (uint16_t)((oclus >> 16) & 0xFFFFu);
    d->wrt_time = 0;
    d->wrt_date = 0;
    d->clus_lo = (uint16_t)(oclus & 0xFFFFu);
    d->size = osize;
    if (blk_write(slot_lba, 1, sec) != 0) return -4;
  }
  if (oattr & ATTR_DIR) {
    /* Repoint the moved folder's ".." at its new parent. */
    uint32_t c = (oclus == 0) ? g_root_clus : oclus;
    int done = 0;
    while (c >= 2 && !done) {
      for (uint32_t s = 0; s < g_spc && !done; s++) {
        if (blk_read(clus_to_lba(c) + s, 1, sec) != 0) return -4;
        for (int e = 0; e < 16; e++) {
          dirent_t *d = (dirent_t *)(sec + e * 32);
          if (d->name[0] == 0x00) { done = 1; break; }
          if (d->name[0] == '.' && d->name[1] == '.' && d->name[2] == ' ') {
            d->clus_hi = (uint16_t)((nparent >> 16) & 0xFFFFu);
            d->clus_lo = (uint16_t)(nparent & 0xFFFFu);
            if (blk_write(clus_to_lba(c) + s, 1, sec) != 0) return -4;
            done = 1;
            break;
          }
        }
      }
      if (!done) {
        c = fat_next(c);
        if (c == 0) return -4; /* no ".." found: corrupt dir */
      }
    }
  }
  /* New link is good: drop the old entry (data stays where it is). */
  if (blk_read(oslot_lba, 1, sec) != 0) return -4;
  sec[oslot_off] = 0xE5;
  if (blk_write(oslot_lba, 1, sec) != 0) return -4;
  return 0;
}

int fat_is_dir(const char *path) {
  if (!g_ok || !path) return 0;
  const char *p = path;
  while (*p == '/' || *p == '\\') p++;
  if (*p == '\0') return 1; /* root */
  uint32_t clus, size;
  uint8_t attr;
  if (resolve(path, &clus, &size, &attr) != 0) return 0;
  return (attr & ATTR_DIR) != 0;
}

int fat_read(const char *path, void *buf, uint32_t cap, uint32_t *size_out) {
  if (!g_ok) return -1;
  uint32_t clus, size;
  uint8_t attr;
  if (resolve(path, &clus, &size, &attr) != 0) return -2;
  if (attr & ATTR_DIR) return -3;
  if (size_out) *size_out = size;
  if (size == 0 || cap == 0) return 0;
  uint32_t want = (size < cap) ? size : cap;
  uint8_t *out = (uint8_t *)buf;
  uint32_t got = 0;
  uint32_t c = clus;
  static uint8_t sec[512];
  while (got < want && c >= 2) {
    for (uint32_t s = 0; s < g_spc && got < want; s++) {
      if (blk_read(clus_to_lba(c) + s, 1, sec) != 0) return -4;
      uint32_t chunk = want - got;
      if (chunk > 512u * g_spc) chunk = 512u * g_spc;
      if (chunk > 512u) chunk = 512u;
      naos_memcpy(out + got, sec, chunk);
      got += chunk;
    }
    if (got < want) {
      c = fat_next(c);
      if (c == 0) return -5; /* chain ended before size bytes */
    }
  }
  return (int)got;
}

/* ---- file-descriptor support ---- */

int fat_fopen_r(const char *path, uint32_t *clus_out, uint32_t *size_out) {
  uint32_t clus, size;
  uint8_t attr;
  if (!g_ok || !path) return -1;
  if (resolve(path, &clus, &size, &attr) != 0) return -2;
  if (attr & ATTR_DIR) return -3;
  if (clus_out) *clus_out = clus;
  if (size_out) *size_out = size;
  return 0;
}

int fat_parent_leaf(const char *path, uint32_t *parent_out,
                    uint8_t leaf83_out[11]) {
  if (!g_ok || !path) return -1;
  return resolve_parent(path, parent_out, leaf83_out);
}

/* Positional read: [pos, pos+len) of the file, capped by size. */
int fat_read_at(uint32_t clus, uint32_t size, uint32_t pos, void *buf,
                uint32_t len) {
  static uint8_t sec[512];
  uint8_t *out = (uint8_t *)buf;
  uint32_t got = 0;
  uint32_t c = clus;
  uint32_t off = 0; /* file offset of the current sector's first byte */
  uint32_t end;
  if (!g_ok || !buf) return -1;
  if (len == 0 || pos >= size) return 0;
  end = pos + len;
  if (end < pos || end > size) end = size; /* wrap-safe cap */
  while (got + pos < end && c >= 2) {
    for (uint32_t s = 0; s < g_spc && got + pos < end; s++) {
      uint32_t sec_end;
      uint32_t from, to;
      if (blk_read(clus_to_lba(c) + s, 1, sec) != 0) return -4;
      sec_end = off + 512u;
      from = (pos > off) ? pos - off : 0;
      to = (end < sec_end) ? end - off : 512u;
      if (to > from) {
        naos_memcpy(out + got, sec + from, to - from);
        got += to - from;
      }
      off = sec_end;
    }
    if (got + pos < end) {
      c = fat_next(c);
      if (c == 0) return -5;
    }
  }
  return (int)got;
}
