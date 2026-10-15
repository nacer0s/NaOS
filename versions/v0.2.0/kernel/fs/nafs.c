/* NaOS-FS reader/writer. All offsets bounds-checked against the image
 * size; a corrupt image prints an error, never faults. Command context
 * only (single-threaded shell: shared static buffers are safe). */
#include "nafs.h"
#include "fat32.h"
#include "../lib/kprintf.h"
#include "../../common/cstring.h"

#define NAFS_MAGIC 0x5346414Eu
#define NAFS_VERSION 1
#define NAFS_DEPTH 8
#define NAFS_MAX_IMG (128u * 1024u) /* biggest host image we edit in RAM */
#define NAFS_MAX_BLOCKS (NAFS_MAX_IMG / 512u)

static int g_mounted;
static uint32_t g_clus, g_size; /* host file on FAT */
static uint32_t g_blocks, g_root;
static char g_label[17];
static char g_fatpath[128];      /* host image path (write-back + remount) */
static uint8_t g_img[NAFS_MAX_IMG]; /* whole-image work buffer (write path) */
static int g_mem; /* nonzero: blk() reads from g_img, not FAT */

static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[3] << 24));
}

/* Read one 512B image block into buf. 0 ok. Source follows g_mem. */
static int blk(uint32_t b, uint8_t *buf) {
  uint64_t off = (uint64_t)b * 512u;
  int r;
  if (!g_mounted) return -1;
  if (off + 512u > g_size) return -1;
  if (g_mem) {
    if (off + 512u > NAFS_MAX_IMG) return -1;
    naos_memcpy(buf, g_img + (uint32_t)off, 512);
    return 0;
  }
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

static int mount_inner(const char *fatpath, int verbose);

int nafs_mount(const char *fatpath) {
  return mount_inner(fatpath, 1);
}

static int mount_inner(const char *fatpath, int verbose) {
  static uint8_t sb[512];
  uint32_t sum = 0;
  uint32_t i;
  g_mounted = 0;
  if (!fatpath || !*fatpath) return -1;
  if (fat_fopen_r(fatpath, &g_clus, &g_size) != 0) {
    if (verbose) kprintf("nafs: cannot open '%s'\n", fatpath);
    return -1;
  }
  if (g_size < 512) {
    if (verbose) kprintf("nafs: '%s' too small\n", fatpath);
    return -1;
  }
  {
    int r = fat_read_at(g_clus, g_size, 0, sb, 512);
    if (r != 512) {
      if (verbose) kprintf("nafs: cannot read superblock\n");
      return -1;
    }
  }
  for (i = 0; i < 508; i++) sum += sb[i];
  if (get32(sb + 0) != NAFS_MAGIC || get32(sb + 4) != NAFS_VERSION ||
      get32(sb + 508) != sum) {
    if (verbose)
      kprintf("nafs: bad magic/version/checksum (not a NaOS-FS image)\n");
    return -1;
  }
  g_blocks = get32(sb + 8);
  g_root = get32(sb + 12);
  if (g_blocks == 0 || (uint64_t)g_blocks * 512u > g_size ||
      g_root >= g_blocks) {
    if (verbose) kprintf("nafs: corrupt superblock\n");
    return -1;
  }
  for (i = 0; i < 16; i++) g_label[i] = (char)sb[16 + i];
  g_label[16] = '\0';
  i = 0;
  while (i < 127 && fatpath[i]) {
    g_fatpath[i] = fatpath[i];
    i++;
  }
  g_fatpath[i] = '\0';
  g_mounted = 1;
  if (verbose)
    kprintf("nafs: mounted '%s' (%u blocks, root@%u)\n", g_label,
            (unsigned)g_blocks, (unsigned)g_root);
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

/* ---- writer (v0.2.0): no format change, no free-space metadata. The whole
 * host image is edited in g_img (cap NAFS_MAX_IMG) and written back once;
 * any failure before the write-back leaves the disk untouched. */

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* Recompute the superblock checksum over the work buffer. */
static void sb_checksum(void) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < 508; i++) sum += g_img[i];
  put32(g_img + 508, sum);
}

/* Mark every reachable block (superblock, dirs, file data) into used[].
 * Dir loops terminate: an already-marked dir block is never re-entered.
 * Blocks beyond g_blocks are ignored (corrupt-safe). */
static void mark_used(uint8_t *used, uint32_t nblocks) {
  static uint8_t db[512];
  uint32_t stack[NAFS_DEPTH];
  int top = 0;
  for (uint32_t i = 0; i < nblocks; i++) used[i] = 0;
  if (g_root >= g_blocks) return;
  used[0] = 1; /* superblock */
  stack[top++] = g_root;
  while (top > 0) {
    uint32_t dir = stack[--top];
    if (dir >= g_blocks || used[dir]) continue; /* visited: loop-safe */
    if (blk(dir, db) != 0) continue;
    used[dir] = 1; /* dirs are storage too: never hand their block out */
    for (int e = 0; e < 16; e++) {
      const uint8_t *en = db + (uint32_t)e * 32;
      uint8_t t = en[20];
      uint32_t st, sz;
      if (t != 1 && t != 2) continue;
      st = get32(en + 24);
      sz = get32(en + 28);
      if (st >= g_blocks) continue;
      if (t == 2) {
        if (!used[st] && top < NAFS_DEPTH) {
          used[st] = 1;
          stack[top++] = st;
        } else {
          used[st] = 1;
        }
      } else {
        uint32_t need = (sz + 511u) / 512u;
        for (uint32_t b = 0; b < need; b++)
          if (st + b < g_blocks) used[st + b] = 1;
      }
    }
  }
}

