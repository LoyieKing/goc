/* PASS: a cptr T* destination transparently stores an uptr encoding. */
#include "goc.h"

struct Node {
  cptr(int) field;
};

struct Node g_node;

void store_stack_pointer(void) {
  int local = 1;
  sptr(int) p = &local;
  g_node.field = p; /* source spelling stays T*; storage uses uptr */
}

int read_stack_pointer(void) { return *g_node.field; }
