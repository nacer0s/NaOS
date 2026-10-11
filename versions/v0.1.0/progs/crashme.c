/* NaOS disk program: deliberate divide-by-zero to demo the kill path. */
#include "user/ulib.h"

int main(void) {
  volatile unsigned int zero = 0;
  volatile unsigned int x = 42 / zero;
  u_puts("crashme: survived?! ");
  u_print_num(x);
  u_puts("\n");
  return 0;
}
