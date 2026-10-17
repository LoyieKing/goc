/* Explicit uptr conversions (goc_uptr.h) through the compiler's inline
 * bodies: round trips, then one wrong tag per mode, which must print
 * "goc FATAL uptr: ..." and trap. */
#include "goc.h"
#include "goc_uptr.h"
#include "fake_g.h"

#include <stdint.h>
#include <string.h>

static int global_word = 3;

int main(int argc, char **argv) {
  int local = 7;
  uintptr_t original = (uintptr_t)&local;
  uintptr_t lo = original - 0x1000;
  uintptr_t hi = original + 0x1000;
  goc_fake_g_set(lo, hi);
  if (goc_stack_lo() != lo || goc_stack_hi() != hi)
    return 10;

  goc_uptr s = goc_uptr_from_sptr((goc_sptr)&local);
  if (!goc_uptr_is_stack_encoded(s) || (uintptr_t)s != original - hi)
    return 11;
  if ((uintptr_t)goc_uptr_as_sptr(s) != original)
    return 12;
  if ((uintptr_t)goc_uptr_as_sptr_hi(s, hi + 0x100) != original + 0x100)
    return 13;
  if ((uintptr_t)goc_uptr_from_sptr_hi((goc_sptr)&local, hi) != original - hi)
    return 14;
  if (goc_uptr_decode(s) != (void *)&local)
    return 15;

  goc_uptr c = goc_uptr_from_cptr((goc_cptr)&global_word);
  if (!goc_uptr_is_cptr_encoded(c) || goc_uptr_as_cptr(c) != (void *)&global_word)
    return 16;
  if (goc_uptr_as_cptr_hi(goc_uptr_from_cptr_hi((goc_cptr)&global_word, hi), hi) !=
      (void *)&global_word)
    return 17;
  if (goc_uptr_decode(c) != (void *)&global_word)
    return 18;

  /* Stack movement: the encoding is relative to stack.hi. */
  goc_fake_g_set(lo + 0x200, hi + 0x200);
  if ((uintptr_t)goc_uptr_as_sptr(s) != original + 0x200)
    return 19;

  if (argc < 2)
    return 0;
  if (!strcmp(argv[1], "as_sptr"))      /* cptr word, stack expected */
    (void)goc_uptr_as_sptr(c);
  else if (!strcmp(argv[1], "as_cptr")) /* stack word, cptr expected */
    (void)goc_uptr_as_cptr(s);
  else if (!strcmp(argv[1], "from_cptr"))
    (void)goc_uptr_from_cptr((goc_cptr)(uintptr_t)s);
  else if (!strcmp(argv[1], "from_sptr")) /* above stack.hi: offset >= 0 */
    (void)goc_uptr_from_sptr((goc_sptr)(hi + 0x300));
  else if (!strcmp(argv[1], "require_cptr")) /* a live stack address */
    (void)goc_uptr_require_cptr((void *)(original + 0x200));
  return 20; /* the bad tag was not caught */
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
// A wrong tag writes the message and traps (SIGILL).
// RUN: not --crash %t.exe as_sptr 2>&1 | FileCheck --check-prefix=ASSPTR %s
// ASSPTR: goc FATAL uptr: goc_uptr_as_sptr: MSB clear
// RUN: not --crash %t.exe as_cptr 2>&1 | FileCheck --check-prefix=ASCPTR %s
// ASCPTR: goc FATAL uptr: goc_uptr_as_cptr: MSB set
// RUN: not --crash %t.exe from_cptr 2>&1 | FileCheck --check-prefix=FROMCPTR %s
// FROMCPTR: goc FATAL uptr: goc_uptr_from_cptr: address has MSB set
// RUN: not --crash %t.exe from_sptr 2>&1 | FileCheck --check-prefix=FROMSPTR %s
// FROMSPTR: goc FATAL uptr: goc_uptr_from_sptr: offset cleared MSB
// RUN: not --crash %t.exe require_cptr 2>&1 | FileCheck --check-prefix=REQUIRECPTR %s
// REQUIRECPTR: goc FATAL uptr: sptr cannot be returned as cptr
