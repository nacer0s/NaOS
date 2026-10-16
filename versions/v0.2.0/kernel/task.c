/* NaOS round-robin scheduler. Context switch = resume another task's saved
 * interrupt frame (see isr.asm g_switch_to). Task 0 is the kernel/shell:
 * its frame is re-saved on every kernel-mode timer tick, so it is always
 * resumable. New user tasks start from a crafted first frame via iretq. */
#include "task.h"
#include "cpu/gdt.h"
#include "cpu/tss.h"
#include "cpu/pic.h"
#include "drv/pit.h"
#include "drv/fb.h"
#include "drv/usb_kbd.h"
#include "net/net.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/cow.h"
#include "mm/kmalloc.h"
#include "fs/fat32.h"
#include "../common/cstring.h"
#include "../common/serial.h"

#define KSTACK_PAGES 2
#define USTACK_PAGES 4

static task_t g_tasks[MAX_TASKS];
static int g_current = 0;
static uint64_t g_last_switch;
static int g_lock_depth;

/* Freed task memory is cached as whole bundles (stacks + tables) for
 * reuse instead of going back to the PMM: returning pages would scramble
 * the LIFO order that pmm_alloc_contig depends on. */
typedef struct {
  uint64_t kbase, ubase;
  uint64_t tbl[4];
  uint64_t cr3;
} bundle_t;
static bundle_t g_bundles[16];
static int g_bundle_count;

volatile int g_nopreempt = 0;
volatile uint64_t g_switch_to = 0;

static int64_t task_fork(isr_frame_t *f); /* defined below sched_syscall */
static int sys_open(isr_frame_t *f);
static int64_t sys_read(isr_frame_t *f);
static int64_t sys_write(isr_frame_t *f);
static int sys_close(isr_frame_t *f);
static int64_t sys_seek(isr_frame_t *f);
static void fds_init(int id);
static void fds_cleanup(int id);

int sched_lock(void) {
  int was = g_lock_depth;
  g_lock_depth++;
  g_nopreempt = 1;
  return was;
}

void sched_unlock(void) {
  if (g_lock_depth > 0) g_lock_depth--;
  if (g_lock_depth == 0) g_nopreempt = 0;
}

void sched_init(uint64_t boot_stack_top) {
  for (int i = 0; i < MAX_TASKS; i++) {
    g_tasks[i].used = 0;
    g_tasks[i].state = T_EMPTY;
  }
  g_tasks[0].used = 1;
  g_tasks[0].state = T_READY;
  g_tasks[0].ksp = 0; /* set on first kernel tick */
  g_tasks[0].kstack_top = boot_stack_top;
  g_tasks[0].kstack_base = 0;
  g_tasks[0].ustack_base = 0;
  g_tasks[0].cr3 = 0; /* filled below: shared boot tables */
  naos_memset(g_tasks[0].name, 0, sizeof(g_tasks[0].name));
  g_tasks[0].name[0] = 'k';
  g_tasks[0].name[1] = 'e';
  g_tasks[0].name[2] = 'r';
  g_tasks[0].name[3] = 'n';
  g_tasks[0].name[4] = 'e';
  g_tasks[0].name[5] = 'l';
  g_current = 0;
  g_last_switch = 0;
  tss_init(boot_stack_top);
  g_tasks[0].cr3 = vmm_cr3();
  kprintf("sched: quantum=%u ticks, %d slots\n", (unsigned)QUANTUM, MAX_TASKS);
}

/* First valid frame wins the round-robin scan after `g_current`. */
static int pick_next(void) {
  for (int step = 1; step <= MAX_TASKS; step++) {
    int i = (g_current + step) % MAX_TASKS;
    if (g_tasks[i].used && g_tasks[i].state == T_READY && g_tasks[i].ksp != 0)
      return i;
  }
  return -1;
}

static void switch_to(int next) {
  g_current = next;
  tss_set_rsp0(g_tasks[next].kstack_top);
  /* Kernel mappings are identical in every space, so loading CR3 here is
   * safe; the frame swap after this runs on the new tables. */
  __asm__ volatile ("mov %0, %%cr3" :: "r"(g_tasks[next].cr3) : "memory");
  g_last_switch = pit_ticks();
  g_switch_to = g_tasks[next].ksp;
}

