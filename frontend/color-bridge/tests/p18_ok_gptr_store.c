/* Vertical: gptr store (P17 colors) → P18 bridge → WB */
#include "goc.h"

struct GoBox {
  gptr(int) p;
};

struct GoBox g_go;

void p18_ok_gptr_store(void) {
  gptr(int) g = goc_gptr_from_handle((uintptr_t)0x3000);
  g_go.p = g;
}
