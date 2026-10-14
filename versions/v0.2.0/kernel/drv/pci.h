/* NaOS PCI: config-space reads (bus 0) + class scan. Own code. */
#ifndef NAOS_PCI_H
#define NAOS_PCI_H

#include <stdint.h>

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                 uint16_t v);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                 uint32_t v);
/* Find first device with class/subclass (bus 0). Returns 1, fills bus/s/f. */
int pci_find_class(uint8_t class_, uint8_t subclass,
                   uint8_t *bus, uint8_t *slot, uint8_t *func);
/* Same, also matching prog-if (e.g. XHCI = 0x0C/0x03/0x30). */
int pci_find_class_pi(uint8_t class_, uint8_t subclass, uint8_t progif,
                      uint8_t *bus, uint8_t *slot, uint8_t *func);
void pci_list(void); /* print bus 0 devices (shell `pci` command) */

#endif /* NAOS_PCI_H */