static void reap_dead(void) {
  for (int i = 1; i < MAX_TASKS; i++) {
    int shared = 0;
    uint32_t p;
    if (!g_tasks[i].used || g_tasks[i].state != T_DEAD || i == g_current)
      continue;
    /* Drop program pages first (task tables still mapped: identity). */
    vmm_free_prog(g_tasks[i].tbl[3]);
    fds_cleanup(i); /* release write buffers (unflushed writes die here) */
    /* CoW-shared pages must not enter bundles (a recycled bundle would
     * alias a live sibling's memory): check stacks + user PT. */
    for (p = 0; p < g_tasks[i].ustack_pages; p++) {
      if (cow_shared(g_tasks[i].ustack_base + (uint64_t)p * 0x1000ULL)) {
        shared = 1;
        break;
      }
    }
    if (!shared && g_tasks[i].tbl[3] != 0) {
      const uint64_t *pt = (const uint64_t *)(uintptr_t)g_tasks[i].tbl[3];
      for (uint64_t e = 0; e < 512 && !shared; e++) {
        if ((pt[e] & 1ULL) && cow_shared(pt[e] & ~0xFFFULL)) shared = 1;
      }
    }
    if (!shared && g_bundle_count < 16) {
      g_bundles[g_bundle_count].kbase = g_tasks[i].kstack_base;
      g_bundles[g_bundle_count].ubase = g_tasks[i].ustack_base;
      for (int t = 0; t < 4; t++)
        g_bundles[g_bundle_count].tbl[t] = g_tasks[i].tbl[t];
      g_bundles[g_bundle_count].cr3 = g_tasks[i].cr3;
      g_bundle_count++;
    } else {
      for (uint32_t p = 0; p < g_tasks[i].kstack_pages; p++)
        pmm_free_page(g_tasks[i].kstack_base + (uint64_t)p * 0x1000ULL);
      for (uint32_t p = 0; p < g_tasks[i].ustack_pages; p++)
        cow_release(g_tasks[i].ustack_base + (uint64_t)p * 0x1000ULL);
      for (int t = 0; t < 4; t++)
        pmm_free_page(g_tasks[i].tbl[t]);
    }
    g_tasks[i].used = 0;
    g_tasks[i].state = T_EMPTY;
  }
}

void sched_on_tick(isr_frame_t *f) {
  pit_tick();
  caret_refresh(); /* blink from IRQ: alive even if input parks the shell */
  usb_kbd_poll(); /* drain XHCI event ring (no-op without USB kbd) */
  net_poll();     /* drain NIC RX ring (no-op without network) */
  if (((f->cs) & 3) == 3)
    g_tasks[g_current].ksp = (uint64_t)(uintptr_t)f;
  else
    g_tasks[0].ksp = (uint64_t)(uintptr_t)f;
  /* Wake sleepers. */
  uint64_t now = pit_ticks();
  for (int i = 1; i < MAX_TASKS; i++)
    if (g_tasks[i].used && g_tasks[i].state == T_SLEEP && now >= g_tasks[i].wake)
      g_tasks[i].state = T_READY;
  if (g_nopreempt) return;
  reap_dead(); /* PMM-touching: only when preemption is allowed, so it can
                * never interleave with a locked contig-alloc in spawn. */
  if (now - g_last_switch < QUANTUM) return;
  int next = pick_next();
  if (next < 0 || next == g_current) return;
  switch_to(next);
}

static void console_write(const char *p, uint64_t len) {
  if (len > 2048) len = 2048;
  for (uint64_t i = 0; i < len; i++) {
    fb_putc(p[i]);
    serial_putc(p[i]);
  }
}

/* Is [va, va+len) mapped user-accessible in task t's space?
 * (All user memory lives in PD[0]'s 2MB window: stacks + programs.) */
static int user_range_ok(const task_t *t, uint64_t va, uint64_t len) {
  if (len == 0) return 1;
  if (len > 2048) return 0;
  if (t->tbl[3] == 0) return 0;
  uint64_t end = va + len;
  if (end < va) return 0;
  if (va < 0x40000000ULL || end > 0x40200000ULL) return 0;
  const uint64_t *pt = (const uint64_t *)(uintptr_t)t->tbl[3];
  for (uint64_t a = va & ~0xFFFULL; a < end; a += 0x1000ULL) {
    uint64_t e = pt[(a >> 12) & 0x1FFULL];
    if (!(e & 1ULL) || !(e & 4ULL)) return 0; /* need present + user */
  }
  return 1;
}

