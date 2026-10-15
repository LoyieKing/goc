/* PASS: stack-only struct instance with auto_ptr field holds &local (sptr refine). */
#include "goc.h"

struct Frame {
  auto_ptr(int) slot;
};

int test_main(void) {
  int local = 99;
  struct Frame fr;
  fr.slot = &local;
  return *fr.slot;
}
