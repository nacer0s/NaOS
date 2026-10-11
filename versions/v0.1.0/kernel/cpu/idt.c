/* NaOS IDT setup. Stubs (isr0..isr47) live in isr.asm, handler in isr.c. */
#include "idt.h"

typedef struct {
  uint16_t offset_low;
  uint16_t selector;
  uint8_t  ist;
  uint8_t  type_attr;
  uint16_t offset_mid;
  uint32_t offset_high;
  uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) idtr_t;

static idt_entry_t g_idt[256];
static idtr_t g_idtr;

extern void idt_load(uint64_t idtr_addr);
/* ISR stub addresses, defined in isr.asm */
extern void *g_isr_stubs[48];
extern void isr128(void); /* int 0x80 syscall gate */

static void set_gate(int vec, void *handler, int dpl) {
  uint64_t addr = (uint64_t)(uintptr_t)handler;
  g_idt[vec].offset_low = (uint16_t)(addr & 0xFFFF);
  g_idt[vec].selector = 0x08;
  g_idt[vec].ist = 0;
  g_idt[vec].type_attr = (uint8_t)(0x8E | ((dpl & 3) << 5));
  g_idt[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
  g_idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
  g_idt[vec].zero = 0;
}

void idt_init(void) {
  for (int i = 0; i < 256; i++) {
    g_idt[i].offset_low = 0;
    g_idt[i].selector = 0;
    g_idt[i].ist = 0;
    g_idt[i].type_attr = 0;
    g_idt[i].offset_mid = 0;
    g_idt[i].offset_high = 0;
    g_idt[i].zero = 0;
  }
  for (int i = 0; i < 48; i++) set_gate(i, g_isr_stubs[i], 0);
  set_gate(0x80, isr128, 3); /* syscalls callable from ring 3 */
  g_idtr.limit = (uint16_t)(sizeof(g_idt) - 1);
  g_idtr.base = (uint64_t)(uintptr_t)&g_idt;
  idt_load((uint64_t)(uintptr_t)&g_idtr);
}
