/* NaOS IDT (64-bit): 256 entries, vectors 0-47 installed (CPU + PIC IRQs). */
#ifndef NAOS_IDT_H
#define NAOS_IDT_H

#include <stdint.h>

void idt_init(void);

#endif /* NAOS_IDT_H */