/* First fit of `need` contiguous free blocks from 1 up (0 = superblock,
 * never allocated). need == 0 takes no space: returns g_blocks sentinel. */
static uint32_t alloc_run(uint8_t *used, uint32_t nblocks, uint32_t need) {
  if (need == 0) return nblocks;
  for (uint32_t s = 1; s + need <= nblocks; s++) {
    uint32_t b;
    for (b = 0; b < need; b++)
      if (used[s + b]) break;
    if (b == need) return s;
  }
  return 0;
}

/* Split path into parent dir block + leaf. Leaf: 1-19 chars (mknafs rule).
 * found = 1 with entry details when the leaf exists. Returns 0 ok,
 * -1 missing middle dir / unreadable, -2 root itself or bad leaf. */
static int split_parent(const char *path, uint32_t *parent_out, char *leaf_out,
                        int *found, uint32_t *f_idx, uint8_t *f_type,
                        uint32_t *f_start, uint32_t *f_size) {
  static uint8_t db[512];
  char comp[24];
  uint32_t dir = g_root;
  const char *p = path;
  int depth = 0, n, i;
  *found = 0;
  while (*p == '/') p++;
  if (*p == '\0') return -2; /* root itself */
  for (;;) {
    n = 0;
    while (*p && *p != '/' && n < 23) comp[n++] = *p++;
    comp[n] = '\0';
    while (*p == '/') p++;
    if (n == 0) return -2;
    if (*p == '\0') break; /* comp is the leaf */
    if (++depth > NAFS_DEPTH) return -1;
    if (blk(dir, db) != 0) {
      kprintf("nafs: corrupt image\n");
      return -1;
    }
    {
      int hit = 0;
      for (i = 0; i < 16; i++) {
        const uint8_t *en = db + (uint32_t)i * 32;
        if (en[20] != 2 || !name_eq(en, comp)) continue;
        if (get32(en + 24) >= g_blocks) return -1;
        dir = get32(en + 24);
        hit = 1;
        break;
      }
      if (!hit) return -1; /* missing middle dir */
    }
  }
  if (n < 1 || n > 19) return -2; /* mknafs leaf rule */
  if (++depth > NAFS_DEPTH) return -1; /* leaf counts too, like resolve() */
  for (i = 0; i < n; i++) leaf_out[i] = comp[i];
  leaf_out[n] = '\0';
  *parent_out = dir;
  if (blk(dir, db) != 0) {
    kprintf("nafs: corrupt image\n");
    return -1;
  }
  for (i = 0; i < 16; i++) {
    const uint8_t *en = db + (uint32_t)i * 32;
    uint8_t t = en[20];
    if (t != 1 && t != 2) continue;
    if (!name_eq(en, comp)) continue;
    *found = 1;
    *f_idx = (uint32_t)i;
    *f_type = t;
    *f_start = get32(en + 24);
    *f_size = get32(en + 28);
    return 0;
  }
  return 0; /* parent ok, leaf absent (create case) */
}

/* Load the host image into the work buffer (cap) + verify superblock. */
static int load_image(void) {
  uint32_t sz = 0;
  if (!g_mounted) {
    kprintf("nafs: not mounted\n");
    return -1;
  }
  if (g_size > NAFS_MAX_IMG) {
    kprintf("nafs: image too big to edit (cap 128KB)\n");
    return -1;
  }
  if (fat_read(g_fatpath, g_img, g_size, &sz) != (int)g_size ||
      sz != g_size) {
    kprintf("nafs: cannot stage host image\n");
    return -1;
  }
  if (get32(g_img + 0) != NAFS_MAGIC || get32(g_img + 4) != NAFS_VERSION) {
    kprintf("nafs: host image changed under us\n");
    return -1;
  }
  {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 508; i++) sum += g_img[i];
    if (get32(g_img + 508) != sum) {
      kprintf("nafs: host image checksum changed\n");
      return -1;
    }
  }
  return 0;
}

/* Write the work buffer back + quiet remount (revalidates everything,
 * refreshes the host cluster after FAT realloc). 0 ok. */
static int store_image(void) {
  int r;
  sb_checksum();
  r = fat_write(g_fatpath, g_img, g_size);
  if (r != 0) {
    kprintf("nafs: write-back failed\n");
    return -1;
  }
  g_mem = 0;
  if (mount_inner(g_fatpath, 0) != 0) {
    kprintf("nafs: remount failed\n");
    return -1;
  }
  return 0;
}

