/* NaOS block layer: AHCI, then NVMe, then ATA PIO fallback. */
#include "blk.h"
#include "ahci.h"
#include "nvme.h"
#include "ata.h"
#include "../lib/kprintf.h"

static int g_drv; /* 0=ahci 1=nvme 2=ata */

void blk_init(void) {
  if (ahci_init()) {
    g_drv = 0;
    kprintf("blk: using ahci\n");
  } else if (nvme_init()) {
    g_drv = 1;
    kprintf("blk: using nvme\n");
  } else {
    g_drv = 2;
    ata_init();
    kprintf("blk: using ata\n");
  }
}

const char *blk_name(void) {
  return g_drv == 0 ? "ahci" : (g_drv == 1 ? "nvme" : "ata");
}

int blk_read(uint32_t lba, uint8_t count, void *buf) {
  if (g_drv == 0) return ahci_read(lba, count, buf);
  if (g_drv == 1) return nvme_read(lba, count, buf);
  return ata_read(lba, count, buf);
}

int blk_write(uint32_t lba, uint8_t count, const void *buf) {
  if (g_drv == 0) return ahci_write(lba, count, buf);
  if (g_drv == 1) return nvme_write(lba, count, buf);
  return ata_write(lba, count, buf);
}