void sched_syscall(isr_frame_t *f) {
  uint64_t num = f->rax;
  switch (num) {
    case SYS_EXIT:
      g_tasks[g_current].state = T_DEAD;
      {
        int n = pick_next();
        if (n < 0) { /* unreachable: kernel task is always resumable */
          kprintf("sched: no task to run!\n");
          for (;;) { __asm__ volatile ("hlt"); }
        }
        switch_to(n);
      }
      break;
    case SYS_PRINT: {
      uint64_t len = (f->rsi > 2048) ? 2048 : f->rsi;
      uint64_t ptr = f->rdi;
      if (!user_range_ok(&g_tasks[g_current], ptr, len)) {
        f->rax = (uint64_t)-1; /* bad pointer: fail, don't halt the OS */
        break;
      }
      console_write((const char *)(uintptr_t)ptr, len);
      f->rax = len;
      break;
    }
    case SYS_YIELD: {
      int n = pick_next();
      if (n >= 0 && n != g_current) switch_to(n);
      break;
    }
    case SYS_GETPID:
      f->rax = (uint64_t)g_current;
      break;
    case SYS_SLEEP: {
      if (f->rdi == 0) {
        int n = pick_next();
        if (n >= 0 && n != g_current) switch_to(n);
        break;
      }
      g_tasks[g_current].state = T_SLEEP;
      g_tasks[g_current].wake = pit_ticks() + f->rdi;
      {
        int n = pick_next();
        if (n < 0) { /* nothing else: stay (will be woken by ticks) */
          g_tasks[g_current].state = T_READY;
        } else {
          switch_to(n);
        }
      }
      break;
    }
    case SYS_FORK:
      f->rax = (uint64_t)(int64_t)task_fork(f);
      break;
    case SYS_OPEN:
      f->rax = (uint64_t)(int64_t)sys_open(f);
      break;
    case SYS_READ:
      f->rax = (uint64_t)sys_read(f);
      break;
    case SYS_WRITE:
      f->rax = (uint64_t)sys_write(f);
      break;
    case SYS_CLOSE:
      f->rax = (uint64_t)(int64_t)sys_close(f);
      break;
    case SYS_SEEK:
      f->rax = (uint64_t)(int64_t)sys_seek(f);
      break;
    default:
      f->rax = (uint64_t)-1;
      break;
  }
}

/* Copy a user path (≤255 chars + NUL) into kbuf. 0 ok. */
static int copy_path(const task_t *t, uint64_t uptr, char *kbuf) {
  const char *u;
  if (!user_range_ok(t, uptr, 256)) return -1;
  u = (const char *)(uintptr_t)uptr;
  for (int i = 0; i < 256; i++) {
    kbuf[i] = u[i];
    if (u[i] == '\0') return 0;
  }
  return -1; /* unterminated */
}

static void fds_init(int id) {
  for (int f = 0; f < MAX_FDS; f++) {
    g_tasks[id].fds[f].used = 0;
    g_tasks[id].fds[f].wbuf = 0;
  }
}

static void fds_cleanup(int id) {
  for (int f = 0; f < MAX_FDS; f++) {
    if (g_tasks[id].fds[f].used && g_tasks[id].fds[f].wbuf) {
      pmm_free_page((uint64_t)(uintptr_t)g_tasks[id].fds[f].wbuf);
      g_tasks[id].fds[f].wbuf = 0;
    }
    g_tasks[id].fds[f].used = 0;
  }
}

/* fds 0/1/2 are console (0 invalid, 1/2 write-only). Table holds 3+. */
static int fd_alloc(void) {
  for (int f = 3; f < MAX_FDS; f++) {
    if (!g_tasks[g_current].fds[f].used) return f;
  }
  return -1;
}

static int sys_open(isr_frame_t *f) {
  /* RDI=path RSI=mode (0=r, 1=w, 2=a). */
  static char kpath[256];
  int mode = (int)f->rsi;
  int id;
  ffd_t *fd;
  if (mode < 0 || mode > 2) return -1;
  if (copy_path(&g_tasks[g_current], f->rdi, kpath) != 0) return -1;
  id = fd_alloc();
  if (id < 0) return -1;
  fd = &g_tasks[g_current].fds[id];
  fd->used = 0;
  fd->wbuf = 0;
  if (mode == 0) {
    uint32_t c, sz;
    if (fat_fopen_r(kpath, &c, &sz) != 0) return -2;
    fd->write = 0;
    fd->clus = c;
    fd->size = sz;
    fd->pos = 0;
    fd->used = 1;
    return id;
  }
  /* Write/append: validate the parent now, stage content in RAM,
   * create-or-replace the file at close. */
  {
    uint32_t parent, c = 0, sz = 0;
    uint8_t leaf[11];
    int r = fat_parent_leaf(kpath, &parent, leaf, fd->up);
    if (r != 0) return -2;
    if (mode == 2 && fat_fopen_r(kpath, &c, &sz) == 0) {
      uint64_t b;
      if (sz > FD_WRITE_CAP) return -3; /* too big to stage */
      if (sz > 0) {
        b = pmm_alloc_page();
        if (!b) return -4;
        {
          int got = fat_read_at(c, sz, 0, (void *)(uintptr_t)b, sz);
          if (got < 0 || (uint32_t)got != sz) {
            pmm_free_page(b);
            return -4;
          }
        }
        fd->wbuf = (char *)(uintptr_t)b;
      } else {
        fd->wbuf = 0;
      }
      fd->wlen = sz;
    } else {
      fd->wbuf = 0;
      fd->wlen = 0;
    }
    fd->write = 1;
    fd->clus = 0;
    fd->size = 0;
    fd->pos = fd->wlen;
    fd->parent = parent;
    for (int k = 0; k < 11; k++) fd->leaf[k] = leaf[k];
    fd->used = 1;
    return id;
  }
}

