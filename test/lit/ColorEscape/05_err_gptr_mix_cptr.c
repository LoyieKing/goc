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

// RUN: %goc-clang-ll -o %t.ll %s
// RUN: not %goc-color-escape %t.ll -o %t.color.ll 2>&1 | FileCheck %s
// CHECK: error: gptr cannot be stored into a cptr field/slot
