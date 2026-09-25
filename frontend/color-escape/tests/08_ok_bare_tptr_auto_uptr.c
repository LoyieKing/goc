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
