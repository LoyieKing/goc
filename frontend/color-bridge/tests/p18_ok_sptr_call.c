/* Vertical: sptr live across call → maps */
#include "goc.h"

void external_safepoint(void);

void p18_ok_sptr_call(sptr(int) p) {
  sptr(int) local = p;
  external_safepoint();
  (void)local;
}
