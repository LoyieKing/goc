/* Part of cli_worker.c, which includes it; not compiled on its own.
 * Worker objects and messages, os.setReadHandler/setWriteHandler, the qjs:os timer and worker exports. */

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
  while (state->rw) {
    GocQjsCliRW *rh = state->rw;
    state->rw = rh->next;
    JS_FreeValue(state->ctx, rh->funcs[0]);
    JS_FreeValue(state->ctx, rh->funcs[1]);
    js_free(state->ctx, rh);
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

/* Linux poll(2) bits. Same values as the kernel and Go's syscall.POLLIN. */
#define GOC_POLLIN 0x001
#define GOC_POLLOUT 0x004
#define GOC_POLLERR 0x008
#define GOC_POLLHUP 0x010
#define GOC_POLLNVAL 0x020

static GocQjsCliRW *goc_qjs_cli_find_rw(GocQjsCliRuntime *state, int fd) {
  for (GocQjsCliRW *rh = state->rw; rh; rh = rh->next) {
    if (rh->fd == fd)
      return rh;
  }
  return NULL;
}

static void goc_qjs_cli_free_rw(JSContext *ctx, GocQjsCliRuntime *state,
                                GocQjsCliRW *rh) {
  GocQjsCliRW **link = &state->rw;
  while (*link && *link != rh)
    link = &(*link)->next;
  if (*link == rh)
    *link = rh->next;
  JS_FreeValue(ctx, rh->funcs[0]);
  JS_FreeValue(ctx, rh->funcs[1]);
  js_free(ctx, rh);
}

/* os.setReadHandler / os.setWriteHandler. magic 0 is read, 1 is write.
   null removes that direction, matching quickjs-libc.c js_os_setReadHandler. */
static JSValue goc_qjs_cli_set_rw_handler(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic) {
  (void)this_val;
  GocQjsCliRuntime *state = goc_qjs_cli_get_runtime(ctx, true);
  if (!state)
    return JS_EXCEPTION;
  int32_t fd;
  JSValueConst func = argc > 1 ? argv[1] : JS_UNDEFINED;
  if (argc < 1 || JS_ToInt32(ctx, &fd, argv[0]))
    return JS_EXCEPTION;
  GocQjsCliRW *rh = goc_qjs_cli_find_rw(state, fd);
  if (JS_IsNull(func)) {
    if (!rh)
      return JS_UNDEFINED;
    JS_FreeValue(ctx, rh->funcs[magic]);
    rh->funcs[magic] = JS_NULL;
    if (JS_IsNull(rh->funcs[0]) && JS_IsNull(rh->funcs[1]))
      goc_qjs_cli_free_rw(ctx, state, rh);
    return JS_UNDEFINED;
  }
  if (!JS_IsFunction(ctx, func))
    return JS_ThrowTypeError(ctx, "not a function");
  if (!rh) {
    rh = js_malloc(ctx, sizeof(*rh));
    if (!rh)
      return JS_EXCEPTION;
    rh->fd = fd;
    rh->poll_index = -1;
    rh->funcs[0] = JS_NULL;
    rh->funcs[1] = JS_NULL;
    rh->next = NULL;
    GocQjsCliRW **tail = &state->rw;
    while (*tail)
      tail = &(*tail)->next;
    *tail = rh;
  }
  JS_FreeValue(ctx, rh->funcs[magic]);
  rh->funcs[magic] = JS_DupValue(ctx, func);
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry goc_qjs_cli_timer_funcs[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, goc_qjs_cli_set_timer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, goc_qjs_cli_set_timer, 1),
    JS_CFUNC_DEF("clearTimeout", 1, goc_qjs_cli_clear_timer),
    JS_CFUNC_DEF("clearInterval", 1, goc_qjs_cli_clear_timer),
    JS_CFUNC_MAGIC_DEF("setReadHandler", 2, goc_qjs_cli_set_rw_handler, 0),
    JS_CFUNC_MAGIC_DEF("setWriteHandler", 2, goc_qjs_cli_set_rw_handler, 1),
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
