/* PASS: non-trivial colored .c → Clang plugin Sema → color → goobj (stackmap). */
#include "goc.h"

void external_safepoint(void);

void p21_sptr_across_call(sptr(int) p) {
  sptr(int) slot = p;
  external_safepoint();
  slot = slot;
}
