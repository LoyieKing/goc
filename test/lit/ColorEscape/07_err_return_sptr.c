/* FAIL: returning sptr / &local escapes. */
#include "goc.h"

sptr(int) bad_ret(void) {
  int local = 3;
  sptr(int) p = &local;
  return p;
}

// RUN: not %goc-clang-ll -o %t.ll %s 2>&1 | FileCheck %s
// CHECK: error: goc: sptr escape
