/* PASS: gptr store — Clang plugin frontend → goobj WB path. */
#include "goc.h"

void p21_gptr_store(gptr(int) *slot, gptr(int) newv) {
  gptr(int) a = newv;
  *slot = a;
}
