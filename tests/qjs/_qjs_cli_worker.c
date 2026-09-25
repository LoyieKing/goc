/* qjs:os Worker and timer support for the goc-built CLI.
 *
 * QuickJS runtimes are isolated, but deliberately scheduled synchronously by
 * the owning CLI goroutine. No two runtimes enter QuickJS at once: this keeps
 * the freestanding allocator and all C state safe without calling pthreads,
 * libc, cgo, or fork from a QuickJS C frame.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../third_party/quickjs-ng/quickjs.h"

extern int goc_qjs_cli_install(JSContext *ctx);
extern int goc_qjs_cli_unhandled_rejections(JSContext *ctx);
extern JSValue goc_qjs_cli_rejection_reason(JSContext *ctx);
extern void goc_qjs_cli_clear_rejections(JSContext *ctx);

typedef struct GocQjsCliMessage GocQjsCliMessage;
typedef struct GocQjsCliTimer GocQjsCliTimer;
typedef struct GocQjsCliRuntime GocQjsCliRuntime;
typedef struct GocQjsCliWorker GocQjsCliWorker;

struct GocQjsCliMessage {
  JSRuntime *owner_rt;
  uint8_t *data;
  size_t data_len;
  uint8_t **sab_tab;
  size_t sab_tab_len;
  GocQjsCliMessage *next;
};

struct GocQjsCliTimer {
  int64_t id;
  int64_t deadline_ns;
  int64_t delay_ns;
  bool repeats;
  JSValue func;
  GocQjsCliTimer *next;
};

struct GocQjsCliWorker {
  GocQjsCliRuntime *owner;
  GocQjsCliRuntime *child;
  GocQjsCliWorker *peer;
  bool parent_port;
  bool closed;
  bool started;
  JSValue onmessage;
  char *basename;
  char *filename;
  char *error;
  GocQjsCliMessage *incoming_head, *incoming_tail;
  GocQjsCliMessage *outgoing_head, *outgoing_tail;
  GocQjsCliWorker *next;
};

struct GocQjsCliRuntime {
  JSRuntime *rt;
  JSContext *ctx;
  GocQjsCliTimer *timers;
  int64_t next_timer_id;
  GocQjsCliWorker *workers;
  GocQjsCliWorker *parent_worker;
  GocQjsCliWorker *parent_port;
  JSValue module_promise;
  bool module_pending;
  bool module_loaded;
  bool is_worker;
  GocQjsCliRuntime *next;
};

typedef struct {
  uint32_t refs;
  uint32_t padding;
  JSRuntime *owner_rt;
} GocQjsCliSabHeader;

static GocQjsCliRuntime *goc_qjs_cli_runtimes;
static JSClassID goc_qjs_cli_worker_class_id;

static void goc_qjs_cli_cleanup_runtime(GocQjsCliRuntime *state);

static char *goc_qjs_cli_strdup_rt(JSRuntime *rt, const char *text) {
  size_t length = 0;
  while (text[length])
    length++;
  char *copy = js_malloc_rt(rt, length + 1);
  if (!copy)
    return NULL;
  for (size_t i = 0; i <= length; i++)
    copy[i] = text[i];
  return copy;
}

static int goc_qjs_cli_registry_failure(JSContext *ctx) {
  if (!JS_HasException(ctx))
    JS_ThrowInternalError(ctx, "could not register qjs:os Worker APIs");
  return -1;
}

static int64_t goc_qjs_cli_now_ns(void) {
  struct {
    int64_t sec;
    int64_t nsec;
  } ts;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(228L), "D"(1L), "S"(&ts)
                   : "rcx", "r11", "memory");
  if (result < 0)
    return 0;
  if (ts.sec > INT64_MAX / 1000000000)
    return INT64_MAX;
  return ts.sec * 1000000000 + ts.nsec;
}

/* The scheduler waits on the Go runtime rather than a C or libc sleep. */
#define GOC_GO_CLOBBERS \
  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", \
  "r11", "r12", "r13", "r14", "r15", "xmm0", "xmm1", "xmm2", \
  "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", \
  "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc", "memory"

static void goc_qjs_cli_wait_ns(int64_t nanoseconds) {
  if (nanoseconds <= 0)
    return;
  __asm__ volatile(
      "movq %0, %%rax\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocQjsCliWorkerSleep.goabi"
      :
      : "m"(nanoseconds)
      : GOC_GO_CLOBBERS);
}

static GocQjsCliRuntime *goc_qjs_cli_find_runtime(JSRuntime *rt) {
  for (GocQjsCliRuntime *state = goc_qjs_cli_runtimes; state;
       state = state->next) {
    if (state->rt == rt)
      return state;
  }
  return NULL;
}

static void goc_qjs_cli_free_message(GocQjsCliMessage *message) {
  if (!message)
    return;
  for (size_t i = 0; i < message->sab_tab_len; i++) {
    GocQjsCliSabHeader *header =
        (GocQjsCliSabHeader *)(message->sab_tab[i] - sizeof(*header));
    if (__atomic_sub_fetch(&header->refs, 1, __ATOMIC_ACQ_REL) == 0)
      js_free_rt(header->owner_rt, header);
  }
  js_free_rt(message->owner_rt, message->sab_tab);
  js_free_rt(message->owner_rt, message->data);
  js_free_rt(message->owner_rt, message);
}

static void goc_qjs_cli_free_message_queue(GocQjsCliMessage *message) {
  while (message) {
    GocQjsCliMessage *next = message->next;
    goc_qjs_cli_free_message(message);
    message = next;
  }
}

