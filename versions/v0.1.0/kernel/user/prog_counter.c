/* NaOS demo program: counts up forever, sleeping between lines.
 * Runs at ring 3; only talks to the kernel via syscalls. */
#include "ulib.h"

void prog_counter(void) {
  long pid = u_getpid();
  unsigned long i = 0;
  for (;;) {
    u_puts("counter ");
    u_print_num((unsigned long)pid);
    u_puts(" ");
    u_print_num(i++);
    u_puts("\n");
    u_sleep(30);
  }
}
