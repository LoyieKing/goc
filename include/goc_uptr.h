/* goc_uptr.h — uptr encode/decode (docs/language.md §3.3)
 *
 * Protocol (amd64 and arm64 user VA):
 *   MSB=0: absolute cptr address; use as pointer.
 *   MSB=1: int64 two's-complement offset from owner g's stack.hi;
 *          abs = (uintptr_t)((int64_t)stack_hi + (int64_t)stored).
 * Stack grows down → abs < hi → offset negative → MSB naturally 1.
 * Decode only with owner g's hi; wrong tag / cross-g → FATAL.
 * No dsptr. No silent sptr→uptr promote (frontend still errors).
 *
 * There is no runtime library: goc-uptr-inline (SMUptrInline.cpp) gives
 * every function declared here an inline body, at every -O. The current-g
 * API reads the live g (amd64 %fs:-8, arm64 x28): stack.lo at g+0,
 * stack.hi at g+8. A wrong tag writes "goc FATAL uptr: ..." and traps.
 * Protocol and helpers: docs/design/runtime.md §4.
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

/* ---- Explicit-hi API ---- */
goc_uptr goc_uptr_from_cptr_hi(goc_cptr p, uintptr_t stack_hi);
goc_uptr goc_uptr_from_sptr_hi(goc_sptr p, uintptr_t stack_hi);
goc_cptr goc_uptr_as_cptr_hi(goc_uptr u, uintptr_t stack_hi);
goc_sptr goc_uptr_as_sptr_hi(goc_uptr u, uintptr_t stack_hi);

/* ---- Current-g API (stack.hi of the running goroutine) ---- */
goc_uptr goc_uptr_from_cptr(goc_cptr p);
goc_uptr goc_uptr_from_sptr(goc_sptr p);
goc_cptr goc_uptr_as_cptr(goc_uptr u);
goc_sptr goc_uptr_as_sptr(goc_uptr u);
/* Compiler-inserted, tag-aware conversions for source-level T* storage. */
goc_uptr goc_uptr_from_ptr(void *p);
void *goc_uptr_decode(goc_uptr u);
void *goc_uptr_require_cptr(void *p);

/* The running goroutine's g.stack.hi / g.stack.lo. */
uintptr_t goc_stack_hi(void);
uintptr_t goc_stack_lo(void);

/* Lowered C alloca(size): allocations stay live until the containing function
 * returns, then goc_dynrelease releases the invocation's entire scope.
 * --fast-stack-alloca: a 16 MiB bump pool per object, one goroutine only.
 * Otherwise malloc/free per alloca. */
void *goc_dynalloc(size_t bytes, void **scope);
void goc_dynrelease(void **scope);

#ifdef __cplusplus
}
#endif

#endif /* GOC_UPTR_H */
