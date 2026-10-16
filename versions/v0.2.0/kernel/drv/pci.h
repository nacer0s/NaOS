/* NaOS PCI: config-space reads + all-buses scan. Firmware (UEFI) owns
 * bus numbering; we walk every bus 0..255 (empty slots master-abort to
 * 0xFFFF, microseconds each). Own code. */
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
/* Find first device with class/subclass (all buses). Fills bus/s/f. */
int pci_find_class(uint8_t class_, uint8_t subclass,
                   uint8_t *bus, uint8_t *slot, uint8_t *func);
/* Same, also matching prog-if (e.g. XHCI = 0x0C/0x03/0x30). */
int pci_find_class_pi(uint8_t class_, uint8_t subclass, uint8_t progif,
                      uint8_t *bus, uint8_t *slot, uint8_t *func);
/* Find first device with vendor:device IDs. Returns 1, fills bus/s/f. */
int pci_find_vendor(uint16_t vendor, uint16_t device,
                    uint8_t *bus, uint8_t *slot, uint8_t *func);
/* Find a PCI capability (e.g. 0x11 = MSI-X) by ID. Returns its config
 * offset, or 0 when absent. Safe on header types without a cap list. */
uint8_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap);
void pci_list(void); /* print all buses' devices (shell `pci` command) */

#endif /* NAOS_PCI_H */
