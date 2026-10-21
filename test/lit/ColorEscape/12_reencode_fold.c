#include "goc.h"
#include "goc_uptr.h"
#include "fake_g.h"

#include <stdint.h>
#include <stdlib.h>

// The QuickJS frame chain: sf->prev_frame = rt->current_stack_frame and
// back. Both fields hold encoded words, so copying one into the other is a
// plain word copy (no decode/re-encode range check), and the copy still
// follows the stack when it moves.
struct Frame {
  struct Frame *prev;
};

struct Runtime {
  struct Frame *current;
};

static struct Runtime rt;

__attribute__((noinline)) static void push(struct Frame *sf) {
  sf->prev = rt.current;
  rt.current = sf;
}

// Same through a local, as QuickJS writes it (prev_sf = rt->current; ...).
// sf is a stack alloca in this function so its address escapes to rt, matching
// JSStackFrame in js_call_c_function.
__attribute__((noinline)) static void push_via_local(void) {
  struct Frame sf;
  struct Frame *prev = rt.current;
  sf.prev = prev;
  rt.current = &sf;
  rt.current = sf.prev; // pop before sf dies
}

__attribute__((noinline)) static void pop(struct Frame *sf) {
  rt.current = sf->prev;
}

int main(void) {
  struct Frame outer, inner;
  uintptr_t original = (uintptr_t)&outer;
  uintptr_t lo = original - 0x1000;
  uintptr_t hi = original + 0x1000;
  const uintptr_t delta = 0x100;

  goc_fake_g_set(lo, hi);
  rt.current = 0;
  push(&outer);
  // Exercise the local-variable shape (IR only); then the normal push/pop path.
  push_via_local(); // noinline, returns with rt restored? see below
  push(&inner);
  pop(&inner);
  if (rt.current != &outer)
    return 1;
  goc_fake_g_set(lo + delta, hi + delta);
  if ((uintptr_t)rt.current != original + delta)
    return 2;
  // A heap frame takes the cptr branch and round-trips unchanged.
  struct Frame *h = malloc(sizeof *h);
  push(h);
  pop(h);
  if (rt.current != (struct Frame *)(original + delta))
    return 4;
  pop(&outer);
  return rt.current == 0 ? 0 : 3;
}

// RUN: %goc-clang-ll -I %S -o %t.ll %s
// RUN: %goc-color-escape %t.ll -o %t.color.ll
// RUN: FileCheck --check-prefix=IR %s < %t.color.ll
// push: the stored frame is encoded once; prev copies the encoded word.
// IR-LABEL: define internal void @push(
// IR-NOT:   @goc_uptr_decode
// IR:       call ptr @goc_uptr_from_ptr
// IR-NOT:   @goc_uptr_decode
// IR:       ret void
// push_via_local: decode into a local; the encode of that local reads the
// encoded word from a shadow slot (no second range check).
// IR-LABEL: define internal void @push_via_local(
// IR:       call ptr @goc_uptr_decode
// IR:       call ptr @goc_uptr_from_ptr
// IR-NOT:   @goc_uptr_from_ptr
// IR:       ret void
// pop: an encoded word copied into another uptr slot, no helper at all.
// IR-LABEL: define internal void @pop(
// IR-NOT:   @goc_uptr_
// IR:       ret void
// RUN: %opt-goc -passes=goc-uptr-inline -S %t.color.ll -o %t.gate.ll
// RUN: %goc-clang -O0 %t.gate.ll -o %t.exe
// RUN: %t.exe
