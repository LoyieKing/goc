/* A stack pointer that comes back from a call.
 *
 * pick() returns a pointer loaded from a slot that holds &buf[k] of its
 * caller, and identity() returns its stack-pointer argument. With the
 * product default (--default-ptr-color cptr) the call result is colored
 * cptr, yet it points into the caller's frame. grow() then recurses through
 * huge frames, so the goroutine stack is copied while that pointer is live
 * (in a callee-saved register or a spill slot). Using it afterwards must
 * read and write the moved frame, not the old stack.
 */
#include "goc.h"

#define BAD (-1000000000L)

__attribute__((noinline)) long *pick(long **pp) { return *pp; }
__attribute__((noinline)) long *identity(long *p) { return p; }

__attribute__((noinline)) long grow(int n) {
  volatile long big[600];
  for (int i = 0; i < 600; i++)
    big[i] = n + i;
  long r = n > 0 ? grow(n - 1) : 0;
  return r + big[n % 600] - n;
}

long retptr_pick(int k, int depth) {
  long buf[8];
  /* keep scalar: this test is about the pointer, not SSE on the Go stack */
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
  for (int i = 0; i < 8; i++)
    buf[i] = 100 + i;
  long *slot = &buf[k & 7];
  long *p = pick(&slot);        /* call result aliases buf */
  long g = grow(depth);         /* stack copy while p is live */
  *p += 1000;                   /* must write the moved buf */
  long s = 0;
  /* keep scalar: this test is about the pointer, not SSE on the Go stack */
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
  for (int i = 0; i < 8; i++)
    s += buf[i];
  return (*p == 1100 + (k & 7) && s == 100 * 8 + 28 + 1000) ? s + g : BAD;
}

long retptr_identity(int k, int depth) {
  long buf[8];
  /* keep scalar: this test is about the pointer, not SSE on the Go stack */
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
  for (int i = 0; i < 8; i++)
    buf[i] = 7 * i;
  long *p = identity(&buf[k & 7]);
  long *q = identity(&buf[(k + 1) & 7]);
  long g = grow(depth);
  *p = -1;
  *q = -2;
  if (buf[k & 7] != -1 || buf[(k + 1) & 7] != -2)
    return BAD;
  long g2 = grow(depth);        /* a second copy, p and q still live */
  return (*p == -1 && *q == -2) ? g + g2 : BAD;
}
