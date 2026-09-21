/* PASS: non-trivial .c → in-tree Clang IR → llc ISel → goobj (NOT P21 seed).
 * Unique immediate 0x28C0DE42 must appear in goobj TEXT. */
#include "goc.h"

void p28_external_hook(void);

int p28_real_body(int x) {
  /* Magic constant proves body came from this source, not seedMinimal. */
  int v = x + 0x28C0DE42;
  p28_external_hook(); /* CALL site → Go-frame / PCDATA hook via elfpack */
  return v;
}
