#include "goc.h"
#include "goc_uptr.h"

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

  goc_test_set_stack_lo(lo);
  goc_test_set_stack_hi(hi);
  holder.pointer = &local;
  relay.pointer = holder.pointer;

  goc_test_set_stack_lo(lo + delta);
  goc_test_set_stack_hi(hi + delta);
  return (uintptr_t)relay.pointer == original + delta ? 0 : 1;
}