static void goc_qjs_cli_queue_message(GocQjsCliMessage **head,
                                      GocQjsCliMessage **tail,
                                      GocQjsCliMessage *message) {
  message->next = NULL;
  if (*tail)
    (*tail)->next = message;
  else
    *head = message;
  *tail = message;
}

static GocQjsCliMessage *goc_qjs_cli_pop_message(GocQjsCliMessage **head,
                                                 GocQjsCliMessage **tail) {
  GocQjsCliMessage *message = *head;
  if (!message)
    return NULL;
  *head = message->next;
  if (!*head)
    *tail = NULL;
  message->next = NULL;
  return message;
}

static void goc_qjs_cli_sab_free(void *opaque, void *ptr) {
  (void)opaque;
  if (!ptr)
    return;
  GocQjsCliSabHeader *header =
      (GocQjsCliSabHeader *)((uint8_t *)ptr - sizeof(*header));
  if (__atomic_sub_fetch(&header->refs, 1, __ATOMIC_ACQ_REL) == 0)
    js_free_rt(header->owner_rt, header);
}

static void *goc_qjs_cli_sab_alloc(void *opaque, size_t size) {
  JSRuntime *rt = opaque;
  if (size > (size_t)-1 - sizeof(GocQjsCliSabHeader))
    return NULL;
  GocQjsCliSabHeader *header =
      js_malloc_rt(rt, sizeof(*header) + size);
  if (!header)
    return NULL;
  header->refs = 1;
  header->padding = 0;
  header->owner_rt = rt;
  return (uint8_t *)header + sizeof(*header);
}

static void goc_qjs_cli_sab_dup(void *opaque, void *ptr) {
  (void)opaque;
  GocQjsCliSabHeader *header =
      (GocQjsCliSabHeader *)((uint8_t *)ptr - sizeof(*header));
  __atomic_add_fetch(&header->refs, 1, __ATOMIC_ACQ_REL);
}

static void goc_qjs_cli_remove_worker(GocQjsCliRuntime *state,
                                      GocQjsCliWorker *worker) {
  GocQjsCliWorker **link = &state->workers;
  while (*link && *link != worker)
    link = &(*link)->next;
  if (*link == worker)
    *link = worker->next;
  worker->next = NULL;
}

static void goc_qjs_cli_runtime_finalizer(JSRuntime *rt, void *opaque);

static GocQjsCliRuntime *goc_qjs_cli_get_runtime(JSContext *ctx, bool create) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  GocQjsCliRuntime *state = goc_qjs_cli_find_runtime(rt);
  if (state || !create)
    return state;

  state = js_mallocz(ctx, sizeof(*state));
  if (!state)
    return NULL;
  state->rt = rt;
  state->ctx = ctx;
  state->next_timer_id = 1;
  state->module_promise = JS_UNDEFINED;
  if (JS_AddRuntimeFinalizer(rt, goc_qjs_cli_runtime_finalizer, state) < 0) {
    js_free(ctx, state);
    return NULL;
  }
  state->next = goc_qjs_cli_runtimes;
  goc_qjs_cli_runtimes = state;
  return state;
}

static void goc_qjs_cli_stop_worker(GocQjsCliWorker *worker);

static void goc_qjs_cli_free_worker(JSRuntime *rt, GocQjsCliWorker *worker) {
  goc_qjs_cli_stop_worker(worker);
  JS_FreeValueRT(rt, worker->onmessage);
  js_free_rt(rt, worker->basename);
  js_free_rt(rt, worker->filename);
  js_free_rt(rt, worker->error);
  goc_qjs_cli_free_message_queue(worker->incoming_head);
  goc_qjs_cli_free_message_queue(worker->outgoing_head);
  js_free_rt(rt, worker);
}

static void goc_qjs_cli_runtime_finalizer(JSRuntime *rt, void *opaque) {
  GocQjsCliRuntime *state = opaque;
  GocQjsCliRuntime **link = &goc_qjs_cli_runtimes;
  while (*link && *link != state)
    link = &(*link)->next;
  if (*link == state)
    *link = state->next;

  while (state->workers) {
    GocQjsCliWorker *worker = state->workers;
    state->workers = worker->next;
    worker->next = NULL;
    goc_qjs_cli_stop_worker(worker);
    goc_qjs_cli_free_worker(rt, worker);
  }
  /* Runtime-scoped JSValues must be released before JS_RunGC(), via the
   * explicit cleanup hook; the QuickJS runtime finalizer runs too late. */
  while (state->timers) {
    GocQjsCliTimer *timer = state->timers;
    state->timers = timer->next;
    js_free_rt(rt, timer);
  }
  (void)rt;
  js_free_rt(rt, state);
}

static int64_t goc_qjs_cli_deadline(int64_t now, int64_t delay) {
  if (delay > 0 && now > INT64_MAX - delay)
    return INT64_MAX;
  return now + delay;
}

