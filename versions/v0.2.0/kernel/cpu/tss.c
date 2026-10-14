/* NaOS TSS (104 bytes + no I/O bitmap => base+104 denies all user ports). */
#include "tss.h"
#include "gdt.h"
#include <stddef.h>

typedef struct {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap;
} __attribute__((packed)) tss_t;

static tss_t g_tss;

extern void gdt_install_tss(uint64_t base, uint32_t limit);

void tss_init(uint64_t rsp0) {
  for (size_t i = 0; i < sizeof(g_tss); i++)
    ((uint8_t *)&g_tss)[i] = 0;
  g_tss.rsp0 = rsp0;
  g_tss.iomap = sizeof(tss_t); /* I/O bitmap starts past end: deny all */
  gdt_install_tss((uint64_t)(uintptr_t)&g_tss, sizeof(tss_t) - 1);
  __asm__ volatile ("ltr %%ax" :: "a"((uint16_t)GDT_TSS) : "memory");
}

void tss_set_rsp0(uint64_t rsp0) {
  g_tss.rsp0 = rsp0;
}
