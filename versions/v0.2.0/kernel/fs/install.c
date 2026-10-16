/* NaOS system installer: copy the running system onto a disk so it
 * boots without live media. Whole-disk FAT32 (superfloppy, like our
 * ESP images): UEFI fallback boot finds \EFI\BOOT\BOOTX64.EFI with no
 * NVRAM entries needed. Own code. */
#include "install.h"
#include "fat32.h"
#include "mkfs.h"
#include "../drv/blk.h"
#include "../drv/ramblk.h"
#include "../lib/kprintf.h"
#include "../mm/kmalloc.h"
#include "../../common/cstring.h"

#define COPY_CHUNK 8192u
#define MAX_ENTRIES 256u

/* Collected tree: static slots (the heap only hands out single pages,
 * so no large contiguous alloc here), per-path small buffers. */
static char *g_paths[MAX_ENTRIES];
static uint8_t g_isdir[MAX_ENTRIES];
static uint32_t g_n;

static const char *drv_kind(int drv) {
  if (drv == 0) return "ahci";
  if (drv == 1) return "nvme";
  if (drv == 2) return "ata";
  return "?";
}

/* "ahci0".."ahci31", "nvme", "ata" -> drv/port. 0 ok. */
static int parse_id(const char *id, int *drv, int *port) {
  if (!id || !drv || !port) return -1;
  if (id[0] == 'a' && id[1] == 'h' && id[2] == 'c' && id[3] == 'i') {
    unsigned p = 0;
    int i = 4;
    if (id[i] == '\0') return -1;
    while (id[i] >= '0' && id[i] <= '9') {
      p = p * 10u + (unsigned)(id[i] - '0');
      if (p >= 32u) return -1;
      i++;
    }
    if (id[i] != '\0') return -1;
    *drv = 0;
    *port = (int)p;
    return 0;
  }
  if (naos_strcmp(id, "nvme") == 0) {
    *drv = 1;
    *port = 0;
    return 0;
  }
  if (naos_strcmp(id, "ata") == 0) {
    *drv = 2;
    *port = 0;
    return 0;
  }
  return -1;
}

static void drive_id(int drv, int port, char *out) {
  int n = 0;
  const char *k = drv_kind(drv);
  while (*k) {
    out[n++] = *k++;
  }
  if (drv == 0) {
    char tmp[4];
    int m = 0;
    unsigned p = (unsigned)port;
    if (p == 0) {
      tmp[m++] = '0';
    } else {
      while (p > 0 && m < 3) {
        tmp[m++] = (char)('0' + p % 10u);
        p /= 10u;
      }
    }
    while (m > 0) out[n++] = tmp[--m];
  }
  out[n] = '\0';
}

static void print_size(uint64_t sectors) {
  if (sectors >= 4194304ULL)
    kprintf("%u GB", (unsigned)(sectors / 4194304ULL));
  else
    kprintf("%u MB", (unsigned)(sectors / 2048ULL));
}

/* List every probed real disk + the live session. 0 ok. */
int drives_show(void) {
  int n, i, boot_drv, boot_port;
  blk_probe_all();
  blk_current(&boot_drv, &boot_port);
  n = blk_ndrives();
  if (n == 0) kprintf("drives: no real disks (live session only)\n");
  for (i = 0; i < n; i++) {
    int d, p, is_boot;
    uint64_t s;
    char id[8];
    blk_drive(i, &d, &p, &s, &is_boot);
    drive_id(d, p, id);
    kprintf("  %s  ", id);
    print_size(s);
    if (d == boot_drv && p == boot_port) kprintf("  [boot volume]");
    kprintf("\n");
  }
  if (ramblk_ok()) {
    kprintf("  live  ");
    print_size(ramblk_sectors());
    if (boot_drv == 3) kprintf("  [boot volume]");
    kprintf(" (RAM session)\n");
  }
  return 0;
}

/* Select drv/port and remount FAT there. 0 ok. */
static int use_vol(int drv, int port) {
  if (blk_select(drv, port) != 0) return -1;
  if (fat_init() != 0) return -2;
  return 0;
}

typedef struct {
  uint32_t n; /* == g_n when done */
  int oom;
} collect_t;

static void collect_one(const char *path, int is_dir, void *ctx) {
  collect_t *c = (collect_t *)ctx;
  uint32_t len = 0;
  char *copy;
  if (g_n >= MAX_ENTRIES) return;
  while (path[len] && len < 511) len++;
  copy = (char *)kmalloc(len + 1u);
  if (!copy) {
    c->oom = 1;
    return;
  }
  for (uint32_t i = 0; i <= len; i++) copy[i] = path[i];
  g_paths[g_n] = copy;
  g_isdir[g_n] = is_dir ? 1u : 0u;
  g_n++;
  c->n = g_n;
}

static int restore_src(int sdrv, int sport) {
  if (blk_select(sdrv, sport) != 0) {
    kprintf("install-system: lost the boot volume!\n");
    return -1;
  }
  if (fat_init() != 0) {
    kprintf("install-system: boot volume will not remount\n");
    return -1;
  }
  return 0;
}

