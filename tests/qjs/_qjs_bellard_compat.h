/* quickjs-ng API used by the goc CLI host (tests/qjs/_qjs_cli_*.c), mapped
 * onto Bellard's QuickJS 2026-06-04 public API. Included right after
 * third_party/quickjs-bellard/quickjs.h, only for QJS_FLAVOR=bellard
 * (-DGOC_QJS_BELLARD). Nothing here changes the ng build.
 *
 * Known gaps (documented in docs/benchmark.md):
 *  - JS_SetImmutableArrayBuffer: Bellard has no immutable ArrayBuffers, so
 *    `import ... with { type: "bytes" }` yields a mutable Uint8Array.
 *  - JS_WRITE_OBJ_STRIP_DEBUG/SOURCE do not exist; qjs:bjson omits them.
 *  - qjs.getStringKind (ng-internal js_std_cmd) is not provided.
 *  - JS_AddRuntimeFinalizer: emulated in _qjs_bellard_api.c; finalizers run
 *    at the start of JS_FreeRuntime instead of after its final GC.
 */
#ifndef GOC_QJS_BELLARD_COMPAT_H
#define GOC_QJS_BELLARD_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ng: JSHostPromiseRejectionTracker takes `bool is_handled`; Bellard JS_BOOL. */
#define GOC_QJS_TRACKER_BOOL JS_BOOL

/* ng: JS_NewClassID(rt, &id); Bellard: JS_NewClassID(&id). */
#define JS_NewClassID(rt, pid) ((void)(rt), (JS_NewClassID)(pid))

/* ng: JS_IsBigInt(v); Bellard: JS_IsBigInt(ctx, v) (ctx unused). */
#define JS_IsBigInt(v) (JS_IsBigInt)(NULL, (v))

/* ng: int JS_SetOpaque(obj, p) (always 0 for objects); Bellard returns void. */
#define JS_SetOpaque(obj, p) ((JS_SetOpaque)((obj), (p)), 0)

/* ng: JS_ThrowPlainError throws a plain Error. Bellard has no public plain
 * Error thrower: format through JS_ThrowInternalError, then rethrow the
 * message as `new Error(message)`. */
static inline JSValue goc_bellard_rethrow_plain(JSContext *ctx, JSValue ignored) {
  (void)ignored;
  JSValue exc = JS_GetException(ctx);
  JSValue msg = JS_GetPropertyStr(ctx, exc, "message");
  JS_FreeValue(ctx, exc);
  JSValue err = JS_NewError(ctx);
  if (JS_IsException(err)) {
    JS_FreeValue(ctx, msg);
    return JS_EXCEPTION;
  }
  JS_DefinePropertyValueStr(ctx, err, "message", msg,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  return JS_Throw(ctx, err);
}
#define JS_ThrowPlainError(ctx, ...) \
  goc_bellard_rethrow_plain((ctx), JS_ThrowInternalError((ctx), __VA_ARGS__))

/* ng: JS_ToObjectString(ctx, v) == Object.prototype.toString.call(v). */
static inline JSValue JS_ToObjectString(JSContext *ctx, JSValueConst val) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue object = JS_GetPropertyStr(ctx, global, "Object");
  JS_FreeValue(ctx, global);
  JSValue proto = JS_GetPropertyStr(ctx, object, "prototype");
  JS_FreeValue(ctx, object);
  JSValue fn = JS_GetPropertyStr(ctx, proto, "toString");
  JS_FreeValue(ctx, proto);
  if (JS_IsException(fn))
    return fn;
  JSValue result = JS_Call(ctx, fn, val, 0, NULL);
  JS_FreeValue(ctx, fn);
  return result;
}

/* ng: JS_NewArrayFrom takes ownership of values[0..count). */
static inline JSValue JS_NewArrayFrom(JSContext *ctx, int count,
                                      const JSValue *values) {
  JSValue array = JS_NewArray(ctx);
  int i = 0;
  if (JS_IsException(array))
    goto fail;
  for (; i < count; i++) {
    if (JS_SetPropertyUint32(ctx, array, (uint32_t)i, values[i]) < 0) {
      i++;
      JS_FreeValue(ctx, array);
      goto fail;
    }
  }
  return array;
fail:
  for (; i < count; i++)
    JS_FreeValue(ctx, values[i]);
  return JS_EXCEPTION;
}

/* ng: JS_GetLength(ctx, obj, &len) = ToLength(obj.length). */
static inline int JS_GetLength(JSContext *ctx, JSValueConst obj, int64_t *pres) {
  JSValue len = JS_GetPropertyStr(ctx, obj, "length");
  if (JS_IsException(len))
    return -1;
  /* JS_ToInt64 saturates; clamp to [0, 2^53-1] like ToLength. */
  int r = JS_ToInt64(ctx, pres, len);
  JS_FreeValue(ctx, len);
  if (r == 0) {
    if (*pres < 0)
      *pres = 0;
    else if (*pres > ((int64_t)1 << 53) - 1)
      *pres = ((int64_t)1 << 53) - 1;
  }
  return r;
}

