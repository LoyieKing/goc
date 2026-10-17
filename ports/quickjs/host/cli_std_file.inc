/* Part of cli_std_os.c, which includes it; not compiled on its own.
 * qjs:std FILE objects and the std module tables. */

static int goc_qjs_cli_file_close(GocQjsCliFile *file, bool wait_popen) {
  int32_t result = 0;
  if (file->fd >= 0 && !file->stdio) {
    long ret = goc_qjs_cli_syscall1(3, file->fd);
    result = goc_qjs_cli_sys_error(ret);
  }
  file->fd = -1;
  if (wait_popen && file->popen && file->pid > 0) {
    GocQjsCliProcessRequest request = {0};
    request.op = 2;
    request.pid = file->pid;
    request.block = 1;
    goc_qjs_cli_call_go_process(&request);
    if (request.result < 0 && result == 0) result = request.result;
    else if (request.result >= 0) result = request.status;
    file->pid = 0;
  }
  return result;
}
static void goc_qjs_cli_file_finalizer(JSRuntime *rt, JSValueConst value) {
  GocQjsCliFile *file = JS_GetOpaque(value, goc_qjs_cli_file_class_id);
  (void)rt;
  if (file) {
    goc_qjs_cli_file_close(file, true);
    js_free_rt(rt, file);
  }
}
static JSClassDef goc_qjs_cli_file_class_def = {
  "FILE",
  .finalizer = goc_qjs_cli_file_finalizer,
};
static JSValue goc_qjs_cli_new_file(JSContext *ctx, int fd, bool popen,
                                    int32_t pid) {
  JSValue object = JS_NewObjectClass(ctx, goc_qjs_cli_file_class_id);
  if (JS_IsException(object)) {
    if (fd > 2) goc_qjs_cli_syscall1(3, fd);
    if (popen && pid > 0) {
      GocQjsCliProcessRequest request = {0};
      request.op = 2; request.pid = pid; request.block = 1;
      goc_qjs_cli_call_go_process(&request);
    }
    return JS_EXCEPTION;
  }
  GocQjsCliFile *file = js_mallocz(ctx, sizeof(*file));
  if (!file) {
    JS_FreeValue(ctx, object);
    if (fd > 2) goc_qjs_cli_syscall1(3, fd);
    if (popen && pid > 0) {
      GocQjsCliProcessRequest request = {0};
      request.op = 2; request.pid = pid; request.block = 1;
      goc_qjs_cli_call_go_process(&request);
    }
    return JS_EXCEPTION;
  }
  file->fd = fd;
  file->stdio = fd >= 0 && fd <= 2;
  file->popen = popen;
  file->pid = pid;
  JS_SetOpaque(object, file);
  return object;
}
static GocQjsCliFile *goc_qjs_cli_get_file(JSContext *ctx, JSValueConst value) {
  GocQjsCliFile *file = JS_GetOpaque2(ctx, value, goc_qjs_cli_file_class_id);
  if (!file) return NULL;
  if (file->fd < 0) {
    JS_ThrowTypeError(ctx, "invalid file handle");
    return NULL;
  }
  return file;
}
static int goc_qjs_cli_file_mode(const char *mode, bool fdopen_mode,
                                 int *flags) {
  bool plus = false, binary = false, exclusive = false;
  if (!mode || !mode[0]) return -1;
  char first = mode[0];
  if (first != 'r' && first != 'w' && first != 'a') return -1;
  for (size_t i = 1; mode[i]; i++) {
    if (mode[i] == '+') { if (plus) return -1; plus = true; }
    else if (mode[i] == 'b') { if (binary || fdopen_mode) return -1; binary = true; }
    else if (mode[i] == 'x' && !fdopen_mode) { if (exclusive) return -1; exclusive = true; }
    else return -1;
  }
  if (exclusive && first != 'w') return -1;
  if (fdopen_mode) {
    *flags = first == 'r' ? O_RDONLY : first == 'w' ? O_WRONLY : O_WRONLY;
    if (plus) *flags = O_RDWR;
    return 0;
  }
  int value = first == 'r' ? O_RDONLY : first == 'w' ?
              O_WRONLY | O_CREAT | O_TRUNC : O_WRONLY | O_CREAT | O_APPEND;
  if (plus) value = (value & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
  if (exclusive) value |= O_EXCL;
  *flags = value;
  return 0;
}
static JSValue goc_qjs_cli_std_open(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  const char *mode = JS_ToCString(ctx, argv[1]);
  if (!mode) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
  int flags;
  if (goc_qjs_cli_file_mode(mode, false, &flags) < 0) {
    JS_FreeCString(ctx, mode); JS_FreeCString(ctx, path);
    return JS_ThrowTypeError(ctx, "invalid file mode");
  }
  long fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                                  flags | O_CLOEXEC, 0666);
  int32_t error = goc_qjs_cli_sys_error(fd);
  if (argc >= 3 && goc_qjs_cli_set_error(ctx, argv[2], error) < 0) {
    if (fd >= 0) goc_qjs_cli_syscall1(3, fd);
    JS_FreeCString(ctx, mode); JS_FreeCString(ctx, path);
    return JS_EXCEPTION;
  }
  JS_FreeCString(ctx, mode); JS_FreeCString(ctx, path);
  if (fd < 0) return JS_NULL;
  return goc_qjs_cli_new_file(ctx, (int)fd, false, 0);
}
static JSValue goc_qjs_cli_std_fdopen(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  int32_t fd;
  if (JS_ToInt32(ctx, &fd, argv[0])) return JS_EXCEPTION;
  const char *mode = JS_ToCString(ctx, argv[1]);
  if (!mode) return JS_EXCEPTION;
  int flags;
  if (goc_qjs_cli_file_mode(mode, true, &flags) < 0) {
    JS_FreeCString(ctx, mode);
    return JS_ThrowTypeError(ctx, "invalid file mode");
  }
  JS_FreeCString(ctx, mode);
  long access = goc_qjs_cli_syscall3(72, fd, 3, 0); /* fcntl F_GETFL */
  int32_t error = access < 0 ? (int32_t)access : 0;
  if (access >= 0 && ((flags == O_RDONLY && (access & O_ACCMODE) == O_WRONLY) ||
      (flags == O_WRONLY && (access & O_ACCMODE) == O_RDONLY) ||
      (flags == O_RDWR && (access & O_ACCMODE) != O_RDWR)))
    error = -EINVAL;
  if (argc >= 3 && goc_qjs_cli_set_error(ctx, argv[2],
                                          error < 0 ? -error : 0) < 0)
    return JS_EXCEPTION;
  if (error) return JS_NULL;
  return goc_qjs_cli_new_file(ctx, fd, false, 0);
}
static JSValue goc_qjs_cli_std_tmpfile(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  char path[32] = "/tmp/qjXXXXXX";
  long fd = -EIO;
  int32_t error = -EIO;
  for (int attempt = 0; attempt < 128; attempt++) {
    uint8_t random[6];
    long got = goc_qjs_cli_random_bytes(random, sizeof(random));
    if (got < 0) { fd = got; break; }
    for (int i = 0; i < 6; i++) path[7 + i] = alphabet[random[i] % 62];
    fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                               O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd == -EEXIST) continue;
    break;
  }
  if (fd >= 0) {
    error = 0;
    long removed = goc_qjs_cli_syscall3(263, GOC_AT_FDCWD, (long)path, 0);
    if (removed < 0) { goc_qjs_cli_syscall1(3, fd); fd = removed; error = (int32_t)removed; }
  } else error = (int32_t)fd;
  if (argc >= 1 && goc_qjs_cli_set_error(ctx, argv[0],
                                          error < 0 ? -error : 0) < 0) {
    if (fd >= 0) goc_qjs_cli_syscall1(3, fd);
    return JS_EXCEPTION;
  }
  if (fd < 0) return JS_NULL;
  return goc_qjs_cli_new_file(ctx, (int)fd, false, 0);
}
static JSValue goc_qjs_cli_std_popen(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  const char *command = JS_ToCString(ctx, argv[0]);
  if (!command) return JS_EXCEPTION;
  const char *mode = JS_ToCString(ctx, argv[1]);
  if (!mode) { JS_FreeCString(ctx, command); return JS_EXCEPTION; }
  if (goc_qjs_cli_strlen(mode) != 1 || (mode[0] != 'r' && mode[0] != 'w')) {
    JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command);
    return JS_ThrowTypeError(ctx, "invalid file mode");
  }
  int32_t pipe_fds[2];
  long pipe_result = goc_qjs_cli_syscall2(293, (long)pipe_fds, O_CLOEXEC);
  if (pipe_result < 0) {
    if (argc >= 3 && goc_qjs_cli_set_error(ctx, argv[2],
                                            (int32_t)-pipe_result) < 0) {
      JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command); return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command); return JS_NULL;
  }
  const char *child_argv[4] = { "/bin/sh", "-c", command, NULL };
  GocQjsCliProcessRequest request = {0};
  request.op = 1; request.block = 0; request.use_path = 0;
  request.stdin_fd = mode[0] == 'w' ? (int32_t)pipe_fds[0] : 0;
  request.stdout_fd = mode[0] == 'r' ? (int32_t)pipe_fds[1] : 1;
  request.stderr_fd = 2;
  request.argc = 3; request.argv = child_argv;
  request.file = "/bin/sh";
  goc_qjs_cli_call_go_process(&request);
  int32_t pid = request.result;
  int parent_fd = (int)(mode[0] == 'r' ? pipe_fds[0] : pipe_fds[1]);
  int child_fd = (int)(mode[0] == 'r' ? pipe_fds[1] : pipe_fds[0]);
  goc_qjs_cli_syscall1(3, child_fd);
  if (pid < 0) {
    goc_qjs_cli_syscall1(3, parent_fd);
    if (argc >= 3 && goc_qjs_cli_set_error(ctx, argv[2], -pid) < 0) {
      JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command); return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command); return JS_NULL;
  }
  if (argc >= 3 && goc_qjs_cli_set_error(ctx, argv[2], 0) < 0) {
    goc_qjs_cli_syscall1(3, parent_fd);
    GocQjsCliProcessRequest wait_request = {0};
    wait_request.op = 2; wait_request.pid = pid; wait_request.block = 1;
    goc_qjs_cli_call_go_process(&wait_request);
    JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command); return JS_EXCEPTION;
  }
  JS_FreeCString(ctx, mode); JS_FreeCString(ctx, command);
  return goc_qjs_cli_new_file(ctx, parent_fd, true, pid);
}
static JSValue goc_qjs_cli_std_load_file(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  bool binary = false;
  if (argc >= 2) {
    JSValue option = JS_GetPropertyStr(ctx, argv[1], "binary");
    if (JS_IsException(option)) return JS_EXCEPTION;
    if (!JS_IsUndefined(option)) {
      int enabled = JS_ToBool(ctx, option);
      if (enabled < 0) { JS_FreeValue(ctx, option); return JS_EXCEPTION; }
      binary = enabled;
    }
    JS_FreeValue(ctx, option);
  }
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  long fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                                 O_RDONLY | O_CLOEXEC, 0);
  JS_FreeCString(ctx, path);
  if (fd < 0) return JS_NULL;
  size_t capacity = 8192, length = 0;
  uint8_t *data = js_malloc(ctx, capacity);
  if (!data) { goc_qjs_cli_syscall1(3, fd); return JS_EXCEPTION; }
  for (;;) {
    if (length == capacity) {
      if (capacity > SIZE_MAX / 2) {
        js_free(ctx, data); goc_qjs_cli_syscall1(3, fd);
        return JS_ThrowOutOfMemory(ctx);
      }
      size_t next = capacity * 2;
      uint8_t *new_data = js_realloc(ctx, data, next);
      if (!new_data) { js_free(ctx, data); goc_qjs_cli_syscall1(3, fd); return JS_EXCEPTION; }
      data = new_data; capacity = next;
    }
    size_t available = capacity - length;
    if (available > 0x7ffff000u) available = 0x7ffff000u;
    long nread = goc_qjs_cli_syscall3(0, fd, (long)(data + length), (long)available);
    if (nread == -EINTR) continue;
    if (nread < 0) {
      js_free(ctx, data); goc_qjs_cli_syscall1(3, fd); return JS_NULL;
    }
    if (nread == 0) break;
    length += (size_t)nread;
  }
  goc_qjs_cli_syscall1(3, fd);
  JSValue result = binary ? JS_NewUint8ArrayCopy(ctx, data, length) :
                            JS_NewStringLen(ctx, (const char *)data, length);
  js_free(ctx, data);
  return result;
}
static JSValue goc_qjs_cli_std_write_file(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  JSValueConst data = argv[1];
  JSValue unref = JS_UNDEFINED;
  const uint8_t *bytes = (const uint8_t *)"";
  size_t length = 0;
  bool release = false;
  if (JS_IsObject(data)) {
    JSValue buffer = JS_GetPropertyStr(ctx, data, "buffer");
    if (JS_IsException(buffer)) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    if (JS_IsArrayBuffer(buffer)) data = unref = buffer;
    else JS_FreeValue(ctx, buffer);
  }
  if (JS_IsArrayBuffer(data)) {
    bytes = JS_GetArrayBuffer(ctx, &length, data);
    if (!bytes) { JS_FreeValue(ctx, unref); JS_FreeCString(ctx, path); return JS_EXCEPTION; }
  } else if (!JS_IsUndefined(data)) {
    const char *str = JS_ToCStringLen(ctx, &length, data);
    if (!str) { JS_FreeValue(ctx, unref); JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    bytes = (const uint8_t *)str; release = true;
  }
  long fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (fd < 0) {
    JSValue exception = JS_ThrowPlainError(ctx, "error opening %s for writing", path);
    if (release) JS_FreeCString(ctx, (const char *)bytes);
    JS_FreeValue(ctx, unref); JS_FreeCString(ctx, path); return exception;
  }
  int result = goc_qjs_cli_write_all((int)fd, bytes, length);
  long close_result = goc_qjs_cli_syscall1(3, fd);
  if (release) JS_FreeCString(ctx, (const char *)bytes);
  JS_FreeValue(ctx, unref);
  if (result < 0 || close_result < 0) {
    JSValue exception = JS_ThrowPlainError(ctx, "error writing to %s", path);
    JS_FreeCString(ctx, path); return exception;
  }
  JS_FreeCString(ctx, path);
  return JS_UNDEFINED;
}
static JSValue goc_qjs_cli_std_strerror(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  GocQjsCliStrerrorRequest request = {0};
  if (JS_ToInt32(ctx, &request.code, argv[0])) return JS_EXCEPTION;
  char message[256];
  request.output = message;
  request.capacity = sizeof(message);
  request.length = -1;
  goc_qjs_cli_call_go_strerror(&request);
  if (request.length < 0 || (uint64_t)request.length > request.capacity)
    return JS_ThrowInternalError(ctx, "could not format system error");
  return JS_NewStringLen(ctx, message, (size_t)request.length);
}
static JSValue goc_qjs_cli_std_printf(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  return goc_qjs_cli_format(ctx, argc, argv, 1);
}
static JSValue goc_qjs_cli_std_sprintf(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  return goc_qjs_cli_format(ctx, argc, argv, -1);
}
static JSValue goc_qjs_cli_std_puts(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  for (int i = 0; i < argc; i++) {
    size_t length;
    const char *string = JS_ToCStringLen(ctx, &length, argv[i]);
    if (!string) return JS_EXCEPTION;
    goc_qjs_cli_write_all(1, string, length);
    JS_FreeCString(ctx, string);
  }
  return JS_UNDEFINED;
}
static JSValue goc_qjs_cli_file_close_method(JSContext *ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
  GocQjsCliFile *file = JS_GetOpaque2(ctx, this_val, goc_qjs_cli_file_class_id);
  if (!file) return JS_EXCEPTION;
  if (file->fd < 0) return JS_ThrowTypeError(ctx, "invalid file handle");
  if (file->stdio) return JS_ThrowTypeError(ctx, "cannot close stdio");
  return JS_NewInt32(ctx, goc_qjs_cli_file_close(file, true));
}
static JSValue goc_qjs_cli_file_puts(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic) {
  GocQjsCliFile *file = magic ? goc_qjs_cli_get_file(ctx, this_val) : NULL;
  if (magic && !file) return JS_EXCEPTION;
  int fd = magic ? file->fd : 1;
  for (int i = 0; i < argc; i++) {
    size_t length;
    const char *string = JS_ToCStringLen(ctx, &length, argv[i]);
    if (!string) return JS_EXCEPTION;
    if (goc_qjs_cli_write_all(fd, string, length) < 0 && file) file->error = true;
    JS_FreeCString(ctx, string);
  }
  return JS_UNDEFINED;
}
static JSValue goc_qjs_cli_file_printf(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  JSValue result = goc_qjs_cli_format(ctx, argc, argv, file->fd);
  if (JS_IsException(result)) file->error = true;
  return result;
}
static JSValue goc_qjs_cli_file_flush(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (!goc_qjs_cli_get_file(ctx, this_val)) return JS_EXCEPTION;
  return JS_UNDEFINED;
}
static JSValue goc_qjs_cli_file_tell(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  long result = goc_qjs_cli_syscall3(8, file->fd, 0, SEEK_CUR);
  int64_t position = result;
  return magic ? JS_NewBigInt64(ctx, position) : JS_NewInt64(ctx, position);
}
static JSValue goc_qjs_cli_file_seek(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  int64_t position; int32_t whence;
  if (JS_ToInt64Ext(ctx, &position, argv[0]) || JS_ToInt32(ctx, &whence, argv[1]))
    return JS_EXCEPTION;
  long result = goc_qjs_cli_syscall3(8, file->fd, (long)position, whence);
  if (result >= 0) file->eof = false;
  else file->error = true;
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_file_eof(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  return file ? JS_NewBool(ctx, file->eof) : JS_EXCEPTION;
}
static JSValue goc_qjs_cli_file_error(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  return file ? JS_NewBool(ctx, file->error) : JS_EXCEPTION;
}
static JSValue goc_qjs_cli_file_clearerr(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  file->eof = file->error = false;
  return JS_UNDEFINED;
}
static JSValue goc_qjs_cli_file_fileno(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  return file ? JS_NewInt32(ctx, file->fd) : JS_EXCEPTION;
}
static JSValue goc_qjs_cli_file_read_write(JSContext *ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst *argv,
                                           int magic) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  uint64_t position = 0, length = 0;
  if (argc > 1 && JS_ToIndex(ctx, &position, argv[1])) return JS_EXCEPTION;
  if (argc > 2 && JS_ToIndex(ctx, &length, argv[2])) return JS_EXCEPTION;
  size_t size = 0;
  uint8_t *buffer;
  const char *string = NULL;
  if (magic && JS_IsString(argv[0])) {
    string = JS_ToCStringLen(ctx, &size, argv[0]);
    buffer = (uint8_t *)string;
  } else {
    buffer = JS_GetArrayBuffer(ctx, &size, argv[0]);
  }
  if (!buffer) return JS_EXCEPTION;
  if (position > size) position = size;
  if (argc < 3) length = size - position;
  if (length > size - (size_t)position) length = size - (size_t)position;
  size_t amount = (size_t)length;
  if (amount > 0x7ffff000u) amount = 0x7ffff000u;
  long result = magic ? goc_qjs_cli_syscall3(1, file->fd,
                          (long)(buffer + position), (long)amount) :
                        goc_qjs_cli_syscall3(0, file->fd,
                          (long)(buffer + position), (long)amount);
  if (string) JS_FreeCString(ctx, string);
  if (result == 0 && !magic) file->eof = true;
  if (result < 0) file->error = true;
  if (result > 0 && magic) file->eof = false;
  return JS_NewInt64(ctx, result);
}
static JSValue goc_qjs_cli_file_getline(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  GocQjsCliBuffer output = { ctx, NULL, 0, 0 };
  for (;;) {
    uint8_t byte;
    long result = goc_qjs_cli_syscall3(0, file->fd, (long)&byte, 1);
    if (result == -EINTR) continue;
    if (result < 0) {
      file->error = true; goc_qjs_cli_buffer_free(&output); return JS_EXCEPTION;
    }
    if (result == 0) {
      file->eof = true;
      if (output.length == 0) { goc_qjs_cli_buffer_free(&output); return JS_NULL; }
      break;
    }
    if (byte == '\n') break;
    if (goc_qjs_cli_buffer_append(&output, &byte, 1) < 0) {
      goc_qjs_cli_buffer_free(&output); return JS_ThrowOutOfMemory(ctx);
    }
  }
  JSValue result = JS_NewStringLen(ctx,
                  output.data ? (const char *)output.data : "", output.length);
  goc_qjs_cli_buffer_free(&output);
  return result;
}
static JSValue goc_qjs_cli_file_read_as(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  uint64_t max_size = UINT64_MAX;
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToIndex(ctx, &max_size, argv[0]))
    return JS_EXCEPTION;
  GocQjsCliBuffer output = { ctx, NULL, 0, 0 };
  uint8_t chunk[4096];
  while (max_size) {
    size_t request = max_size < sizeof(chunk) ? (size_t)max_size : sizeof(chunk);
    long result = goc_qjs_cli_syscall3(0, file->fd, (long)chunk, (long)request);
    if (result == -EINTR) continue;
    if (result < 0) {
      file->error = true; goc_qjs_cli_buffer_free(&output); return JS_EXCEPTION;
    }
    if (result == 0) { file->eof = true; break; }
    if (goc_qjs_cli_buffer_append(&output, chunk, (size_t)result) < 0) {
      goc_qjs_cli_buffer_free(&output); return JS_ThrowOutOfMemory(ctx);
    }
    max_size -= (uint64_t)result;
  }
  JSValue result = magic ? JS_NewStringLen(ctx,
                                            output.data ? (const char *)output.data : "",
                                            output.length) :
                            JS_NewArrayBufferCopy(ctx, output.data, output.length);
  goc_qjs_cli_buffer_free(&output);
  return result;
}
static JSValue goc_qjs_cli_file_get_byte(JSContext *ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  uint8_t byte;
  long result;
  do { result = goc_qjs_cli_syscall3(0, file->fd, (long)&byte, 1); }
  while (result == -EINTR);
  if (result == 0) file->eof = true;
  if (result < 0) { file->error = true; return JS_NewInt32(ctx, (int32_t)result); }
  return JS_NewInt32(ctx, result == 0 ? -1 : byte);
}
static JSValue goc_qjs_cli_file_put_byte(JSContext *ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  GocQjsCliFile *file = goc_qjs_cli_get_file(ctx, this_val);
  if (!file) return JS_EXCEPTION;
  int32_t value;
  if (JS_ToInt32(ctx, &value, argv[0])) return JS_EXCEPTION;
  uint8_t byte = (uint8_t)value;
  long result;
  do { result = goc_qjs_cli_syscall3(1, file->fd, (long)&byte, 1); }
  while (result == -EINTR);
  if (result < 0) file->error = true;
  else file->eof = false;
  return JS_NewInt32(ctx, result < 0 ? (int32_t)result : value & 255);
}

