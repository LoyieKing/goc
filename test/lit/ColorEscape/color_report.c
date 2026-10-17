/* Color report fixture (RUN and CHECK lines at the end). */
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

// The color report (print<goc-color>) lists resolved pointer colors and
// does not change the IR. The RUN lines are at the end: CHECK lines match
// declaration line numbers above.
// RUN: %goc-clang-ll -g -o %t.ll %s
// RUN: %goc-clang-ll -o %t.nog.ll %s
// RUN: %opt-goc -passes=goc-color-escape -S %t.ll -o %t.off.ll 2>&1 | FileCheck --check-prefix=OFF %s
// OFF-NOT: goc-color-report
// RUN: %opt-goc -passes='print<goc-color>,goc-color-escape' -S %t.ll -o %t.on.ll 2>&1 | FileCheck --check-prefixes=ON,NOUNION %s
// RUN: diff %t.off.ll %t.on.ll
// ON: goc-color-report kind name loc color source spelling
// ON-DAG: goc-color-report field Node.bare {{[^ ]*}}:5{{(:[0-9]+)?}} sptr inferred T*
// ON-DAG: goc-color-report field Node.cheap {{[^ ]*}}:6{{(:[0-9]+)?}} uptr inferred cptr
// ON-DAG: goc-color-report field Node.enc {{[^ ]*}}:7{{(:[0-9]+)?}} uptr explicit uptr
// ON-DAG: goc-color-report field Node.gp {{[^ ]*}}:8{{(:[0-9]+)?}} gptr explicit gptr
// ON-DAG: goc-color-report field Node.ap {{[^ ]*}}:9{{(:[0-9]+)?}} sptr inferred auto_ptr
// ON-DAG: goc-color-report global g_bare {{[^ ]*}}:18{{(:[0-9]+)?}} uptr inferred T*
// ON-DAG: goc-color-report global g_explicit {{[^ ]*}}:19{{(:[0-9]+)?}} cptr explicit cptr
// ON-DAG: goc-color-report param only_stack.out {{[^ ]*}}:22{{(:[0-9]+)?}} sptr inferred T*
// ON-DAG: goc-color-report return ret_bare {{[^ ]*}}:24{{(:[0-9]+)?}} cptr inferred T*
// ON-DAG: goc-color-report param ret_bare.p {{[^ ]*}}:24{{(:[0-9]+)?}} auto inferred T*
// ON-DAG: goc-color-report return ret_cptr {{[^ ]*}}:29{{(:[0-9]+)?}} cptr explicit cptr
// ON-DAG: goc-color-report param ret_cptr.p {{[^ ]*}}:29{{(:[0-9]+)?}} auto inferred T*
// ON-DAG: goc-color-report local store_all.bare_local {{[^ ]*}}:36{{(:[0-9]+)?}} sptr inferred T*
// ON-DAG: goc-color-report local store_all.sp {{[^ ]*}}:37{{(:[0-9]+)?}} sptr explicit sptr
// ON-DAG: goc-color-report local store_all.ap {{[^ ]*}}:38{{(:[0-9]+)?}} sptr inferred auto_ptr
// Union fields are not listed.
// NOUNION-NOT: {{hidden|Hide\.}}
// With -default-ptr-color=cptr, unproven formals take the default; a proven
// stack formal and explicit colors stay.
// RUN: %opt-goc -passes='print<goc-color>,goc-color-escape' -default-ptr-color=cptr -S %t.ll -o %t.def.ll 2>&1 | FileCheck --check-prefix=DEF %s
// RUN: %opt-goc -passes=goc-color-escape -default-ptr-color=cptr -S %t.ll -o %t.def-off.ll
// RUN: diff %t.def-off.ll %t.def.ll
// DEF-DAG: goc-color-report param ret_bare.p {{[^ ]*}} cptr default-ptr-color T*
// DEF-DAG: goc-color-report param ret_cptr.p {{[^ ]*}} cptr default-ptr-color T*
// DEF-DAG: goc-color-report param only_stack.out {{[^ ]*}} sptr inferred T*
// DEF-DAG: goc-color-report local store_all.sp {{[^ ]*}} sptr explicit sptr
// DEF-DAG: goc-color-report local store_all.bare_local {{[^ ]*}} sptr inferred T*
// DEF-DAG: goc-color-report field Node.enc {{[^ ]*}} uptr explicit uptr
// DEF-DAG: goc-color-report field Node.cheap {{[^ ]*}} uptr inferred cptr
// DEF-DAG: goc-color-report global g_explicit {{[^ ]*}} cptr explicit cptr
// Without debug info the same colors are listed under IR names.
// RUN: %opt-goc -passes='print<goc-color>,goc-color-escape' -S %t.nog.ll -o %t.nog-on.ll 2>&1 | FileCheck --check-prefix=NOG %s
// RUN: %opt-goc -passes=goc-color-escape -S %t.nog.ll -o %t.nog-off.ll
// RUN: diff %t.nog-off.ll %t.nog-on.ll
// NOG-DAG: goc-color-report param only_stack.#0 {{[^ ]*}} sptr
// NOG-DAG: goc-color-report field Node.#1 {{[^ ]*}} uptr inferred cptr
