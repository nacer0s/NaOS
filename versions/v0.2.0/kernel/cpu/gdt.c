/* NaOS GDT: raw 8-byte entries (TSS occupies two slots). */
#include "gdt.h"

static uint64_t g_gdt[8];

typedef struct {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) gdtr_t;

static gdtr_t g_gdtr;

extern void gdt_flush(uint64_t gdtr_addr);

void gdt_init(void) {
  g_gdt[0] = 0x0000000000000000ULL; /* null */
  g_gdt[1] = 0x00AF9A000000FFFFULL; /* kernel code */
  g_gdt[2] = 0x00CF92000000FFFFULL; /* kernel data */
  g_gdt[3] = 0x00CFF2000000FFFFULL; /* user data (DPL3) */
  g_gdt[4] = 0x00AFFA000000FFFFULL; /* user code (DPL3, 64-bit) */
  /* Slots 5-6: TSS descriptor, installed by tss_init (needs the address). */
  g_gdt[5] = 0;
  g_gdt[6] = 0;
  g_gdt[7] = 0;
  g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
  g_gdtr.base = (uint64_t)(uintptr_t)&g_gdt;
  gdt_flush((uint64_t)(uintptr_t)&g_gdtr);
}

/* Installed by tss_init: 16-byte available-TSS descriptor at slot 5. */
void gdt_install_tss(uint64_t base, uint32_t limit) {
  uint64_t lo = (limit & 0xFFFFULL) |
                ((base & 0xFFFFFFULL) << 16) |
                (0x89ULL << 40) |               /* P=1, DPL=0, type 9 (TSS) */
                (((uint64_t)(limit >> 16) & 0xFULL) << 48);
  uint64_t hi = (base >> 32) & 0xFFFFFFFFULL;
  g_gdt[5] = lo;
  g_gdt[6] = hi;
}
