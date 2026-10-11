/* NaOS COM1 serial driver. Freestanding, shared by bootloader and kernel.
 * QEMU: -serial stdio captures this. Our primary debugger. */
#ifndef NAOS_SERIAL_H
#define NAOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex64(uint64_t v);
void serial_put_uint64(uint64_t v);

#endif /* NAOS_SERIAL_H */
