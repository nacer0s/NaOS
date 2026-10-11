/* NaOS port I/O. Freestanding, shared by bootloader (x86) and kernel. */
#ifndef NAOS_IO_H
#define NAOS_IO_H

#include <stdint.h>

static inline void naos_outb(uint16_t port, uint8_t val) {
  __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void naos_outw(uint16_t port, uint16_t val) {
  __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void naos_outl(uint16_t port, uint32_t val) {
  __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t naos_inw(uint16_t port) {
  uint16_t r;
  __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
  return r;
}

static inline uint32_t naos_inl(uint16_t port) {
  uint32_t r;
  __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
  return r;
}

static inline uint8_t naos_inb(uint16_t port) {
  uint8_t r;
  __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
  return r;
}

static inline void naos_insw(uint16_t port, void *buf, uint32_t count) {
  __asm__ volatile ("rep insw"
                    : "+D"(buf), "+c"(count)
                    : "d"(port)
                    : "memory");
}

static inline void naos_outsw(uint16_t port, const void *buf, uint32_t count) {
  __asm__ volatile ("rep outsw"
                    : "+S"(buf), "+c"(count)
                    : "d"(port)
                    : "memory");
}

#endif /* NAOS_IO_H */