static int64_t sys_read(isr_frame_t *f) {
  /* RDI=fd RSI=buf RDX=len. */
  static uint8_t kb[2048];
  int id = (int)f->rdi;
  uint64_t len = f->rdx;
  ffd_t *fd;
  int n;
  if (id == 1 || id == 2) return -1; /* console: write-only */
  if (id < 3 || id >= MAX_FDS) return -1;
  fd = &g_tasks[g_current].fds[id];
  if (!fd->used || fd->write) return -1;
  if (len == 0) return 0;
  if (len > sizeof(kb)) len = sizeof(kb);
  if (!user_range_ok(&g_tasks[g_current], f->rsi, len)) return -1;
  n = fat_read_at(fd->clus, fd->size, fd->pos, kb, (uint32_t)len);
  if (n < 0) return -5;
  if (n > 0) {
    naos_memcpy((void *)(uintptr_t)f->rsi, kb, (size_t)n);
    fd->pos += (uint32_t)n;
  }
  return n;
}

static int64_t sys_write(isr_frame_t *f) {
  /* RDI=fd RSI=buf RDX=len. */
  static uint8_t kb[2048];
  int id = (int)f->rdi;
  uint64_t len = f->rdx;
  if (id == 1 || id == 2) { /* console */
    if (len > 2048) len = 2048;
    if (len == 0) return 0;
    if (!user_range_ok(&g_tasks[g_current], f->rsi, len)) return -1;
    console_write((const char *)(uintptr_t)f->rsi, len);
    return (int64_t)len;
  }
  if (id < 3 || id >= MAX_FDS) return -1;
  {
    ffd_t *fd = &g_tasks[g_current].fds[id];
    if (!fd->used || !fd->write) return -1;
    if (len == 0) return 0;
    if (len > sizeof(kb)) len = sizeof(kb);
    if (!user_range_ok(&g_tasks[g_current], f->rsi, len)) return -1;
    if (fd->pos > fd->wlen) fd->pos = fd->wlen;
    if ((uint64_t)fd->pos + len > FD_WRITE_CAP) return -1;
    if (!fd->wbuf) {
      uint64_t b = pmm_alloc_page();
      if (!b) return -4;
      fd->wbuf = (char *)(uintptr_t)b;
      fd->wlen = 0;
      fd->pos = 0;
    }
    naos_memcpy(kb, (const void *)(uintptr_t)f->rsi, (size_t)len);
    naos_memcpy(fd->wbuf + fd->pos, kb, (size_t)len);
    fd->pos += (uint32_t)len;
    if (fd->pos > fd->wlen) fd->wlen = fd->pos;
    return (int64_t)len;
  }
}

/* Reposition an open file. RDI=fd RSI=offset RDX=whence (0=set, 1=cur,
 * 2=end). Read mode ranges over [0, size] (past-end reads yield 0);
 * write mode ranges over the staged [0, wlen] so programs can overwrite,
 * not just append (no sparse files: past-end seeks clamp). Returns the
 * new position, negative on error. */
static int64_t sys_seek(isr_frame_t *f) {
  int id = (int)f->rdi;
  int64_t off = (int64_t)f->rsi;
  int wh = (int)f->rdx;
  ffd_t *fd;
  uint32_t size, base;
  int64_t np;
  if (id < 3 || id >= MAX_FDS) return -1;
  fd = &g_tasks[g_current].fds[id];
  if (!fd->used) return -1;
  if (wh < 0 || wh > 2) return -1;
  size = fd->write ? fd->wlen : fd->size;
  base = (wh == 0) ? 0 : (wh == 1 ? fd->pos : size);
  np = (int64_t)base + off;
  if (np < 0) return -1;
  if (fd->write) {
    if (np > (int64_t)size) np = size; /* clamp: no sparse staging */
  } else if (np > 0x7FFFFFFFu) {
    return -1;
  }
  fd->pos = (uint32_t)np;
  return np;
}

