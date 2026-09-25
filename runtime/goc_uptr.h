/* goc_uptr.h — P19/P22 uptr MSB encode/decode (syntax-guide v0.2.1 §3.3)
 *
 * Protocol (amd64 user VA):
 *   MSB=0: absolute cptr address; use as pointer.
 *   MSB=1: int64 two's-complement offset from owner g's stack.hi;
 *          abs = (uintptr_t)((int64_t)stack_hi + (int64_t)stored).
 * Stack grows down → abs < hi → offset negative → MSB naturally 1.
 * Decode only with owner g's hi; wrong tag / cross-g → FATAL.
 * No dsptr. No silent sptr→uptr promote (frontend still errors).
 *
 * P22: non-*_hi APIs read live g->stack.hi via TLS (goc_runtime_stack_hi /
 * FS:-8) when built with GOC_UPTR_HAVE_TLS and linked into a Go binary;
 * *_hi + goc_test_set_stack_hi remain for freestanding host proofs.
 */
#ifndef GOC_UPTR_H
#define GOC_UPTR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GOC_UPTR_MSB
#define GOC_UPTR_MSB ((uintptr_t)1 << 63)
#endif

#ifndef GOC_H
typedef void *goc_cptr;
typedef void *goc_sptr;
typedef void *goc_uptr;
typedef void *goc_auto_ptr;
#endif

static inline bool goc_uptr_is_stack_encoded(goc_uptr u) {
  return ((uintptr_t)u & GOC_UPTR_MSB) != 0;
}
static inline bool goc_uptr_is_cptr_encoded(goc_uptr u) {
  return ((uintptr_t)u & GOC_UPTR_MSB) == 0;
}

/* ---- Explicit-hi API (tests / IR lower / no TLS) ---- */
goc_uptr goc_uptr_from_cptr_hi(goc_cptr p, uintptr_t stack_hi);
goc_uptr goc_uptr_from_sptr_hi(goc_sptr p, uintptr_t stack_hi);
goc_cptr goc_uptr_as_cptr_hi(goc_uptr u, uintptr_t stack_hi);
goc_sptr goc_uptr_as_sptr_hi(goc_uptr u, uintptr_t stack_hi);

/* ---- Current-g API (goc_stack_hi → TLS g->stack.hi when HAVE_TLS) ---- */
goc_uptr goc_uptr_from_cptr(goc_cptr p);
goc_uptr goc_uptr_from_sptr(goc_sptr p);
goc_cptr goc_uptr_as_cptr(goc_uptr u);
goc_sptr goc_uptr_as_sptr(goc_uptr u);
/* Compiler-inserted, tag-aware conversions for source-level T* storage. */
goc_uptr goc_uptr_from_ptr(void *p);
void *goc_uptr_decode(goc_uptr u);
void *goc_uptr_require_cptr(void *p);

/* Stack.hi provider: test override > TLS g->stack.hi > host approx. */
uintptr_t goc_stack_hi(void);
uintptr_t goc_stack_lo(void);
void goc_test_set_stack_hi(uintptr_t hi);
void goc_test_set_stack_lo(uintptr_t lo);
void goc_test_clear_stack_bounds(void);

/* TLS hooks (goc_uptr_tls_amd64.S). Present when GOC_UPTR_HAVE_TLS .syso linked. */
uintptr_t goc_runtime_stack_hi(void);
uintptr_t goc_runtime_stack_lo(void);
uintptr_t goc_runtime_getg(void);

void goc_uptr_fatal(const char *msg) __attribute__((noreturn));

/* Lowered C alloca(size): allocations stay live until the containing function
 * returns, then goc_dynrelease releases the invocation's entire scope. */
void *goc_dynalloc(size_t bytes, void **scope);
void goc_dynrelease(void **scope);

/* GOC_DYNALLOC_POOL: single-thread bump pool instead of malloc per alloca.
 * Define it only when one goroutine owns every goc_dynalloc (QuickJS).
 * Default pool is 16 MiB; override with -DGOC_DYNALLOC_POOL_SIZE=bytes. */

#ifdef __cplusplus
}
#endif

#endif /* GOC_UPTR_H */
