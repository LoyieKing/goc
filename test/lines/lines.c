/* Real C file and line numbers in the Go object (gocobj 1.4).
 *
 * ra() returns its return address, a pc inside its caller. The Go side
 * looks that pc up with runtime.FuncForPC and runtime.CallersFrames and
 * checks the file, the line and, for lines_inlined, the inlined frame of
 * inner() above the frame of lines_inlined. Each function also returns the
 * line it expects, so the test does not hard-code line numbers.
 */
#include "goc.h"

__attribute__((noinline)) static long ra(void) { return (long)__builtin_return_address(0); }

static inline long inner(long x, sptr(long) line) {
  *line = __LINE__ + 1;
  return ra() + x;
}

long lines_direct(sptr(long) line) {
  *line = __LINE__ + 1;
  return ra();
}

long lines_inlined(long x, sptr(long) inl_line, sptr(long) call_line) {
  *call_line = __LINE__ + 1;
  long r = inner(x, inl_line) - x;
  return r;
}
