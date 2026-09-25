/* Promise-hook path: an initialized child must observe the parent promise
 * through JSRuntime.parent_promise, even when the C call tree grows the g stack.
 */
#include "../../third_party/quickjs-ng/quickjs.h"
#include "../../runtime/goc_uptr.h"

typedef struct GocLink {
  struct GocLink *next;
  int value;
} GocLink;

static GocLink *goc_parent_link;
extern void *goc_malloc(size_t);

static __attribute__((noinline)) void goc_force_stack_growth(void) {
  volatile char pad[65536] __attribute__((aligned(8)));
  pad[0] = 1;
  pad[65535] = 2;
  /* Pin the full object: O3 can otherwise split the two volatile bytes into
   * separate slots and the probe never exercises an actual stack copy. */
  __asm__ volatile("" : : "m"(pad) : "memory");
}

int goc_qjs_link_copy_probe(void) {
  GocLink outer = {NULL, 8}, link;
  goc_parent_link = &outer;
  uintptr_t before = goc_stack_hi();
  link = (GocLink){goc_parent_link, 9};
  goc_parent_link = &link;
  goc_force_stack_growth();
  uintptr_t after = goc_stack_hi();
  int ok = goc_parent_link == &link && goc_parent_link->next == &outer &&
           goc_parent_link->next->value == 8;
  goc_parent_link = NULL;
  return (ok ? 1 : 0) | (before != after ? 2 : 0);
}

int goc_qjs_frame_chain_probe(int heap_frame) {
  GocLink outer = {NULL, 8}, local = {NULL, 9};
  GocLink *sf = heap_frame ? goc_malloc(sizeof(GocLink)) : &local;
  sf->value = 9;
  goc_parent_link = &outer;
  sf->next = goc_parent_link;
  goc_parent_link = sf;
  uintptr_t before = goc_stack_hi();
  goc_force_stack_growth();
  uintptr_t after = goc_stack_hi();
  int ok = goc_parent_link == sf && sf->next == &outer &&
           sf->next->value == 8;
  goc_parent_link = sf->next;
  ok &= goc_parent_link == &outer;
  goc_parent_link = NULL;
  return (ok ? 1 : 0) | (before != after ? 2 : 0);
}

static int goc_qjs_parent_count;

static void goc_qjs_promise_hook(JSContext *ctx, JSPromiseHookType type,
                                 JSValueConst promise, JSValueConst parent,
                                 void *opaque) {
  (void)ctx;
  (void)promise;
  (void)opaque;
  if (type == JS_PROMISE_HOOK_INIT && !JS_IsUndefined(parent))
    ++goc_qjs_parent_count;
}

void goc_qjs_install_promise_hook(JSRuntime *rt) {
  goc_qjs_parent_count = 0;
  JS_SetPromiseHook(rt, goc_qjs_promise_hook, NULL);
}

int goc_qjs_promise_parent_hooks(void) { return goc_qjs_parent_count; }
