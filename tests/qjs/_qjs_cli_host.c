#include "../../third_party/quickjs-ng/quickjs.h"

int goc_qjs_cli_std_extra_add(JSContext *ctx, JSModuleDef *module);
int goc_qjs_cli_std_extra_init(JSContext *ctx, JSModuleDef *module);
int goc_qjs_cli_os_extra_add(JSContext *ctx, JSModuleDef *module);
int goc_qjs_cli_os_extra_init(JSContext *ctx, JSModuleDef *module);
int goc_qjs_cli_worker_add(JSContext *ctx, JSModuleDef *module);
int goc_qjs_cli_worker_init(JSContext *ctx, JSModuleDef *module);

#if !defined(__linux__) || !defined(__x86_64__)
#error "the QuickJS CLI host bridge requires Linux/amd64"
#endif

static long goc_qjs_cli_open_syscall(const char *path) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(2L), "D"(path), "S"(0L)
                   : "rcx", "r11", "memory");
  return result;
}

static long goc_qjs_cli_read_syscall(long fd, char *buf, unsigned long len) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(0L), "D"(fd), "S"(buf), "d"(len)
                   : "rcx", "r11", "memory");
  return result;
}

static long goc_qjs_cli_close_syscall(long fd) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(3L), "D"(fd)
                   : "rcx", "r11", "memory");
  return result;
}

static long goc_qjs_cli_getcwd_syscall(char *buf, unsigned long len) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(79L), "D"(buf), "S"(len)
                   : "rcx", "r11", "memory");
  return result;
}

static size_t goc_qjs_cli_strlen(const char *str) {
  size_t len = 0;
  while (str[len] != '\0')
    len++;
  return len;
}

static int goc_qjs_cli_starts_with(const char *str, const char *prefix) {
  while (*prefix != '\0') {
    if (*str++ != *prefix++)
      return 0;
  }
  return 1;
}

/* Resolve only relative filesystem specifiers. Bare module names (including
 * qjs: names) remain untouched so the loader can issue a meaningful error. */
static char *goc_qjs_cli_normalize(JSContext *ctx, const char *base_name,
                                   const char *module_name, void *opaque) {
  (void)opaque;
  if (module_name[0] != '.' && module_name[0] != '/')
    return js_strdup(ctx, module_name);

  size_t name_len = goc_qjs_cli_strlen(module_name);
  size_t base_dir_len = 0;
  int use_base = module_name[0] != '/';
  if (use_base) {
    size_t base_len = goc_qjs_cli_strlen(base_name);
    size_t slash = base_len;
    while (slash != 0 && base_name[slash - 1] != '/')
      slash--;
    if (slash != 0) {
      base_dir_len = slash - 1;
      if (base_dir_len == 0 && base_name[0] == '/')
        base_dir_len = 1;
    }
  }
  if (base_dir_len > (size_t)-1 - name_len - 2) {
    JS_ThrowInternalError(ctx, "module path is too long");
    return NULL;
  }

  size_t input_len = base_dir_len + name_len + (use_base && base_dir_len != 0 ? 1 : 0);
  char *input = js_malloc(ctx, input_len + 1);
  if (!input)
    return NULL;
  size_t at = 0;
  if (base_dir_len != 0) {
    for (size_t i = 0; i < base_dir_len; i++)
      input[at++] = base_name[i];
  }
  if (use_base && base_dir_len != 0 && input[at - 1] != '/')
    input[at++] = '/';
  for (size_t i = 0; i < name_len; i++)
    input[at++] = module_name[i];
  input[at] = '\0';

  char *normalized = js_malloc(ctx, input_len + 2);
  if (!normalized) {
    js_free(ctx, input);
    return NULL;
  }

  int absolute = input[0] == '/';
  size_t out_len = 0;
  if (absolute)
    normalized[out_len++] = '/';
  size_t i = 0;
  while (i < at) {
    while (i < at && input[i] == '/')
      i++;
    size_t start = i;
    while (i < at && input[i] != '/')
      i++;
    size_t part_len = i - start;
    if (part_len == 0 || (part_len == 1 && input[start] == '.'))
      continue;
    if (part_len == 2 && input[start] == '.' && input[start + 1] == '.') {
      size_t previous = out_len;
      while (previous > (absolute ? 1u : 0u) &&
             normalized[previous - 1] != '/')
        previous--;
      size_t previous_len = out_len - previous;
      int previous_is_parent = previous_len == 2 &&
                               normalized[previous] == '.' &&
                               normalized[previous + 1] == '.';
      if (previous > (absolute ? 1u : 0u) && !previous_is_parent) {
        out_len = previous == 0 ? 0 : previous - 1;
      } else if (!absolute) {
        if (out_len != 0)
          normalized[out_len++] = '/';
        normalized[out_len++] = '.';
        normalized[out_len++] = '.';
      }
      continue;
    }
    if (out_len != 0 && normalized[out_len - 1] != '/')
      normalized[out_len++] = '/';
    for (size_t j = 0; j < part_len; j++)
      normalized[out_len++] = input[start + j];
  }
  if (out_len == 0)
    normalized[out_len++] = '.';
  normalized[out_len] = '\0';
  js_free(ctx, input);
  return normalized;
}

