/* Part of cli_host.c, which includes it; not compiled on its own.
 * Print, gc, evalScript, the std/bjson/os module inits, unhandled rejections, interrupts and install. */

static long goc_qjs_cli_write_syscall(const char *buf, unsigned long len) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(1L), "D"(1L), "S"(buf), "d"(len)
                   : "rcx", "r11", "memory");
  return result;
}

static int goc_qjs_cli_write_all(const char *buf, size_t len) {
  while (len != 0) {
    /* Linux caps a single write transfer at 0x7ffff000 bytes. */
    unsigned long chunk = len > 0x7ffff000UL ? 0x7ffff000UL : (unsigned long)len;
    long written = goc_qjs_cli_write_syscall(buf, chunk);
    if (written == -4) /* EINTR */
      continue;
    if (written <= 0)
      return -1;
    buf += (unsigned long)written;
    len -= (size_t)written;
  }
  return 0;
}

static JSValue goc_qjs_cli_print(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  (void)this_val;
  for (int i = 0; i < argc; i++) {
    if (i != 0 && goc_qjs_cli_write_all(" ", 1) < 0)
      return JS_ThrowInternalError(ctx, "write to stdout failed");

    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
    if (!str && JS_IsObject(argv[i])) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      JSValue display = JS_ToObjectString(ctx, argv[i]);
      if (!JS_IsException(display))
        str = JS_ToCStringLen(ctx, &len, display);
      JS_FreeValue(ctx, display);
    }
    if (!str) {
      /* Upstream qjs print uses a visible marker and consumes conversion
       * exceptions; printing a module namespace must not reject its job. */
      JS_FreeValue(ctx, JS_GetException(ctx));
      if (goc_qjs_cli_write_all("<exception>", 11) < 0)
        return JS_ThrowInternalError(ctx, "write to stdout failed");
      continue;
    }

    int write_error = goc_qjs_cli_write_all(str, len);
    JS_FreeCString(ctx, str);
    if (write_error < 0)
      return JS_ThrowInternalError(ctx, "write to stdout failed");
  }

  if (goc_qjs_cli_write_all("\n", 1) < 0)
    return JS_ThrowInternalError(ctx, "write to stdout failed");
  return JS_UNDEFINED;
}

