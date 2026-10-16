/* NaOS PCI config access via 0xCF8/0xCFC. All-buses scan: firmware owns
 * bus numbering, so walk 0..255 (empty slots master-abort to 0xFFFF). */
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

/* Walk every present function on every bus. fn returns 1 to stop.
 * Returns 1 when stopped early. */
typedef int (*pci_each_fn)(uint8_t bus, uint8_t slot, uint8_t func,
                           void *ctx);
static int pci_each(pci_each_fn fn, void *ctx) {
  uint16_t b;
  for (b = 0; b < 256; b++) {
    uint8_t s;
    for (s = 0; s < 32; s++) {
      uint8_t f, nf;
      if (pci_read16((uint8_t)b, s, 0, 0) == 0xFFFF) continue;
      nf = (pci_read8((uint8_t)b, s, 0, 0x0E) & 0x80) ? 8 : 1;
      for (f = 0; f < nf; f++) {
        if (pci_read16((uint8_t)b, s, f, 0) == 0xFFFF) continue;
        if (fn((uint8_t)b, s, f, ctx)) return 1;
      }
    }
  }
  return 0;
}

typedef struct {
  uint8_t class_, subclass, progif;
  uint8_t bus, slot, func;
} class_ctx_t;

static int class_match(uint8_t bus, uint8_t slot, uint8_t func, void *ctx) {
  class_ctx_t *c = (class_ctx_t *)ctx;
  if (pci_read8(bus, slot, func, 0x0B) == c->class_ &&
      pci_read8(bus, slot, func, 0x0A) == c->subclass &&
      (c->progif == 0xFF || pci_read8(bus, slot, func, 0x09) == c->progif)) {
    c->bus = bus;
    c->slot = slot;
    c->func = func;
    return 1;
  }
  return 0;
}

typedef struct {
  uint16_t vendor, device;
  uint8_t bus, slot, func;
} vendor_ctx_t;

static int vendor_match(uint8_t bus, uint8_t slot, uint8_t func, void *ctx) {
  vendor_ctx_t *c = (vendor_ctx_t *)ctx;
  if (pci_read16(bus, slot, func, 0x00) == c->vendor &&
      pci_read16(bus, slot, func, 0x02) == c->device) {
    c->bus = bus;
    c->slot = slot;
    c->func = func;
    return 1;
  }
  return 0;
}

int pci_find_class(uint8_t class_, uint8_t subclass,
                   uint8_t *bus, uint8_t *slot, uint8_t *func) {
  return pci_find_class_pi(class_, subclass, 0xFF, bus, slot, func);
}

int pci_find_class_pi(uint8_t class_, uint8_t subclass, uint8_t progif,
                      uint8_t *bus, uint8_t *slot, uint8_t *func) {
  class_ctx_t c;
  c.class_ = class_;
  c.subclass = subclass;
  c.progif = progif;
  if (!pci_each(class_match, &c)) return 0;
  *bus = c.bus;
  *slot = c.slot;
  *func = c.func;
  return 1;
}

int pci_find_vendor(uint16_t vendor, uint16_t device,
                    uint8_t *bus, uint8_t *slot, uint8_t *func) {
  vendor_ctx_t c;
  c.vendor = vendor;
  c.device = device;
  if (!pci_each(vendor_match, &c)) return 0;
  *bus = c.bus;
  *slot = c.slot;
  *func = c.func;
  return 1;
}

uint8_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap) {
  uint8_t off;
  int guard;
  /* Status bit 4 = capability list present; header type bit 7 =
   * multifunction is handled by the caller (we get an exact b:s.f). */
  if (!(pci_read16(bus, slot, func, 0x06) & 0x10u)) return 0;
  off = (uint8_t)(pci_read8(bus, slot, func, 0x34) & 0xFCu);
  for (guard = 0; guard < 48 && off >= 0x40u; guard++) {
    uint8_t id = pci_read8(bus, slot, func, off);
    uint8_t next = (uint8_t)(pci_read8(bus, slot, func, (uint8_t)(off + 1)) & 0xFCu);
    if (id == cap) return off;
    if (next < 0x40u) break;
    off = next;
  }
  return 0;
}

static int list_one(uint8_t bus, uint8_t slot, uint8_t func, void *ctx) {
  (void)ctx;
  kprintf("%x:%x.%u  %x/%x %x:%x%s\n", (unsigned)bus, (unsigned)slot,
          (unsigned)func, (unsigned)pci_read8(bus, slot, func, 0x0B),
          (unsigned)pci_read8(bus, slot, func, 0x0A),
          (unsigned)pci_read16(bus, slot, func, 0x00),
          (unsigned)pci_read16(bus, slot, func, 0x02),
          pci_find_cap(bus, slot, func, 0x11) ? " MSIX" : "");
  return 0;
}

void pci_list(void) {
  kprintf("bus:slot.func class/subclass vendor:device flags\n");
  pci_each(list_one, 0);
}
