/* P21 vertical: colored .c → color.ll → bridge → goobj (stackmap) */
#include "goc.h"

void external_safepoint(void);

void p21_ok_sptr_call(sptr(int) p) {
  sptr(int) local = p;
  external_safepoint();
  (void)local;
}
