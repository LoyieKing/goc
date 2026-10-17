#include "goc.h"
#include "goc_uptr.h"
#include "fake_g.h"

#include <stdint.h>

struct Holder {
  cptr(int) pointer;
};

struct Relay {
  cptr(int) pointer;
};

static struct Holder holder;
static struct Relay relay;

int main(void) {
  int local = 7;
  uintptr_t original = (uintptr_t)&local;
  uintptr_t lo = original - 0x1000;
  uintptr_t hi = original + 0x1000;
  const uintptr_t delta = 0x100;

  goc_fake_g_set(lo, hi);
  holder.pointer = &local;
  relay.pointer = holder.pointer;

  goc_fake_g_set(lo + delta, hi + delta);
  return (uintptr_t)relay.pointer == original + delta ? 0 : 1;
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
