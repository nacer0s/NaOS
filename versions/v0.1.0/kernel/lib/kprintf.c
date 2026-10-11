/* NaOS kprintf: %c %s %d %i %u %x %X %p %% -> fb + serial. */
#include "kprintf.h"
#include "../drv/fb.h"
#include "../task.h"
#include "../../common/serial.h"
#include <stdarg.h>

static char *g_cap;
static size_t g_cap_size;
static size_t g_cap_used;
static int g_cap_full;

void kprintf_begin_capture(char *buf, size_t size) {
  g_cap = buf;
  g_cap_size = size;
  g_cap_used = 0;
  g_cap_full = 0;
}

size_t kprintf_end_capture(void) {
  g_cap = 0;
  g_cap_size = 0;
  return g_cap_used;
}

int kprintf_capture_full(void) { return g_cap_full; }

static void out_c(char c) {
  if (g_cap) {
    if (g_cap_used < g_cap_size) g_cap[g_cap_used++] = c;
    else g_cap_full = 1;
    return;
  }
  if (fb_ready()) fb_putc(c);
  serial_putc(c);
}

static void out_s(const char *s) {
  if (!s) s = "(null)";
  while (*s) out_c(*s++);
}

static void out_u64(uint64_t v, int base, int upper) {
  char tmp[65];
  int n = 0;
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (v == 0) { out_c('0'); return; }
  while (v > 0 && n < 64) { tmp[n++] = dig[v % (uint64_t)base]; v /= (uint64_t)base; }
  while (n > 0) out_c(tmp[--n]);
}

static void out_i64(int64_t v) {
  if (v < 0) { out_c('-'); v = -v; }
  out_u64((uint64_t)v, 10, 0);
}

void kprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sched_lock(); /* no preemption mid-line: fb cursor is shared state */
  while (*fmt) {
    if (*fmt != '%') { out_c(*fmt++); continue; }
    fmt++;
    switch (*fmt) {
      case '%': out_c('%'); break;
      case 'c': out_c((char)va_arg(ap, int)); break;
      case 's': out_s(va_arg(ap, const char *)); break;
      case 'd': case 'i': out_i64((int64_t)va_arg(ap, int)); break;
      case 'u': out_u64((uint64_t)va_arg(ap, unsigned int), 10, 0); break;
      case 'x': out_u64((uint64_t)va_arg(ap, unsigned int), 16, 0); break;
      case 'X': out_u64((uint64_t)va_arg(ap, unsigned int), 16, 1); break;
      case 'p': out_s("0x"); out_u64((uint64_t)va_arg(ap, void *), 16, 0); break;
      default: out_c('%'); out_c(*fmt); break;
    }
    fmt++;
  }
  sched_unlock();
  va_end(ap);
}