static inline JSValue JS_GetPropertyInt64(JSContext *ctx, JSValueConst obj,
                                          int64_t idx) {
  if (idx >= 0 && idx <= 0xfffffffe)
    return JS_GetPropertyUint32(ctx, obj, (uint32_t)idx);
  JSValue key = JS_NewInt64(ctx, idx);
  JSAtom atom = JS_ValueToAtom(ctx, key);
  JS_FreeValue(ctx, key);
  if (atom == JS_ATOM_NULL)
    return JS_EXCEPTION;
  JSValue v = JS_GetProperty(ctx, obj, atom);
  JS_FreeAtom(ctx, atom);
  return v;
}

static inline JSValue goc_bellard_uint8_view(JSContext *ctx, JSValue buffer) {
  if (JS_IsException(buffer))
    return buffer;
  JSValue view = JS_NewTypedArray(ctx, 1, &buffer, JS_TYPED_ARRAY_UINT8);
  JS_FreeValue(ctx, buffer);
  return view;
}

static inline JSValue JS_NewUint8ArrayCopy(JSContext *ctx, const uint8_t *buf,
                                           size_t len) {
  return goc_bellard_uint8_view(ctx, JS_NewArrayBufferCopy(ctx, buf, len));
}

/* ng: Uint8Array over a caller buffer released through realloc_func(.., 0).
 * Bellard's external ArrayBuffers take a free function with a different
 * signature, so copy and release the source on success (ownership moves to
 * the result, as with ng). */
typedef void *GocQjsReallocFunc(JSRuntime *rt, void *opaque, void *ptr,
                                size_t size);
static inline JSValue JS_NewUint8Array(JSContext *ctx, uint8_t *buf, size_t len,
                                       GocQjsReallocFunc *realloc_func,
                                       void *opaque, bool is_shared) {
  (void)is_shared;
  JSValue view = JS_NewUint8ArrayCopy(ctx, buf, len);
  if (!JS_IsException(view) && realloc_func)
    realloc_func(JS_GetRuntime(ctx), opaque, buf, 0);
  return view;
}

static inline int JS_SetImmutableArrayBuffer(JSValueConst buffer, bool immutable) {
  (void)buffer;
  (void)immutable;
  return 0;
}

/* ng: JS_IsArrayBuffer(v). Bellard exposes class ids only through
 * JS_GetClassID; learn the ArrayBuffer id from a probe object once. */
static inline bool goc_bellard_is_array_buffer(JSContext *ctx, JSValueConst v) {
  static JSClassID ab_class_id;
  if (!JS_IsObject(v))
    return false;
  if (ab_class_id == 0) {
    JSValue probe = JS_NewArrayBufferCopy(ctx, NULL, 0);
    if (JS_IsException(probe)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return false;
    }
    ab_class_id = JS_GetClassID(probe);
    JS_FreeValue(ctx, probe);
  }
  return JS_GetClassID(v) == ab_class_id;
}
#define JS_IsArrayBuffer(v) goc_bellard_is_array_buffer(ctx, (v))

/* ng: SharedArrayBuffer table for JS_WriteObject2 / JS_ReadObject2. */
typedef struct JSSABTab {
  uint8_t **tab;
  size_t len;
} JSSABTab;
static inline uint8_t *goc_bellard_write_object2(JSContext *ctx, size_t *psize,
                                                 JSValueConst obj, int flags,
                                                 JSSABTab *sab_tab) {
  return (JS_WriteObject2)(ctx, psize, obj, flags, &sab_tab->tab,
                           &sab_tab->len);
}
#define JS_WriteObject2(ctx, psize, obj, flags, sab) \
  goc_bellard_write_object2((ctx), (psize), (obj), (flags), (sab))
static inline JSValue JS_ReadObject2(JSContext *ctx, const uint8_t *buf,
                                     size_t buf_len, int flags,
                                     JSSABTab *sab_tab) {
  sab_tab->tab = NULL;
  sab_tab->len = 0;
  return JS_ReadObject(ctx, buf, buf_len, flags);
}

/* ng: JS_AddRuntimeFinalizer (emulated in _qjs_bellard_api.c). */
typedef void JSRuntimeFinalizer(JSRuntime *rt, void *arg);
int JS_AddRuntimeFinalizer(JSRuntime *rt, JSRuntimeFinalizer *finalizer,
                           void *arg);

#endif /* GOC_QJS_BELLARD_COMPAT_H */