static int sys_close(isr_frame_t *f) {
  int id = (int)f->rdi;
  ffd_t *fd;
  int r = 0;
  if (id >= 0 && id <= 2) return 0; /* console: no-op */
  if (id < 3 || id >= MAX_FDS) return -1;
  fd = &g_tasks[g_current].fds[id];
  if (!fd->used) return -1;
  if (fd->write)
    r = fat_write_parts(fd->parent, fd->leaf, fd->up,
                         fd->wbuf ? fd->wbuf : "", fd->wlen);
  if (fd->wbuf) {
    pmm_free_page((uint64_t)(uintptr_t)fd->wbuf);
    fd->wbuf = 0;
  }
  fd->used = 0;
  return r;
}

/* First frame: zeros for regs, vec/err dummies, then the iretq tail
 * (rip=user entry, cs=0x23, rflags=0x202, rsp=user stack VA, ss=0x1B). */
static void craft_frame(uint64_t ksp_top, void (*entry)(void),
                        uint64_t ustack_top_va) {
  uint64_t *w = (uint64_t *)(uintptr_t)(ksp_top - 176);
  for (int i = 0; i < 22; i++) w[i] = 0;
  w[15] = 32;    /* vec (dummy IRQ) */
  w[16] = 0;     /* err */
  w[17] = (uint64_t)(uintptr_t)entry;
  w[18] = 0x23;  /* UCODE | RPL3 */
  w[19] = 0x202; /* IF=1, bit1 */
  w[20] = ustack_top_va;
  w[21] = 0x1B;  /* UDATA | RPL3 */
}

/* Shared tail: craft first frame + publish the task. Lock held. */
static int spawn_commit(int id, const char *name, void (*entry)(void),
                        uint64_t ks, uint64_t us, uint64_t cr3,
                        uint64_t tbl[4]);

int task_spawn(const char *name, void (*entry)(void)) {
  sched_lock();
  int id = -1;
  for (int i = 1; i < MAX_TASKS; i++) {
    if (!g_tasks[i].used) { id = i; break; }
  }
  if (id < 0) { sched_unlock(); return -1; }
  uint64_t ks = 0, us = 0, cr3 = 0, tbl[4] = {0, 0, 0, 0};
  if (g_bundle_count > 0) {
    g_bundle_count--;
    ks = g_bundles[g_bundle_count].kbase;
    us = g_bundles[g_bundle_count].ubase;
    for (int t = 0; t < 4; t++) tbl[t] = g_bundles[g_bundle_count].tbl[t];
    cr3 = g_bundles[g_bundle_count].cr3;
  } else {
    ks = pmm_alloc_contig(KSTACK_PAGES);
    if (ks == 0) { sched_unlock(); return -2; }
    us = pmm_alloc_contig(USTACK_PAGES);
    if (us == 0) {
      for (uint32_t p = 0; p < KSTACK_PAGES; p++)
        pmm_free_page(ks + (uint64_t)p * 0x1000ULL);
      sched_unlock();
      return -2;
    }
    if (!vmm_create_space(us, USTACK_PAGES, &cr3, tbl)) {
      for (uint32_t p = 0; p < KSTACK_PAGES; p++)
        pmm_free_page(ks + (uint64_t)p * 0x1000ULL);
      for (uint32_t p = 0; p < USTACK_PAGES; p++)
        pmm_free_page(us + (uint64_t)p * 0x1000ULL);
      sched_unlock();
      return -2;
    }
  }
  return spawn_commit(id, name, entry, ks, us, cr3, tbl);
}

/* Shared tail: craft first frame + publish the task. Lock held. */
static int spawn_commit(int id, const char *name, void (*entry)(void),
                        uint64_t ks, uint64_t us, uint64_t cr3,
                        uint64_t tbl[4]) {
  uint64_t kstop = ks + (uint64_t)KSTACK_PAGES * 0x1000ULL;
  uint64_t ustop_va = USTACK_VA + (uint64_t)USTACK_PAGES * 0x1000ULL;
  naos_memset((void *)(uintptr_t)ks, 0, KSTACK_PAGES * 0x1000U);
  craft_frame(kstop, entry, ustop_va);
  fds_init(id);
  g_tasks[id].used = 1;
  g_tasks[id].state = T_READY;
  g_tasks[id].ksp = kstop - 176;
  g_tasks[id].kstack_top = kstop;
  g_tasks[id].kstack_base = ks;
  g_tasks[id].kstack_pages = KSTACK_PAGES;
  g_tasks[id].ustack_base = us;
  g_tasks[id].ustack_pages = USTACK_PAGES;
  g_tasks[id].cr3 = cr3;
  for (int t = 0; t < 4; t++) g_tasks[id].tbl[t] = tbl[t];
  g_tasks[id].wake = 0;
  size_t n = 0;
  while (name[n] && n + 1 < sizeof(g_tasks[id].name)) {
    g_tasks[id].name[n] = name[n];
    n++;
  }
  g_tasks[id].name[n] = '\0';
  sched_unlock();
  return id;
}

