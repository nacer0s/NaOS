/* NaOS RAM-disk backend: FAT image preloaded below 1G by the bootloader.
 * Same 512B sector interface as the other blk backends; writes live in
 * RAM (session-only). Used only when no real disk mounts. Own code. */
#ifndef NAOS_RAMBLK_H
#define NAOS_RAMBLK_H

#include <stdint.h>

/* Adopt bootloader runs (prefix-packed, zero = end). Safe to call always. */
void ramblk_init(const uint64_t *bases, const uint32_t *pages);
int ramblk_ok(void);
uint64_t ramblk_sectors(void);
int ramblk_read(uint32_t lba, uint8_t count, void *buf);
int ramblk_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* NAOS_RAMBLK_H */
