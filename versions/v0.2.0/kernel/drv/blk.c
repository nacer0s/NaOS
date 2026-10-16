/* NaOS block layer: AHCI, NVMe, probed ATA, then RAM-disk live session. */
#include "blk.h"
#include "ahci.h"
#include "nvme.h"
#include "ata.h"
#include "ramblk.h"
#include "../lib/kprintf.h"

static int g_drv; /* 0=ahci 1=nvme 2=ata 3=ramdisk */
static int g_boot_drv = -1;
static int g_boot_port;

/* One-time full probe (lazy: first show-drives/install, never boot). */
static int g_probed;
static uint32_t g_ahci_mask;
static uint64_t g_ahci_sec[32];
static int g_nvme_ok;
static uint64_t g_nvme_sec;
static int g_ata_ok;
static uint64_t g_ata_sec;

int blk_boot_drv(void) { return g_boot_drv; }
int blk_boot_port(void) { return g_boot_port; }

/* Device the block layer is routed at right now (what fat is on). */
void blk_current(int *drv, int *port) {
  if (drv) *drv = g_drv;
  if (port) *port = (g_drv == 0) ? (int)ahci_port() : 0;
}

void blk_init(void) {
  if (ahci_init()) {
    g_drv = 0;
    kprintf("blk: using ahci\n");
  } else if (nvme_init()) {
    g_drv = 1;
    kprintf("blk: using nvme\n");
  } else if (ata_probe()) {
    g_drv = 2;
    kprintf("blk: using ata\n");
  } else if (ramblk_ok()) {
    g_drv = 3;
    kprintf("blk: using ramdisk (live)\n");
  } else {
    /* Legacy path: ATA init already ran inside the probe; reads will
     * simply fail and the filesystem reports not-ready, as before. */
    g_drv = 2;
    kprintf("blk: using ata\n");
  }
  g_boot_drv = g_drv;
  g_boot_port = (g_drv == 0) ? ahci_port() : 0;
}

/* Init every present driver once (skips the active one: already up).
 * Caches sector counts, parks back on the boot port. 0 ok. */
int blk_probe_all(void) {
  int save_drv, save_port = 0;
  if (g_probed) return 0;
  g_probed = 1;
  save_drv = g_drv;
  if (save_drv == 0) save_port = ahci_port();
  g_ahci_mask = ahci_ports();
  if (g_ahci_mask == 0 && save_drv == 0) {
    /* Active AHCI but no ports probe now: keep the boot state. */
    g_ahci_mask = (uint32_t)(1u << (unsigned)save_port);
    g_ahci_sec[save_port] = ahci_sectors();
  } else {
    for (int p = 0; p < 32; p++) {
      if (!(g_ahci_mask & (1u << (unsigned)p))) continue;
      if (ahci_use_port((uint8_t)p) != 0) {
        g_ahci_mask &= ~(uint32_t)(1u << (unsigned)p);
        continue;
      }
      g_ahci_sec[p] = ahci_sectors();
    }
  }
  if (save_drv == 1) {
    g_nvme_ok = 1;
    g_nvme_sec = nvme_sectors();
  } else if (nvme_init()) {
    g_nvme_ok = 1;
    g_nvme_sec = nvme_sectors();
  }
  if (save_drv == 2) {
    g_ata_ok = 1;
    g_ata_sec = ata_sectors();
  } else if (ata_probe()) {
    g_ata_ok = 1;
    g_ata_sec = ata_sectors();
  }
  /* Park back where boot left us. */
  if (save_drv == 0) {
    ahci_use_port((uint8_t)save_port);
    g_drv = 0;
  } else {
    g_drv = save_drv;
  }
  return 0;
}

/* Drive i: ahci ports ascending, then nvme, then ata. 0 ok. */
int blk_drive(int i, int *drv, int *port, uint64_t *sectors, int *is_boot) {
  int n = 0;
  for (int p = 0; p < 32; p++) {
    if (!(g_ahci_mask & (1u << (unsigned)p))) continue;
    if (n++ == i) {
      if (drv) *drv = 0;
      if (port) *port = p;
      if (sectors) *sectors = g_ahci_sec[p];
      if (is_boot)
        *is_boot = (g_boot_drv == 0 && g_boot_port == p);
      return 0;
    }
  }
  if (g_nvme_ok) {
    if (n++ == i) {
      if (drv) *drv = 1;
      if (port) *port = 0;
      if (sectors) *sectors = g_nvme_sec;
      if (is_boot) *is_boot = (g_boot_drv == 1);
      return 0;
    }
  }
  if (g_ata_ok) {
    if (n++ == i) {
      if (drv) *drv = 2;
      if (port) *port = 0;
      if (sectors) *sectors = g_ata_sec;
      if (is_boot) *is_boot = (g_boot_drv == 2);
      return 0;
    }
  }
  return -1;
}

int blk_ndrives(void) {
  int n = 0;
  int d, p;
  uint64_t s;
  int b;
  while (blk_drive(n, &d, &p, &s, &b) == 0) n++;
  return n;
}

/* Route future I/O at a probed disk (fat_init() remounts after). 0 ok. */
int blk_select(int drv, int port) {
  if (drv == 0) {
    if (port < 0 || port >= 32) return -1;
    if (!(g_ahci_mask & (1u << (unsigned)port))) return -1;
    if (ahci_use_port((uint8_t)port) != 0) return -1;
    g_drv = 0;
    return 0;
  }
  if (drv == 1) {
    if (!g_nvme_ok) return -1;
    g_drv = 1;
    return 0;
  }
  if (drv == 2) {
    if (!g_ata_ok) return -1;
    g_drv = 2;
    return 0;
  }
  if (drv == 3) {
    if (!ramblk_ok()) return -1;
    g_drv = 3;
    return 0;
  }
  return -1;
}

void blk_restore(void) {
  if (g_boot_drv == 0) {
    ahci_use_port((uint8_t)g_boot_port);
    g_drv = 0;
  } else if (g_boot_drv >= 1 && g_boot_drv <= 3) {
    g_drv = g_boot_drv;
  }
}

const char *blk_name(void) {
  if (g_drv == 0) return "ahci";
  if (g_drv == 1) return "nvme";
  if (g_drv == 3) return "ramdisk";
  return "ata";
}

/* Switch to the staged live image (no-op when absent). For a failed
 * filesystem mount: the disk is foreign, but the session gets files. */
void blk_use_ramdisk(void) {
  if (!ramblk_ok()) return;
  g_drv = 3;
  kprintf("blk: using ramdisk (live)\n");
}

int blk_read(uint32_t lba, uint8_t count, void *buf) {
  if (g_drv == 0) return ahci_read(lba, count, buf);
  if (g_drv == 1) return nvme_read(lba, count, buf);
  if (g_drv == 3) return ramblk_read(lba, count, buf);
  return ata_read(lba, count, buf);
}

int blk_write(uint32_t lba, uint8_t count, const void *buf) {
  if (g_drv == 0) return ahci_write(lba, count, buf);
  if (g_drv == 1) return nvme_write(lba, count, buf);
  if (g_drv == 3) return ramblk_write(lba, count, buf);
  return ata_write(lba, count, buf);
}
