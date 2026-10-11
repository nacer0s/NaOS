/* NaOS block layer: AHCI, NVMe, probed ATA, then RAM-disk live session. */
#include "blk.h"
#include "ahci.h"
#include "nvme.h"
#include "ata.h"
#include "ramblk.h"
#include "../lib/kprintf.h"

static int g_drv; /* 0=ahci 1=nvme 2=ata 3=ramdisk */

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
}

const char *blk_name(void) {
  if (g_drv == 0) return "ahci";
  if (g_drv == 1) return "nvme";
  if (g_drv == 3) return "ramdisk";
  return "ata";
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
