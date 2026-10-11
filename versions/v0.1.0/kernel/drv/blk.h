/* NaOS block layer: AHCI first, ATA PIO fallback. Own code. */
#ifndef NAOS_BLK_H
#define NAOS_BLK_H

#include <stdint.h>

void blk_init(void); /* probe AHCI, else ATA; prints what won */
const char *blk_name(void); /* "ahci" or "ata" */
int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* NAOS_BLK_H */