static JSValue goc_qjs_cli_gc(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

static int goc_qjs_cli_bool_option(JSContext *ctx, JSValueConst options,
                                    const char *name, int *value) {
  JSValue property = JS_GetPropertyStr(ctx, options, name);
  if (JS_IsException(property))
    return -1;
  if (!JS_IsUndefined(property)) {
    int converted = JS_ToBool(ctx, property);
    if (converted < 0) {
      JS_FreeValue(ctx, property);
      return -1;
    }
    *value = converted;
  }
  JS_FreeValue(ctx, property);
  return 0;
}

static JSValue goc_qjs_cli_eval_script(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  (void)this_val;
  int barrier = 0, eval_function = 0, eval_module = 0;
  int compile_only = 0, compile_module = 0, async = 0;
  JSValueConst input = argc ? argv[0] : JS_UNDEFINED;
  if (argc >= 2 &&
      (goc_qjs_cli_bool_option(ctx, argv[1], "backtrace_barrier", &barrier) < 0 ||
       goc_qjs_cli_bool_option(ctx, argv[1], "eval_function", &eval_function) < 0 ||
       goc_qjs_cli_bool_option(ctx, argv[1], "eval_module", &eval_module) < 0 ||
       goc_qjs_cli_bool_option(ctx, argv[1], "compile_only", &compile_only) < 0 ||
       goc_qjs_cli_bool_option(ctx, argv[1], "compile_module", &compile_module) < 0 ||
       goc_qjs_cli_bool_option(ctx, argv[1], "async", &async) < 0))
    return JS_EXCEPTION;

  if (eval_module) {
    if (JS_VALUE_GET_TAG(input) != JS_TAG_MODULE)
      return JS_ThrowTypeError(ctx, "not a module");
    if (JS_ResolveModule(ctx, input) < 0 ||
        goc_qjs_cli_set_import_meta(ctx, input) < 0)
      return JS_EXCEPTION;
    return JS_EvalFunction(ctx, JS_DupValue(ctx, input));
  }
  if (eval_function)
    return JS_EvalFunction(ctx, JS_DupValue(ctx, input));

  size_t length;
  const char *source = JS_ToCStringLen(ctx, &length, input);
  if (!source)
    return JS_EXCEPTION;
  int flags = compile_module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
  if (barrier)
    flags |= JS_EVAL_FLAG_BACKTRACE_BARRIER;
  if (compile_only)
    flags |= JS_EVAL_FLAG_COMPILE_ONLY;
  if (async)
    flags |= JS_EVAL_FLAG_ASYNC;
  JSValue result = JS_Eval(ctx, source, length, "<evalScript>", flags);
  JS_FreeCString(ctx, source);
  return result;
}

static int goc_qjs_cli_std_init(JSContext *ctx, JSModuleDef *module) {
  JSValue gc = JS_NewCFunction(ctx, goc_qjs_cli_gc, "gc", 0);
  if (JS_IsException(gc))
    return -1;
  if (JS_SetModuleExport(ctx, module, "gc", gc) < 0)
    return -1;
  JSValue eval_script = JS_NewCFunction(ctx, goc_qjs_cli_eval_script,
                                         "evalScript", 1);
  if (JS_IsException(eval_script))
    return -1;
  if (JS_SetModuleExport(ctx, module, "evalScript", eval_script) < 0)
    return -1;
  return goc_qjs_cli_std_extra_init(ctx, module);
}

static JSValue goc_qjs_cli_bjson_read(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  uint64_t offset, length;
  int32_t flags;
  if (JS_ToIndex(ctx, &offset, argv[1]) < 0 ||
      JS_ToIndex(ctx, &length, argv[2]) < 0 ||
      JS_ToInt32(ctx, &flags, argv[3]) < 0)
    return JS_EXCEPTION;
  flags &= ~JS_READ_OBJ_SAB;
  size_t size;
  uint8_t *data = JS_GetArrayBuffer(ctx, &size, argv[0]);
  if (!data)
    return JS_EXCEPTION;
  if (offset > size || length > size - offset)
    return JS_ThrowRangeError(ctx, "array buffer overflow");
  return JS_ReadObject(ctx, data + offset, (size_t)length, flags);
}

static JSValue goc_qjs_cli_bjson_write(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  int32_t flags;
  if (JS_ToInt32(ctx, &flags, argv[1]) < 0)
    return JS_EXCEPTION;
  flags &= ~JS_WRITE_OBJ_SAB;
  size_t size;
  uint8_t *data = JS_WriteObject(ctx, &size, argv[0], flags);
  if (!data)
    return JS_EXCEPTION;
  JSValue result = JS_NewArrayBufferCopy(ctx, data, size);
  js_free(ctx, data);
  return result;
}

static const JSCFunctionListEntry goc_qjs_cli_bjson_funcs[] = {
  JS_CFUNC_DEF("read", 4, goc_qjs_cli_bjson_read),
  JS_CFUNC_DEF("write", 2, goc_qjs_cli_bjson_write),
#define BJSON_FLAG(name) JS_PROP_INT32_DEF(#name, JS_##name, JS_PROP_CONFIGURABLE)
  BJSON_FLAG(READ_OBJ_BYTECODE),
  BJSON_FLAG(READ_OBJ_REFERENCE),
  BJSON_FLAG(WRITE_OBJ_BYTECODE),
  BJSON_FLAG(WRITE_OBJ_REFERENCE),
#ifdef JS_WRITE_OBJ_STRIP_DEBUG /* quickjs-ng only */
  BJSON_FLAG(WRITE_OBJ_STRIP_DEBUG),
  BJSON_FLAG(WRITE_OBJ_STRIP_SOURCE),
#endif
#undef BJSON_FLAG
};

static int goc_qjs_cli_bjson_init(JSContext *ctx, JSModuleDef *module) {
  return JS_SetModuleExportList(ctx, module, goc_qjs_cli_bjson_funcs,
                                sizeof(goc_qjs_cli_bjson_funcs) /
                                sizeof(goc_qjs_cli_bjson_funcs[0]));
}

static JSValue goc_qjs_cli_os_now(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  struct { long sec, nsec; } ts;
  long result;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(228L), "D"(1L), "S"(&ts)
                   : "rcx", "r11", "memory");
  if (result < 0)
    return JS_ThrowInternalError(ctx, "clock_gettime failed: %ld", -result);
  return JS_NewInt64(ctx, (int64_t)ts.sec * 1000000 + ts.nsec / 1000);
}

