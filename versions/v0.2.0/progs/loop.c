/* NaOS disk program: cooperative counting with yield (no sleep). */
#include "user/ulib.h"

int main(void) {
  for (unsigned int i = 0; i < 5; i++) {
    u_puts("loop ");
    u_print_num(i);
    u_puts("\n");
    u_yield();
  }
  u_puts("loop done\n");
  return 0;
}
