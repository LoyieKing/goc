/* FAIL: gptr must not be stored into a cptr field. */
#include "goc.h"

struct Box {
  cptr(int) p;
};

struct Box g_box;

void mix(void) {
  gptr(int) g = goc_gptr_from_handle((uintptr_t)0x1000);
  g_box.p = (cptr(int))g; /* illegal mix */
}
