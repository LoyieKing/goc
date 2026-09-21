/* PASS: gptr stored into gptr field only. */
#include "goc.h"

struct GoBox {
  gptr(int) p;
};

struct GoBox g_go;

void ok_gptr(void) {
  gptr(int) g = goc_gptr_from_handle((uintptr_t)0x2000);
  g_go.p = g;
}