/* Copy the running system onto drive <id> (needs the word `erase`).
 * Refuses the boot volume. 0 ok. */
int install_run(const char *id, int erase) {
  static uint8_t chunk[COPY_CHUNK];
  int sdrv, sport, tdrv = -1, tport = 0, i, n;
  uint64_t tsecs = 0;
  char tid[8];
  collect_t col;
  if (!id) {
    kprintf("usage: install-system <drive> erase\n");
    kprintf("  pick <drive> from show-drives; erase formats it.\n");
    return -1;
  }
  blk_probe_all();
  blk_current(&sdrv, &sport);
  if (parse_id(id, &tdrv, &tport) != 0) {
    kprintf("install-system: unknown drive '%s'\n", id);
    return -1;
  }
  n = blk_ndrives();
  tsecs = 0;
  for (i = 0; i < n; i++) {
    int d, p, b;
    uint64_t s;
    if (blk_drive(i, &d, &p, &s, &b) != 0) continue;
    if (d == tdrv && p == tport) {
      tsecs = s;
      break;
    }
  }
  if (i == n) {
    kprintf("install-system: drive '%s' not present\n", id);
    return -1;
  }
  if (tdrv == sdrv && tport == sport) {
    kprintf("install-system: '%s' is the boot volume\n", id);
    kprintf("  boot a live session (CD/USB) to install elsewhere.\n");
    return -1;
  }
  drive_id(tdrv, tport, tid);
  if (!erase) {
    kprintf("install-system: this formats %s (", tid);
    print_size(tsecs);
    kprintf(") — ALL DATA LOST\n");
    kprintf("  re-run as: install-system %s erase\n", tid);
    return -1;
  }
  kprintf("install-system: formatting %s (", tid);
  print_size(tsecs);
  kprintf(")...\n");
  if (blk_select(tdrv, tport) != 0) {
    kprintf("install-system: cannot select '%s'\n", tid);
    return -1;
  }
  {
    int r = fat_format(tsecs);
    if (r == -2) {
      kprintf("install-system: disk too small for FAT32 (need ~34MB)\n");
      restore_src(sdrv, sport);
      return -1;
    }
    if (r != 0) {
      kprintf("install-system: format failed\n");
      restore_src(sdrv, sport);
      return -1;
    }
  }
  if (fat_init() != 0) {
    kprintf("install-system: fresh volume will not mount\n");
    restore_src(sdrv, sport);
    return -1;
  }
  /* Standard layout first (harmless when the source lacks one). */
  fat_mkdir("/EFI");
  fat_mkdir("/EFI/BOOT");
  fat_mkdir("/NaOS");
  fat_mkdir("/PROGS");
  /* Collect the source tree (paths only: no fat calls in callbacks). */
  g_n = 0;
  col.n = 0;
  col.oom = 0;
  if (use_vol(sdrv, sport) != 0 || fat_walk("/", collect_one, &col) != 0 ||
      col.oom) {
    kprintf("install-system: cannot read the boot volume\n");
    for (i = 0; (uint32_t)i < g_n; i++) kfree(g_paths[i]);
    g_n = 0;
    restore_src(sdrv, sport);
    return -1;
  }
  kprintf("install-system: %u entries...\n", (unsigned)g_n);
  for (i = 0; (uint32_t)i < g_n; i++) {
    const char *p = g_paths[i];
    if (g_isdir[i]) {
      if (use_vol(tdrv, tport) != 0) break;
      {
        int r = fat_mkdir(p);
        if (r != 0 && r != -6) break; /* -6 exists: fine */
      }
    } else {
      uint32_t clus, size, off = 0;
      if (use_vol(sdrv, sport) != 0) break;
      if (fat_fopen_r(p, &clus, &size) != 0) break;
      if (use_vol(tdrv, tport) != 0) break;
      /* Truncate semantics: without this, append would concatenate
       * onto any stale content (fresh format => always missing). */
      fat_delete(p);
      while (off < size) {
        uint32_t want = size - off;
        int got;
        if (want > COPY_CHUNK) want = COPY_CHUNK;
        if (use_vol(sdrv, sport) != 0) break;
        got = fat_read_at(clus, size, off, chunk, want);
        if (got != (int)want) break;
        if (use_vol(tdrv, tport) != 0) break;
        if (fat_append(p, chunk, want) != 0) break;
        off += want;
      }
      if (off != size) break;
    }
  }
  if ((uint32_t)i != g_n) {
    kprintf("install-system: copy failed at '%s'\n", g_paths[i]);
    for (i = 0; (uint32_t)i < g_n; i++) kfree(g_paths[i]);
    g_n = 0;
    restore_src(sdrv, sport);
    return -1;
  }
  for (i = 0; (uint32_t)i < g_n; i++) kfree(g_paths[i]);
  g_n = 0;
  kprintf("install-system: installed. Root of %s:\n", tid);
  use_vol(tdrv, tport);
  fat_ls("/");
  if (restore_src(sdrv, sport) != 0) return -1;
  kprintf("install-system: done — reboot-system without the live media.\n");
  return 0;
}
