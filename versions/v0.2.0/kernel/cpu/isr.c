/* NaOS ISR dispatcher. Exceptions print + halt (kernel) or kill (user);
 * IRQs dispatch to timer/kbd; 0x80 is the ring-3 syscall gate. */
#include "isr.h"
#include "pic.h"
#include "../task.h"
#include "../drv/pit.h"
#include "../drv/ps2_kbd.h"
#include "../lib/kprintf.h"
#include "../../common/io.h"

static const char *kExcNames[32] = {
  "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
  "Bound range", "Invalid opcode", "No coprocessor", "Double fault",
  "Coprocessor overrun", "Invalid TSS", "Segment missing", "Stack fault",
  "General protection", "Page fault", "Reserved", "x87 FP", "Alignment",
  "Machine check", "SIMD FP", "Virtualization", "Control protection",
  "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
  "Hypervisor", "VMM comm", "Security", "Reserved"
};

void isr_handler(isr_frame_t *f) {
  if (f->vec == 0x80) {
    if (((f->cs) & 3) != 3) {
      kprintf("\nSYSCALL from ring 0?!\n");
      __asm__ volatile ("cli");
      for (;;) { __asm__ volatile ("hlt"); }
    }
    sched_syscall(f);
    return;
  }
  if (f->vec < 32) {
    if (((f->cs) & 3) == 3) {
      /* User fault: kill the task, keep the OS alive. */
      if (f->vec == 14) {
        uint64_t cr2, cr3;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        /* Present + write on a CoW page: copy it and resume. */
        if ((f->err & 3) == 3 && task_cow_fixup(cr2)) return;
        const char *why = ((f->err & 1) == 0) ? "not-present"
                          : ((f->err & 4) ? "protection" : "reserved");
        kprintf("\nTASK FAULT vec=%u (Page fault) %s err=%x addr=%p rip=%p cr3=%p rsp=%p -- killed\n",
                (unsigned)f->vec, why,
                (unsigned)f->err, (void *)(uintptr_t)cr2,
                (void *)(uintptr_t)f->rip, (void *)(uintptr_t)cr3,
                (void *)(uintptr_t)f->rsp);
      } else {
        kprintf("\nTASK FAULT vec=%u (%s) err=%x rip=%p -- killed\n",
                (unsigned)f->vec, kExcNames[f->vec],
                (unsigned)f->err, (void *)(uintptr_t)f->rip);
      }
      task_fault_current();
      return;
    }
    if (f->vec == 14) {
      uint64_t cr2;
      __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
      const char *why = ((f->err & 1) == 0) ? "not-present"
                        : ((f->err & 4) ? "protection" : "reserved");
      kprintf("\nPAGE FAULT %s err=%x addr=%p rip=%p -- HALT\n", why,
              (unsigned)f->err, (void *)(uintptr_t)cr2,
              (void *)(uintptr_t)f->rip);
    } else {
      kprintf("\nEXC #%u (%s) err=%x rip=%p -- HALT\n",
              (unsigned)f->vec, kExcNames[f->vec],
              (unsigned)f->err, (void *)(uintptr_t)f->rip);
    }
    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
  }
  if (f->vec == 32) {
    sched_on_tick(f);
    pic_eoi(32);
    return;
  }
  if (f->vec == 33) {
    /* PS/2 data ready: drain to ring buffer. */
    uint8_t sc = naos_inb(0x60);
    kbd_push(sc);
    pic_eoi(33);
    return;
  }
  /* Other IRQs / spurious: acknowledge. */
  pic_eoi((uint8_t)f->vec);
}
