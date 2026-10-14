/* NaOS PS/2 keyboard: IRQ1 ring buffer + set-1 to ASCII (shift/caps).
 * Returns ASCII 1..127, or K_* codes (>= 0x100) for extended keys. */
#ifndef NAOS_PS2_KBD_H
#define NAOS_PS2_KBD_H

#include <stdint.h>

#define KBD_UP    0x100
#define KBD_DOWN  0x101
#define KBD_LEFT  0x102
#define KBD_RIGHT 0x103
#define KBD_PGUP  0x104
#define KBD_PGDN  0x105

void kbd_init(void);
void kbd_push(uint8_t sc);   /* called from IRQ1 */
int kbd_empty(void);
int kbd_getc(void);          /* blocking: ASCII or K_* code */

#endif /* NAOS_PS2_KBD_H */