static JSValue goc_qjs_cli_set_timer(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic) {
  (void)this_val;
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, true);
  if (!state)
    return JS_EXCEPTION;
  JSValueConst callback = argc > 0 ? argv[0] : JS_UNDEFINED;
  JSValueConst delay_value = argc > 1 ? argv[1] : JS_UNDEFINED;
  if (!JS_IsFunction(ctx, callback))
    return JS_ThrowTypeError(ctx, "not a function");

  int64_t delay_ms;
  if (JS_ToInt64(ctx, &delay_ms, delay_value) < 0)
    return JS_EXCEPTION;
  if (delay_ms < 1)
    delay_ms = 1;
  if (delay_ms > INT64_MAX / 1000000)
    delay_ms = INT64_MAX / 1000000;
  int64_t delay_ns = delay_ms * 1000000;

  GocQjsCliTimer *timer = js_mallocz(ctx, sizeof(*timer));
  if (!timer)
    return JS_EXCEPTION;
  timer->id = state->next_timer_id++;
  if (state->next_timer_id > 9007199254740991LL)
    state->next_timer_id = 1;
  timer->deadline_ns = goc_qjs_cli_deadline(goc_qjs_cli_now_ns(), delay_ns);
  timer->delay_ns = delay_ns;
  timer->repeats = magic != 0;
  timer->func = JS_DupValue(ctx, callback);
  timer->next = NULL;
  GocQjsCliTimer **tail = &state->timers;
  while (*tail)
    tail = &(*tail)->next;
  *tail = timer;
  return JS_NewInt64(ctx, timer->id);
}

static JSValue goc_qjs_cli_clear_timer(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, false);
  if (!state)
    return JS_UNDEFINED;
  int64_t id;
  JSValueConst id_value = argc > 0 ? argv[0] : JS_UNDEFINED;
  if (JS_ToInt64(ctx, &id, id_value) < 0)
    return JS_EXCEPTION;
  if (id <= 0)
    return JS_UNDEFINED;
  GocQjsCliTimer **link = &state->timers;
  while (*link) {
    GocQjsCliTimer *timer = *link;
    if (timer->id == id) {
      *link = timer->next;
      JS_FreeValue(ctx, timer->func);
      js_free(ctx, timer);
      break;
    }
    link = &timer->next;
  }
  return JS_UNDEFINED;
}

static int goc_qjs_cli_next_timer(GocQjsCliRuntime *state,
                                  int64_t *deadline_ns) {
  if (!state || !state->timers)
    return 0;
  int64_t min = INT64_MAX;
  for (GocQjsCliTimer *timer = state->timers; timer; timer = timer->next) {
    if (timer->deadline_ns < min)
      min = timer->deadline_ns;
  }
  *deadline_ns = min;
  return 1;
}

static int goc_qjs_cli_has_due_timer(GocQjsCliRuntime *state,
                                     int64_t now_ns) {
  int64_t deadline;
  return goc_qjs_cli_next_timer(state, &deadline) && deadline <= now_ns;
}

static int goc_qjs_cli_run_due_timer(GocQjsCliRuntime *state,
                                     JSContext *ctx) {
  int64_t now_ns = goc_qjs_cli_now_ns();
  GocQjsCliTimer **selected = NULL;
  for (GocQjsCliTimer **link = &state->timers; *link;
       link = &(*link)->next) {
    GocQjsCliTimer *timer = *link;
    if (timer->deadline_ns <= now_ns &&
        (!selected || timer->deadline_ns < (*selected)->deadline_ns))
      selected = link;
  }
  if (!selected)
    return 0;

  GocQjsCliTimer *timer = *selected;
  JSValue func = JS_DupValue(ctx, timer->func);
  if (timer->repeats) {
    timer->deadline_ns = goc_qjs_cli_deadline(now_ns, timer->delay_ns);
  } else {
    *selected = timer->next;
    JS_FreeValue(ctx, timer->func);
    js_free(ctx, timer);
  }
  JSValue result = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
  JS_FreeValue(ctx, func);
  if (JS_IsException(result))
    return -1;
  JS_FreeValue(ctx, result);
  return 1;
}

static int goc_qjs_cli_save_error(GocQjsCliWorker *worker, JSContext *ctx,
                                  JSValueConst reason) {
  size_t length;
  const char *text = JS_ToCStringLen(ctx, &length, reason);
  if (!text) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    static const char fallback[] = "worker exception could not be converted";
    length = sizeof(fallback) - 1;
    char *copy = js_malloc_rt(worker->owner->rt, sizeof(fallback));
    if (!copy)
      return -1;
    for (size_t i = 0; i < sizeof(fallback); i++)
      copy[i] = fallback[i];
    js_free_rt(worker->owner->rt, worker->error);
    worker->error = copy;
    return 0;
  }
  GocQjsCliRuntime *owner = worker->owner;
  char *copy = js_malloc_rt(owner->rt, length + 1);
  if (copy) {
    for (size_t i = 0; i < length; i++)
      copy[i] = text[i];
    copy[length] = '\0';
  }
  JS_FreeCString(ctx, text);
  js_free_rt(owner->rt, worker->error);
  worker->error = copy;
  return copy ? 0 : -1;
}

static void goc_qjs_cli_prefix_error(GocQjsCliWorker *worker,
                                     const char *phase) {
  if (!worker->error)
    return;
  size_t prefix = 0, length = 0;
  while (phase[prefix])
    prefix++;
  while (worker->error[length])
    length++;
  char *message = js_malloc_rt(worker->owner->rt, prefix + length + 3);
  if (!message)
    return;
  for (size_t i = 0; i < prefix; i++)
    message[i] = phase[i];
  message[prefix] = ':';
  message[prefix + 1] = ' ';
  for (size_t i = 0; i <= length; i++)
    message[prefix + 2 + i] = worker->error[i];
  js_free_rt(worker->owner->rt, worker->error);
  worker->error = message;
}

static int goc_qjs_cli_capture_worker_exception(GocQjsCliWorker *worker,
                                                JSContext *ctx) {
  JSValue exception = JS_GetException(ctx);
  int result = goc_qjs_cli_save_error(worker, ctx, exception);
  JS_FreeValue(ctx, exception);
  return result;
}

