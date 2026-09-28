/* Expected Sema failure: a stack pointer must not be returned.
 *
 * Storing an sptr into a plain T* / cptr global or heap field is allowed:
 * the IR pass encodes that store as uptr and decodes T* loads (see
 * tests/sema/02_ok_sptr_to_cptr_uptr.c). Returning a stack address has no
 * such fix, because the frame is gone after the return. */
#include "goc.h"

int *bad_return(void) {
  int local = 1;
  return &local; /* goc: sptr escape — returning stack pointer */
}
