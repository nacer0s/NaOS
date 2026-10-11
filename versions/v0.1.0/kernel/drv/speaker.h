/* NaOS PC speaker: PIT channel 2 square wave + port 0x61 gate. */
#ifndef NAOS_SPEAKER_H
#define NAOS_SPEAKER_H

#include <stdint.h>

void speaker_beep(uint32_t hz, uint32_t ms); /* blocking, interrupts on */

#endif /* NAOS_SPEAKER_H */