int nafs_write(const char *fatpath, const char *nafs_path) {
  static uint8_t used[NAFS_MAX_BLOCKS];
  static uint8_t sec[512];
  uint32_t parent, f_idx, f_start = 0, f_size = 0, s_clus, s_size;
  uint8_t f_type = 0;
  char leaf[20];
  int found, r;
  uint32_t need, start, off;
  if (!fatpath || !nafs_path) return -1;
  if (fat_fopen_r(fatpath, &s_clus, &s_size) != 0) {
    kprintf("nafs: cannot open '%s'\n", fatpath);
    return -1;
  }
  if (load_image() != 0) return -1;
  g_mem = 1; /* resolve + walks below read the buffer, not FAT */
  r = split_parent(nafs_path, &parent, leaf, &found, &f_idx, &f_type,
                   &f_start, &f_size);
  if (r == -2) {
    kprintf("nafs: bad path '%s'\n", nafs_path);
    g_mem = 0;
    return -1;
  }
  if (r != 0) {
    kprintf("nafs: no such folder for '%s'\n", nafs_path);
    g_mem = 0;
    return -1;
  }
  if (found && f_type == 2) {
    kprintf("nafs: '%s' is a directory\n", nafs_path);
    g_mem = 0;
    return -1;
  }
  mark_used(used, g_blocks);
  if (found && f_type == 1) {
    /* Replacing: release the old blocks first (reuse allowed). */
    uint32_t need_old = (f_size + 511u) / 512u, b;
    for (b = 0; b < need_old; b++)
      if (f_start + b < g_blocks) used[f_start + b] = 0;
  }
  need = (s_size + 511u) / 512u;
  start = alloc_run(used, g_blocks, need);
  if (need > 0 && start == 0) {
    kprintf("nafs: no contiguous space (%u bytes)\n", (unsigned)s_size);
    g_mem = 0;
    return -1;
  }
  /* Stream the source straight into the image blocks. */
  for (off = 0; off < s_size;) {
    uint32_t n = s_size - off;
    uint32_t b = start + off / 512u;
    uint32_t boff = off % 512u;
    if (n > 512u - boff) n = 512u - boff;
    r = fat_read_at(s_clus, s_size, off, sec, n);
    if (r != (int)n) {
      kprintf("nafs: source read failed\n");
      g_mem = 0;
      return -1;
    }
    naos_memcpy(g_img + b * 512u + boff, sec, n);
    off += n;
  }
  /* Publish the dirent (reuse the slot when replacing). */
  {
    uint32_t idx = (uint32_t)f_idx;
    uint8_t *en;
    int i;
    if (!found) {
      if (blk(parent, sec) != 0) {
        kprintf("nafs: corrupt image\n");
        g_mem = 0;
        return -1;
      }
      idx = 16;
      for (i = 0; i < 16; i++) {
        if (sec[(uint32_t)i * 32 + 20] == 0) {
          idx = (uint32_t)i;
          break;
        }
      }
      if (idx >= 16) {
        kprintf("nafs: directory full\n");
        g_mem = 0;
        return -1;
      }
    }
    en = g_img + parent * 512u + idx * 32u;
    for (i = 0; i < 20; i++) en[i] = 0;
    for (i = 0; leaf[i] && i < 19; i++) en[i] = (uint8_t)leaf[i];
    en[20] = 1;
    en[21] = en[22] = en[23] = 0;
    put32(en + 24, (need == 0) ? g_blocks : start);
    put32(en + 28, s_size);
  }
  g_mem = 0;
  if (store_image() != 0) return -1;
  kprintf("nafs: wrote %u bytes to '%s'\n", (unsigned)s_size, nafs_path);
  return 0;
}

int nafs_delete(const char *nafs_path) {
  uint32_t parent, f_idx, f_start, f_size;
  uint8_t f_type = 0;
  char leaf[20];
  int found, r, i;
  if (!nafs_path) return -1;
  if (load_image() != 0) return -1;
  g_mem = 1;
  r = split_parent(nafs_path, &parent, leaf, &found, &f_idx, &f_type,
                   &f_start, &f_size);
  if (r == -2) {
    kprintf("nafs: cannot delete '/'\n");
    g_mem = 0;
    return -1;
  }
  if (r != 0 || !found) {
    kprintf("nafs: no such file '%s'\n", nafs_path);
    g_mem = 0;
    return -1;
  }
  if (f_type == 2) {
    static uint8_t db[512];
    if (blk(f_start, db) != 0) {
      kprintf("nafs: corrupt image\n");
      g_mem = 0;
      return -1;
    }
    for (i = 0; i < 16; i++) {
      if (db[(uint32_t)i * 32 + 20] != 0) {
        kprintf("nafs: directory not empty\n");
        g_mem = 0;
        return -1;
      }
    }
  } else if (f_type != 1) {
    kprintf("nafs: no such file '%s'\n", nafs_path);
    g_mem = 0;
    return -1;
  }
  naos_memset(g_img + parent * 512u + f_idx * 32u, 0, 32);
  g_mem = 0;
  if (store_image() != 0) return -1;
  kprintf("nafs: deleted '%s'\n", nafs_path);
  return 0;
}
