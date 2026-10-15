/* C->C calls with stack arguments on arm64 (moved 16 bytes up past the
 * LR slot Go unwinds through), also across a stack copy (rec grows it). */
typedef struct { long a, b; } P;
__attribute__((noinline)) static long many(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j, P p, double x, int k) {
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j + 11*p.a + 12*p.b + (long)(x*100) + 13*k;
}
__attribute__((noinline)) static long rec(long n, long a, long b, long c, long d, long e, long f, long g, long h, long i) {
  char buf[512]; buf[n & 511] = (char)n;
  if (n == 0) return a+b+c+d+e+f+g+h+i;
  return rec(n-1, a, b, c, d, e, f, g, h, i+1) + buf[n & 511] - (char)n;
}
long callmany(long s) { P p = {s, 2*s}; return many(s,1,2,3,4,5,6,7,8,9,p,1.5,-3); }
long deep(long n) { return rec(n, 1,2,3,4,5,6,7,8,0); }
