/* NaOS block layer: AHCI, NVMe, probed ATA, then RAM-disk live session. */
#ifndef NAOS_BLK_H
#define NAOS_BLK_H

#include <stdint.h>

void blk_init(void); /* probe order above; prints what won */
void blk_use_ramdisk(void); /* force the live image (if staged) */
const char *blk_name(void); /* "ahci", "nvme", "ata" or "ramdisk" */
int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* NAOS_BLK_H */
