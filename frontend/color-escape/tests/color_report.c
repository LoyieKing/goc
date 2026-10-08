/* Color report fixture. Not a pass/fail golden: check_color_report.sh reads it. */
#include "goc.h"

struct Node {
  int *bare;
  cptr(int) cheap;
  uptr(int) enc;
  gptr(int) gp;
  auto_ptr(int) ap;
};

union Hide {
  int *hidden;
  int x;
};

struct Node g_node;
int *g_bare;
cptr(int) g_explicit;
int g_obj;

static void only_stack(int *out) { *out = 1; }

int *ret_bare(int *p) {
  (void)p;
  return &g_obj;
}

cptr(int) ret_cptr(int *p) {
  (void)p;
  return &g_obj;
}

void store_all(void) {
  int local = 1;
  int *bare_local = &local;
  sptr(int) sp = &local;
  auto_ptr(int) ap = &local;
  struct Node fr;

  fr.bare = &local;
  fr.ap = ap;
  g_node.cheap = sp;
  g_node.enc = goc_uptr_from_sptr((goc_sptr)sp);
  g_node.gp = goc_gptr_from_handle(1);
  g_bare = &local;
  g_explicit = goc_uptr_from_sptr((goc_sptr)sp);
  only_stack(&local);
  (void)bare_local;
  (void)fr;
}

int use_union(void) {
  union Hide h;
  h.hidden = 0;
  return h.x;
}