/* Load without stdio: this callback can run on a Go goroutine stack, where
 * libc file I/O is not safe. The source buffer uses QuickJS's allocator. */
static char *goc_qjs_cli_load_source(JSContext *ctx, const char *filename,
                                     size_t *source_len) {
  long fd = goc_qjs_cli_open_syscall(filename);
  if (fd < 0)
    return NULL;

  size_t capacity = 8192;
  size_t used = 0;
  char *buf = js_malloc(ctx, capacity);
  if (!buf) {
    goc_qjs_cli_close_syscall(fd);
    return NULL;
  }

  for (;;) {
    if (capacity - used <= 1) {
      if (capacity > (size_t)-1 / 2) {
        js_free(ctx, buf);
        goc_qjs_cli_close_syscall(fd);
        JS_ThrowInternalError(ctx, "module source is too large");
        return NULL;
      }
      size_t new_capacity = capacity * 2;
      char *new_buf = js_realloc(ctx, buf, new_capacity);
      if (!new_buf) {
        js_free(ctx, buf);
        goc_qjs_cli_close_syscall(fd);
        return NULL;
      }
      buf = new_buf;
      capacity = new_capacity;
    }

    size_t available = capacity - used - 1;
    if (available > 0x7ffff000UL)
      available = 0x7ffff000UL;
    long nread = goc_qjs_cli_read_syscall(fd, buf + used,
                                         (unsigned long)available);
    if (nread == -4) /* EINTR */
      continue;
    if (nread < 0) {
      js_free(ctx, buf);
      goc_qjs_cli_close_syscall(fd);
      return NULL;
    }
    if (nread == 0)
      break;
    used += (size_t)nread;
  }

  buf[used] = '\0';
  goc_qjs_cli_close_syscall(fd);
  *source_len = used;
  return buf;
}

static char *goc_qjs_cli_getcwd(JSContext *ctx) {
  size_t capacity = 256;
  char *buf = js_malloc(ctx, capacity);
  if (!buf)
    return NULL;
  for (;;) {
    long result = goc_qjs_cli_getcwd_syscall(buf, (unsigned long)capacity);
    if (result >= 0)
      return buf;
    if (result != -34 || capacity > (size_t)-1 / 2) { /* ERANGE */
      js_free(ctx, buf);
      return NULL;
    }
    size_t new_capacity = capacity * 2;
    char *new_buf = js_realloc(ctx, buf, new_capacity);
    if (!new_buf) {
      js_free(ctx, buf);
      return NULL;
    }
    buf = new_buf;
    capacity = new_capacity;
  }
}