static int goc_qjs_cli_check_worker_rejections(GocQjsCliWorker *worker,
                                                JSContext *ctx) {
  if (goc_qjs_cli_unhandled_rejections(ctx) == 0)
    return 0;
  JSValue reason = goc_qjs_cli_rejection_reason(ctx);
  int result = goc_qjs_cli_save_error(worker, ctx, reason);
  goc_qjs_cli_prefix_error(worker, "unhandled Worker rejection");
  JS_FreeValue(ctx, reason);
  return result < 0 ? -1 : 1;
}

static int goc_qjs_cli_run_jobs(GocQjsCliWorker *worker,
                                GocQjsCliRuntime *state, JSContext *ctx) {
  while (JS_IsJobPending(state->rt)) {
    JSContext *job_ctx = NULL;
    int result = JS_ExecutePendingJob(state->rt, &job_ctx);
    if (result < 0) {
      goc_qjs_cli_capture_worker_exception(worker, job_ctx ? job_ctx : ctx);
      goc_qjs_cli_prefix_error(worker, "pending job");
      return -1;
    }
    if (result == 0)
      break;
  }
  if (state->module_pending) {
    int promise_state = JS_PromiseState(ctx, state->module_promise);
    if (promise_state == JS_PROMISE_FULFILLED) {
      JS_FreeValue(ctx, state->module_promise);
      state->module_promise = JS_UNDEFINED;
      state->module_pending = false;
      state->module_loaded = true;
    } else if (promise_state == JS_PROMISE_REJECTED) {
      JSValue reason = JS_PromiseResult(ctx, state->module_promise);
      goc_qjs_cli_save_error(worker, ctx, reason);
      goc_qjs_cli_prefix_error(worker, "module evaluation");
      JS_FreeValue(ctx, reason);
      return -1;
    }
  }
  if (goc_qjs_cli_check_worker_rejections(worker, ctx) > 0)
    return -1;
  return 0;
}

static GocQjsCliMessage *goc_qjs_cli_encode_message(JSContext *ctx,
                                                    JSValueConst value) {
  size_t data_len = 0;
  JSSABTab sab_tab = {0};
  uint8_t *data = JS_WriteObject2(ctx, &data_len, value,
                                  JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE,
                                  &sab_tab);
  if (!data)
    return NULL;

  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, true);
  if (!state) {
    js_free(ctx, data);
    js_free(ctx, sab_tab.tab);
    return NULL;
  }
  GocQjsCliMessage *message = js_mallocz(ctx, sizeof(*message));
  if (!message)
    goto fail;
  message->owner_rt = state->rt;
  message->data = js_malloc(ctx, data_len ? data_len : 1);
  if (!message->data)
    goto fail;
  for (size_t i = 0; i < data_len; i++)
    message->data[i] = data[i];
  message->data_len = data_len;
  if (sab_tab.len) {
    if (sab_tab.len > (size_t)-1 / sizeof(message->sab_tab[0]))
      goto fail;
    message->sab_tab = js_malloc(ctx, sab_tab.len * sizeof(message->sab_tab[0]));
    if (!message->sab_tab)
      goto fail;
    for (size_t i = 0; i < sab_tab.len; i++) {
      message->sab_tab[i] = sab_tab.tab[i];
      goc_qjs_cli_sab_dup(NULL, sab_tab.tab[i]);
    }
    message->sab_tab_len = sab_tab.len;
  }
  js_free(ctx, data);
  js_free(ctx, sab_tab.tab);
  return message;

fail:
  if (message) {
    if (message->sab_tab) {
      for (size_t i = 0; i < message->sab_tab_len; i++)
        goc_qjs_cli_sab_free(NULL, message->sab_tab[i]);
      js_free(ctx, message->sab_tab);
    }
    js_free(ctx, message->data);
    js_free(ctx, message);
  }
  js_free(ctx, data);
  js_free(ctx, sab_tab.tab);
  if (!JS_HasException(ctx))
    JS_ThrowOutOfMemory(ctx);
  return NULL;
}

static JSValue goc_qjs_cli_worker_post_message(JSContext *ctx,
                                                JSValueConst this_val,
                                                int argc,
                                                JSValueConst *argv) {
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, true);
  if (!state)
    return JS_ThrowInternalError(ctx, "Worker runtime is not initialized");
  GocQjsCliWorker *worker =
      JS_GetOpaque2(ctx, this_val, goc_qjs_cli_worker_class_id);
  if (!worker)
    return JS_EXCEPTION;
  JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
  GocQjsCliMessage *message = goc_qjs_cli_encode_message(ctx, value);
  if (!message)
    return JS_EXCEPTION;
  if (worker->parent_port) {
    if (!worker->peer || worker->peer->closed) {
      goc_qjs_cli_free_message(message);
      return JS_ThrowInternalError(ctx, "Worker parent is no longer available");
    }
    goc_qjs_cli_queue_message(&worker->peer->outgoing_head,
                              &worker->peer->outgoing_tail, message);
  } else {
    if (!worker->child || worker->closed) {
      goc_qjs_cli_free_message(message);
      return JS_ThrowInternalError(ctx, "Worker is no longer available");
    }
    goc_qjs_cli_queue_message(&worker->incoming_head,
                              &worker->incoming_tail, message);
  }
  return JS_UNDEFINED;
}

