/* P29 golden: Go ABIInternal on clang-compiled code.
 *
 * goc_goabi.py gives supported functions a Go-ABI entry thunk, renames their
 * SysV bodies to <name>.impl, and retains ordinary C-to-C calls. These probes
 * cover register shuffles, floating-point registers, SysV stack overflow,
 * mixed integer/FP arguments, and aggregate returns whose SysV eightbytes use
 * different register classes. Clang coerces an eightbyte shared by several C
 * fields (e.g. {float, int32}) to one IR integer, so the Go-facing result is
 * derived from the IR fields, one per eightbyte.
 *
 * No libc calls: the Go test binary is CGO_ENABLED=0 (internal linking).
 */
#include "goc.h"

int goabi_add2(int a, int b) { return a + b; }

/* Internal call + stack locals: exercises the .impl body and its frame. */
int goabi_stack(int n) {
  int buf[8];
  for (int i = 0; i < 8; i++)
    buf[i] = i;
  buf[n & 7] = n;
  return buf[0] + buf[1] + goabi_add2(1, 1);
}

/* Pointer argument through the color surface (sptr = stack pointer contract). */
int goabi_store(sptr(int) out, int v) {
  *out = v;
  return v * 2;
}

long goabi_addl(long a, long b, long c, long d, long e, long f) {
  return a + b + c + d + e + f;
}

int64_t goabi_add7(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                   int64_t f, int64_t g) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g;
}

int64_t goabi_add10(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                    int64_t f, int64_t g, int64_t h, int64_t i, int64_t j) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h +
         9 * i + 10 * j;
}

double goabi_float_mix(double a, int64_t b, float c, double d) {
  return 2.0 * a + (double)b + 3.0 * (double)c + 4.0 * d;
}

double goabi_float9(double a, double b, double c, double d, double e,
                    double f, double g, double h, double i) {
  return a + 2.0 * b + 3.0 * c + 4.0 * d + 5.0 * e + 6.0 * f + 7.0 * g +
         8.0 * h + 9.0 * i;
}

int64_t goabi_mixed_stack(int64_t a, int64_t b, int64_t c, int64_t d,
                          int64_t e, int64_t f, int64_t g, double h,
                          double i, double j, double k, double l, double m,
                          double n, double o, double p) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g +
         (int64_t)(h + 2.0 * i + 3.0 * j + 4.0 * k + 5.0 * l + 6.0 * m +
                   7.0 * n + 8.0 * o + 9.0 * p);
}

struct goabi_pair {
  uint64_t lo;
  uint64_t hi;
};

struct goabi_pair goabi_pair(uint64_t a, uint64_t b) {
  struct goabi_pair result = {a + b, a + (b << 1)};
  return result;
}

/* SysV: XMM0 + RAX; Go: X0 + AX. */
struct goabi_double_tag {
  double value;
  int64_t tag;
};

struct goabi_double_tag goabi_double_tag(double value, int64_t tag) {
  struct goabi_double_tag result = {value + 0.5, tag + 7};
  return result;
}

/* SysV: RAX + XMM0; Go: AX + X0. */
struct goabi_tag_double {
  int64_t tag;
  double value;
};

struct goabi_tag_double goabi_tag_double(int64_t tag, double value) {
  struct goabi_tag_double result = {tag + 2, value + 3.0};
  return result;
}

/* SysV: XMM0 + XMM1; Go: X0 + X1. */
struct goabi_two_doubles {
  double lo;
  double hi;
};

struct goabi_two_doubles goabi_two_doubles(double a, double b) {
  struct goabi_two_doubles result = {a + b, a * b};
  return result;
}

/* Force a real goroutine stack copy while an sptr lives in a C frame slot. */
static __attribute__((noinline)) void goabi_touch(volatile char *p, int n) {
  for (int i = 0; i < n; i += 4096)
    p[i] = 1;
}

static __attribute__((noinline)) void goabi_consume_stack(void) {
  volatile char bytes[65536] __attribute__((aligned(8)));
  /* The address must escape. A local loop is visible to SROA, which then
   * shrinks the frame and the copy never happens. */
  goabi_touch(bytes, 65536);
  __asm__ volatile("" :: "r"(bytes) : "memory");
}

static uintptr_t goabi_stack_hi(void) {
  uintptr_t g, hi;
  __asm__ volatile("movq %%fs:-8, %0" : "=r"(g));
  __asm__ volatile("movq 8(%1), %0" : "=r"(hi) : "r"(g));
  return hi;
}

int goabi_movable_local(void) {
  int local = 7;
  sptr(int) p = &local;
  uintptr_t before = goabi_stack_hi();
  goabi_consume_stack();
  uintptr_t after = goabi_stack_hi();
  return (p == &local && *p == 7 ? 1 : 0) | (before != after ? 2 : 0);
}

/* The seventh SysV integer argument lives in the caller's outgoing stack
 * area. A stack copy in the callee's split preamble must adjust that exact
 * word using the map at the caller's call site. */
static __attribute__((noinline)) void goabi_write_stack_arg(
    int a, int b, int c, int d, int e, int f, sptr(int) out) {
  volatile char pad[65536] __attribute__((aligned(8)));
  goabi_touch(pad, 65536);
  *out += a + b + c + d + e + f + pad[0];
}

int goabi_stack_pointer_argument(void) {
  int local = 41;
  uintptr_t before = goabi_stack_hi();
  goabi_write_stack_arg(0, 0, 0, 0, 0, 0, &local);
  uintptr_t after = goabi_stack_hi();
  return (local == 42 ? 1 : 0) | (before != after ? 2 : 0);
}

/* The first SysV pointer argument is in DI, not the outgoing stack area.
 * Its slow-stub save needs a different map from the C function body. */
static __attribute__((noinline)) void goabi_write_register_arg(sptr(int) out) {
  volatile char pad[65536] __attribute__((aligned(8)));
  goabi_touch(pad, 65536);
  *out += pad[0];
}

int goabi_register_pointer_argument(void) {
  int local = 41;
  uintptr_t before = goabi_stack_hi();
  goabi_write_register_arg(&local);
  uintptr_t after = goabi_stack_hi();
  return (local == 42 ? 1 : 0) | (before != after ? 2 : 0);
}
