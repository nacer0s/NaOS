/* NaOS disk program: says hello and exits (tests exit + reaping). */
#include "user/ulib.h"

int main(void) {
  u_puts("hello from disk! pid=");
  u_print_num((unsigned long)u_getpid());
  u_puts("\n");
  return 0;
}
