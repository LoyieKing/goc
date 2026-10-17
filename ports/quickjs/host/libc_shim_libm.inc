/* Part of libc_shim.c, which includes it; not compiled on its own.
 * Libm, compiler-rt 128-bit division, bare-name libcall aliases. */

/* ---------- libm ----------
 * sqrt/floor/ceil/trunc/round are inline. Transcendentals call glibc via
 * goc_libm_glibc.S. A few helpers call Go through __go_extern stubs. */
/* sqrt/floor/ceil/trunc/round are IEEE-754 specified. The C bodies match
 * glibc bit for bit (floor/ceil/trunc/round follow musl, MIT). */
typedef union { double f; uint64_t i; } goc_dbits;
static const double goc_toint = 4503599627370496.0; /* 2^52 */
double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
double floor(double x) {
  goc_dbits u = {x};
  int e = (int)(u.i >> 52 & 0x7ff);
  double y;
  if (e >= 0x3ff + 52 || x == 0) return x;
  if (u.i >> 63) y = x - goc_toint + goc_toint - x; else y = x + goc_toint - goc_toint - x;
  if (e <= 0x3ff - 1) return u.i >> 63 ? -1.0 : 0.0;
  if (y > 0) return x + y - 1;
  return x + y;
}
double ceil(double x) {
  goc_dbits u = {x};
  int e = (int)(u.i >> 52 & 0x7ff);
  double y;
  if (e >= 0x3ff + 52 || x == 0) return x;
  if (u.i >> 63) y = x - goc_toint + goc_toint - x; else y = x + goc_toint - goc_toint - x;
  if (e <= 0x3ff - 1) return u.i >> 63 ? -0.0 : 1.0;
  if (y < 0) return x + y + 1;
  return x + y;
}
double trunc(double x) {
  goc_dbits u = {x};
  int e = (int)(u.i >> 52 & 0x7ff) - 0x3ff + 12;
  uint64_t m;
  if (e >= 52 + 12) return x;
  if (e < 12) e = 1;
  m = -1ULL >> e;
  if ((u.i & m) == 0) return x;
  u.i &= ~m;
  return u.f;
}
double round(double x) {
  goc_dbits u = {x};
  int e = (int)(u.i >> 52 & 0x7ff);
  double y;
  if (e >= 0x3ff + 52) return x;
  if (u.i >> 63) x = -x;
  if (e < 0x3ff - 1) return 0 * u.f; /* |x| < 0.5: +-0 */
  y = x + goc_toint - goc_toint - x;
  if (y > 0.5) y = y + x - 1;
  else if (y <= -0.5) y = y + x + 1;
  else y = y + x;
  if (u.i >> 63) y = -y;
  return y;
}
double goc_round(double x) { return round(x); }

/* Transcendentals call glibc through ports/quickjs/host/asm/goc_libm_glibc.S. The asm
 * symbol is goc_libc_<name>; gocobj2goobj names it main.goc_libc_<name>.
 * A Go -> cgo -> glibc hop is not bit-identical in cost, and a C libm
 * would change the last bit. The trampoline realigns SP, then calls
 * glibc, so the SysV registers set up here are the ones glibc sees. */
#define GOC_LIBC_CLOBBERS \
  "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", \
  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
  "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", \
  "cc", "memory"

#define GOC_LIBC1(name) \
  double name(double x) { \
    double out; \
    __asm__ volatile( \
        "movsd %1, %%xmm0\n\t" \
        "call goc_libc_" #name "\n\t" \
        "movsd %%xmm0, %0" \
        : "=m"(out) : "m"(x) : GOC_LIBC_CLOBBERS); \
    return out; \
  }

#define GOC_LIBC2(name) \
  double name(double x, double y) { \
    double out; \
    __asm__ volatile( \
        "movsd %1, %%xmm0\n\t" \
        "movsd %2, %%xmm1\n\t" \
        "call goc_libc_" #name "\n\t" \
        "movsd %%xmm0, %0" \
        : "=m"(out) : "m"(x), "m"(y) : GOC_LIBC_CLOBBERS); \
    return out; \
  }

GOC_LIBC2(pow)
GOC_LIBC2(hypot)
GOC_LIBC2(fmod)
GOC_LIBC1(acos)
GOC_LIBC1(acosh)
GOC_LIBC1(asin)
GOC_LIBC1(asinh)
GOC_LIBC1(atan)
GOC_LIBC2(atan2)
GOC_LIBC1(atanh)
GOC_LIBC1(cbrt)
GOC_LIBC1(cos)
GOC_LIBC1(cosh)
GOC_LIBC1(exp)
GOC_LIBC1(expm1)
GOC_LIBC1(log)
GOC_LIBC1(log10)
GOC_LIBC1(log1p)
GOC_LIBC1(log2)
GOC_LIBC1(sin)
GOC_LIBC1(sinh)
GOC_LIBC1(tan)
GOC_LIBC1(tanh)


