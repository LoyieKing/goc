/* PASS: callee writes through auto_ptr out-param into caller's stack buffer. */
#include "goc.h"

void fill(int *out) {
  *out = 42;
}

int test_main(void) {
  int local = 0;
  fill(&local);
  return local;
}