static int goc_qjs_cli_set_import_meta(JSContext *ctx, JSValueConst module) {
  JSModuleDef *module_def = JS_VALUE_GET_PTR(module);
  JSAtom name_atom = JS_GetModuleName(ctx, module_def);
  const char *name = JS_AtomToCString(ctx, name_atom);
  JS_FreeAtom(ctx, name_atom);
  if (!name)
    return -1;

  size_t name_len = goc_qjs_cli_strlen(name);
  char *cwd = NULL;
  if (name[0] != '/') {
    cwd = goc_qjs_cli_getcwd(ctx);
    if (!cwd) {
      JS_FreeCString(ctx, name);
      return JS_HasException(ctx) ? -1 : 0;
    }
  }
  size_t cwd_len = cwd ? goc_qjs_cli_strlen(cwd) : 0;
  int add_slash = cwd && cwd_len != 0 && cwd[cwd_len - 1] != '/';
  if (cwd_len > (size_t)-1 - name_len - (size_t)add_slash) {
    if (cwd)
      js_free(ctx, cwd);
    JS_FreeCString(ctx, name);
    JS_ThrowInternalError(ctx, "module URL is too long");
    return -1;
  }

  size_t path_len = cwd ? cwd_len + (size_t)add_slash + name_len : name_len;
  if (path_len > ((size_t)-1 - 8) / 3) {
    if (cwd)
      js_free(ctx, cwd);
    JS_FreeCString(ctx, name);
    JS_ThrowInternalError(ctx, "module URL is too long");
    return -1;
  }
  char *url = js_malloc(ctx, 8 + path_len * 3 + 1);
  if (!url) {
    if (cwd)
      js_free(ctx, cwd);
    JS_FreeCString(ctx, name);
    return -1;
  }

  static const char hex[] = "0123456789ABCDEF";
  size_t out = 0;
  const char prefix[] = "file://";
  for (size_t i = 0; i < sizeof(prefix) - 1; i++)
    url[out++] = prefix[i];
  for (size_t i = 0; i < path_len; i++) {
    char ch;
    if (cwd && i < cwd_len)
      ch = cwd[i];
    else if (add_slash && i == cwd_len)
      ch = '/';
    else
      ch = name[i - (cwd ? cwd_len + (size_t)add_slash : 0)];
    unsigned char byte = (unsigned char)ch;
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '/' || byte == '-' ||
        byte == '.' || byte == '_' || byte == '~') {
      url[out++] = ch;
    } else {
      url[out++] = '%';
      url[out++] = hex[byte >> 4];
      url[out++] = hex[byte & 15];
    }
  }
  url[out] = '\0';

  JSValue meta = JS_GetImportMeta(ctx, module_def);
  if (JS_IsException(meta)) {
    js_free(ctx, url);
    if (cwd)
      js_free(ctx, cwd);
    JS_FreeCString(ctx, name);
    return -1;
  }
  JSValue url_value = JS_NewStringLen(ctx, url, out);
  js_free(ctx, url);
  if (cwd)
    js_free(ctx, cwd);
  JS_FreeCString(ctx, name);
  if (JS_IsException(url_value)) {
    JS_FreeValue(ctx, meta);
    return -1;
  }
  if (JS_DefinePropertyValueStr(ctx, meta, "url", url_value, JS_PROP_C_W_E) < 0 ||
      JS_DefinePropertyValueStr(ctx, meta, "main", JS_FALSE, JS_PROP_C_W_E) < 0) {
    JS_FreeValue(ctx, meta);
    return -1;
  }
  JS_FreeValue(ctx, meta);
  return 0;
}

