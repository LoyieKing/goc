/* FAIL: returning sptr / &local escapes. */
#include "goc.h"

sptr(int) bad_ret(void) {
  int local = 3;
  sptr(int) p = &local;
  return p;
}
