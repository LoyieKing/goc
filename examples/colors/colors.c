/* Minimal goc-colored C example (stack out-param). */
#include "goc.h"

void hello_fill(sptr(int) out) {
  *out = 42;
}

int hello_sum(cptr(int) a, cptr(int) b) {
  return *a + *b;
}
