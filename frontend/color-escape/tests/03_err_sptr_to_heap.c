/* FAIL: sptr / stack absolute stored into global field — must be compile error. */
#include "goc.h"

struct Node {
  auto_ptr(int) field;
};

struct Node g_node;

void bad_escape(void) {
  int local = 1;
  sptr(int) p = &local;
  g_node.field = p; /* illegal: sptr into heap/global */
}