/* ---- programs from disk ---- */

#define PROG_MAX_BYTES (128u * 1024u)
#define PROG_BASE 0x40100000ULL
#define PROG_END  0x40200000ULL

typedef struct {
  uint8_t ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} UElf64_Ehdr;

typedef struct {
  uint32_t ptype;
  uint32_t pflags;
  uint64_t off;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} UElf64_Phdr;

#define UPT_LOAD 1u

int task_spawn_elf(const char *name, const char *path) {
  static uint8_t s_img[PROG_MAX_BYTES]; /* scratch: avoids huge kmalloc */
  sched_lock();
  int id = -1;
  for (int i = 1; i < MAX_TASKS; i++) {
    if (!g_tasks[i].used) { id = i; break; }
  }
  if (id < 0) { sched_unlock(); return -1; }
  /* Read + validate before allocating anything. */
  uint32_t size = 0;
  int r = fat_read(path, s_img, PROG_MAX_BYTES, &size);
  if (r < 0 || size < sizeof(UElf64_Ehdr) || size > PROG_MAX_BYTES) {
    sched_unlock();
    return -3;
  }
  const UElf64_Ehdr *eh = (const UElf64_Ehdr *)s_img;
  uint64_t entry = eh->entry; /* save: img is scratch, entry used late */
  int ok = (eh->ident[0] == 0x7F && eh->ident[1] == 'E' &&
            eh->ident[2] == 'L' && eh->ident[3] == 'F' &&
            eh->ident[4] == 2 && eh->ident[5] == 1 &&
            eh->machine == 62 && eh->type == 2 &&
            eh->phentsize == sizeof(UElf64_Phdr) &&
            eh->phoff + (uint64_t)eh->phnum * sizeof(UElf64_Phdr) <= size &&
            entry >= PROG_BASE && entry < PROG_END);
  const UElf64_Phdr *ph = 0;
  uint64_t total_pages = 0;
  if (ok) {
    ph = (const UElf64_Phdr *)(s_img + eh->phoff);
    for (uint16_t i = 0; i < eh->phnum && ok; i++) {
      if (ph[i].ptype != UPT_LOAD || ph[i].memsz == 0) continue;
      if (ph[i].vaddr < PROG_BASE || ph[i].vaddr + ph[i].memsz > PROG_END ||
          ph[i].vaddr + ph[i].memsz < ph[i].vaddr ||
          ph[i].filesz > ph[i].memsz ||
          ph[i].off + ph[i].filesz > size ||
          ph[i].off + ph[i].filesz < ph[i].off)
        ok = 0;
      else
        total_pages += (ph[i].memsz + 0xFFFULL) / 0x1000ULL;
    }
    if (total_pages == 0 || total_pages > 256) ok = 0;
  }
  if (!ok) { sched_unlock(); return -3; }
  /* Allocate task resources. */
  uint64_t ks = 0, us = 0, cr3 = 0, tbl[4] = {0, 0, 0, 0};
  int from_bundle = 0;
  if (g_bundle_count > 0) {
    g_bundle_count--;
    ks = g_bundles[g_bundle_count].kbase;
    us = g_bundles[g_bundle_count].ubase;
    for (int t = 0; t < 4; t++) tbl[t] = g_bundles[g_bundle_count].tbl[t];
    cr3 = g_bundles[g_bundle_count].cr3;
    from_bundle = 1;
  } else {
    ks = pmm_alloc_contig(KSTACK_PAGES);
    us = (ks != 0) ? pmm_alloc_contig(USTACK_PAGES) : 0;
    if (ks == 0 || us == 0 || !vmm_create_space(us, USTACK_PAGES, &cr3, tbl))
      ok = 0;
  }
  /* Map program pages into the task PT (tbl[3]). */
  if (ok) {
    uint64_t pt = tbl[3];
    for (uint16_t i = 0; i < eh->phnum && ok; i++) {
      if (ph[i].ptype != UPT_LOAD || ph[i].memsz == 0) continue;
      uint64_t va = ph[i].vaddr & ~0xFFFULL;
      uint64_t end = (ph[i].vaddr + ph[i].memsz + 0xFFFULL) & ~0xFFFULL;
      for (uint64_t a = va; a < end && ok; a += 0x1000ULL) {
        uint64_t pg = pmm_alloc_page();
        if (pg == 0) { ok = 0; break; }
        naos_memset((void *)(uintptr_t)pg, 0, 0x1000);
        /* File-covered window of this page, all in VA terms. */
        uint64_t file_va_end = ph[i].vaddr + ph[i].filesz;
        uint64_t page_va_end = a + 0x1000ULL;
        uint64_t cs = (a > ph[i].vaddr) ? a : ph[i].vaddr;
        uint64_t ce = (page_va_end < file_va_end) ? page_va_end : file_va_end;
        if (ce > cs) {
          uint64_t fo = ph[i].off + (cs - ph[i].vaddr);
          naos_memcpy((void *)(uintptr_t)(pg + (cs - a)),
                      s_img + fo, (size_t)(ce - cs));
        }
        if (vmm_map_user(pt, a, pg, (ph[i].pflags & 2) ? 1 : 0) != 0) {
          pmm_free_page(pg);
          ok = 0;
        }
      }
    }
  }
  if (!ok) {
    if (tbl[3] != 0) vmm_free_prog(tbl[3]); /* drop partial mappings */
    if (from_bundle) { /* hand the bundle back */
      g_bundles[g_bundle_count].kbase = ks;
      g_bundles[g_bundle_count].ubase = us;
      for (int t = 0; t < 4; t++)
        g_bundles[g_bundle_count].tbl[t] = tbl[t];
      g_bundles[g_bundle_count].cr3 = cr3;
      g_bundle_count++;
    } else {
      if (ks != 0) {
        for (uint32_t p = 0; p < KSTACK_PAGES; p++)
          pmm_free_page(ks + (uint64_t)p * 0x1000ULL);
      }
      if (us != 0) {
        for (uint32_t p = 0; p < USTACK_PAGES; p++)
          pmm_free_page(us + (uint64_t)p * 0x1000ULL);
      }
      for (int t = 0; t < 4; t++)
        if (tbl[t] != 0) pmm_free_page(tbl[t]);
    }
    sched_unlock();
    return -4;
  }
  return spawn_commit(id, name, (void (*)(void))(uintptr_t)entry,
                      ks, us, cr3, tbl);
}