static JSValue goc_qjs_cli_worker_set_onmessage(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 JSValueConst value) {
  GocQjsCliWorker *worker =
      JS_GetOpaque2(ctx, this_val, goc_qjs_cli_worker_class_id);
  if (!worker)
    return JS_EXCEPTION;
  if (!JS_IsNull(value) && !JS_IsFunction(ctx, value))
    return JS_ThrowTypeError(ctx, "not a function");
  JS_FreeValue(ctx, worker->onmessage);
  worker->onmessage = JS_DupValue(ctx, value);
  return JS_UNDEFINED;
}

static JSValue goc_qjs_cli_worker_get_onmessage(JSContext *ctx,
                                                JSValueConst this_val) {
  GocQjsCliWorker *worker =
      JS_GetOpaque2(ctx, this_val, goc_qjs_cli_worker_class_id);
  if (!worker)
    return JS_EXCEPTION;
  return JS_DupValue(ctx, worker->onmessage);
}

static const JSCFunctionListEntry goc_qjs_cli_worker_proto_funcs[] = {
    JS_CFUNC_DEF("postMessage", 1, goc_qjs_cli_worker_post_message),
    JS_CGETSET_DEF("onmessage", goc_qjs_cli_worker_get_onmessage,
                   goc_qjs_cli_worker_set_onmessage),
};

static void goc_qjs_cli_stop_worker(GocQjsCliWorker *worker) {
  if (!worker->child)
    return;
  GocQjsCliRuntime *child = worker->child;
  worker->child = NULL;
  child->parent_worker = NULL;
  goc_qjs_cli_free_message_queue(worker->incoming_head);
  worker->incoming_head = worker->incoming_tail = NULL;
  goc_qjs_cli_free_message_queue(worker->outgoing_head);
  worker->outgoing_head = worker->outgoing_tail = NULL;
  if (child->ctx) {
    goc_qjs_cli_cleanup_runtime(child);
    goc_qjs_cli_clear_rejections(child->ctx);
    JS_FreeContext(child->ctx);
    child->ctx = NULL;
  }
  JS_FreeRuntime(child->rt);
}

static void goc_qjs_cli_cleanup_runtime(GocQjsCliRuntime *state) {
  if (!state)
    return;
  for (GocQjsCliWorker *worker = state->workers; worker;
       worker = worker->next) {
    goc_qjs_cli_stop_worker(worker);
    JS_FreeValue(state->ctx, worker->onmessage);
    worker->onmessage = JS_NULL;
  }
  if (state->parent_port) {
    JS_FreeValue(state->ctx, state->parent_port->onmessage);
    state->parent_port->onmessage = JS_NULL;
  }
  if (state->module_pending) {
    JS_FreeValue(state->ctx, state->module_promise);
    state->module_promise = JS_UNDEFINED;
    state->module_pending = false;
  }
  while (state->timers) {
    GocQjsCliTimer *timer = state->timers;
    state->timers = timer->next;
    JS_FreeValue(state->ctx, timer->func);
    js_free(state->ctx, timer);
  }
}

void goc_qjs_cli_worker_cleanup(JSContext *ctx) {
  goc_qjs_cli_cleanup_runtime(goc_qjs_cli_find_runtime(JS_GetRuntime(ctx)));
}

static JSValue goc_qjs_cli_new_worker_object(JSContext *ctx,
                                             JSValueConst new_target,
                                             GocQjsCliRuntime *owner,
                                             bool parent_port,
                                             GocQjsCliWorker *peer) {
  JSValue proto = JS_UNDEFINED;
  if (JS_IsUndefined(new_target)) {
    proto = JS_GetClassProto(ctx, goc_qjs_cli_worker_class_id);
  } else {
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  }
  if (JS_IsException(proto))
    return JS_EXCEPTION;

  JSValue object = JS_NewObjectProtoClass(ctx, proto,
                                          goc_qjs_cli_worker_class_id);
  JS_FreeValue(ctx, proto);
  if (JS_IsException(object))
    return JS_EXCEPTION;

  GocQjsCliWorker *worker = js_mallocz(ctx, sizeof(*worker));
  if (!worker) {
    JS_FreeValue(ctx, object);
    return JS_EXCEPTION;
  }
  worker->owner = owner;
  worker->peer = peer;
  worker->parent_port = parent_port;
  worker->onmessage = JS_NULL;
  worker->closed = false;
  if (JS_SetOpaque(object, worker) < 0) {
    js_free(ctx, worker);
    JS_FreeValue(ctx, object);
    return JS_EXCEPTION;
  }
  if (!parent_port) {
    GocQjsCliWorker **tail = &owner->workers;
    while (*tail)
      tail = &(*tail)->next;
    *tail = worker;
  } else {
    owner->parent_port = worker;
  }
  return object;
}

static void goc_qjs_cli_worker_finalizer(JSRuntime *rt, JSValueConst value) {
  GocQjsCliWorker *worker =
      JS_GetOpaque(value, goc_qjs_cli_worker_class_id);
  if (!worker)
    return;
  worker->closed = true;
  if (!worker->parent_port)
    goc_qjs_cli_remove_worker(worker->owner, worker);
  else if (worker->owner->parent_port == worker)
    worker->owner->parent_port = NULL;
  goc_qjs_cli_free_worker(rt, worker);
}

static const JSClassDef goc_qjs_cli_worker_class = {
    "Worker", .finalizer = goc_qjs_cli_worker_finalizer};

