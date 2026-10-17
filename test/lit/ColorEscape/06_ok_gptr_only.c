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

// RUN: %goc-clang-ll -o %t.ll %s
// RUN: %goc-color-escape %t.ll -o %t.color.ll 2>&1 | FileCheck %s
// CHECK: summary: 0 error(s)
