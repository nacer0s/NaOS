/* NaOS GDT: null + kernel code/data + user data/code + TSS (2 slots).
 * Selectors: KCODE 0x08, KDATA 0x10, UDATA 0x18, UCODE 0x20, TSS 0x28. */
#ifndef NAOS_GDT_H
#define NAOS_GDT_H

#include <stdint.h>

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA 0x18
#define GDT_UCODE 0x20
#define GDT_TSS   0x28

void gdt_init(void);

#endif /* NAOS_GDT_H */
