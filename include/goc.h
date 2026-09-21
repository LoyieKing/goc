/* goc.h — P28 in-tree Clang / P27 plugin color surface (syntax-guide v0.2.1)
 *
 * When compiled with -fplugin=libGocClang.so, native attributes
 *   __attribute__((goc_cptr|goc_sptr|goc_uptr|goc_auto_ptr|goc_gptr))
 * are registered by the plugin and lowered to AnnotateAttr "goc.color.*".
 * Without the plugin, fall back to clang::annotate (P17-compatible).
 *
 * Sema (plugin): sptr store to heap/global = hard error; no dsptr; no auto-promote.
 */
#ifndef GOC_H
#define GOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prefer plugin-native spellings when the plugin is loaded (define GOC_USE_PLUGIN_ATTRS
 * from the goc driver). Otherwise annotate tokens survive into IR for P17 pass. */
/* Native goc_* attrs: in-tree Attr.td (P28) or -fplugin (P27). */
#if defined(GOC_USE_PLUGIN_ATTRS) || defined(GOC_USE_INTREE_ATTRS)
#  define GOC_ANN_CPTR __attribute__((goc_cptr))
#  define GOC_ANN_SPTR __attribute__((goc_sptr))
#  define GOC_ANN_UPTR __attribute__((goc_uptr))
#  define GOC_ANN_AUTO __attribute__((goc_auto_ptr))
#  define GOC_ANN_GPTR __attribute__((goc_gptr))
#else
#  define GOC_ANN_CPTR __attribute__((annotate("goc.color.cptr")))
#  define GOC_ANN_SPTR __attribute__((annotate("goc.color.sptr")))
#  define GOC_ANN_UPTR __attribute__((annotate("goc.color.uptr")))
#  define GOC_ANN_AUTO __attribute__((annotate("goc.color.auto")))
#  define GOC_ANN_GPTR __attribute__((annotate("goc.color.gptr")))
#endif

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

typedef struct JSObject JSObject;
typedef struct JSValue {
  int tagged_value;
  cptr(JSObject) pointer;
} JSValue;

static inline JSValue goc_js_from_obj(cptr(JSObject) p, int tag) {
  JSValue v;
  v.tagged_value = tag;
  v.pointer = p;
  return v;
}
static inline JSObject *goc_js_get_obj(JSValue v) { return v.pointer; }

#define __goc_nosplit __attribute__((annotate("goc.attr.nosplit")))
#define __goc_nowb __attribute__((annotate("goc.attr.nowb")))
#define __goc_safepoint __attribute__((annotate("goc.attr.safepoint")))

#ifdef __cplusplus
}
#endif
#endif /* GOC_H */
