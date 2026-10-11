/* NaOS 8259 PIC: remap to 0x20/0x28, masks, EOI. */
#ifndef NAOS_PIC_H
#define NAOS_PIC_H

#include <stdint.h>

void pic_init(void);          /* remap + mask all */
void pic_enable_irq(uint8_t irq);  /* unmask 0-15 */
void pic_eoi(uint8_t vec);    /* vec = IDT vector 32-47 */

#endif /* NAOS_PIC_H */
