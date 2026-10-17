/* Part of cli_worker.c, which includes it; not compiled on its own.
 * The worker thread loop, message delivery and the qjs:os event dispatch. */

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

/* True while the qjs:os loop has timers, fd handlers, startup work, or a
 * live message port. */
int goc_qjs_cli_workers_pending(JSRuntime *rt) {
  GocQjsCliRuntime *state = goc_qjs_cli_find_runtime(rt);
  if (!state)
    return 0;
  if (state->timers || state->rw)
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

/* deadline_ns < 0 waits until an fd is ready. Otherwise the wait also ends at
   that monotonic time, so os.setTimeout still runs. One handler is called,
   matching js_os_poll, because the handler may edit the list. */
static int goc_qjs_cli_poll_rw(GocQjsCliRuntime *state, JSContext *ctx,
                               int64_t deadline_ns) {
  int count = 0;
  for (GocQjsCliRW *rh = state->rw; rh; rh = rh->next) {
    if (!JS_IsNull(rh->funcs[0]) || !JS_IsNull(rh->funcs[1]))
      count++;
  }
  if (count == 0)
    return 0;

  GocQjsCliPollFd *fds = js_malloc(ctx, (size_t)count * sizeof(*fds));
  if (!fds)
    return -1;
  int index = 0;
  for (GocQjsCliRW *rh = state->rw; rh; rh = rh->next) {
    int events = 0;
    if (!JS_IsNull(rh->funcs[0]))
      events |= GOC_POLLIN;
    if (!JS_IsNull(rh->funcs[1]))
      events |= GOC_POLLOUT;
    if (!events) {
      rh->poll_index = -1;
      continue;
    }
    rh->poll_index = index;
    fds[index].fd = rh->fd;
    fds[index].events = (int16_t)events;
    fds[index].revents = 0;
    index++;
  }

  int32_t timeout_ms = -1;
  if (deadline_ns >= 0) {
    int64_t now = goc_qjs_cli_now_ns();
    if (deadline_ns <= now) {
      timeout_ms = 0;
    } else {
      int64_t ms = (deadline_ns - now + 999999) / 1000000;
      if (ms < 1)
        ms = 1;
      if (ms > INT32_MAX)
        ms = INT32_MAX;
      timeout_ms = (int32_t)ms;
    }
  }

  GocQjsCliPollRequest request = {
      .fds = fds,
      .count = index,
      .timeout_ms = timeout_ms,
  };
  goc_qjs_cli_call_go_poll(&request);
  int ready = request.result;
  if (ready < 0) {
    js_free(ctx, fds);
    JS_ThrowInternalError(ctx, "os poll failed: %d", -ready);
    return -1;
  }
  if (ready == 0) {
    js_free(ctx, fds);
    return 0;
  }

  JSValue func = JS_UNDEFINED;
  bool found = false;
  for (GocQjsCliRW *rh = state->rw; rh; rh = rh->next) {
    if (rh->poll_index < 0)
      continue;
    int16_t revents = fds[rh->poll_index].revents;
    int magic = -1;
    if (!JS_IsNull(rh->funcs[0]) &&
        (revents & (GOC_POLLIN | GOC_POLLERR | GOC_POLLHUP | GOC_POLLNVAL)))
      magic = 0;
    else if (!JS_IsNull(rh->funcs[1]) &&
             (revents & (GOC_POLLOUT | GOC_POLLERR | GOC_POLLHUP | GOC_POLLNVAL)))
      magic = 1;
    if (magic < 0)
      continue;
    func = JS_DupValue(ctx, rh->funcs[magic]);
    found = true;
    break;
  }
  js_free(ctx, fds);
  if (!found)
    return 0;
  JSValue result = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
  JS_FreeValue(ctx, func);
  if (JS_IsException(result))
    return -1;
  JS_FreeValue(ctx, result);
  return 1;
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
    if (state->rw) {
      int rw = goc_qjs_cli_poll_rw(state, ctx, have_deadline ? earliest : -1);
      if (rw != 0)
        return rw;
      continue;
    }

    int64_t now = goc_qjs_cli_now_ns();
    int64_t wait_ns = have_deadline && earliest > now ? earliest - now :
                      have_deadline ? 0 : INT64_MAX;
    goc_qjs_cli_wait_ns(wait_ns);
  }
}
