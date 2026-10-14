/* NaOS-FS reader. All offsets bounds-checked against the image size;
 * a corrupt image prints an error, never faults. Command context only. */
#include "nafs.h"
#include "fat32.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define NAFS_MAGIC 0x5346414Eu
#define NAFS_VERSION 1
#define NAFS_DEPTH 8

static int g_mounted;
static uint32_t g_clus, g_size; /* host file on FAT */
static uint32_t g_blocks, g_root;
static char g_label[17];

static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[3] << 24));
}

/* Read one 512B image block into buf. 0 ok. */
static int blk(uint32_t b, uint8_t *buf) {
  uint64_t off = (uint64_t)b * 512u;
  int r;
  if (!g_mounted) return -1;
  if (off + 512u > g_size) return -1;
  r = fat_read_at(g_clus, g_size, (uint32_t)off, buf, 512);
  return (r == 512) ? 0 : -1;
}

/* Compare a 20B on-disk name with a NUL-terminated component. */
static int name_eq(const uint8_t *disk, const char *comp) {
  for (int i = 0; i < 20; i++) {
    char c = comp[i];
    if (disk[i] != (uint8_t)c) return 0;
    if (c == '\0') return 1;
  }
  return comp[20] == '\0' && disk[19] == '\0';
}

/* Resolve an absolute NaFS path to (type, start, size). 0 ok. */
static int resolve(const char *path, uint8_t *type, uint32_t *start,
                   uint32_t *size) {
  static uint8_t db[512];
  uint32_t dir = g_root;
  const char *p = path;
  int depth = 0;
  if (!g_mounted || !path) return -1;
  while (*p == '/') p++;
  if (*p == '\0') { /* root itself */
    *type = 2;
    *start = g_root;
    *size = 0;
    return 0;
  }
  for (;;) {
    char comp[24];
    int n = 0;
    int found = 0;
    while (*p && *p != '/' && n < 23) comp[n++] = *p++;
    comp[n] = '\0';
    while (*p == '/') p++;
    if (n == 0) return -1;
    if (++depth > NAFS_DEPTH) return -1;
    if (blk(dir, db) != 0) return -1;
    for (int e = 0; e < 16; e++) {
      const uint8_t *en = db + (uint32_t)e * 32;
      uint8_t t = en[20];
      if (t != 1 && t != 2) continue;
      if (!name_eq(en, comp)) continue;
      *type = t;
      *start = get32(en + 24);
      *size = get32(en + 28);
      found = 1;
      break;
    }
    if (!found) return -1;
    if (*p == '\0') return 0; /* last component: whatever it is */
    if (*type != 2) return -1; /* middle component must be a dir */
    if (*start >= g_blocks) return -1;
    dir = *start;
  }
}

int nafs_mount(const char *fatpath) {
  static uint8_t sb[512];
  uint32_t sum = 0;
  g_mounted = 0;
  if (!fatpath || !*fatpath) return -1;
  if (fat_fopen_r(fatpath, &g_clus, &g_size) != 0) {
    kprintf("nafs: cannot open '%s'\n", fatpath);
    return -1;
  }
  if (g_size < 512) {
    kprintf("nafs: '%s' too small\n", fatpath);
    return -1;
  }
  {
    int r = fat_read_at(g_clus, g_size, 0, sb, 512);
    if (r != 512) {
      kprintf("nafs: cannot read superblock\n");
      return -1;
    }
  }
  for (int i = 0; i < 508; i++) sum += sb[i];
  if (get32(sb + 0) != NAFS_MAGIC || get32(sb + 4) != NAFS_VERSION ||
      get32(sb + 508) != sum) {
    kprintf("nafs: bad magic/version/checksum (not a NaOS-FS image)\n");
    return -1;
  }
  g_blocks = get32(sb + 8);
  g_root = get32(sb + 12);
  if (g_blocks == 0 || (uint64_t)g_blocks * 512u > g_size ||
      g_root >= g_blocks) {
    kprintf("nafs: corrupt superblock\n");
    return -1;
  }
  for (int i = 0; i < 16; i++) g_label[i] = (char)sb[16 + i];
  g_label[16] = '\0';
  g_mounted = 1;
  kprintf("nafs: mounted '%s' (%u blocks, root@%u)\n", g_label, (unsigned)g_blocks,
          (unsigned)g_root);
  return 0;
}

int nafs_list(const char *dirpath) {
  static uint8_t db[512];
  uint8_t type;
  uint32_t start, size;
  if (!dirpath || !*dirpath) dirpath = "/";
  if (resolve(dirpath, &type, &start, &size) != 0 || type != 2) {
    kprintf("nafs: no such dir '%s'\n", dirpath);
    return -1;
  }
  if (blk(start, db) != 0) return -1;
  for (int e = 0; e < 16; e++) {
    const uint8_t *en = db + (uint32_t)e * 32;
    uint8_t t = en[20];
    char nm[21];
    if (t != 1 && t != 2) continue;
    for (int i = 0; i < 20; i++) nm[i] = (char)en[i];
    nm[20] = '\0';
    if (t == 2) kprintf("  %s/ <DIR>\n", nm);
    else kprintf("  %s %u bytes\n", nm, (unsigned)get32(en + 28));
  }
  return 0;
}

int nafs_read(const char *filepath) {
  static uint8_t db[512];
  uint8_t type;
  uint32_t start, size, off = 0;
  if (resolve(filepath, &type, &start, &size) != 0 || type != 1) {
    kprintf("nafs: no such file '%s'\n", filepath);
    return -1;
  }
  if (size > 16384) {
    kprintf("nafs: file too big (%u, cap 16KB)\n", (unsigned)size);
    return -1;
  }
  while (off < size) {
    uint32_t b = start + off / 512u;
    uint32_t boff = off % 512u;
    uint32_t n = 512u - boff;
    if (n > size - off) n = size - off;
    if (b >= g_blocks || blk(b, db) != 0) return -1;
    /* Text-only print: NUL-terminate the chunk first. */
    {
      char tmp[513];
      uint32_t m = (n > 512) ? 512 : n;
      naos_memcpy(tmp, db + boff, m);
      tmp[m] = '\0';
      kprintf("%s", tmp);
    }
    off += n;
  }
  return 0;
}