#ifdef GOC_QJS_BELLARD
/* ng provides globalThis.performance inside the engine; Bellard's qjs adds it
   in js_std_add_helpers (quickjs-libc.c), which this host does not use. Same
   semantics as Bellard: CLOCK_MONOTONIC in milliseconds, as a float. */
static JSValue goc_qjs_cli_perf_now(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  struct { long sec, nsec; } ts;
  long result;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(228L), "D"(1L), "S"(&ts)
                   : "rcx", "r11", "memory");
  if (result < 0)
    return JS_ThrowInternalError(ctx, "clock_gettime failed: %ld", -result);
  return JS_NewFloat64(ctx, (double)((int64_t)ts.sec * 1000000000 + ts.nsec) / 1e6);
}
#endif

static int goc_qjs_cli_os_init(JSContext *ctx, JSModuleDef *module) {
  JSValue platform = JS_NewStringLen(ctx, "linux", 5);
  if (JS_IsException(platform))
    return -1;
  if (JS_SetModuleExport(ctx, module, "platform", platform) < 0)
    return -1;
  JSValue now = JS_NewCFunction(ctx, goc_qjs_cli_os_now, "now", 0);
  if (JS_IsException(now))
    return -1;
  if (JS_SetModuleExport(ctx, module, "now", now) < 0 ||
      goc_qjs_cli_os_extra_init(ctx, module) < 0)
    return -1;
  return goc_qjs_cli_worker_init(ctx, module);
}

static JSValue goc_qjs_cli_get_string_kind(JSContext *ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1)
    return JS_NewInt32(ctx, -1);
#ifdef GOC_QJS_BELLARD
  return JS_NewInt32(ctx, -1); /* ng-internal js_std_cmd; not in Bellard */
#else
  return JS_NewInt32(ctx, (int32_t)js_std_cmd(3, ctx, &argv[0]));
#endif
}

typedef struct GocQjsRejection {
  JSValue promise, reason;
  struct GocQjsRejection *next;
} GocQjsRejection;
typedef struct GocQjsRejectionState {
  JSRuntime *runtime;
  GocQjsRejection *rejections;
  int count;
  struct GocQjsRejectionState *next;
} GocQjsRejectionState;
static GocQjsRejectionState *goc_qjs_cli_rejection_states;

static GocQjsRejectionState *goc_qjs_cli_state(JSContext *ctx) {
  JSRuntime *runtime = JS_GetRuntime(ctx);
  for (GocQjsRejectionState *state = goc_qjs_cli_rejection_states;
       state; state = state->next) {
    if (state->runtime == runtime)
      return state;
  }
  return NULL;
}

static void goc_qjs_cli_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                            JSValueConst reason,
                                            GOC_QJS_TRACKER_BOOL is_handled,
                                            void *opaque) {
  GocQjsRejectionState *state = opaque;
  if (is_handled) {
    GocQjsRejection **link = &state->rejections;
    while (*link && JS_VALUE_GET_PTR((*link)->promise) != JS_VALUE_GET_PTR(promise))
      link = &(*link)->next;
    if (*link) {
      GocQjsRejection *node = *link;
      *link = node->next;
      JS_FreeValue(ctx, node->promise);
      JS_FreeValue(ctx, node->reason);
      js_free(ctx, node);
      --state->count;
    }
  } else {
    ++state->count;
    GocQjsRejection *node = js_malloc(ctx, sizeof(*node));
    if (!node)
      return;
    node->promise = JS_DupValue(ctx, promise);
    node->reason = JS_DupValue(ctx, reason);
    node->next = state->rejections;
    state->rejections = node;
  }
}