/* ---- fork with copy-on-write ----
 * Child shares every present user page read-only (COW bit); the first
 * write to a shared page copies it (task_cow_fixup, WASW preserves the
 * original writability). Returns child id, -1 (no fork here / no slot),
 * or -2 (out of memory). Lock held throughout. */

/* User PT indices that fork shares: stacks 0..3, programs 16..511. */
static int fork_idx(int i) {
  return (i >= 0 && i < 4) || (i >= 16 && i < 512);
}

static int64_t task_fork(isr_frame_t *f) {
  int parent = g_current;
  int id = -1;
  uint64_t ks = 0, cr3 = 0, tbl[4] = {0, 0, 0, 0};
  uint64_t *ppt, *cpt;
  int i;
  if (parent <= 0 || g_tasks[parent].tbl[3] == 0) return -1;
  sched_lock();
  for (i = 1; i < MAX_TASKS; i++) {
    if (!g_tasks[i].used) { id = i; break; }
  }
  if (id < 0) { sched_unlock(); return -1; }
  ks = pmm_alloc_contig(KSTACK_PAGES);
  if (ks == 0) { sched_unlock(); return -2; }
  if (!vmm_fork_tables(&cr3, tbl)) {
    for (uint32_t p = 0; p < KSTACK_PAGES; p++)
      pmm_free_page(ks + (uint64_t)p * 0x1000ULL);
    sched_unlock();
    return -2;
  }
  ppt = (uint64_t *)(uintptr_t)g_tasks[parent].tbl[3];
  cpt = (uint64_t *)(uintptr_t)tbl[3];
  /* Share pass: refcount first (abort cleanly before touching PTEs). */
  for (i = 0; i < 512; i++) {
    uint64_t e;
    if (!fork_idx(i)) continue;
    e = ppt[i];
    if (!(e & 1ULL)) continue;
    if (cow_share(e & ~0xFFFULL) != 0) {
      /* Unwind: release what this fork already shared. */
      for (int j = 0; j < i; j++) {
        uint64_t ej;
        if (!fork_idx(j)) continue;
        ej = ppt[j];
        if (!(ej & 1ULL)) continue;
        cow_release(ej & ~0xFFFULL);
      }
      for (int t = 0; t < 4; t++) pmm_free_page(tbl[t]);
      for (uint32_t p = 0; p < KSTACK_PAGES; p++)
        pmm_free_page(ks + (uint64_t)p * 0x1000ULL);
      sched_unlock();
      return -2;
    }
  }
  /* Convert pass: both sides read-only + COW (+WASW), child mirrors. */
  for (i = 0; i < 512; i++) {
    uint64_t e, ne;
    if (!fork_idx(i)) continue;
    e = ppt[i];
    if (!(e & 1ULL)) continue;
    ne = (e & ~0x002ULL) | PTE_COW;
    if (e & 0x002ULL) ne |= PTE_WASW;
    ppt[i] = ne;
    cpt[i] = ne;
  }
  /* Flush the parent's TLB (we run on its tables right now). */
  __asm__ volatile ("mov %0, %%cr3" :: "r"(g_tasks[parent].cr3) : "memory");
  /* Child resumes from a copy of our syscall frame; it sees fork()=0. */
  {
    uint64_t kstop = ks + (uint64_t)KSTACK_PAGES * 0x1000ULL;
    isr_frame_t *cf;
    naos_memset((void *)(uintptr_t)ks, 0, KSTACK_PAGES * 0x1000U);
    naos_memcpy((void *)(uintptr_t)(kstop - sizeof(isr_frame_t)),
                f, sizeof(isr_frame_t));
    cf = (isr_frame_t *)(uintptr_t)(kstop - sizeof(isr_frame_t));
    cf->rax = 0;
    g_tasks[id].used = 1;
    g_tasks[id].state = T_READY;
    g_tasks[id].ksp = kstop - sizeof(isr_frame_t);
    g_tasks[id].kstack_top = kstop;
    g_tasks[id].kstack_base = ks;
    g_tasks[id].kstack_pages = KSTACK_PAGES;
    g_tasks[id].ustack_base = g_tasks[parent].ustack_base;
    g_tasks[id].ustack_pages = g_tasks[parent].ustack_pages;
    g_tasks[id].cr3 = cr3;
    for (int t = 0; t < 4; t++) g_tasks[id].tbl[t] = tbl[t];
    g_tasks[id].wake = 0;
    fds_init(id); /* forked child starts with no open files */
    {
      size_t n = 0;
      while (g_tasks[parent].name[n] && n + 1 < sizeof(g_tasks[id].name)) {
        g_tasks[id].name[n] = g_tasks[parent].name[n];
        n++;
      }
      g_tasks[id].name[n] = '\0';
    }
  }
  sched_unlock();
  return id;
}

