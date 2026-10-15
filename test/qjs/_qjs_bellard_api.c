/* Exported entry points the Go side links by name (//go:linkname) that
 * Bellard's QuickJS only provides as static inline functions in quickjs.h.
 * quickjs-ng exports them from quickjs.c, so this TU is built only for
 * QJS_FLAVOR=bellard. The header's inline bodies are renamed on include and
 * re-exported under the public name. */
#define JS_FreeValue goc_bellard_inline_JS_FreeValue
#define JS_FreeValueRT goc_bellard_inline_JS_FreeValueRT
#define JS_DupValue goc_bellard_inline_JS_DupValue
#include "../../third_party/quickjs-bellard/quickjs.h"
#undef JS_FreeValue
#undef JS_FreeValueRT
#undef JS_DupValue

void JS_FreeValue(JSContext *ctx, JSValue v) {
  goc_bellard_inline_JS_FreeValue(ctx, v);
}

void JS_FreeValueRT(JSRuntime *rt, JSValue v) {
  goc_bellard_inline_JS_FreeValueRT(rt, v);
}

JSValue JS_DupValue(JSContext *ctx, JSValue v) {
  return goc_bellard_inline_JS_DupValue(ctx, v);
}

/* ng's JS_AddRuntimeFinalizer, used by the CLI worker/timer registry
 * (_qjs_cli_worker.c). quickjs.c is built with
 * -DJS_FreeRuntime=goc_bellard_JS_FreeRuntime so this TU owns the public
 * JS_FreeRuntime: it runs the registered finalizers (LIFO, like ng), then
 * frees the runtime. ng runs them after the runtime's final GC; here they run
 * just before it, which only matters for finalizers that touch JS objects
 * (the CLI's only frees its own C bookkeeping and releases JSValues). */
typedef void JSRuntimeFinalizer(JSRuntime *rt, void *arg);
typedef struct GocBellardFinalizer {
  JSRuntime *rt;
  JSRuntimeFinalizer *fn;
  void *arg;
  struct GocBellardFinalizer *next;
} GocBellardFinalizer;
static GocBellardFinalizer *goc_bellard_finalizers;

void goc_bellard_JS_FreeRuntime(JSRuntime *rt);

int JS_AddRuntimeFinalizer(JSRuntime *rt, JSRuntimeFinalizer *finalizer,
                           void *arg) {
  GocBellardFinalizer *f = js_malloc_rt(rt, sizeof(*f));
  if (!f)
    return -1;
  f->rt = rt;
  f->fn = finalizer;
  f->arg = arg;
  f->next = goc_bellard_finalizers;
  goc_bellard_finalizers = f;
  return 0;
}

void JS_FreeRuntime(JSRuntime *rt) {
  GocBellardFinalizer **link = &goc_bellard_finalizers;
  while (*link) {
    GocBellardFinalizer *f = *link;
    if (f->rt != rt) {
      link = &f->next;
      continue;
    }
    *link = f->next;
    f->fn(rt, f->arg);
    js_free_rt(rt, f);
    link = &goc_bellard_finalizers; /* a finalizer may have edited the list */
  }
  goc_bellard_JS_FreeRuntime(rt);
}
