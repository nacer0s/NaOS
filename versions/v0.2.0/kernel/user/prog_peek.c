/* NaOS demo program: tries to read kernel memory (0x100000).
 * Must die with a protection fault — proves per-task isolation. */
#include "ulib.h"

void prog_peek(void) {
  u_puts("peek: reading 0x100000...\n");
  volatile unsigned char v = *(volatile unsigned char *)0x100000UL;
  u_puts("peek: SURVIVED?! value=");
  u_print_num(v);
  u_puts("\n");
  u_exit();
}
