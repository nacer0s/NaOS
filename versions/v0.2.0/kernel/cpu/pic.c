/* NaOS 8259 PIC driver. Own init sequence. */
#include "pic.h"
#include "../../common/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static uint8_t g_mask1 = 0xFF, g_mask2 = 0xFF;

void pic_init(void) {
  naos_outb(PIC1_CMD, 0x11);   /* ICW1: init + ICW4 */
  naos_outb(PIC2_CMD, 0x11);
  naos_outb(PIC1_DATA, 0x20);  /* ICW2: vectors 0x20-0x27 */
  naos_outb(PIC2_DATA, 0x28);  /* ICW2: vectors 0x28-0x2F */
  naos_outb(PIC1_DATA, 0x04);  /* ICW3: slave at IRQ2 */
  naos_outb(PIC2_DATA, 0x02);
  naos_outb(PIC1_DATA, 0x01);  /* ICW4: 8086 mode */
  naos_outb(PIC2_DATA, 0x01);
  naos_outb(PIC1_DATA, g_mask1); /* mask all */
  naos_outb(PIC2_DATA, g_mask2);
}

void pic_enable_irq(uint8_t irq) {
  if (irq < 8) {
    g_mask1 &= (uint8_t)~(1u << irq);
    naos_outb(PIC1_DATA, g_mask1);
  } else if (irq < 16) {
    g_mask2 &= (uint8_t)~(1u << (irq - 8));
    naos_outb(PIC2_DATA, g_mask2);
    g_mask1 &= (uint8_t)~(1u << 2); /* cascade */
    naos_outb(PIC1_DATA, g_mask1);
  }
}

void pic_eoi(uint8_t vec) {
  if (vec >= 40) naos_outb(PIC2_CMD, 0x20);
  naos_outb(PIC1_CMD, 0x20);
}
