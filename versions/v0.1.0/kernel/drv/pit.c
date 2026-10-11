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
