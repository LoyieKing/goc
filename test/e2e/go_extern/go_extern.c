/* Golden: C calls Go through __go_extern stubs (spill, g, xmm15, pcsp).
 * C stub names must differ from the Go symbols they call. */
#include "goc.h"
#include <stdint.h>

typedef struct {
  int64_t sec;
  int64_t nsec;
} go_extern_pair_t;

__go_extern("main.go_extern_add")
int64_t goc_x_add(int64_t a, int64_t b);

__go_extern("main.go_extern_pair")
go_extern_pair_t goc_x_pair(int64_t a, int64_t b);

__go_extern("main.go_extern_grow")
int64_t goc_x_grow(int64_t n);

int64_t go_extern_probe(int64_t a, int64_t b) {
  int64_t sum = goc_x_add(a, b);
  go_extern_pair_t p = goc_x_pair(a, b);
  int64_t grown = goc_x_grow(32);
  return sum + p.sec + p.nsec + grown;
}
