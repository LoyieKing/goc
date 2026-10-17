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
