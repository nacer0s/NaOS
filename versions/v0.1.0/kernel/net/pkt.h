/* NaOS net helpers: big-endian put/get, ones-complement sums, IRQ guard.
 * Header-only; included by net.c, udp.c, dhcp.c, tcp.c. Own code. */
#ifndef NAOS_PKT_H
#define NAOS_PKT_H

#include <stdint.h>

static inline void pkt_put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static inline void pkt_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static inline uint16_t pkt_get16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t pkt_get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

/* Unfolded ones-complement sum (no complement); add parts then fold. */
static inline uint32_t pkt_onesum(const uint8_t *p, uint32_t len) {
  uint32_t s = 0;
  while (len > 1) {
    s += ((uint32_t)p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if (len) s += (uint32_t)p[0] << 8;
  return s;
}

static inline uint16_t pkt_fold(uint32_t s) {
  while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
  return (uint16_t)~s;
}

/* Save RFLAGS + cli. Restores exact prior state (safe in IRQ context). */
static inline uint64_t pkt_irq_save(void) {
  uint64_t f;
  __asm__ volatile ("pushfq; popq %0" : "=r" (f) :: "memory");
  __asm__ volatile ("cli" ::: "memory");
  return f;
}

static inline void pkt_irq_restore(uint64_t f) {
  __asm__ volatile ("pushq %0; popfq" :: "r" (f) : "memory");
}

#endif /* NAOS_PKT_H */
