/* NaOS ATA driver: primary-master PATA, LBA28, polling PIO. QEMU IDE disk. */
#ifndef NAOS_ATA_H
#define NAOS_ATA_H

#include <stdint.h>

int ata_init(void);                       /* 0 ok: logs model + sectors */
uint64_t ata_sectors(void);
int ata_probe(void);                      /* 1 if a signed disk answers */
int ata_read(uint32_t lba, uint8_t count, void *buf); /* count 1..255 */
int ata_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* NAOS_ATA_H */
