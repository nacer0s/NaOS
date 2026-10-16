/* NaOS disk program: seek-syscall proof. Writes a file, re-reads it
 * from the middle via SEEK_SET/SEEK_CUR/SEEK_END, then overwrites one
 * byte in place and verifies the mix. Prints PASS/FAIL lines. */
#include "user/ulib.h"

static int fails;

static void check(long cond, const char *msg) {
  if (cond) {
    u_puts("seektest: PASS ");
    u_puts(msg);
    u_puts("\n");
  } else {
    u_puts("seektest: FAIL ");
    u_puts(msg);
    u_puts("\n");
    fails++;
  }
}

int main(void) {
  static const char msg[] = "ABCDEFGHIJ";
  static char buf[12];
  long fd, n, p;

  fd = u_open("/SEEK.TXT", 1);
  check(fd >= 0, "open w\n");
  if (fd < 0) return 0;
  n = u_write(fd, msg, 10);
  check(n == 10, "write 10\n");
  p = u_seek(fd, 0, ULIB_SEEK_SET);
  check(p == 0, "w-seek set 0\n");
  n = u_write(fd, "zz", 2);
  check(n == 2, "overwrite 2\n");
  check(u_close(fd) == 0, "close/save\n");

  fd = u_open("/SEEK.TXT", 0);
  check(fd >= 0, "open r\n");
  if (fd < 0) return 0;
  p = u_seek(fd, 4, ULIB_SEEK_SET);
  check(p == 4, "r-seek set 4\n");
  n = u_read(fd, buf, 3);
  buf[3] = '\0';
  check(n == 3 && buf[0] == 'E' && buf[1] == 'F' && buf[2] == 'G',
        "read EFG\n");
  p = u_seek(fd, -2, ULIB_SEEK_CUR);
  check(p == 5, "r-seek cur -2\n");
  p = u_seek(fd, 0, ULIB_SEEK_END);
  check(p == 10, "r-seek end\n");
  n = u_read(fd, buf, 4);
  check(n == 0, "read past end is 0\n");
  p = u_seek(fd, -99, ULIB_SEEK_SET);
  check(p < 0, "negative seek refused\n");
  u_close(fd);

  /* Whole file must read zzCDEFGHIJ (in-place overwrite stuck). */
  fd = u_open("/SEEK.TXT", 0);
  n = u_read(fd, buf, 10);
  buf[10] = '\0';
  check(n == 10 && buf[0] == 'z' && buf[1] == 'z' && buf[2] == 'C' &&
        buf[9] == 'J', "content zzCDEFGHIJ\n");
  u_close(fd);
  if (fails == 0) u_puts("seektest: ALL PASS\n");
  return 0;
}
