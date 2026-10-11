/* NaOS PCI config access via 0xCF8/0xCFC. Bus 0 scan (QEMU + most PCs'
 * boot hardware lives there; bridges beyond are out of scope v1). */
#include "pci.h"
#include "../lib/kprintf.h"
#include "../../common/io.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static uint32_t addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
  return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
         ((uint32_t)func << 8) | (off & 0xFCu);
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
  naos_outl(CFG_ADDR, addr(bus, slot, func, off));
  return naos_inl(CFG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
  uint32_t v = pci_read(bus, slot, func, off);
  return (uint16_t)((v >> ((off & 3) * 8)) & 0xFFFFu);
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
  uint32_t v = pci_read(bus, slot, func, off);
  return (uint8_t)((v >> ((off & 3) * 8)) & 0xFFu);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                 uint16_t v) {
  /* Read-modify-write the aligned dword (offset may be 2-aligned). */
  uint8_t a = off & 0xFCu;
  uint32_t cur = pci_read(bus, slot, func, a);
  uint32_t sh = (off & 3) * 8;
  cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
  naos_outl(CFG_ADDR, addr(bus, slot, func, a));
  naos_outl(CFG_DATA, cur);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                 uint32_t v) {
  naos_outl(CFG_ADDR, addr(bus, slot, func, off));
  naos_outl(CFG_DATA, v);
}

int pci_find_class(uint8_t class_, uint8_t subclass,
                   uint8_t *bus, uint8_t *slot, uint8_t *func) {
  return pci_find_class_pi(class_, subclass, 0xFF, bus, slot, func);
}

int pci_find_class_pi(uint8_t class_, uint8_t subclass, uint8_t progif,
                      uint8_t *bus, uint8_t *slot, uint8_t *func) {
  for (uint8_t s = 0; s < 32; s++) {
    if (pci_read16(0, s, 0, 0) == 0xFFFF) continue;
    uint8_t nf = (pci_read8(0, s, 0, 0x0E) & 0x80) ? 8 : 1;
    for (uint8_t f = 0; f < nf; f++) {
      if (pci_read16(0, s, f, 0) == 0xFFFF) continue;
      if (pci_read8(0, s, f, 0x0B) == class_ &&
          pci_read8(0, s, f, 0x0A) == subclass &&
          (progif == 0xFF || pci_read8(0, s, f, 0x09) == progif)) {
        *bus = 0;
        *slot = s;
        *func = f;
        return 1;
      }
    }
  }
  return 0;
}

void pci_list(void) {
  kprintf("bus:slot.func class/subclass\n");
  for (uint8_t s = 0; s < 32; s++) {
    if (pci_read16(0, s, 0, 0) == 0xFFFF) continue;
    uint8_t nf = (pci_read8(0, s, 0, 0x0E) & 0x80) ? 8 : 1;
    for (uint8_t f = 0; f < nf; f++) {
      if (pci_read16(0, s, f, 0) == 0xFFFF) continue;
      kprintf("00:%x.%u  %x/%x\n", (unsigned)s, (unsigned)f,
              (unsigned)pci_read8(0, s, f, 0x0B),
              (unsigned)pci_read8(0, s, f, 0x0A));
    }
  }
}
