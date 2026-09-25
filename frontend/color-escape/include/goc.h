/* goc.h — P17 pointer-color surface (syntax-guide v0.2.1)
 *
 * Colors are declared with clang::annotate strings that survive into LLVM IR as
 * llvm.var.annotation / llvm.ptr.annotation / llvm.global.annotations.
 * The goc-color-escape pass reads those + provenance to refine auto_ptr and
 * reject illegal sptr escapes (no silent promote to uptr; no dsptr).
 */
#ifndef GOC_H
#define GOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- annotate tokens (must match pass) ---- */
#define GOC_ANN_CPTR __attribute__((annotate("goc.color.cptr")))
#define GOC_ANN_SPTR __attribute__((annotate("goc.color.sptr")))
#define GOC_ANN_UPTR __attribute__((annotate("goc.color.uptr")))
#define GOC_ANN_AUTO __attribute__((annotate("goc.color.auto")))
#define GOC_ANN_GPTR __attribute__((annotate("goc.color.gptr")))

/*
 * Color macros: one pointer layer.
 *   cptr(int) p;   → int * __annotate(cptr) p;
 * `T *` without annotate ≡ auto_ptr (pass infers).
 */
#define cptr(T) T *GOC_ANN_CPTR
#define sptr(T) T *GOC_ANN_SPTR
#define uptr(T) T *GOC_ANN_UPTR
#define auto_ptr(T) T *GOC_ANN_AUTO
#define gptr(T) T *GOC_ANN_GPTR

/* Void aliases for APIs that erase T */
typedef void *GOC_ANN_CPTR goc_cptr;
typedef void *GOC_ANN_SPTR goc_sptr;
typedef void *GOC_ANN_UPTR goc_uptr;
typedef void *GOC_ANN_AUTO goc_auto_ptr;
typedef void *GOC_ANN_GPTR goc_gptr;

/* ---- uptr encode / decode builtins (pass-recognized names) ----
 * MSB protocol (P19): MSB=0 absolute cptr; MSB=1 int64 two's-complement
 * offset from owner g.stack.hi; abs = hi + stored. See
 * ../../p19-uptr-runtime/include/goc_uptr.h and docs/UPTR-MSB.md.
 * Frontend (P17) checks color/escape; IR lower: goc-uptr-lower;
 * link libgoc_uptr.a for host execution (goc_stubs.c identity is IR-only).
 */
goc_uptr goc_uptr_from_sptr(goc_sptr p);
goc_uptr goc_uptr_from_cptr(goc_cptr p);
goc_sptr goc_uptr_as_sptr(goc_uptr u);
goc_cptr goc_uptr_as_cptr(goc_uptr u);
/* Compiler-inserted conversions for T* slots promoted to uptr storage. */
goc_uptr goc_uptr_from_ptr(void *p);
void *goc_uptr_decode(goc_uptr u);
void *goc_uptr_require_cptr(void *p);
/* Explicit-hi variants (tests / IR lower without TLS) */
goc_uptr goc_uptr_from_sptr_hi(goc_sptr p, uintptr_t stack_hi);
goc_uptr goc_uptr_from_cptr_hi(goc_cptr p, uintptr_t stack_hi);
goc_sptr goc_uptr_as_sptr_hi(goc_uptr u, uintptr_t stack_hi);
goc_cptr goc_uptr_as_cptr_hi(goc_uptr u, uintptr_t stack_hi);

/* Non-stack / Go-heap producers (provenance hints for the pass) */
goc_cptr goc_alloc(size_t n);
void goc_free(goc_cptr p);
goc_gptr goc_gptr_from_handle(uintptr_t handle);
uintptr_t goc_gptr_to_handle(goc_gptr p);

/* Stack bounds: P19 runtime / test harness; production → owner g->stack.hi */
uintptr_t goc_stack_hi(void);
uintptr_t goc_stack_lo(void);
bool goc_stack_check(size_t need);

/* ---- JSValue: explicit struct, not NaN-box (syntax-guide §6) ---- */
typedef struct JSObject JSObject;

typedef struct JSValue {
  int tagged_value;
  cptr(JSObject) pointer; /* color on pointer field */
} JSValue;

static inline JSValue goc_js_from_obj(cptr(JSObject) p, int tag) {
  JSValue v;
  v.tagged_value = tag;
  v.pointer = p;
  return v;
}

static inline cptr(JSObject) goc_js_get_obj(JSValue v) {
  return v.pointer;
}

/* Optional attrs (parsed as annotate; enforced lightly in P17) */
#define __goc_nosplit __attribute__((annotate("goc.attr.nosplit")))
#define __goc_nowb __attribute__((annotate("goc.attr.nowb")))
#define __goc_safepoint __attribute__((annotate("goc.attr.safepoint")))

#ifdef __cplusplus
}
#endif

#endif /* GOC_H */
