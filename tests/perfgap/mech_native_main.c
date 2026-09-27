#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
long mech_leafcalls(long), mech_fib(long), mech_localaddr(long), mech_interp(long),
    mech_chase(long), mech_indirect(long);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
struct { const char *name; long (*f)(long); long n; double div; } B[] = {
  {"leafcalls", mech_leafcalls, 200000000, 200000000.0},
  {"fib", mech_fib, 32, 7049155.0},   /* calls for fib(32) = 2*fib(33)-1 */
  {"localaddr", mech_localaddr, 200000000, 200000000.0},
  {"interp", mech_interp, 1600000, 409600000.0},
  {"chase", mech_chase, 200000000, 200000000.0},
  {"indirect", mech_indirect, 200000000, 200000000.0},
};
int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 5;
  for (unsigned b = 0; b < sizeof B / sizeof B[0]; b++) {
    if (argc > 2 && strcmp(argv[2], B[b].name)) continue;
    double best = 1e30; long r = 0;
    for (int k = 0; k < rounds; k++) {
      double t0 = now(); r = B[b].f(B[b].n); double t = now() - t0;
      if (t < best) best = t;
    }
    printf("%s ns/op=%.4f result=%ld\n", B[b].name, best / B[b].div, r);
  }
  return 0;
}
