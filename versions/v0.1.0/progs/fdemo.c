/* NaOS disk program: file-syscall demo. Writes /HELLO.TXT, reads it
 * back in two chunks, prints everything to stdout (fd 1). */
#include "user/ulib.h"

int main(void) {
  static const char msg[] = "hello from a program file!\nsecond line here.\n";
  static char buf[64];
  long fd;
  long n;

  fd = u_open("/HELLO.TXT", 1);
  if (fd < 0) {
    u_puts("fdemo: open w failed\n");
    return 0;
  }
  n = u_write(fd, msg, sizeof(msg) - 1);
  if (n != (long)(sizeof(msg) - 1)) {
    u_puts("fdemo: short write\n");
    u_close(fd);
    return 0;
  }
  if (u_close(fd) != 0) {
    u_puts("fdemo: close/save failed\n");
    return 0;
  }
  u_puts("fdemo: saved /HELLO.TXT\n");

  fd = u_open("/HELLO.TXT", 0);
  if (fd < 0) {
    u_puts("fdemo: open r failed\n");
    return 0;
  }
  for (;;) {
    n = u_read(fd, buf, sizeof(buf));
    if (n < 0) {
      u_puts("fdemo: read failed\n");
      break;
    }
    if (n == 0) break;
    u_write(1, buf, (unsigned long)n);
  }
  u_close(fd);
  /* stdout also works directly (fd 1). */
  u_write(1, "fdemo: done\n", 12);
  return 0;
}
