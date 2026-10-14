/* NaOS user library: syscall wrappers (int 0x80) + tiny helpers.
 * Linked into the kernel image but executed at ring 3. Uses no
 * privileged instructions and no kernel-only state. */
#ifndef NAOS_ULIB_H
#define NAOS_ULIB_H

#include <stdint.h>
#include <stddef.h>

#define ULIB_SYS_EXIT   0
#define ULIB_SYS_PRINT  1
#define ULIB_SYS_YIELD  2
#define ULIB_SYS_GETPID 3
#define ULIB_SYS_SLEEP  4
#define ULIB_SYS_FORK   5
#define ULIB_SYS_OPEN   6 /* path, mode(0=r,1=w,2=a) -> fd or negative */
#define ULIB_SYS_READ   7
#define ULIB_SYS_WRITE  8
#define ULIB_SYS_CLOSE  9

static inline long u_sys0(int n) {
  long r;
  __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n) : "memory");
  return r;
}

static inline long u_sys1(int n, unsigned long a1) {
  long r;
  __asm__ volatile ("int $0x80"
                    : "=a"(r) : "a"(n), "D"(a1) : "memory");
  return r;
}

static inline long u_sys2(int n, unsigned long a1, unsigned long a2) {
  long r;
  __asm__ volatile ("int $0x80"
                    : "=a"(r) : "a"(n), "D"(a1), "S"(a2) : "memory");
  return r;
}

static inline long u_sys3(int n, unsigned long a1, unsigned long a2,
                          unsigned long a3) {
  long r;
  __asm__ volatile ("int $0x80"
                    : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                    : "memory");
  return r;
}

static inline void u_exit(void) {
  u_sys0(ULIB_SYS_EXIT);
  for (;;) { } /* unreachable: task is gone */
}

static inline long u_print(const char *s, unsigned long len) {
  return u_sys2(ULIB_SYS_PRINT, (unsigned long)s, len);
}

static inline long u_yield(void) {
  return u_sys0(ULIB_SYS_YIELD);
}

static inline long u_getpid(void) {
  return u_sys0(ULIB_SYS_GETPID);
}

static inline long u_sleep(unsigned long ticks) {
  return u_sys1(ULIB_SYS_SLEEP, ticks);
}

static inline long u_fork(void) {
  return u_sys0(ULIB_SYS_FORK);
}

static inline long u_open(const char *path, unsigned long mode) {
  return u_sys2(ULIB_SYS_OPEN, (unsigned long)path, mode);
}

static inline long u_read(long fd, void *buf, unsigned long len) {
  return u_sys3(ULIB_SYS_READ, (unsigned long)fd, (unsigned long)buf, len);
}

static inline long u_write(long fd, const void *buf, unsigned long len) {
  return u_sys3(ULIB_SYS_WRITE, (unsigned long)fd, (unsigned long)buf,
                len);
}

static inline long u_close(long fd) {
  return u_sys1(ULIB_SYS_CLOSE, (unsigned long)fd);
}

static inline size_t u_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

static inline void u_puts(const char *s) {
  u_print(s, (unsigned long)u_strlen(s));
}

/* 32-bit arithmetic only: 64-bit division would call libgcc helpers
 * living in supervisor pages, faulting at ring 3. */
static inline void u_print_num(unsigned long v) {
  unsigned int x = (unsigned int)v;
  char tmp[11];
  int n = 0;
  if (x == 0) { u_print("0", 1); return; }
  while (x > 0 && n < 10) { tmp[n++] = (char)('0' + (x % 10)); x /= 10; }
  while (n > 0) { n--; u_print(&tmp[n], 1); }
}

#endif /* NAOS_ULIB_H */