static int goc_qjs_cli_track_rejections(JSContext *ctx) {
  if (goc_qjs_cli_state(ctx))
    return 0;
  GocQjsRejectionState *state = js_mallocz(ctx, sizeof(*state));
  if (!state)
    return -1;
  state->runtime = JS_GetRuntime(ctx);
  state->next = goc_qjs_cli_rejection_states;
  goc_qjs_cli_rejection_states = state;
  JS_SetHostPromiseRejectionTracker(state->runtime,
                                    goc_qjs_cli_rejection_tracker, state);
  return 0;
}

int goc_qjs_cli_unhandled_rejections(JSContext *ctx) {
  GocQjsRejectionState *state = goc_qjs_cli_state(ctx);
  return state ? state->count : 0;
}

JSValue goc_qjs_cli_rejection_reason(JSContext *ctx) {
  GocQjsRejectionState *state = goc_qjs_cli_state(ctx);
  return state && state->rejections ?
      JS_DupValue(ctx, state->rejections->reason) : JS_UNDEFINED;
}

void goc_qjs_cli_clear_rejections(JSContext *ctx) {
  GocQjsRejectionState **link = &goc_qjs_cli_rejection_states;
  while (*link && (*link)->runtime != JS_GetRuntime(ctx))
    link = &(*link)->next;
  if (!*link)
    return;
  GocQjsRejectionState *state = *link;
  JS_SetHostPromiseRejectionTracker(state->runtime, NULL, NULL);
  *link = state->next;
  while (state->rejections) {
    GocQjsRejection *node = state->rejections;
    state->rejections = node->next;
    JS_FreeValue(ctx, node->promise);
    JS_FreeValue(ctx, node->reason);
    js_free(ctx, node);
  }
  js_free(ctx, state);
}

static int goc_qjs_cli_interrupt_countdown;

static int goc_qjs_cli_interrupt(JSRuntime *rt, void *opaque) {
  (void)rt;
  (void)opaque;
  return --goc_qjs_cli_interrupt_countdown <= 0;
}

void goc_qjs_cli_enable_interrupt(JSRuntime *rt, int countdown) {
  goc_qjs_cli_interrupt_countdown = countdown;
  JS_SetInterruptHandler(rt, goc_qjs_cli_interrupt, NULL);
}

/* qjs.c treats a file as a module when the name ends in .mjs or
   JS_DetectModule says so. ng returns bool, Bellard returns int. */
int goc_qjs_cli_detect_module(const char *input, size_t len) {
  return JS_DetectModule(input, len) ? 1 : 0;
}

/* Same prelude as qjs.c when argv contains --std. */
int goc_qjs_cli_load_std(JSContext *ctx) {
#ifdef GOC_QJS_BELLARD
  static const char source[] =
      "import * as std from 'std';\n"
      "import * as os from 'os';\n"
      "globalThis.std = std;\n"
      "globalThis.os = os;\n";
#else
  static const char source[] =
      "import * as bjson from 'qjs:bjson';\n"
      "import * as std from 'qjs:std';\n"
      "import * as os from 'qjs:os';\n"
      "globalThis.bjson = bjson;\n"
      "globalThis.std = std;\n"
      "globalThis.os = os;\n";
#endif
  JSValue compiled = JS_Eval(ctx, source, sizeof(source) - 1, "<input>",
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled))
    return -1;
  if (goc_qjs_cli_set_import_meta(ctx, compiled) < 0) {
    JS_FreeValue(ctx, compiled);
    return -1;
  }
  JSValue result = JS_EvalFunction(ctx, compiled);
  if (JS_IsException(result))
    return -1;
  JSRuntime *rt = JS_GetRuntime(ctx);
  for (;;) {
    JSContext *job_ctx = NULL;
    int pending = JS_ExecutePendingJob(rt, &job_ctx);
    if (pending < 0) {
      JS_FreeValue(ctx, result);
      return -1;
    }
    if (pending == 0)
      break;
  }
  if (JS_PromiseState(ctx, result) != JS_PROMISE_FULFILLED) {
    if (JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
      JSValue reason = JS_PromiseResult(ctx, result);
      JS_FreeValue(ctx, result);
      JS_Throw(ctx, reason);
    } else {
      JS_FreeValue(ctx, result);
      JS_ThrowInternalError(ctx, "--std module evaluation did not finish");
    }
    return -1;
  }
  JS_FreeValue(ctx, result);
  return 0;
}

