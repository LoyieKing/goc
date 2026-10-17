/* Part of cli_std_os.c, which includes it; not compiled on its own.
 * qjs:os descriptors, filesystem, processes and the os module table. */

/* ----- qjs:os raw descriptor and filesystem API. ----- */
static JSValue goc_qjs_cli_os_open(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  int32_t flags, mode = 0666;
  if (JS_ToInt32(ctx, &flags, argv[1]) ||
      (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &mode, argv[2]))) {
    JS_FreeCString(ctx, path); return JS_EXCEPTION;
  }
  long fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                                  flags | O_CLOEXEC, mode);
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, (int32_t)fd);
}
static JSValue goc_qjs_cli_os_close(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  int32_t fd;
  if (JS_ToInt32(ctx, &fd, argv[0])) return JS_EXCEPTION;
  return JS_NewInt32(ctx, (int32_t)goc_qjs_cli_syscall1(3, fd));
}
static JSValue goc_qjs_cli_os_seek(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  int32_t fd, whence;
  int64_t position;
  if (JS_ToInt32(ctx, &fd, argv[0]) || JS_ToInt64Ext(ctx, &position, argv[1]) ||
      JS_ToInt32(ctx, &whence, argv[2])) return JS_EXCEPTION;
  bool bigint = JS_IsBigInt(argv[1]);
  long result = goc_qjs_cli_syscall3(8, fd, (long)position, whence);
  int64_t value = result;
  return bigint ? JS_NewBigInt64(ctx, value) : JS_NewInt64(ctx, value);
}
static JSValue goc_qjs_cli_os_read_write(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv,
                                         int magic) {
  int32_t fd; uint64_t position, length; size_t size;
  if (JS_ToInt32(ctx, &fd, argv[0]) || JS_ToIndex(ctx, &position, argv[2]) ||
      JS_ToIndex(ctx, &length, argv[3])) return JS_EXCEPTION;
  uint8_t *buffer = JS_GetArrayBuffer(ctx, &size, argv[1]);
  if (!buffer) return JS_EXCEPTION;
  if (position > size || length > size - (size_t)position)
    return JS_ThrowRangeError(ctx, "read/write array buffer overflow");
  if (length > 0x7ffff000u) length = 0x7ffff000u;
  long result = goc_qjs_cli_syscall3(magic ? 1 : 0, fd,
                       (long)(buffer + position), (long)length);
  return JS_NewInt64(ctx, result);
}
static JSValue goc_qjs_cli_os_isatty(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  int32_t fd;
  if (JS_ToInt32(ctx, &fd, argv[0])) return JS_EXCEPTION;
  uint8_t termios[64];
  long result = goc_qjs_cli_syscall3(16, fd, 0x5401, (long)termios);
  return JS_NewBool(ctx, result == 0);
}
static JSValue goc_qjs_cli_os_remove(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  long result = goc_qjs_cli_syscall3(263, GOC_AT_FDCWD, (long)path, 0);
  if (result == -EISDIR || result == -EPERM)
    result = goc_qjs_cli_syscall3(263, GOC_AT_FDCWD, (long)path, AT_REMOVEDIR);
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_os_rename(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  const char *old_path = JS_ToCString(ctx, argv[0]);
  if (!old_path) return JS_EXCEPTION;
  const char *new_path = JS_ToCString(ctx, argv[1]);
  if (!new_path) { JS_FreeCString(ctx, old_path); return JS_EXCEPTION; }
  long result = goc_qjs_cli_syscall4(264, GOC_AT_FDCWD, (long)old_path,
                                      GOC_AT_FDCWD, (long)new_path);
  JS_FreeCString(ctx, new_path); JS_FreeCString(ctx, old_path);
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_os_getcwd(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  size_t capacity = 256;
  char *buffer = js_malloc(ctx, capacity);
  if (!buffer) return JS_EXCEPTION;
  long result;
  for (;;) {
    result = goc_qjs_cli_syscall2(79, (long)buffer, (long)capacity);
    if (result != -ERANGE) break;
    if (capacity > SIZE_MAX / 2) { js_free(ctx, buffer); return JS_ThrowOutOfMemory(ctx); }
    size_t next = capacity * 2;
    char *grown = js_realloc(ctx, buffer, next);
    if (!grown) { js_free(ctx, buffer); return JS_EXCEPTION; }
    buffer = grown; capacity = next;
  }
  if (result < 0) { buffer[0] = '\0'; result = -result; }
  else result = 0;
  JSValue path = JS_NewStringLen(ctx, buffer, goc_qjs_cli_strlen(buffer));
  js_free(ctx, buffer);
  return goc_qjs_cli_tuple(ctx, path, (int32_t)result);
}
static JSValue goc_qjs_cli_os_chdir(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  long result = goc_qjs_cli_syscall1(80, (long)path);
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_os_mkdir(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  int32_t mode = 0777;
  if (argc >= 2 && JS_ToInt32(ctx, &mode, argv[1])) return JS_EXCEPTION;
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  long result = goc_qjs_cli_syscall3(258, GOC_AT_FDCWD, (long)path, mode);
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, (int32_t)result);
}
typedef struct GocQjsCliDirent64 {
  uint64_t ino;
  int64_t offset;
  uint16_t record_length;
  uint8_t type;
  char name[];
} GocQjsCliDirent64;
static JSValue goc_qjs_cli_os_readdir(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  long fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  JS_FreeCString(ctx, path);
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) { if (fd >= 0) goc_qjs_cli_syscall1(3, fd); return array; }
  int32_t error = goc_qjs_cli_errno(fd);
  uint32_t index = 0;
  uint8_t entries[8192];
  while (fd >= 0) {
    long count = goc_qjs_cli_syscall3(217, fd, (long)entries, sizeof(entries));
    if (count == -EINTR) continue;
    if (count < 0) { error = goc_qjs_cli_errno(count); break; }
    if (count == 0) break;
    size_t offset = 0;
    while (offset < (size_t)count) {
      GocQjsCliDirent64 *entry = (GocQjsCliDirent64 *)(entries + offset);
      if (entry->record_length < offsetof(GocQjsCliDirent64, name) + 1 ||
          entry->record_length > (size_t)count - offset) { error = EIO; break; }
      size_t name_length = goc_qjs_cli_strlen(entry->name);
      JSValue name = JS_NewStringLen(ctx, entry->name, name_length);
      if (JS_IsException(name)) {
        goc_qjs_cli_syscall1(3, fd); JS_FreeValue(ctx, array); return JS_EXCEPTION;
      }
      if (JS_SetPropertyUint32(ctx, array, index++, name) < 0) {
        goc_qjs_cli_syscall1(3, fd); JS_FreeValue(ctx, array); return JS_EXCEPTION;
      }
      offset += entry->record_length;
    }
    if (error) break;
  }
  if (fd >= 0) goc_qjs_cli_syscall1(3, fd);
  return goc_qjs_cli_tuple(ctx, array, error);
}

#if defined(__linux__)
typedef struct GocQjsCliTimespec { int64_t sec; int64_t nsec; } GocQjsCliTimespec;
typedef struct GocQjsCliStat {
  uint64_t dev;
  uint64_t ino;
  uint64_t nlink;
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t pad;
  uint64_t rdev;
  int64_t size;
  int64_t blksize;
  int64_t blocks;
  GocQjsCliTimespec atim;
  GocQjsCliTimespec mtim;
  GocQjsCliTimespec ctim;
  int64_t reserved[3];
} GocQjsCliStat;
_Static_assert(sizeof(GocQjsCliStat) == 144,
               "Linux amd64 stat syscall layout mismatch");
#endif
static int goc_qjs_cli_define_number(JSContext *ctx, JSValueConst object,
                                     const char *name, int64_t number) {
  return JS_DefinePropertyValueStr(ctx, object, name, JS_NewInt64(ctx, number),
                                   JS_PROP_C_W_E);
}
static JSValue goc_qjs_cli_os_stat(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  GocQjsCliStat stat_buffer;
  long result = goc_qjs_cli_syscall4(262, GOC_AT_FDCWD, (long)path,
                          (long)&stat_buffer, magic ? AT_SYMLINK_NOFOLLOW : 0);
  JS_FreeCString(ctx, path);
  JSValue value = JS_NULL;
  int32_t error = goc_qjs_cli_errno(result);
  if (result >= 0) {
    value = JS_NewObject(ctx);
    if (JS_IsException(value)) return value;
    int failed =
      goc_qjs_cli_define_number(ctx, value, "dev", stat_buffer.dev) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "ino", stat_buffer.ino) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "mode", stat_buffer.mode) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "nlink", stat_buffer.nlink) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "uid", stat_buffer.uid) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "gid", stat_buffer.gid) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "rdev", stat_buffer.rdev) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "size", stat_buffer.size) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "blocks", stat_buffer.blocks) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "atime",
              stat_buffer.atim.sec * 1000 + stat_buffer.atim.nsec / 1000000) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "mtime",
              stat_buffer.mtim.sec * 1000 + stat_buffer.mtim.nsec / 1000000) < 0 ||
      goc_qjs_cli_define_number(ctx, value, "ctime",
              stat_buffer.ctim.sec * 1000 + stat_buffer.ctim.nsec / 1000000) < 0;
    if (failed) { JS_FreeValue(ctx, value); return JS_EXCEPTION; }
  }
  return goc_qjs_cli_tuple(ctx, value, error);
}
static JSValue goc_qjs_cli_os_utimes(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  int64_t atime, mtime;
  if (JS_ToInt64Ext(ctx, &atime, argv[1]) || JS_ToInt64Ext(ctx, &mtime, argv[2]))
    return JS_EXCEPTION;
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  GocQjsCliTimespec times[2];
  times[0].sec = atime / 1000; times[0].nsec = (atime % 1000) * 1000000;
  times[1].sec = mtime / 1000; times[1].nsec = (mtime % 1000) * 1000000;
  if (times[0].nsec < 0) { times[0].sec--; times[0].nsec += 1000000000; }
  if (times[1].nsec < 0) { times[1].sec--; times[1].nsec += 1000000000; }
  long result = goc_qjs_cli_syscall4(280, GOC_AT_FDCWD, (long)path,
                                      (long)times, 0);
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_os_realpath(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  long fd = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                                  O_PATH | O_CLOEXEC, 0);
  JS_FreeCString(ctx, path);
  char proc_path[64] = "/proc/self/fd/";
  char digits[24]; int count = 0;
  long value = fd;
  if (fd >= 0) {
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (int i = 0; i < count; i++) proc_path[14 + i] = digits[count - 1 - i];
    proc_path[14 + count] = '\0';
  }
  size_t capacity = 256;
  char *resolved = js_malloc(ctx, capacity);
  if (!resolved) {
    if (fd >= 0) goc_qjs_cli_syscall1(3, fd);
    return JS_EXCEPTION;
  }
  long result = fd;
  while (result >= 0) {
    result = goc_qjs_cli_syscall4(267, GOC_AT_FDCWD, (long)proc_path,
                                   (long)resolved, capacity - 1);
    if (result < 0 || (size_t)result < capacity - 1) break;
    if (capacity >= GOC_FILE_PATH_LIMIT) { result = -ENAMETOOLONG; break; }
    size_t next = capacity * 2;
    char *grown = js_realloc(ctx, resolved, next);
    if (!grown) {
      js_free(ctx, resolved);
      goc_qjs_cli_syscall1(3, fd);
      return JS_EXCEPTION;
    }
    resolved = grown;
    capacity = next;
  }
  if (fd >= 0) goc_qjs_cli_syscall1(3, fd);
  int32_t error = goc_qjs_cli_errno(result);
  if (result < 0) resolved[0] = '\0';
  else { resolved[result] = '\0'; }
  JSValue output = JS_NewStringLen(ctx, resolved, goc_qjs_cli_strlen(resolved));
  js_free(ctx, resolved);
  return goc_qjs_cli_tuple(ctx, output, error);
}
static JSValue goc_qjs_cli_os_symlink(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  const char *target = JS_ToCString(ctx, argv[0]);
  if (!target) return JS_EXCEPTION;
  const char *path = JS_ToCString(ctx, argv[1]);
  if (!path) { JS_FreeCString(ctx, target); return JS_EXCEPTION; }
  long result = goc_qjs_cli_syscall3(266, (long)target, GOC_AT_FDCWD, (long)path);
  JS_FreeCString(ctx, path); JS_FreeCString(ctx, target);
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_os_readlink(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_EXCEPTION;
  size_t capacity = 256;
  char *buffer = js_malloc(ctx, capacity);
  if (!buffer) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
  long result;
  for (;;) {
    result = goc_qjs_cli_syscall4(267, GOC_AT_FDCWD, (long)path,
                                   (long)buffer, capacity);
    if (result < 0 || (size_t)result < capacity) break;
    if (capacity >= GOC_FILE_PATH_LIMIT) { result = -ENAMETOOLONG; break; }
    size_t next = capacity * 2;
    char *grown = js_realloc(ctx, buffer, next);
    if (!grown) { js_free(ctx, buffer); JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    buffer = grown; capacity = next;
  }
  JS_FreeCString(ctx, path);
  int32_t error = goc_qjs_cli_errno(result);
  JSValue value;
  if (result < 0) value = JS_NewStringLen(ctx, "", 0);
  else value = JS_NewStringLen(ctx, buffer, (size_t)result);
  js_free(ctx, buffer);
  return goc_qjs_cli_tuple(ctx, value, error);
}
static JSValue goc_qjs_cli_os_mkdstemp(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const char *input = NULL;
  size_t input_length = 0;
  if (argc > 0) {
    input = JS_ToCStringLen(ctx, &input_length, argv[0]);
    if (!input) return JS_EXCEPTION;
  } else {
    input = "tmp"; input_length = 3;
  }
  bool has_template = input_length >= 6;
  if (has_template) {
    for (size_t i = input_length - 6; i < input_length; i++)
      if (input[i] != 'X') has_template = false;
  }
  size_t path_length = input_length + (has_template ? 0 : 6);
  if (path_length >= GOC_FILE_PATH_LIMIT) {
    if (argc > 0) JS_FreeCString(ctx, input);
    return JS_ThrowRangeError(ctx, "temporary path is too long");
  }
  char *path = js_malloc(ctx, path_length + 1);
  if (!path) { if (argc > 0) JS_FreeCString(ctx, input); return JS_EXCEPTION; }
  for (size_t i = 0; i < input_length; i++) path[i] = input[i];
  if (!has_template) for (size_t i = 0; i < 6; i++) path[input_length + i] = 'X';
  path[path_length] = '\0';
  if (argc > 0) JS_FreeCString(ctx, input);
  int32_t result = -EEXIST;
  int32_t fd = -1;
  for (int attempt = 0; attempt < 128; attempt++) {
    uint8_t random[6];
    long got = goc_qjs_cli_random_bytes(random, sizeof(random));
    if (got < 0) { result = (int32_t)got; break; }
    for (size_t i = path_length - 6; i < path_length; i++)
      path[i] = alphabet[random[i - (path_length - 6)] % 62];
    if (magic == 'd') {
      long made = goc_qjs_cli_syscall3(258, GOC_AT_FDCWD, (long)path, 0700);
      result = made < 0 ? (int32_t)made : 0;
      if (made == -EEXIST) continue;
    } else {
      long made = goc_qjs_cli_syscall4(257, GOC_AT_FDCWD, (long)path,
                      O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      result = (int32_t)made;
      if (made == -EEXIST) continue;
      if (made >= 0) fd = (int32_t)made;
    }
    break;
  }
  JSValue value = JS_NewStringLen(ctx, path, path_length);
  js_free(ctx, path);
  return goc_qjs_cli_tuple(ctx, value, result == 0 && magic == 'd' ? 0 :
                           magic == 'd' ? result : fd >= 0 ? fd : result);
}
static JSValue goc_qjs_cli_os_sleep(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  int64_t milliseconds;
  if (JS_ToInt64Ext(ctx, &milliseconds, argv[0])) return JS_EXCEPTION;
  if (milliseconds < 0) milliseconds = 0;
  GocQjsCliTimespec request = { milliseconds / 1000,
                                (milliseconds % 1000) * 1000000 };
  long result = goc_qjs_cli_syscall2(35, (long)&request, 0);
  return JS_NewInt32(ctx, (int32_t)result);
}
static JSValue goc_qjs_cli_os_pipe(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  int32_t fds[2];
  long result = goc_qjs_cli_syscall2(293, (long)fds, O_CLOEXEC);
  if (result < 0) return JS_NULL;
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) { goc_qjs_cli_syscall1(3, fds[0]); goc_qjs_cli_syscall1(3, fds[1]); return array; }
  if (JS_SetPropertyUint32(ctx, array, 0, JS_NewInt32(ctx, fds[0])) < 0 ||
      JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx, fds[1])) < 0) {
    goc_qjs_cli_syscall1(3, fds[0]); goc_qjs_cli_syscall1(3, fds[1]);
    JS_FreeValue(ctx, array); return JS_EXCEPTION;
  }
  return array;
}
static JSValue goc_qjs_cli_os_kill(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  int32_t pid, signal;
  if (JS_ToInt32(ctx, &pid, argv[0]) || JS_ToInt32(ctx, &signal, argv[1]))
    return JS_EXCEPTION;
  return JS_NewInt32(ctx, (int32_t)goc_qjs_cli_syscall2(62, pid, signal));
}
static JSValue goc_qjs_cli_os_getpid(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  return JS_NewInt32(ctx, (int32_t)goc_qjs_cli_syscall0(39));
}
static JSValue goc_qjs_cli_os_dup(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  int32_t fd; if (JS_ToInt32(ctx, &fd, argv[0])) return JS_EXCEPTION;
  return JS_NewInt32(ctx, (int32_t)goc_qjs_cli_syscall1(32, fd));
}
static JSValue goc_qjs_cli_os_dup2(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  int32_t old_fd, new_fd;
  if (JS_ToInt32(ctx, &old_fd, argv[0]) || JS_ToInt32(ctx, &new_fd, argv[1]))
    return JS_EXCEPTION;
  return JS_NewInt32(ctx, (int32_t)goc_qjs_cli_syscall2(33, old_fd, new_fd));
}
static JSValue goc_qjs_cli_os_waitpid(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  GocQjsCliProcessRequest request = {0};
  if (JS_ToInt32(ctx, &request.pid, argv[0]) ||
      JS_ToInt32(ctx, &request.options, argv[1])) return JS_EXCEPTION;
  request.op = 2;
  request.block = (request.options & WNOHANG) == 0;
  goc_qjs_cli_call_go_process(&request);
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) return array;
  if (JS_SetPropertyUint32(ctx, array, 0, JS_NewInt32(ctx, request.result)) < 0 ||
      JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx, request.status)) < 0) {
    JS_FreeValue(ctx, array); return JS_EXCEPTION;
  }
  return array;
}
static JSValue goc_qjs_cli_os_exec(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  JSValue value = JS_GetPropertyStr(ctx, argv[0], "length");
  if (JS_IsException(value)) return JS_EXCEPTION;
  uint32_t count;
  int conversion_error = JS_ToUint32(ctx, &count, value);
  JS_FreeValue(ctx, value);
  if (conversion_error) return JS_EXCEPTION;
  if (count < 1 || count > 65535)
    return JS_ThrowTypeError(ctx, "invalid number of arguments");
  GocQjsCliProcessRequest request = {0};
  request.op = 1; request.block = 1; request.use_path = 1;
  request.stdin_fd = 0; request.stdout_fd = 1; request.stderr_fd = 2;
  request.argc = count;
  const char **arguments = js_mallocz(ctx, sizeof(*arguments) * ((size_t)count + 1));
  if (!arguments) return JS_EXCEPTION;
  uint32_t made = 0;
  for (; made < count; made++) {
    value = JS_GetPropertyUint32(ctx, argv[0], made);
    if (JS_IsException(value)) goto exception;
    arguments[made] = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (!arguments[made]) goto exception;
  }
  request.argv = arguments;
  if (argc >= 2) {
    JSValue options = argv[1];
    value = JS_GetPropertyStr(ctx, options, "block");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      int enabled = JS_ToBool(ctx, value);
      if (enabled < 0) { JS_FreeValue(ctx, value); goto exception; }
      request.block = enabled;
    }
    JS_FreeValue(ctx, value);
    value = JS_GetPropertyStr(ctx, options, "usePath");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      int enabled = JS_ToBool(ctx, value);
      if (enabled < 0) { JS_FreeValue(ctx, value); goto exception; }
      request.use_path = enabled;
    }
    JS_FreeValue(ctx, value);
    value = JS_GetPropertyStr(ctx, options, "file");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      request.file = JS_ToCString(ctx, value);
      if (!request.file) { JS_FreeValue(ctx, value); goto exception; }
    }
    JS_FreeValue(ctx, value);
    value = JS_GetPropertyStr(ctx, options, "cwd");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      request.cwd = JS_ToCString(ctx, value);
      if (!request.cwd) { JS_FreeValue(ctx, value); goto exception; }
    }
    JS_FreeValue(ctx, value);
    static const char *stream_names[3] = { "stdin", "stdout", "stderr" };
    int32_t *stream_fds[3] = { &request.stdin_fd, &request.stdout_fd,
                               &request.stderr_fd };
    for (int i = 0; i < 3; i++) {
      value = JS_GetPropertyStr(ctx, options, stream_names[i]);
      if (JS_IsException(value)) goto exception;
      if (!JS_IsUndefined(value) && JS_ToInt32(ctx, stream_fds[i], value)) {
        JS_FreeValue(ctx, value); goto exception;
      }
      JS_FreeValue(ctx, value);
    }
    value = JS_GetPropertyStr(ctx, options, "env");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      JSPropertyEnum *properties = NULL;
      uint32_t property_count = 0;
      if (JS_GetOwnPropertyNames(ctx, &properties, &property_count, value,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, value); goto exception;
      }
      const char **environment = js_mallocz(ctx,
                                     sizeof(*environment) * ((size_t)property_count + 1));
      if (!environment) {
        JS_FreePropertyEnum(ctx, properties, property_count);
        JS_FreeValue(ctx, value); goto exception;
      }
      request.envp = environment;
      request.envc = property_count;
      for (uint32_t i = 0; i < property_count; i++) {
        JSValue item = JS_GetProperty(ctx, value, properties[i].atom);
        if (JS_IsException(item)) break;
        const char *item_string = JS_ToCString(ctx, item);
        JS_FreeValue(ctx, item);
        if (!item_string) break;
        const char *key = JS_AtomToCString(ctx, properties[i].atom);
        if (!key) { JS_FreeCString(ctx, item_string); break; }
        size_t key_length = goc_qjs_cli_strlen(key);
        size_t value_length = goc_qjs_cli_strlen(item_string);
        char *pair = js_malloc(ctx, key_length + value_length + 2);
        if (!pair) {
          JS_FreeCString(ctx, key); JS_FreeCString(ctx, item_string); break;
        }
        for (size_t j = 0; j < key_length; j++) pair[j] = key[j];
        pair[key_length] = '=';
        for (size_t j = 0; j < value_length; j++) pair[key_length + 1 + j] = item_string[j];
        pair[key_length + value_length + 1] = '\0';
        environment[i] = pair;
        JS_FreeCString(ctx, key); JS_FreeCString(ctx, item_string);
      }
      JS_FreePropertyEnum(ctx, properties, property_count);
      JS_FreeValue(ctx, value);
      for (uint32_t i = 0; i < property_count; i++)
        if (!environment[i]) { request.envc = i; goto exception; }
    }
    value = JS_GetPropertyStr(ctx, options, "uid");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      if (JS_ToUint32(ctx, &request.uid, value)) { JS_FreeValue(ctx, value); goto exception; }
      request.uid_set = 1;
    }
    JS_FreeValue(ctx, value);
    value = JS_GetPropertyStr(ctx, options, "gid");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      if (JS_ToUint32(ctx, &request.gid, value)) { JS_FreeValue(ctx, value); goto exception; }
      request.gid_set = 1;
    }
    JS_FreeValue(ctx, value);
    value = JS_GetPropertyStr(ctx, options, "groups");
    if (JS_IsException(value)) goto exception;
    if (!JS_IsUndefined(value)) {
      request.groups_set = 1;
      int64_t group_count;
      if (JS_GetLength(ctx, value, &group_count) < 0 || group_count < 0) {
        JS_FreeValue(ctx, value); goto exception;
      }
      if (group_count > 64) {
        JS_ThrowRangeError(ctx, "too many groups");
        JS_FreeValue(ctx, value); goto exception;
      }
      uint32_t *groups = js_malloc(ctx, sizeof(*groups) * (size_t)group_count);
      if (!groups && group_count) { JS_FreeValue(ctx, value); goto exception; }
      request.groups = groups;
      for (int64_t i = 0; i < group_count; i++) {
        JSValue item = JS_GetPropertyInt64(ctx, value, i);
        if (JS_IsException(item)) { JS_FreeValue(ctx, value); goto exception; }
        if (!JS_IsUndefined(item) && JS_ToUint32(ctx, &groups[request.groups_len], item)) {
          JS_FreeValue(ctx, item); JS_FreeValue(ctx, value); goto exception;
        }
        if (!JS_IsUndefined(item)) request.groups_len++;
        JS_FreeValue(ctx, item);
      }
    }
    JS_FreeValue(ctx, value);
  }
  goc_qjs_cli_call_go_process(&request);
  for (uint32_t i = 0; i < count; i++) JS_FreeCString(ctx, arguments[i]);
  js_free(ctx, arguments);
  if (request.file) JS_FreeCString(ctx, request.file);
  if (request.cwd) JS_FreeCString(ctx, request.cwd);
  if (request.envp) {
    for (uint32_t i = 0; i < request.envc; i++) js_free(ctx, (void *)request.envp[i]);
    js_free(ctx, (void *)request.envp);
  }
  js_free(ctx, (void *)request.groups);
  return JS_NewInt32(ctx, request.result);
