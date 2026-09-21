/* Vertical: cptr only — no WB */
#include "goc.h"

cptr(int) g_c;

void p18_ok_cptr_only(cptr(int) p) {
  g_c = p;
}
