/* NaOS PC speaker driver. Channel 2 is independent of the timer (ch 0). */
#include "speaker.h"
#include "pit.h"
#include "../../common/io.h"

void speaker_beep(uint32_t hz, uint32_t ms) {
  if (hz < 20 || hz > 20000 || ms == 0) return;
  uint32_t div = 1193180u / hz;
  if (div == 0) div = 1;
  if (div > 0xFFFFu) div = 0xFFFFu;
  uint8_t prev = naos_inb(0x61);
  naos_outb(0x43, 0xB6); /* ch2, square wave */
  naos_outb(0x42, (uint8_t)(div & 0xFF));
  naos_outb(0x42, (uint8_t)((div >> 8) & 0xFF));
  naos_outb(0x61, (uint8_t)(prev | 0x03)); /* gate + speaker on */
  uint64_t end = pit_ticks() + (ms * 100u + 999u) / 1000u;
  while (pit_ticks() < end) { __asm__ volatile ("hlt"); }
  naos_outb(0x61, (uint8_t)(prev & ~0x03u));
}
