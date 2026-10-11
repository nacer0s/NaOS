/* NaOS disk program: fork() demo. Parent and child each print, proving
 * copy-on-write (both touch their stacks after the split). */
#include "user/ulib.h"

static unsigned long g_shared = 42;

int main(void) {
  long c = u_fork();
  if (c < 0) {
    u_puts("forkdemo: fork failed\n");
    return 0;
  }
  if (c == 0) {
    g_shared = 7; /* child-only write: must not touch the parent */
    u_puts("forkdemo: child pid=");
    u_print_num((unsigned long)u_getpid());
    u_puts(" sees ");
    u_print_num(g_shared);
    u_puts("\n");
  } else {
    u_sleep(20); /* let the child print first */
    u_puts("forkdemo: parent pid=");
    u_print_num((unsigned long)u_getpid());
    u_puts(" child=");
    u_print_num((unsigned long)c);
    u_puts(" sees ");
    u_print_num(g_shared);
    u_puts("\n");
  }
  return 0;
}
