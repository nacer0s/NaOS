/* NaOS ATA PIO driver. Polling only (IRQs stay masked). */
#include "ata.h"
#include "../lib/kprintf.h"
#include "../../common/io.h"
#include "../../common/cstring.h"

#define ATA_DATA  0x1F0
#define ATA_ERR   0x1F1
#define ATA_COUNT 0x1F2
#define ATA_LBA0  0x1F3
#define ATA_LBA1  0x1F4
#define ATA_LBA2  0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STAT  0x1F7
#define ATA_CMD   0x1F7
#define ATA_ALT   0x3F6

#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DRQ 0x08
#define ST_ERR 0x01

static uint64_t g_sectors;

static int wait_clear(uint8_t mask, uint32_t tries) {
  while (tries--) {
    if ((naos_inb(ATA_STAT) & mask) == 0) return 0;
  }
  return -1;
}

static int wait_set(uint8_t mask, uint32_t tries) {
  while (tries--) {
    uint8_t s = naos_inb(ATA_STAT);
    if ((s & ST_BSY) == 0) {
      if (s & ST_ERR) return -2;
      if (s & mask) return 0;
    }
  }
  return -1;
}

static void delay400ns(void) {
  for (int i = 0; i < 4; i++) (void)naos_inb(ATA_ALT);
}

int ata_init(void) {
  /* Floating bus (no device) reads 0xFF. */
  if (naos_inb(ATA_STAT) == 0xFF) {
    kprintf("ata: no device\n");
    return -1;
  }
  naos_outb(ATA_DRIVE, 0xE0); /* master, LBA */
  delay400ns();
  if (wait_clear(ST_BSY, 1000000) != 0) {
    kprintf("ata: BSY stuck\n");
    return -1;
  }

  /* IDENTIFY for model + capacity. */
  naos_outb(ATA_CMD, 0xEC);
  delay400ns();
  if (naos_inb(ATA_STAT) == 0) {
    kprintf("ata: no IDENTIFY response\n");
    return -1;
  }
  if (wait_clear(ST_BSY, 1000000) != 0) {
    kprintf("ata: IDENTIFY timeout\n");
    return -1;
  }
  /* ATAPI would set ERR/CYL signatures here; ATA proceeds to DRQ. */
  if (wait_set(ST_DRQ, 1000000) != 0) {
    kprintf("ata: IDENTIFY no data\n");
    return -1;
  }
  uint16_t id[256];
  naos_insw(ATA_DATA, id, 256);
  uint32_t secs = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
  g_sectors = secs;
  char model[41];
  for (int i = 0; i < 20; i++) {
    model[2 * i] = (char)(id[27 + i] >> 8);
    model[2 * i + 1] = (char)(id[27 + i] & 0xFF);
  }
  model[40] = '\0';
  /* Trim trailing spaces. */
  for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
  kprintf("ata: %s sectors=%u\n", model, (unsigned)secs);
  return 0;
}

uint64_t ata_sectors(void) { return g_sectors; }

int ata_read(uint32_t lba, uint8_t count, void *buf) {
  if (count == 0 || !buf) return -1;
  if (wait_clear(ST_BSY, 1000000) != 0) return -1;
  naos_outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
  naos_outb(ATA_COUNT, count);
  naos_outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
  naos_outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  naos_outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  naos_outb(ATA_CMD, 0x20); /* READ SECTORS */
  delay400ns();
  uint16_t *p = (uint16_t *)buf;
  for (uint32_t s = 0; s < count; s++) {
    int r = wait_set(ST_DRQ, 1000000);
    if (r != 0) return -1;
    naos_insw(ATA_DATA, p, 256);
    p += 256;
  }
  return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
  if (count == 0 || !buf) return -1;
  if (wait_clear(ST_BSY, 1000000) != 0) return -1;
  naos_outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
  naos_outb(ATA_COUNT, count);
  naos_outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
  naos_outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  naos_outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  naos_outb(ATA_CMD, 0x30); /* WRITE SECTORS */
  delay400ns();
  const uint16_t *p = (const uint16_t *)buf;
  for (uint32_t s = 0; s < count; s++) {
    int r = wait_set(ST_DRQ, 1000000);
    if (r != 0) return -1;
    naos_outsw(ATA_DATA, p, 256);
    p += 256;
    /* Flush cache so data hits the platter (QEMU: image file). */
    if (s + 1 == count) {
      if (wait_clear(ST_BSY, 1000000) != 0) return -1;
      naos_outb(ATA_CMD, 0xE7);
      if (wait_clear(ST_BSY, 1000000) != 0) return -1;
    }
  }
  return 0;
}