static int goc_qjs_cli_check_attrs(JSContext *ctx, void *opaque,
                                   JSValueConst attributes) {
  (void)opaque;
  JSPropertyEnum *properties;
  uint32_t count;
  if (JS_GetOwnPropertyNames(ctx, &properties, &count, attributes,
                             JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0)
    return -1;
  int status = 0;
  for (uint32_t i = 0; i < count; i++) {
    size_t length;
    const char *key = JS_AtomToCStringLen(ctx, &length, properties[i].atom);
    if (!key) {
      status = -1;
      break;
    }
    if (length != 4 || key[0] != 't' || key[1] != 'y' ||
        key[2] != 'p' || key[3] != 'e') {
      JS_ThrowTypeError(ctx, "import attribute '%s' is not supported", key);
      status = -1;
    }
    JS_FreeCString(ctx, key);
    if (status < 0)
      break;
  }
  JS_FreePropertyEnum(ctx, properties, count);
  return status;
}

static int goc_qjs_cli_import_type(JSContext *ctx, JSValueConst attributes) {
  if (JS_IsUndefined(attributes))
    return 0;
  JSValue type = JS_GetPropertyStr(ctx, attributes, "type");
  if (JS_IsException(type))
    return -1;
  if (!JS_IsString(type)) {
    JS_FreeValue(ctx, type);
    return 0;
  }
  size_t length;
  const char *name = JS_ToCStringLen(ctx, &length, type);
  JS_FreeValue(ctx, type);
  if (!name)
    return -1;
  int result = -1;
  if (length == 4 && goc_qjs_cli_starts_with(name, "json"))
    result = 1;
  else if (length == 4 && goc_qjs_cli_starts_with(name, "text"))
    result = 2;
  else if (length == 5 && goc_qjs_cli_starts_with(name, "bytes"))
    result = 3;
  else
    JS_ThrowTypeError(ctx, "unsupported module type: '%s'", name);
  JS_FreeCString(ctx, name);
  return result;
}

static int goc_qjs_cli_default_module_init(JSContext *ctx, JSModuleDef *module) {
  JSValue value = JS_GetModulePrivateValue(ctx, module);
  return JS_SetModuleExport(ctx, module, "default", value);
}

static void *goc_qjs_cli_realloc_arraybuffer(JSRuntime *rt, void *opaque,
                                              void *ptr, size_t size) {
  (void)opaque;
  return js_realloc_rt(rt, ptr, size);
}

static JSModuleDef *goc_qjs_cli_module_loader(JSContext *ctx,
                                              const char *module_name,
                                              void *opaque,
                                              JSValueConst attributes) {
  (void)opaque;
  if (goc_qjs_cli_starts_with(module_name, "qjs:")) {
    JS_ThrowReferenceError(ctx, "unsupported module '%s'", module_name);
    return NULL;
  }

  int type = goc_qjs_cli_import_type(ctx, attributes);
  if (type < 0)
    return NULL;
  size_t name_len = goc_qjs_cli_strlen(module_name);
  if (type == 0 && name_len >= 5 &&
      module_name[name_len - 5] == '.' && module_name[name_len - 4] == 'j' &&
      module_name[name_len - 3] == 's' && module_name[name_len - 2] == 'o' &&
      module_name[name_len - 1] == 'n')
    type = 1;
  size_t source_len;
  char *source = goc_qjs_cli_load_source(ctx, module_name, &source_len);
  if (!source) {
    if (!JS_HasException(ctx))
      JS_ThrowReferenceError(ctx, "could not load module filename '%s'",
                             module_name);
    return NULL;
  }
  JSValue module;
  if (type == 0)
    module = JS_Eval(ctx, source, source_len, module_name,
                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  else if (type == 1)
    module = JS_ParseJSON(ctx, source, source_len, module_name);
  else if (type == 2)
    module = JS_NewStringLen(ctx, source, source_len);
  else {
    module = JS_NewUint8Array(ctx, (uint8_t *)source, source_len,
                               goc_qjs_cli_realloc_arraybuffer, NULL, false);
    if (!JS_IsException(module)) {
      JSValue buffer = JS_GetTypedArrayBuffer(ctx, module, NULL, NULL, NULL);
      if (JS_IsException(buffer) || JS_SetImmutableArrayBuffer(buffer, true) < 0) {
        JS_FreeValue(ctx, buffer);
        JS_FreeValue(ctx, module);
        module = JS_EXCEPTION;
      } else {
        JS_FreeValue(ctx, buffer);
      }
      source = NULL;
    }
  }
  js_free(ctx, source);
  if (JS_IsException(module))
    return NULL;

  if (type == 0) {
    if (goc_qjs_cli_set_import_meta(ctx, module) < 0) {
      JS_FreeValue(ctx, module);
      return NULL;
    }
    JSModuleDef *module_def = JS_VALUE_GET_PTR(module);
    JS_FreeValue(ctx, module);
    return module_def;
  }
  JSModuleDef *module_def = JS_NewCModule(ctx, module_name,
                                           goc_qjs_cli_default_module_init);
  if (!module_def || JS_AddModuleExport(ctx, module_def, "default") < 0) {
    JS_FreeValue(ctx, module);
    return NULL;
  }
  JS_SetModulePrivateValue(ctx, module_def, module);
  return module_def;
}

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
  BJSON_FLAG(WRITE_OBJ_STRIP_DEBUG),
  BJSON_FLAG(WRITE_OBJ_STRIP_SOURCE),
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
  return JS_NewInt32(ctx, (int32_t)js_std_cmd(3, ctx, &argv[0]));
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
                                            JSValueConst reason, bool is_handled,
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

  JSValue gc = JS_NewCFunction(ctx, goc_qjs_cli_gc, "gc", 0);
  if (JS_IsException(gc)) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  if (JS_SetPropertyStr(ctx, global, "gc", gc) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }

  JSModuleDef *std = JS_NewCModule(ctx, "qjs:std", goc_qjs_cli_std_init);
  if (!std || JS_AddModuleExport(ctx, std, "gc") < 0 ||
      JS_AddModuleExport(ctx, std, "evalScript") < 0 ||
      goc_qjs_cli_std_extra_add(ctx, std) < 0) {
    JS_FreeValue(ctx, global);
    return -1;
  }
  JSModuleDef *os = JS_NewCModule(ctx, "qjs:os", goc_qjs_cli_os_init);
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
