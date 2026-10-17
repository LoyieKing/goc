/* PASS: bare T* keeps its spelling when an sptr value escapes into storage. */
#include "goc.h"

struct Node {
  int *field;
};

struct Node g_node;

void store_stack_pointer(void) {
  int local = 1;
  g_node.field = &local;
}

int read_stack_pointer(void) { return *g_node.field; }

// RUN: %goc-clang-ll -o %t.ll %s
// RUN: %goc-color-escape %t.ll -o %t.color.ll 2>&1 | FileCheck %s
// CHECK: summary: 0 error(s)
