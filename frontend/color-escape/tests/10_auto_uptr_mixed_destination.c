/* Both branches of an aliased destination need the same encoded storage. */
#include "goc.h"
#include "goc_uptr.h"

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
  goc_test_set_stack_lo(lo);
  goc_test_set_stack_hi(hi);
  *(argc == 2 ? &stack_word : &global_word) = &local;
  *(argc == 2 ? &local_holder.pointer : &global_holder.pointer) = &local;
  goc_test_set_stack_lo(lo + 0x100);
  goc_test_set_stack_hi(hi + 0x100);
  if (argc == 2)
    return (uintptr_t)stack_word != original + 0x100 ||
           (uintptr_t)local_holder.pointer != original + 0x100;
  return (uintptr_t)global_word != original + 0x100 ||
         (uintptr_t)global_holder.pointer != original + 0x100;
}
