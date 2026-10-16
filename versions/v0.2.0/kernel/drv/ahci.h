/* NaOS AHCI driver: port 0, one command slot, polling DMA. For real
 * SATA disks (and QEMU -device ahci). Falls back gracefully if absent. */
#ifndef NAOS_AHCI_H
#define NAOS_AHCI_H

#include <stdint.h>

int ahci_init(void); /* 1 if a usable SATA disk is on any port */
uint64_t ahci_sectors(void); /* of the active port */
uint32_t ahci_ports(void);   /* bitmask of present SATA ports */
uint8_t ahci_port(void);     /* active port */
int ahci_use_port(uint8_t p); /* route I/O to port p (must be present) */
int ahci_read(uint32_t lba, uint8_t count, void *buf);
int ahci_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* NAOS_AHCI_H */