static const JSCFunctionListEntry goc_qjs_cli_std_error_props[] = {
  JS_PROP_INT32_DEF("EINVAL", EINVAL, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EIO", EIO, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EACCES", EACCES, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EEXIST", EEXIST, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("ENOSPC", ENOSPC, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("ENOSYS", ENOSYS, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EBUSY", EBUSY, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("ENOENT", ENOENT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EPERM", EPERM, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EPIPE", EPIPE, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("EBADF", EBADF, JS_PROP_CONFIGURABLE),
};
#ifdef GOC_QJS_BELLARD
/* Same entry as quickjs-libc.c js_std_parseExtJSON. */
static JSValue goc_qjs_cli_std_parse_ext_json(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  size_t length;
  const char *text = JS_ToCStringLen(ctx, &length, argc > 0 ? argv[0] : JS_UNDEFINED);
  if (!text)
    return JS_EXCEPTION;
  JSValue value = JS_ParseJSON2(ctx, text, length, "<input>", JS_PARSE_JSON_EXT);
  JS_FreeCString(ctx, text);
  return value;
}
#endif

static const JSCFunctionListEntry goc_qjs_cli_std_funcs[] = {
  JS_CFUNC_DEF("loadFile", 1, goc_qjs_cli_std_load_file),
#ifdef GOC_QJS_BELLARD
  JS_CFUNC_DEF("parseExtJSON", 1, goc_qjs_cli_std_parse_ext_json),
#endif
  JS_CFUNC_DEF("writeFile", 2, goc_qjs_cli_std_write_file),
  JS_CFUNC_DEF("strerror", 1, goc_qjs_cli_std_strerror),
  JS_CFUNC_DEF("open", 2, goc_qjs_cli_std_open),
  JS_CFUNC_DEF("popen", 2, goc_qjs_cli_std_popen),
  JS_CFUNC_DEF("tmpfile", 0, goc_qjs_cli_std_tmpfile),
  JS_CFUNC_DEF("fdopen", 2, goc_qjs_cli_std_fdopen),
  JS_CFUNC_MAGIC_DEF("puts", 1, goc_qjs_cli_file_puts, 0),
  JS_CFUNC_DEF("printf", 1, goc_qjs_cli_std_printf),
  JS_CFUNC_DEF("sprintf", 1, goc_qjs_cli_std_sprintf),
  JS_PROP_INT32_DEF("SEEK_SET", SEEK_SET, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SEEK_CUR", SEEK_CUR, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SEEK_END", SEEK_END, JS_PROP_CONFIGURABLE),
  JS_OBJECT_DEF("Error", goc_qjs_cli_std_error_props,
                GOC_COUNT_OF(goc_qjs_cli_std_error_props), JS_PROP_CONFIGURABLE),
};
static const JSCFunctionListEntry goc_qjs_cli_file_proto[] = {
  JS_CFUNC_DEF("close", 0, goc_qjs_cli_file_close_method),
  JS_CFUNC_MAGIC_DEF("puts", 1, goc_qjs_cli_file_puts, 1),
  JS_CFUNC_DEF("printf", 1, goc_qjs_cli_file_printf),
  JS_CFUNC_DEF("flush", 0, goc_qjs_cli_file_flush),
  JS_CFUNC_MAGIC_DEF("tell", 0, goc_qjs_cli_file_tell, 0),
  JS_CFUNC_MAGIC_DEF("tello", 0, goc_qjs_cli_file_tell, 1),
  JS_CFUNC_DEF("seek", 2, goc_qjs_cli_file_seek),
  JS_CFUNC_DEF("eof", 0, goc_qjs_cli_file_eof),
  JS_CFUNC_DEF("fileno", 0, goc_qjs_cli_file_fileno),
  JS_CFUNC_DEF("error", 0, goc_qjs_cli_file_error),
  JS_CFUNC_DEF("clearerr", 0, goc_qjs_cli_file_clearerr),
  JS_CFUNC_MAGIC_DEF("read", 1, goc_qjs_cli_file_read_write, 0),
  JS_CFUNC_MAGIC_DEF("write", 1, goc_qjs_cli_file_read_write, 1),
  JS_CFUNC_DEF("getline", 0, goc_qjs_cli_file_getline),
  JS_CFUNC_MAGIC_DEF("readAsArrayBuffer", 0, goc_qjs_cli_file_read_as, 0),
  JS_CFUNC_MAGIC_DEF("readAsString", 0, goc_qjs_cli_file_read_as, 1),
  JS_CFUNC_DEF("getByte", 0, goc_qjs_cli_file_get_byte),
  JS_CFUNC_DEF("putByte", 1, goc_qjs_cli_file_put_byte),
};

int goc_qjs_cli_std_extra_add(JSContext *ctx, JSModuleDef *module) {
  if (JS_AddModuleExportList(ctx, module, goc_qjs_cli_std_funcs,
                             GOC_COUNT_OF(goc_qjs_cli_std_funcs)) < 0)
    return -1;
  if (JS_AddModuleExport(ctx, module, "in") < 0 ||
      JS_AddModuleExport(ctx, module, "out") < 0 ||
      JS_AddModuleExport(ctx, module, "err") < 0)
    return -1;
  return 0;
}
int goc_qjs_cli_std_extra_init(JSContext *ctx, JSModuleDef *module) {
  JSRuntime *runtime = JS_GetRuntime(ctx);
  if (goc_qjs_cli_file_class_id == JS_INVALID_CLASS_ID)
    JS_NewClassID(runtime, &goc_qjs_cli_file_class_id);
  if (!JS_IsRegisteredClass(runtime, goc_qjs_cli_file_class_id) &&
      JS_NewClass(runtime, goc_qjs_cli_file_class_id,
                  &goc_qjs_cli_file_class_def) < 0)
    return -1;
  JSValue prototype = JS_NewObject(ctx);
  if (JS_IsException(prototype)) return -1;
  if (JS_SetPropertyFunctionList(ctx, prototype, goc_qjs_cli_file_proto,
                                 GOC_COUNT_OF(goc_qjs_cli_file_proto)) < 0) {
    JS_FreeValue(ctx, prototype); return -1;
  }
  JS_SetClassProto(ctx, goc_qjs_cli_file_class_id, prototype);
  if (JS_SetModuleExportList(ctx, module, goc_qjs_cli_std_funcs,
                             GOC_COUNT_OF(goc_qjs_cli_std_funcs)) < 0)
    return -1;
  JSValue input = goc_qjs_cli_new_file(ctx, 0, false, 0);
  if (JS_IsException(input) || JS_SetModuleExport(ctx, module, "in", input) < 0)
    return -1;
  JSValue output = goc_qjs_cli_new_file(ctx, 1, false, 0);
  if (JS_IsException(output) || JS_SetModuleExport(ctx, module, "out", output) < 0)
    return -1;
  JSValue error = goc_qjs_cli_new_file(ctx, 2, false, 0);
  if (JS_IsException(error) || JS_SetModuleExport(ctx, module, "err", error) < 0)
    return -1;
  return 0;
}