static JSValue goc_qjs_cli_worker_ctor(JSContext *ctx,
                                       JSValueConst new_target,
                                       int argc, JSValueConst *argv) {
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, true);
  if (!state)
    return JS_EXCEPTION;
  if (state->is_worker)
    return JS_ThrowTypeError(ctx, "cannot create a worker inside a worker");

  JSAtom basename_atom = JS_GetScriptOrModuleName(ctx, 1);
  if (basename_atom == JS_ATOM_NULL)
    return JS_ThrowTypeError(ctx,
                             "could not determine calling script or module name");
  const char *basename = JS_AtomToCString(ctx, basename_atom);
  JS_FreeAtom(ctx, basename_atom);
  if (!basename)
    return JS_EXCEPTION;
  JSValueConst filename_value = argc > 0 ? argv[0] : JS_UNDEFINED;
  const char *filename = JS_ToCString(ctx, filename_value);
  if (!filename) {
    JS_FreeCString(ctx, basename);
    return JS_EXCEPTION;
  }

  JSValue object = goc_qjs_cli_new_worker_object(ctx, new_target, state,
                                                  false, NULL);
  if (JS_IsException(object)) {
    JS_FreeCString(ctx, basename);
    JS_FreeCString(ctx, filename);
    return JS_EXCEPTION;
  }
  GocQjsCliWorker *worker =
      JS_GetOpaque(object, goc_qjs_cli_worker_class_id);
  worker->basename = js_strdup(ctx, basename);
  worker->filename = js_strdup(ctx, filename);
  JS_FreeCString(ctx, basename);
  JS_FreeCString(ctx, filename);
  if (!worker->basename || !worker->filename) {
    JS_FreeValue(ctx, object);
    return JS_EXCEPTION;
  }
  return object;
}

static const JSCFunctionListEntry goc_qjs_cli_timer_funcs[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, goc_qjs_cli_set_timer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, goc_qjs_cli_set_timer, 1),
    JS_CFUNC_DEF("clearTimeout", 1, goc_qjs_cli_clear_timer),
    JS_CFUNC_DEF("clearInterval", 1, goc_qjs_cli_clear_timer),
};

int goc_qjs_cli_worker_add(JSContext *ctx, JSModuleDef *module) {
  if (!goc_qjs_cli_get_runtime(ctx, true))
    return -1;
  if (JS_AddModuleExport(ctx, module, "Worker") < 0)
    return goc_qjs_cli_registry_failure(ctx);
  if (JS_AddModuleExportList(ctx, module, goc_qjs_cli_timer_funcs,
                             sizeof(goc_qjs_cli_timer_funcs) /
                                 sizeof(goc_qjs_cli_timer_funcs[0])) < 0)
    return goc_qjs_cli_registry_failure(ctx);
  return 0;
}

int goc_qjs_cli_worker_init(JSContext *ctx, JSModuleDef *module) {
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, true);
  if (!state)
    return -1;
  state->ctx = ctx;
  JSRuntime *rt = JS_GetRuntime(ctx);
  if (JS_NewClassID(rt, &goc_qjs_cli_worker_class_id) == 0)
    return goc_qjs_cli_registry_failure(ctx);
  if (!JS_IsRegisteredClass(rt, goc_qjs_cli_worker_class_id) &&
      JS_NewClass(rt, goc_qjs_cli_worker_class_id,
                  &goc_qjs_cli_worker_class) < 0)
    return goc_qjs_cli_registry_failure(ctx);

  JSSharedArrayBufferFunctions sab_funcs = {
      .sab_alloc = goc_qjs_cli_sab_alloc,
      .sab_free = goc_qjs_cli_sab_free,
      .sab_dup = goc_qjs_cli_sab_dup,
      .sab_opaque = rt,
  };
  JS_SetSharedArrayBufferFunctions(rt, &sab_funcs);

  if (JS_SetModuleExportList(ctx, module, goc_qjs_cli_timer_funcs,
                             sizeof(goc_qjs_cli_timer_funcs) /
                                 sizeof(goc_qjs_cli_timer_funcs[0])) < 0)
    return goc_qjs_cli_registry_failure(ctx);

  JSValue proto = JS_NewObject(ctx);
  if (JS_IsException(proto))
    return -1;
  if (JS_SetPropertyFunctionList(ctx, proto, goc_qjs_cli_worker_proto_funcs,
                                 sizeof(goc_qjs_cli_worker_proto_funcs) /
                                     sizeof(goc_qjs_cli_worker_proto_funcs[0])) < 0) {
    JS_FreeValue(ctx, proto);
    return goc_qjs_cli_registry_failure(ctx);
  }

  JSValue constructor = JS_NewCFunction2(
      ctx, (JSCFunction *)goc_qjs_cli_worker_ctor, "Worker", 1,
      JS_CFUNC_constructor, 0);
  if (JS_IsException(constructor)) {
    JS_FreeValue(ctx, proto);
    return -1;
  }
  if (JS_SetConstructor(ctx, constructor, proto) < 0) {
    JS_FreeValue(ctx, constructor);
    JS_FreeValue(ctx, proto);
    return goc_qjs_cli_registry_failure(ctx);
  }
  JS_SetClassProto(ctx, goc_qjs_cli_worker_class_id, proto);

  if (state->is_worker && state->parent_worker) {
    JSValue parent = goc_qjs_cli_new_worker_object(
        ctx, JS_UNDEFINED, state, true, state->parent_worker);
    if (JS_IsException(parent)) {
      JS_FreeValue(ctx, constructor);
      return -1;
    }
    if (JS_SetPropertyStr(ctx, constructor, "parent", parent) < 0) {
      JS_FreeValue(ctx, constructor);
      return goc_qjs_cli_registry_failure(ctx);
    }
  }
  if (JS_SetModuleExport(ctx, module, "Worker", constructor) < 0)
    return goc_qjs_cli_registry_failure(ctx);
  return 0;
}

