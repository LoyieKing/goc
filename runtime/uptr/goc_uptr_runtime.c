/* P19/P22: uptr MSB encode/decode against owner g stack.hi.
 *
 * Builds:
 *   Host (P19): default — test override + page approx; libc fatal.
 *   Go  (P22):  -DGOC_UPTR_FREESTANDING -DGOC_UPTR_HAVE_TLS
 *               inline FS:-8 → g→stack.hi (no cross-object relocs;
 *               Go internal linker does not apply clang PLT/GOT/.bss).
 */
#include "goc_uptr.h"

#ifndef GOC_UPTR_FREESTANDING
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef GOC_UPTR_WITH_GOC_H
#include "goc.h"
#endif

#ifndef GOC_UPTR_HAVE_TLS
static uintptr_t g_test_hi = 0;
static uintptr_t g_test_lo = 0;
static int g_test_hi_set = 0;
static int g_test_lo_set = 0;
#endif

void goc_uptr_fatal(const char *msg) {
#ifdef GOC_UPTR_FREESTANDING
  (void)msg;
  __builtin_trap();
  for (;;) {
  }
#else
  fprintf(stderr, "goc FATAL uptr: %s\n", msg ? msg : "(null)");
  fflush(stderr);
  abort();
#endif
}

void goc_test_set_stack_hi(uintptr_t hi) {
#ifdef GOC_UPTR_HAVE_TLS
  (void)hi; /* production TLS build: override disabled */
#else
  g_test_hi = hi;
  g_test_hi_set = 1;
#endif
}

void goc_test_set_stack_lo(uintptr_t lo) {
#ifdef GOC_UPTR_HAVE_TLS
  (void)lo;
#else
  g_test_lo = lo;
  g_test_lo_set = 1;
#endif
}

void goc_test_clear_stack_bounds(void) {
#ifndef GOC_UPTR_HAVE_TLS
  g_test_hi = 0;
  g_test_lo = 0;
  g_test_hi_set = 0;
  g_test_lo_set = 0;
#endif
}

/* linux/amd64 Go TLS — same convention as P1 morestack / P5 FS:-8. */
static inline uintptr_t goc_tls_getg(void) {
  uintptr_t g;
  __asm__ volatile("movq %%fs:-8, %0" : "=r"(g));
  return g;
}

static inline uintptr_t goc_tls_stack_hi_inline(void) {
  uintptr_t g = goc_tls_getg();
  uintptr_t hi;
  __asm__ volatile("movq 8(%1), %0" : "=r"(hi) : "r"(g));
  return hi;
}

static inline uintptr_t goc_tls_stack_lo_inline(void) {
  uintptr_t g = goc_tls_getg();
  uintptr_t lo;
  __asm__ volatile("movq 0(%1), %0" : "=r"(lo) : "r"(g));
  return lo;
}

uintptr_t goc_runtime_getg(void) { return goc_tls_getg(); }
uintptr_t goc_runtime_stack_hi(void) { return goc_tls_stack_hi_inline(); }
uintptr_t goc_runtime_stack_lo(void) { return goc_tls_stack_lo_inline(); }

uintptr_t goc_stack_hi(void) {
#ifdef GOC_UPTR_HAVE_TLS
  return goc_tls_stack_hi_inline();
#else
  if (g_test_hi_set)
    return g_test_hi;
  volatile char probe;
  uintptr_t approx = (uintptr_t)&probe;
  return (approx + 0x1000u) & ~(uintptr_t)0xfffu;
#endif
}

uintptr_t goc_stack_lo(void) {
#ifdef GOC_UPTR_HAVE_TLS
  return goc_tls_stack_lo_inline();
#else
  if (g_test_lo_set)
    return g_test_lo;
  return 0;
#endif
}

goc_uptr goc_uptr_from_cptr_hi(goc_cptr p, uintptr_t stack_hi) {
  (void)stack_hi;
  uintptr_t w = (uintptr_t)p;
  if (w & GOC_UPTR_MSB)
    goc_uptr_fatal("goc_uptr_from_cptr: address has MSB set (not encodeable as cptr)");
  return (goc_uptr)w;
}

goc_uptr goc_uptr_from_sptr_hi(goc_sptr p, uintptr_t stack_hi) {
  uintptr_t abs = (uintptr_t)p;
  int64_t off = (int64_t)abs - (int64_t)stack_hi;
  uintptr_t word = (uintptr_t)off;
  if ((word & GOC_UPTR_MSB) == 0)
    goc_uptr_fatal("goc_uptr_from_sptr: offset cleared MSB (abs not below stack.hi?)");
  return (goc_uptr)word;
}

goc_cptr goc_uptr_as_cptr_hi(goc_uptr u, uintptr_t stack_hi) {
  (void)stack_hi;
  uintptr_t w = (uintptr_t)u;
  if (w & GOC_UPTR_MSB)
    goc_uptr_fatal("goc_uptr_as_cptr: MSB set (expected absolute cptr encoding)");
  return (goc_cptr)w;
}

goc_sptr goc_uptr_as_sptr_hi(goc_uptr u, uintptr_t stack_hi) {
  uintptr_t w = (uintptr_t)u;
  if ((w & GOC_UPTR_MSB) == 0)
    goc_uptr_fatal("goc_uptr_as_sptr: MSB clear (expected stack-offset encoding)");
  int64_t off = (int64_t)w;
  uintptr_t abs = (uintptr_t)((int64_t)stack_hi + off);
  return (goc_sptr)abs;
}

goc_uptr goc_uptr_from_cptr(goc_cptr p) {
  return goc_uptr_from_cptr_hi(p, goc_stack_hi());
}

goc_uptr goc_uptr_from_sptr(goc_sptr p) {
  return goc_uptr_from_sptr_hi(p, goc_stack_hi());
}

goc_cptr goc_uptr_as_cptr(goc_uptr u) {
  return goc_uptr_as_cptr_hi(u, goc_stack_hi());
}

goc_sptr goc_uptr_as_sptr(goc_uptr u) {
  return goc_uptr_as_sptr_hi(u, goc_stack_hi());
}
