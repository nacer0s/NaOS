/* NaOS disk program: computes fib(25), prints it, exits. */
#include "user/ulib.h"

static unsigned int fib(unsigned int n) {
  unsigned int a = 0, b = 1;
  for (unsigned int i = 0; i < n; i++) {
    unsigned int t = a + b;
    a = b;
    b = t;
  }
  return a;
}

int main(void) {
  u_puts("fib(25)=");
  u_print_num(fib(25));
  u_puts("\n");
  return 0;
}
