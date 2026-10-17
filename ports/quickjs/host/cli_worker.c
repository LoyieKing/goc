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
#include "../../../include/goc.h"
#ifdef GOC_QJS_BELLARD
#include "../../../third_party/quickjs-bellard/quickjs.h"
#include "bellard_compat.h"
#else
#include "../../../third_party/quickjs-ng/quickjs.h"
#endif

extern int goc_qjs_cli_install(JSContext *ctx);
extern int goc_qjs_cli_unhandled_rejections(JSContext *ctx);
extern JSValue goc_qjs_cli_rejection_reason(JSContext *ctx);
extern void goc_qjs_cli_clear_rejections(JSContext *ctx);

typedef struct GocQjsCliMessage GocQjsCliMessage;
typedef struct GocQjsCliTimer GocQjsCliTimer;
typedef struct GocQjsCliRW GocQjsCliRW;
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

/* One fd, same as Bellard's JSOSRWHandler: funcs[0] is read, funcs[1] is write.
   An empty slot is JS_NULL. Both empty means the node is not on the list. */
struct GocQjsCliRW {
  int fd;
  int poll_index;
  JSValue funcs[2];
  struct GocQjsCliRW *next;
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
  GocQjsCliRW *rw;
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
__go_extern("main.gocQjsCliWorkerSleep")
void goc_x_QjsCliWorkerSleep(int64_t nanoseconds);

static void goc_qjs_cli_wait_ns(int64_t nanoseconds) {
  if (nanoseconds <= 0)
    return;
  goc_x_QjsCliWorkerSleep(nanoseconds);
}

/* Same layout as syscall.PollFd and the Go bridge in ports/quickjs/cli/worker.go. */
typedef struct {
  int32_t fd;
  int16_t events;
  int16_t revents;
} GocQjsCliPollFd;

typedef struct {
  GocQjsCliPollFd *fds;
  int32_t count;
  int32_t timeout_ms;
  int32_t result;
  int32_t pad;
} GocQjsCliPollRequest;

_Static_assert(sizeof(GocQjsCliPollFd) == 8, "pollfd layout");
_Static_assert(sizeof(GocQjsCliPollRequest) == 24, "poll request layout");
_Static_assert(offsetof(GocQjsCliPollRequest, result) == 16, "poll result");

__go_extern("main.gocQjsCliPoll")
void goc_x_QjsCliPoll(GocQjsCliPollRequest *request);

static void goc_qjs_cli_call_go_poll(GocQjsCliPollRequest *request) {
  goc_x_QjsCliPoll(request);
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

/* The rest of this translation unit, split by topic: */
#include "cli_worker_objects.inc"
#include "cli_worker_loop.inc"
