/* goc.h — pointer colors for goc's patched Clang (docs/syntax-guide.md v0.2.1)
 *
 * The colors are native attributes of the patched Clang (clang/patches,
 * clang/sema/SemaGocColors.cpp): __attribute__((goc_cptr|goc_sptr|goc_uptr|
 * goc_auto_ptr|goc_gptr)). Clang mirrors them into the IR, where the color
 * pass reads them.
 *
 * A stack pointer stored into non-stack cptr/auto T* is promoted to uptr by
 * the IR pass. Unsupported escapes and raw sptr returns stay hard errors.
 */
#ifndef GOC_H
#define GOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOC_ANN_CPTR __attribute__((goc_cptr))
#define GOC_ANN_SPTR __attribute__((goc_sptr))
#define GOC_ANN_UPTR __attribute__((goc_uptr))
#define GOC_ANN_AUTO __attribute__((goc_auto_ptr))
#define GOC_ANN_GPTR __attribute__((goc_gptr))

#define cptr(T) T *GOC_ANN_CPTR
#define sptr(T) T *GOC_ANN_SPTR
#define uptr(T) T *GOC_ANN_UPTR
#define auto_ptr(T) T *GOC_ANN_AUTO
#define gptr(T) T *GOC_ANN_GPTR

typedef void *GOC_ANN_CPTR goc_cptr;
typedef void *GOC_ANN_SPTR goc_sptr;
typedef void *GOC_ANN_UPTR goc_uptr;
typedef void *GOC_ANN_AUTO goc_auto_ptr;
typedef void *GOC_ANN_GPTR goc_gptr;

goc_uptr goc_uptr_from_sptr(goc_sptr p);
goc_uptr goc_uptr_from_cptr(goc_cptr p);
goc_sptr goc_uptr_as_sptr(goc_uptr u);
goc_cptr goc_uptr_as_cptr(goc_uptr u);
goc_uptr goc_uptr_from_sptr_hi(goc_sptr p, uintptr_t stack_hi);
goc_uptr goc_uptr_from_cptr_hi(goc_cptr p, uintptr_t stack_hi);
goc_sptr goc_uptr_as_sptr_hi(goc_uptr u, uintptr_t stack_hi);
goc_cptr goc_uptr_as_cptr_hi(goc_uptr u, uintptr_t stack_hi);

goc_cptr goc_alloc(size_t n);
void goc_free(goc_cptr p);
goc_gptr goc_gptr_from_handle(uintptr_t handle);
uintptr_t goc_gptr_to_handle(goc_gptr p);

uintptr_t goc_stack_hi(void);
uintptr_t goc_stack_lo(void);
bool goc_stack_check(size_t need);

/* alloca(n) lowering (syntax-guide §7.2). Compiler-inserted. Result is cptr,
 * not a goroutine-stack sptr. scope is the function watermark. */
goc_cptr goc_dynalloc(size_t bytes, void **scope);
void goc_dynrelease(void **scope);

#define __goc_nosplit __attribute__((annotate("goc.attr.nosplit")))
#define __goc_nowb __attribute__((annotate("goc.attr.nowb")))
#define __goc_safepoint __attribute__((annotate("goc.attr.safepoint")))

/* C-side import of a Go function. Applied to a declaration only.
 * goc emits a SysV-callable stub (spill area, g in R14, clear xmm15,
 * pcsp/stackmap) that calls go_sym.goabi. Ordinary C calls use the
 * declared name. The C name must differ from the Go symbol's base name
 * (e.g. goc_x_Foo calling main.Foo) so the stub and the Go function
 * do not collide. The Go callee is not required to be nosplit. */
#define __go_extern(go_sym) __attribute__((annotate("goc.attr.go_extern:" go_sym)))

#ifdef __cplusplus
}
#endif
#endif /* GOC_H */
