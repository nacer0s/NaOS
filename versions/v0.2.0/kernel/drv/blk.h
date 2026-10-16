/* NaOS block layer: AHCI, NVMe, probed ATA, then RAM-disk live session. */
#ifndef NAOS_BLK_H
#define NAOS_BLK_H

#include <stdint.h>

void blk_init(void); /* probe order above; prints what won */
void blk_use_ramdisk(void); /* force the live image (if staged) */
const char *blk_name(void); /* "ahci", "nvme", "ata" or "ramdisk" */
int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);
/* Drives: every probed real disk (ramdisk excluded). probe_all inits
 * each present driver once (boot path untouched); select routes I/O
 * (call fat_init() after to remount); restore returns to the boot disk. */
int blk_probe_all(void);
int blk_ndrives(void); /* ahci ports, then nvme, then ata */
int blk_drive(int i, int *drv, int *port, uint64_t *sectors, int *is_boot);
int blk_select(int drv, int port); /* 0 ok */
void blk_restore(void);
int blk_boot_drv(void); /* 0 ahci 1 nvme 2 ata 3 ramdisk */
int blk_boot_port(void); /* ahci port at boot (0 otherwise) */
void blk_current(int *drv, int *port); /* device routed right now */

#endif /* NAOS_BLK_H */