/* Resolve a write fault on a CoW page: private copy, remap, resume.
 * Runs with interrupts off (fault gate). Returns 1 when handled. */
int task_cow_fixup(uint64_t addr) {
  int cur = g_current;
  uint64_t *pt, e, oldphys, newpg, idx;
  if (cur <= 0 || g_tasks[cur].tbl[3] == 0) return 0;
  if (addr < 0x40000000ULL || addr >= 0x40200000ULL) return 0;
  idx = (addr >> 12) & 0x1FFULL;
  if (!fork_idx((int)idx)) return 0;
  pt = (uint64_t *)(uintptr_t)g_tasks[cur].tbl[3];
  e = pt[idx];
  if (!(e & 1ULL) || !(e & PTE_COW) || !(e & PTE_WASW)) return 0;
  oldphys = e & ~0xFFFULL;
  newpg = pmm_alloc_page();
  if (newpg == 0) return 0;
  naos_memcpy((void *)(uintptr_t)newpg, (void *)(uintptr_t)oldphys, 0x1000);
  pt[idx] = newpg | 1ULL | 4ULL | 2ULL; /* present + user + RW */
  cow_release(oldphys);
  __asm__ volatile ("invlpg (%0)" :: "r"(addr) : "memory");
  return 1;
}

int task_kill(int id) {
  if (id <= 0 || id >= MAX_TASKS || !g_tasks[id].used) return -1;
  if (g_tasks[id].state == T_DEAD) return -2;
  g_tasks[id].state = T_DEAD; /* reaped on next tick (not current: shell) */
  return 0;
}

/* Called from the fault path with the faulting user frame current. */
void task_fault_current(void) {
  if (g_current <= 0) return; /* kernel faults halt elsewhere */
  g_tasks[g_current].state = T_DEAD;
  int n = pick_next();
  if (n < 0) { /* unreachable: kernel task is always resumable */
    kprintf("sched: no task to run!\n");
    for (;;) { __asm__ volatile ("hlt"); }
  }
  switch_to(n);
}

static const char *state_name(tstate_t s) {
  switch (s) {
    case T_READY: return "run";
    case T_SLEEP: return "sleep";
    case T_DEAD: return "dead";
    default: return "?";
  }
}

void task_list(void) {
  kprintf("id  name      state\n");
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].used) continue;
    kprintf("%u%c  %s %s\n", (unsigned)i,
            (i == g_current) ? '*' : ' ', g_tasks[i].name,
            state_name(g_tasks[i].state));
  }
}
