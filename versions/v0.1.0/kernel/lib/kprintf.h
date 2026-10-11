/* NaOS kernel mini-printf: writes to framebuffer AND serial. Own code. */
#ifndef NAOS_KPRINTF_H
#define NAOS_KPRINTF_H

#include <stddef.h>

void kprintf(const char *fmt, ...);

/* Output capture for shell redirect (`cmd > file`). While active, output
 * goes only into buf (screen+serial stay quiet). Returns bytes stored;
 * kprintf_capture_full() tells whether output was truncated. */
void kprintf_begin_capture(char *buf, size_t size);
size_t kprintf_end_capture(void);
int kprintf_capture_full(void);

#endif /* NAOS_KPRINTF_H */
