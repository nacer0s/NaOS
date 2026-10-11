/* NaOS AHCI driver: port 0, one command slot, polling DMA. For real
 * SATA disks (and QEMU -device ahci). Falls back gracefully if absent. */
#ifndef NAOS_AHCI_H
#define NAOS_AHCI_H

#include <stdint.h>

int ahci_init(void); /* 1 if a usable SATA disk is on port 0 */
uint64_t ahci_sectors(void);
int ahci_read(uint32_t lba, uint8_t count, void *buf);
int ahci_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* NAOS_AHCI_H */
