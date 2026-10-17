/* Both branches of an aliased destination need the same encoded storage. */
#include "goc.h"
#include "goc_uptr.h"
#include "fake_g.h"

#include <stdint.h>

static int *global_word;
struct Holder { int *pointer; int *unrelated; };
static struct Holder global_holder;

int main(int argc, char **argv) {
  (void)argv;
  int local = 9;
  int *stack_word = 0;
  struct Holder local_holder = {0};
  uintptr_t original = (uintptr_t)&local;
  uintptr_t lo = original - 0x1000;
  uintptr_t hi = original + 0x1000;
  goc_fake_g_set(lo, hi);
  *(argc == 2 ? &stack_word : &global_word) = &local;
  *(argc == 2 ? &local_holder.pointer : &global_holder.pointer) = &local;
  goc_fake_g_set(lo + 0x100, hi + 0x100);
  if (argc == 2)
    return (uintptr_t)stack_word != original + 0x100 ||
           (uintptr_t)local_holder.pointer != original + 0x100;
  return (uintptr_t)global_word != original + 0x100 ||
         (uintptr_t)global_holder.pointer != original + 0x100;
}

// A host executable of the compiler's uptr helper bodies: color-escape,
// then goc-uptr-inline (as goc build runs them), then a native link.
// fake_g.h stands in for g, so goc_stack_lo/hi move.
// RUN: %goc-clang-ll -I %S -o %t.ll %s
// RUN: %goc-color-escape %t.ll -o %t.color.ll
// RUN: %opt-goc -passes=goc-uptr-inline -S %t.color.ll -o %t.gate.ll
// No uptr helper call survives goc-uptr-inline:
// RUN: FileCheck --check-prefix=GATE %s < %t.gate.ll
// GATE-NOT: call {{.*}}@goc_{{(uptr_(from|as|decode|require)|stack_|dyn)}}
// RUN: %goc-clang -O0 %t.gate.ll -o %t.exe
// RUN: %t.exe
// RUN: %t.exe local