exception:
  for (uint32_t i = 0; i < made; i++) JS_FreeCString(ctx, arguments[i]);
  js_free(ctx, arguments);
  if (request.file) JS_FreeCString(ctx, request.file);
  if (request.cwd) JS_FreeCString(ctx, request.cwd);
  if (request.envp) {
    for (uint32_t i = 0; i < request.envc; i++)
      if (request.envp[i]) js_free(ctx, (void *)request.envp[i]);
    js_free(ctx, (void *)request.envp);
  }
  js_free(ctx, (void *)request.groups);
  return JS_EXCEPTION;
}

static const JSCFunctionListEntry goc_qjs_cli_os_funcs[] = {
  JS_CFUNC_DEF("open", 2, goc_qjs_cli_os_open),
  JS_PROP_INT32_DEF("O_RDONLY", O_RDONLY, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("O_WRONLY", O_WRONLY, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("O_RDWR", O_RDWR, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("O_APPEND", O_APPEND, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("O_CREAT", O_CREAT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("O_EXCL", O_EXCL, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("O_TRUNC", O_TRUNC, JS_PROP_CONFIGURABLE),
  JS_CFUNC_DEF("close", 1, goc_qjs_cli_os_close),
  JS_CFUNC_DEF("seek", 3, goc_qjs_cli_os_seek),
  JS_CFUNC_MAGIC_DEF("read", 4, goc_qjs_cli_os_read_write, 0),
  JS_CFUNC_MAGIC_DEF("write", 4, goc_qjs_cli_os_read_write, 1),
  JS_CFUNC_DEF("isatty", 1, goc_qjs_cli_os_isatty),
  JS_CFUNC_DEF("remove", 1, goc_qjs_cli_os_remove),
  JS_CFUNC_DEF("rename", 2, goc_qjs_cli_os_rename),
  JS_PROP_INT32_DEF("SIGINT", SIGINT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGABRT", SIGABRT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGFPE", SIGFPE, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGILL", SIGILL, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGSEGV", SIGSEGV, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGTERM", SIGTERM, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGQUIT", SIGQUIT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGPIPE", SIGPIPE, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGALRM", SIGALRM, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGUSR1", SIGUSR1, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGUSR2", SIGUSR2, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGCHLD", SIGCHLD, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGCONT", SIGCONT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGSTOP", SIGSTOP, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGTSTP", SIGTSTP, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGTTIN", SIGTTIN, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("SIGTTOU", SIGTTOU, JS_PROP_CONFIGURABLE),
  JS_CFUNC_DEF("getcwd", 0, goc_qjs_cli_os_getcwd),
  JS_CFUNC_DEF("chdir", 1, goc_qjs_cli_os_chdir),
  JS_CFUNC_DEF("mkdir", 1, goc_qjs_cli_os_mkdir),
  JS_CFUNC_DEF("readdir", 1, goc_qjs_cli_os_readdir),
  JS_CFUNC_MAGIC_DEF("mkdtemp", 0, goc_qjs_cli_os_mkdstemp, 'd'),
  JS_CFUNC_MAGIC_DEF("mkstemp", 0, goc_qjs_cli_os_mkdstemp, 's'),
  JS_PROP_INT32_DEF("S_IFMT", S_IFMT, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFIFO", S_IFIFO, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFCHR", S_IFCHR, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFDIR", S_IFDIR, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFBLK", S_IFBLK, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFREG", S_IFREG, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFSOCK", S_IFSOCK, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_IFLNK", S_IFLNK, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_ISGID", S_ISGID, JS_PROP_CONFIGURABLE),
  JS_PROP_INT32_DEF("S_ISUID", S_ISUID, JS_PROP_CONFIGURABLE),
  JS_CFUNC_MAGIC_DEF("stat", 1, goc_qjs_cli_os_stat, 0),
  JS_CFUNC_DEF("utimes", 3, goc_qjs_cli_os_utimes),
  JS_CFUNC_DEF("sleep", 1, goc_qjs_cli_os_sleep),
  JS_CFUNC_DEF("realpath", 1, goc_qjs_cli_os_realpath),
  JS_CFUNC_MAGIC_DEF("lstat", 1, goc_qjs_cli_os_stat, 1),
  JS_CFUNC_DEF("symlink", 2, goc_qjs_cli_os_symlink),
  JS_CFUNC_DEF("readlink", 1, goc_qjs_cli_os_readlink),
  JS_CFUNC_DEF("exec", 1, goc_qjs_cli_os_exec),
  JS_CFUNC_DEF("getpid", 0, goc_qjs_cli_os_getpid),
  JS_CFUNC_DEF("waitpid", 2, goc_qjs_cli_os_waitpid),
  JS_PROP_INT32_DEF("WNOHANG", WNOHANG, JS_PROP_CONFIGURABLE),
  JS_CFUNC_DEF("pipe", 0, goc_qjs_cli_os_pipe),
  JS_CFUNC_DEF("kill", 2, goc_qjs_cli_os_kill),
  JS_CFUNC_DEF("dup", 1, goc_qjs_cli_os_dup),
  JS_CFUNC_DEF("dup2", 2, goc_qjs_cli_os_dup2),
};

int goc_qjs_cli_os_extra_add(JSContext *ctx, JSModuleDef *module) {
  return JS_AddModuleExportList(ctx, module, goc_qjs_cli_os_funcs,
                                GOC_COUNT_OF(goc_qjs_cli_os_funcs));
}
int goc_qjs_cli_os_extra_init(JSContext *ctx, JSModuleDef *module) {
  return JS_SetModuleExportList(ctx, module, goc_qjs_cli_os_funcs,
                                GOC_COUNT_OF(goc_qjs_cli_os_funcs));
}