static JSValue goc_qjs_cli_read_message(JSContext *ctx,
                                        GocQjsCliMessage *message) {
  JSSABTab sab_tab = {0};
  JSValue value = JS_ReadObject2(ctx, message->data, message->data_len,
                                JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE,
                                &sab_tab);
  js_free(ctx, sab_tab.tab);
  goc_qjs_cli_free_message(message);
  return value;
}

static int goc_qjs_cli_deliver_message(JSContext *ctx,
                                       GocQjsCliWorker *receiver,
                                       GocQjsCliMessage *message) {
  JSValue data = goc_qjs_cli_read_message(ctx, message);
  if (JS_IsException(data)) {
    if (!JS_HasException(ctx))
      JS_ThrowInternalError(ctx, "Worker message deserialization failed");
    return -1;
  }
  JSValue event = JS_NewObject(ctx);
  if (JS_IsException(event)) {
    JS_FreeValue(ctx, data);
    if (!JS_HasException(ctx))
      JS_ThrowInternalError(ctx, "Worker event allocation failed");
    return -1;
  }
  if (JS_SetPropertyStr(ctx, event, "data", data) < 0) {
    JS_FreeValue(ctx, event);
    if (!JS_HasException(ctx))
      JS_ThrowInternalError(ctx, "Worker event data assignment failed");
    return -1;
  }
  JSValue handler = JS_DupValue(ctx, receiver->onmessage);
  JSValue result = JS_Call(ctx, handler, JS_UNDEFINED, 1, &event);
  JS_FreeValue(ctx, handler);
  JS_FreeValue(ctx, event);
  if (JS_IsException(result)) {
    if (!JS_HasException(ctx))
      JS_ThrowInternalError(ctx, "Worker onmessage invocation failed");
    return -1;
  }
  JS_FreeValue(ctx, result);
  return 0;
}

static int goc_qjs_cli_save_worker_exception(GocQjsCliWorker *worker,
                                              JSContext *ctx) {
  return goc_qjs_cli_capture_worker_exception(worker, ctx);
}

static int goc_qjs_cli_run_worker_jobs(GocQjsCliWorker *worker) {
  if (!worker->child)
    return 0;
  return goc_qjs_cli_run_jobs(worker, worker->child, worker->child->ctx);
}

static int goc_qjs_cli_start_worker(GocQjsCliWorker *worker) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    worker->error = goc_qjs_cli_strdup_rt(worker->owner->rt,
                                          "JS_NewRuntime failed for Worker");
    worker->started = true;
    return -1;
  }
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    JS_FreeRuntime(rt);
    worker->error = goc_qjs_cli_strdup_rt(worker->owner->rt,
                                          "JS_NewContext failed for Worker");
    worker->started = true;
    return -1;
  }
  GocQjsCliRuntime *child = goc_qjs_cli_get_runtime(ctx, true);
  if (!child) {
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    worker->error = goc_qjs_cli_strdup_rt(
        worker->owner->rt, "could not initialize Worker runtime state");
    worker->started = true;
    return -1;
  }
  child->ctx = ctx;
  child->is_worker = true;
  child->parent_worker = worker;
  worker->child = child;
  worker->started = true;

  if (goc_qjs_cli_install(ctx) < 0) {
    goc_qjs_cli_save_worker_exception(worker, ctx);
    goc_qjs_cli_prefix_error(worker, "host installation");
    return -1;
  }
  JSValue promise = JS_LoadModule(ctx, worker->basename, worker->filename);
  if (JS_IsException(promise)) {
    goc_qjs_cli_save_worker_exception(worker, ctx);
    goc_qjs_cli_prefix_error(worker, "module loading");
    return -1;
  }
  child->module_promise = promise;
  child->module_pending = true;
  return goc_qjs_cli_run_worker_jobs(worker);
}

static int goc_qjs_cli_run_worker_timer(GocQjsCliWorker *worker) {
  GocQjsCliRuntime *child = worker->child;
  int result = goc_qjs_cli_run_due_timer(child, child->ctx);
  if (result < 0) {
    goc_qjs_cli_save_worker_exception(worker, child->ctx);
    goc_qjs_cli_prefix_error(worker, "timer callback");
    return -1;
  }
  if (result > 0 && goc_qjs_cli_run_worker_jobs(worker) < 0)
    return -1;
  return result;
}

static int goc_qjs_cli_run_worker_message(GocQjsCliWorker *worker) {
  GocQjsCliRuntime *child = worker->child;
  GocQjsCliWorker *parent_port = child->parent_port;
  GocQjsCliMessage *message = goc_qjs_cli_pop_message(
      &worker->incoming_head, &worker->incoming_tail);
  int result = goc_qjs_cli_deliver_message(child->ctx, parent_port, message);
  if (result < 0) {
    goc_qjs_cli_save_worker_exception(worker, child->ctx);
    goc_qjs_cli_prefix_error(worker, "message callback");
    return -1;
  }
  return goc_qjs_cli_run_worker_jobs(worker) < 0 ? -1 : 1;
}

static int goc_qjs_cli_deliver_parent_message(JSContext *ctx,
                                               GocQjsCliWorker *worker) {
  GocQjsCliMessage *message = goc_qjs_cli_pop_message(
      &worker->outgoing_head, &worker->outgoing_tail);
  return goc_qjs_cli_deliver_message(ctx, worker, message) < 0 ? -1 : 1;
}

