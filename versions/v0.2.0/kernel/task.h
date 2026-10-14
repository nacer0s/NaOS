/* NaOS scheduler + syscalls. Each user task has a private address space
 * (supervisor kernel + .utext + own user stack); task 0 shares boot tables. */
#ifndef NAOS_TASK_H
#define NAOS_TASK_H

#include <stdint.h>
#include "cpu/isr.h"

#define MAX_TASKS 16
#define QUANTUM 5 /* timer ticks per timeslice (50ms @100Hz) */
#define MAX_FDS 8
#define FD_WRITE_CAP 4096u /* per-fd write buffer: one PMM page */

/* Open file description (files only; 0/1/2 are console, not table). */
typedef struct {
  int used;
  int write;           /* 0 = read mode, 1 = write/append (buffered) */
  uint32_t clus;       /* read mode: file start (append: presize) */
  uint32_t size;
  uint32_t pos;
  uint32_t parent;     /* write mode: flush target */
  uint8_t leaf[11];
  char *wbuf;          /* write mode: staged content */
  uint32_t wlen;
} ffd_t;

/* Syscalls (int 0x80): number in RAX, args RDI/RSI/RDX. */
#define SYS_EXIT   0
#define SYS_PRINT  1 /* RDI=ptr RSI=len: write bytes to console */
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SLEEP  4 /* RDI=ticks */
#define SYS_FORK   5 /* clone: child gets 0, parent gets child id */
#define SYS_OPEN   6 /* RDI=path RSI=mode(0=r,1=w,2=a): fd or negative */
#define SYS_READ   7 /* RDI=fd RSI=buf RDX=len: bytes or negative */
#define SYS_WRITE  8 /* RDI=fd RSI=buf RDX=len: bytes or negative */
#define SYS_CLOSE  9 /* RDI=fd: 0 ok, negative on error */

typedef enum {
  T_EMPTY = 0,
  T_READY,
  T_SLEEP,
  T_DEAD
} tstate_t;

typedef struct {
  int used;
  tstate_t state;
  char name[16];
  uint64_t ksp;        /* saved interrupt frame (on kstack) */
  uint64_t kstack_top; /* TSS.RSP0 while this task runs */
  uint64_t kstack_base;
  uint32_t kstack_pages;
  uint64_t ustack_base; /* physical (identity) address of user stack */
  uint32_t ustack_pages;
  uint64_t cr3;        /* address space (task 0: shared boot tables) */
  uint64_t tbl[4];     /* PML4/PDPT/PD/PT phys (task 0: unused) */
  uint64_t wake;       /* for T_SLEEP */
  ffd_t fds[MAX_FDS];  /* open files (task 0: unused) */
} task_t;

void sched_init(uint64_t boot_stack_top);
void sched_on_tick(isr_frame_t *f);   /* from IRQ0 */
void sched_syscall(isr_frame_t *f);   /* from int 0x80 (ring 3 only) */
int task_spawn(const char *name, void (*entry)(void));
int task_spawn_elf(const char *name, const char *path); /* program from disk */
int task_kill(int id);
void task_fault_current(void); /* kill a faulting ring-3 task, switch away */
int task_cow_fixup(uint64_t addr); /* 1 if a CoW write fault was resolved */
void task_list(void);
int sched_lock(void);   /* preemption off; returns cookie */
void sched_unlock(void);

extern volatile int g_nopreempt;
extern volatile uint64_t g_switch_to;

#endif /* NAOS_TASK_H */
