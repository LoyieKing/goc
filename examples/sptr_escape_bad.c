/* Expected Sema failure: sptr must not be stored to heap/global. */
#include "goc.h"

static int *g_slot;

void bad_escape(sptr(int) p) {
  g_slot = p; /* goc: sptr escape */
}
