/* Stack-growth and GC stress for the backend stack check.
 *
 * Each recursion level keeps a local buffer and hands the next level a
 * pointer into it (an sptr). After the child returns, the level re-reads its
 * own buffer through the pointer its parent gave it: if a stack copy during
 * the child (growth, or a shrink at a GC) failed to adjust a frame, the
 * values are wrong or the read faults. The three functions have frames in
 * the three stack-check tiers of go1.24-amd64: small (<= StackSmall = 128),
 * big (<= StackBig = 4096) and huge (> StackBig).
 */
#include <stdint.h>
#include "goc.h"

#define BAD (-1000000000L)

__attribute__((noinline)) static long rec_small(sptr(long) up, int n) {
  long buf[4];
  long v = *up;
  for (int i = 0; i < 4; i++)
    buf[i] = v + 4L * n + i;
  long r = n > 0 ? rec_small(&buf[1], n - 1) : 0;
  if (r == BAD || *up != v)
    return BAD;
  for (int i = 0; i < 4; i++)
    if (buf[i] != v + 4L * n + i)
      return BAD;
  return r + buf[3];
}

__attribute__((noinline)) static long rec_big(sptr(long) up, int n) {
  long buf[64];
  long v = *up;
  for (int i = 0; i < 64; i++)
    buf[i] = v + 4L * n + i;
  long r = n > 0 ? rec_big(&buf[1], n - 1) : 0;
  if (r == BAD || *up != v)
    return BAD;
  for (int i = 0; i < 64; i++)
    if (buf[i] != v + 4L * n + i)
      return BAD;
  return r + buf[3];
}

__attribute__((noinline)) static long rec_huge(sptr(long) up, int n) {
  long buf[1024];
  long v = *up;
  for (int i = 0; i < 1024; i++)
    buf[i] = v + 4L * n + i;
  long r = n > 0 ? rec_huge(&buf[1], n - 1) : 0;
  if (r == BAD || *up != v)
    return BAD;
  for (int i = 0; i < 1024; i++)
    if (buf[i] != v + 4L * n + i)
      return BAD;
  return r + buf[3];
}

long stress_small(long seed, int n) { return rec_small(&seed, n); }
long stress_big(long seed, int n) { return rec_big(&seed, n); }
long stress_huge(long seed, int n) { return rec_huge(&seed, n); }

/* A Go-ABI entry whose pointer argument points into the Go caller's frame:
 * the thunk's frame and the C body's both hold it across the recursion. */
long stress_through(sptr(long) p, int n) {
  long r = rec_big(p, n);
  return r == BAD ? BAD : r + *p;
}
