/* NaOS ISR dispatcher: CPU exceptions 0-31, PIC IRQs 32-47. */
#ifndef NAOS_ISR_H
#define NAOS_ISR_H

#include <stdint.h>

/* Frame layout after isr_common (CPL0 frames end at rflags; CPL3 frames
 * also carry ss:rsp pushed by the CPU — same offsets for the shared part). */
typedef struct {
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t vec, err;
  uint64_t rip, cs, rflags;
  uint64_t rsp, ss; /* valid only when (cs & 3) == 3 */
} isr_frame_t;

void isr_handler(isr_frame_t *f);

#endif /* NAOS_ISR_H */