__go_extern("main.gocGoMathRoundToEven")
int64_t goc_x_MathRoundToEven(double x);
typedef struct { double frac; int64_t exp; } goc_frexp_pair;
__go_extern("main.gocGoMathFrexp")
goc_frexp_pair goc_x_MathFrexp(double x);
__go_extern("main.gocGoMathLdexp")
double goc_x_MathLdexp(double x, int32_t exponent);
typedef struct { double whole; double frac; } goc_modf_pair;
__go_extern("main.gocGoMathModf")
goc_modf_pair goc_x_MathModf(double x);
__go_extern("main.gocGoParseFloat")
double goc_x_ParseFloat(const char *p, uint64_t length);

long goc_lrint(double x) {
  return (long)goc_x_MathRoundToEven(x);
}

double frexp(double x, int *exponent) {
  goc_frexp_pair p = goc_x_MathFrexp(x);
  *exponent = (int)p.exp;
  return p.frac;
}

double scalbn(double x, int exponent) {
  return goc_x_MathLdexp(x, (int32_t)exponent);
}

double goc_scalbn(double x, int exponent) { return scalbn(x, exponent); }

double modf(double x, double *integer) {
  goc_modf_pair p = goc_x_MathModf(x);
  *integer = p.whole;
  return p.frac;
}

double strtod(const char *input, char **end) {
  const char *p = input;
  while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
    p++;
  const char *number = p;
  if (*p == '-' || *p == '+')
    p++;
  const char *digits = p;
  while (*p >= '0' && *p <= '9')
    p++;
  int found_digit = p != digits;
  if (*p == '.') {
    p++;
    digits = p;
    while (*p >= '0' && *p <= '9')
      p++;
    found_digit |= p != digits;
  }
  if (!found_digit) {
    if (end)
      *end = (char *)input;
    return 0;
  }
  const char *exponent = p;
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '-' || *p == '+')
      p++;
    digits = p;
    while (*p >= '0' && *p <= '9')
      p++;
    if (digits == p)
      p = exponent;
  }
  if (end)
    *end = (char *)p;
  size_t length = (size_t)(p - number);
  return goc_x_ParseFloat(number, (uint64_t)length);
}

double goc_fabs(double x) { return __builtin_fabs(x); }

#ifdef GOC_QJS_BELLARD
/* ---------- compiler-rt: unsigned 128-bit division ----------
 * Bellard's BigInt divides unsigned __int128 values (js_bigint_divrem,
 * js_bigint_to_string1); LLVM lowers that to a __udivti3 libcall. libgcc is
 * not linked into the goobj, so provide it here: one divq when the divisor
 * fits in 64 bits, shift-subtract otherwise. quickjs-ng never emits it. */
typedef unsigned __int128 goc_u128;
static inline uint64_t goc_divq(uint64_t hi, uint64_t lo, uint64_t d) {
  uint64_t q, r;  /* requires hi < d */
  __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(d) : "cc");
  (void)r;
  return q;
}
goc_u128 __udivti3(goc_u128 n, goc_u128 d) {
  uint64_t dh = (uint64_t)(d >> 64), dl = (uint64_t)d;
  uint64_t nh = (uint64_t)(n >> 64), nl = (uint64_t)n;
  if (dh == 0) {
    if (dl == 0)
      __builtin_trap();
    uint64_t qh = nh / dl, rh = nh % dl;
    return ((goc_u128)qh << 64) | goc_divq(rh, nl, dl);
  }
  if (n < d)
    return 0;
  int shift = __builtin_clzll(dh) - __builtin_clzll(nh);
  goc_u128 q = 0;
  d <<= shift;
  for (int i = 0; i <= shift; i++) {
    q <<= 1;
    if (n >= d) {
      n -= d;
      q |= 1;
    }
    d >>= 1;
  }
  return q;
}
#endif

/* ---------- bare-name aliases for compiler-emitted libcalls ----------
 * llc lowers llvm.memcpy/memset/memmove intrinsics (struct copies) to these
 * libcall names; -D macros cannot rename them, so provide the names here as
 * thin wrappers over the goc_-prefixed implementations. */
void *memcpy(void *d, const void *s, size_t n) { return goc_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return goc_memmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return goc_memset(d, c, n); }