int goc_qjs_cli_install(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  if (JS_IsException(global))
    return -1;

  JSValue print = JS_NewCFunction(ctx, goc_qjs_cli_print, "print", 1);
  if (JS_IsException(print)) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  if (JS_SetPropertyStr(ctx, global, "print", print) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }
#ifdef GOC_QJS_BELLARD
  {
    JSValue perf = JS_NewObject(ctx);
    if (JS_IsException(perf)) {
      JS_FreeValue(ctx, global);
      return -1;
    }
    JSValue now = JS_NewCFunction(ctx, goc_qjs_cli_perf_now, "now", 0);
    if (JS_IsException(now) || JS_SetPropertyStr(ctx, perf, "now", now) < 0 ||
        JS_SetPropertyStr(ctx, global, "performance", perf) < 0) {
      JS_FreeValue(ctx, global);
      return -1;
    }
  }
#endif

  JSValue gc = JS_NewCFunction(ctx, goc_qjs_cli_gc, "gc", 0);
  if (JS_IsException(gc)) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  if (JS_SetPropertyStr(ctx, global, "gc", gc) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }

  /* Bellard registers "std"/"os". quickjs-ng registers "qjs:std"/"qjs:os".
     --std imports those exact names; see goc_qjs_cli_load_std. */
#ifdef GOC_QJS_BELLARD
  const char *std_name = "std";
  const char *os_name = "os";
#else
  const char *std_name = "qjs:std";
  const char *os_name = "qjs:os";
#endif
  JSModuleDef *std = JS_NewCModule(ctx, std_name, goc_qjs_cli_std_init);
  if (!std || JS_AddModuleExport(ctx, std, "gc") < 0 ||
      JS_AddModuleExport(ctx, std, "evalScript") < 0 ||
      goc_qjs_cli_std_extra_add(ctx, std) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  JSModuleDef *os = JS_NewCModule(ctx, os_name, goc_qjs_cli_os_init);
  if (!os || JS_AddModuleExport(ctx, os, "platform") < 0 ||
      JS_AddModuleExport(ctx, os, "now") < 0 ||
      goc_qjs_cli_os_extra_add(ctx, os) < 0 ||
      goc_qjs_cli_worker_add(ctx, os) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  JSModuleDef *bjson = JS_NewCModule(ctx, "qjs:bjson", goc_qjs_cli_bjson_init);
  if (!bjson || JS_AddModuleExportList(ctx, bjson, goc_qjs_cli_bjson_funcs,
                                       sizeof(goc_qjs_cli_bjson_funcs) /
                                       sizeof(goc_qjs_cli_bjson_funcs[0])) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  JSValue qjs = JS_NewObject(ctx);
  if (JS_IsException(qjs)) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  JSValue kind = JS_NewCFunction(ctx, goc_qjs_cli_get_string_kind,
                                 "getStringKind", 1);
  if (JS_IsException(kind)) {
    JS_FreeValue(ctx, qjs);
    JS_FreeValue(ctx, global);
    return -1;
  }
  if (JS_SetPropertyStr(ctx, qjs, "getStringKind", kind) < 0) {
    JS_FreeValue(ctx, qjs);
    JS_FreeValue(ctx, global);
    return -1;
  }
  if (JS_SetPropertyStr(ctx, global, "qjs", qjs) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }

  JS_SetModuleLoaderFunc2(JS_GetRuntime(ctx), goc_qjs_cli_normalize,
                          goc_qjs_cli_module_loader,
                          goc_qjs_cli_check_attrs, NULL);
  if (goc_qjs_cli_track_rejections(ctx) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  JS_FreeValue(ctx, global);
  return 0;
}
