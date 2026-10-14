/* NaOS COM1 (0x3F8) serial driver. Own code, no libc. */
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int g_init = 0;

void serial_init(void) {
  naos_outb(COM1 + 1, 0x00); /* disable interrupts */
  naos_outb(COM1 + 3, 0x80); /* DLAB on */
  naos_outb(COM1 + 0, 0x03); /* divisor 3 -> 38400 baud */
  naos_outb(COM1 + 1, 0x00);
  naos_outb(COM1 + 3, 0x03); /* 8N1 */
  naos_outb(COM1 + 2, 0xC7); /* FIFO on, clear, 14-byte threshold */
  naos_outb(COM1 + 4, 0x0B); /* IRQs + RTS/DTR */
  g_init = 1;
}

static int tx_empty(void) {
  return naos_inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
  if (!g_init) serial_init();
  if (c == '\n') serial_putc('\r');
  while (!tx_empty()) { /* spin */ }
  naos_outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
  while (*s) serial_putc(*s++);
}

void serial_put_hex64(uint64_t v) {
  static const char kHex[] = "0123456789ABCDEF";
  char buf[19];
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 0; i < 16; i++)
    buf[2 + i] = kHex[(v >> (60 - 4 * i)) & 0xF];
  buf[18] = '\0';
  serial_puts(buf);
}

void serial_put_uint64(uint64_t v) {
  char tmp[21];
  int n = 0;
  if (v == 0) { serial_putc('0'); return; }
  while (v > 0 && n < 20) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  while (n > 0) serial_putc(tmp[--n]);
}
