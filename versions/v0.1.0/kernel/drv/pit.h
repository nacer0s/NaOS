/* NaOS PIT timer driver (IRQ0, 100 Hz tick counter). */
#ifndef NAOS_PIT_H
#define NAOS_PIT_H

#include <stdint.h>

void pit_init(uint32_t hz);
void pit_tick(void);       /* called from IRQ0 */
uint64_t pit_ticks(void);

#endif /* NAOS_PIT_H */
