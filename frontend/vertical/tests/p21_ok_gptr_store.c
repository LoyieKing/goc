/* P21 vertical: colored .c → color.ll → bridge → goobj (WB) */
#include "goc.h"

struct GoBox {
  gptr(int) p;
};

struct GoBox g_go;

void p21_ok_gptr_store(void) {
  gptr(int) g = goc_gptr_from_handle((uintptr_t)0x3100);
  g_go.p = g;
}