/* Absolute CLOCK_MONOTONIC nanoseconds, or -1 if no timer is scheduled. */
int64_t goc_qjs_cli_worker_next_deadline(JSRuntime *rt) {
  GocQjsCliRuntime *state = goc_qjs_cli_find_runtime(rt);
  if (!state)
    return -1;
  int64_t earliest = INT64_MAX;
  bool found = false;
  int64_t deadline;
  if (goc_qjs_cli_next_timer(state, &deadline)) {
    earliest = deadline;
    found = true;
  }
  for (GocQjsCliWorker *worker = state->workers; worker;
       worker = worker->next) {
    if (worker->child &&
        goc_qjs_cli_next_timer(worker->child, &deadline) &&
        (!found || deadline < earliest)) {
      earliest = deadline;
      found = true;
    }
  }
  if (!found)
    return -1;
  return earliest;
}

/* True while the qjs:os loop has timers, startup work, or a live message port. */
int goc_qjs_cli_workers_pending(JSRuntime *rt) {
  GocQjsCliRuntime *state = goc_qjs_cli_find_runtime(rt);
  if (!state)
    return 0;
  if (state->timers)
    return 1;
  for (GocQjsCliWorker *worker = state->workers; worker;
       worker = worker->next) {
    if (worker->error || !worker->started)
      return 1;
    if (!worker->child)
      continue;
    if (!JS_IsNull(worker->onmessage) || worker->child->module_pending ||
        JS_IsJobPending(worker->child->rt))
      return 1;
    if (worker->incoming_head &&
        worker->child->parent_port &&
        !JS_IsNull(worker->child->parent_port->onmessage))
      return 1;
  }
  return 0;
}

/* Run one callback/message at a time, waiting through Go when only timers are
 * pending. The parent CLI drains QuickJS jobs between calls to this function. */
int goc_qjs_cli_dispatch_workers(JSContext *ctx) {
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, false);
  if (!state)
    return 0;

  for (;;) {
    for (GocQjsCliWorker *worker = state->workers; worker;
         worker = worker->next) {
      if (worker->error) {
        JS_ThrowInternalError(ctx, "Worker failed: %s", worker->error);
        return -1;
      }
    }

    int timer_result = goc_qjs_cli_run_due_timer(state, ctx);
    if (timer_result < 0)
      return -1;
    if (timer_result > 0)
      return 1;

    for (GocQjsCliWorker *worker = state->workers; worker;
         worker = worker->next) {
      if (worker->outgoing_head && !JS_IsNull(worker->onmessage))
        return goc_qjs_cli_deliver_parent_message(ctx, worker);
    }

    for (GocQjsCliWorker *worker = state->workers; worker;
         worker = worker->next) {
      if (worker->closed)
        continue;
      if (!worker->started) {
        int result = goc_qjs_cli_start_worker(worker);
        if (worker->error) {
          JS_ThrowInternalError(ctx, "Worker failed: %s",
                                worker->error ? worker->error : "unknown error");
          return -1;
        }
        if (result < 0) {
          JS_ThrowInternalError(ctx, "Worker failed during initialization");
          return -1;
        }
        return 1;
      }
      if (!worker->child)
        continue;
      GocQjsCliRuntime *child = worker->child;
      if (JS_IsJobPending(child->rt)) {
        if (goc_qjs_cli_run_worker_jobs(worker) < 0) {
          JS_ThrowInternalError(ctx, "Worker failed: %s",
                                worker->error ? worker->error : "pending job");
          return -1;
        }
        return 1;
      }
      if (child->module_pending &&
          goc_qjs_cli_has_due_timer(child, goc_qjs_cli_now_ns())) {
        if (goc_qjs_cli_run_worker_timer(worker) < 0) {
          JS_ThrowInternalError(ctx, "Worker failed: %s",
                                worker->error ? worker->error : "timer callback");
          return -1;
        }
        return 1;
      }
      if (child->parent_port && !JS_IsNull(child->parent_port->onmessage) &&
          worker->incoming_head) {
        if (goc_qjs_cli_run_worker_message(worker) < 0) {
          JS_ThrowInternalError(ctx, "Worker failed: %s",
                                worker->error ? worker->error : "message handler");
          return -1;
        }
        return 1;
      }
      if (child->module_loaded &&
          goc_qjs_cli_has_due_timer(child, goc_qjs_cli_now_ns())) {
        if (goc_qjs_cli_run_worker_timer(worker) < 0) {
          JS_ThrowInternalError(ctx, "Worker failed: %s",
                                worker->error ? worker->error : "timer callback");
          return -1;
        }
        return 1;
      }
    }

    if (!goc_qjs_cli_workers_pending(state->rt))
      return 0;

    int64_t earliest = INT64_MAX;
    bool have_deadline = false;
    if (goc_qjs_cli_next_timer(state, &earliest))
      have_deadline = true;
    for (GocQjsCliWorker *worker = state->workers; worker;
         worker = worker->next) {
      if (worker->child) {
        int64_t deadline;
        if (goc_qjs_cli_next_timer(worker->child, &deadline) &&
            (!have_deadline || deadline < earliest)) {
          earliest = deadline;
          have_deadline = true;
        }
      }
    }
    int64_t now = goc_qjs_cli_now_ns();
    int64_t wait_ns = have_deadline && earliest > now ? earliest - now :
                      have_deadline ? 0 : INT64_MAX;
    goc_qjs_cli_wait_ns(wait_ns);
  }
}

