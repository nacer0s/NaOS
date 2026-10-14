/* NaOS PIT channel 0, square-wave, IRQ0 rate generator. */
#include "pit.h"
#include "../../common/io.h"

static volatile uint64_t g_ticks;

void pit_init(uint32_t hz) {
  if (hz == 0) hz = 100;
  uint32_t div = 1193180u / hz;
  if (div == 0) div = 1;
  if (div > 0xFFFFu) div = 0xFFFFu;
  naos_outb(0x43, 0x36);
  naos_outb(0x40, (uint8_t)(div & 0xFF));
  naos_outb(0x40, (uint8_t)((div >> 8) & 0xFF));
}

void pit_tick(void) {
  g_ticks++;
}

uint64_t pit_ticks(void) {
  return g_ticks;
}

/* CPU timestamp counter: advances whenever the CPU runs (invariant TSC on
 * everything x86_64; hlt/C1 never stops it). Last-resort blink timebase
 * when both RTC and PIT are unusable. Bit 32 flips every ~1-4s by CPU. */
uint64_t pit_tsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
  return ((uint64_t)hi << 32) | lo;
}
