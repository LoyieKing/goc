/* PASS: explicit goc_uptr_from_sptr before storing into heap/global field. */
#include "goc.h"

struct Holder {
  uptr(int) enc;
};

struct Holder g_holder;

void link_frame(void) {
  int local = 7;
  sptr(int) p = &local;
  g_holder.enc = goc_uptr_from_sptr((goc_sptr)p);
}
