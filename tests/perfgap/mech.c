/* perf-gap mechanism microbenchmarks (docs/perf-gap.md).
 *
 * Built twice from this one file: by goc into a goobj called from Go
 * (main.go), and by clang/gcc natively with mech_native_main.c. Each function
 * loops n times internally so the Go->C thunk cost is amortised away; the
 * result is returned so nothing is optimised out. No libc calls.
 */
#define NOINLINE __attribute__((noinline))

/* 1. plain call to a leaf with no locals: prologue/epilogue cost per call
 *    (goc: morestack check; native: nothing). */
NOINLINE long mech_leaf(long x) { return x * 3 + 1; }
long mech_leafcalls(long n) {
  long acc = 0;
  for (long i = 0; i < n; i++) acc = mech_leaf(acc ^ i);
  return acc;
}

/* 2. recursion: call + frame + return, the JS_CallInternal pattern in small. */
NOINLINE long mech_fibr(long n) { return n < 2 ? n : mech_fibr(n - 1) + mech_fibr(n - 2); }
long mech_fib(long n) { return mech_fibr(n); }

/* 3. the address of a caller local stays live across a call: goc must assume
 *    the stack may move during the call and re-derive the address after it
 *    (frame-address guard). */
NOINLINE long mech_bump(long *p, long i) { *p += i; return *p & 7; }
long mech_localaddr(long n) {
  long local[2] = {0, 0};
  long acc = 0;
  long *p = &local[n & 1];
  for (long i = 0; i < n; i++) acc += mech_bump(p, i);
  return acc + local[0] + local[1];
}

/* 4. computed-goto bytecode loop in the shape of JS_CallInternal: a shared
 *    dispatch or a per-handler (tail-duplicated) one is the layout question. */
#define MECH_PROG 256
#define MECH_NOPS 31
static unsigned char mech_prog[MECH_PROG];
long mech_interp(long n) {
  /* 31 handlers plus an end opcode: more predecessors than LLVM's default
   * tail-duplication limit (16), like JS_CallInternal's ~250. A 256-op
   * pseudo-random program the branch predictor can learn with history. */
  unsigned x = 12345;
  for (int i = 0; i < MECH_PROG - 1; i++) {
    x = x * 1103515245u + 12345u;
    mech_prog[i] = (unsigned char)((x >> 16) % MECH_NOPS);
  }
  mech_prog[MECH_PROG - 1] = MECH_NOPS;
  const unsigned char *prog = mech_prog;
  static const void *tab[] = {
    &&op0, &&op1, &&op2, &&op3, &&op4, &&op5, &&op6, &&op7, &&op8, &&op9, &&op10, &&op11, &&op12, &&op13, &&op14, &&op15, &&op16, &&op17, &&op18, &&op19, &&op20, &&op21, &&op22, &&op23, &&op24, &&op25, &&op26, &&op27, &&op28, &&op29, &&op30, &&op31};
  long a = 1, b = 2, c = 3, iter = 0;
  const unsigned char *pc = prog;
#define NEXT goto *tab[*pc++]
  NEXT;
op0: a += b; NEXT;
op1: b ^= a; NEXT;
op2: c += a >> 3; NEXT;
op3: a = a * 5 + 1; NEXT;
op4: if (a & 1) b++; else c++; NEXT;
op5: b -= c; NEXT;
op6: c ^= b << 1; NEXT;
op7: a += c & 255; NEXT;
op8: if (b < 0) b = -b; NEXT;
op9: c = c * 3 + a; NEXT;
op10: a ^= c >> 2; NEXT;
op11: b += 7; NEXT;
op12: if (c & 4) a--; NEXT;
op13: c -= b >> 1; NEXT;
op14: b = (b + a) & 0xffffff; NEXT;
op15: a -= b & 15; NEXT;
op16: c += 11; NEXT;
op17: a = (a << 1) ^ c; NEXT;
op18: b = b * 7 + 3; NEXT;
op19: if (a > b) c ^= 1; NEXT;
op20: c = (c >> 1) + b; NEXT;
op21: a ^= 0x55; NEXT;
op22: b += c & 7; NEXT;
op23: c -= a & 31; NEXT;
op24: a += (b ^ c) & 63; NEXT;
op25: b = (b >> 2) ^ a; NEXT;
op26: if (c < 0) c = -c; NEXT;
op27: a = a * 9 - b; NEXT;
op28: b ^= c + 1; NEXT;
op29: c += (a & b) | 1; NEXT;
op30: a -= c >> 4; NEXT;
op31:
  if (++iter < n) { pc = prog; NEXT; }
  return a + b + c;
#undef NEXT
}

/* 5. pointer chasing through a static array (loads through C pointers). */
struct mech_node { struct mech_node *next; long v; };
static struct mech_node mech_nodes[4096];
long mech_chase(long n) {
  for (long i = 0; i < 4096; i++) {
    mech_nodes[i].next = &mech_nodes[(i * 1597 + 1) & 4095];
    mech_nodes[i].v = i;
  }
  struct mech_node *p = &mech_nodes[0];
  long acc = 0;
  for (long i = 0; i < n; i++) { acc += p->v; p = p->next; }
  return acc;
}

/* 6. an indirect call through a function-pointer table (JS C functions). */
typedef long (*mech_fn)(long);
NOINLINE long mech_f0(long x) { return x + 1; }
NOINLINE long mech_f1(long x) { return x ^ 5; }
NOINLINE long mech_f2(long x) { return x * 3; }
NOINLINE long mech_f3(long x) { return x - 2; }
static mech_fn mech_fns[4] = {mech_f0, mech_f1, mech_f2, mech_f3};
long mech_indirect(long n) {
  long acc = 0;
  for (long i = 0; i < n; i++) acc = mech_fns[i & 3](acc);
  return acc;
}
