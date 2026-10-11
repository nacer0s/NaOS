/* NaOS demo program: slow ticker, shows interleaving with counter. */
#include "ulib.h"

void prog_ticker(void) {
  unsigned long i = 0;
  for (;;) {
    u_puts("tick ");
    u_print_num(i++);
    u_puts("\n");
    u_sleep(150);
  }
}
