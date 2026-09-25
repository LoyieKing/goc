/* Linux/amd64 std/os surface for the goc-built QuickJS CLI.
 *
 * The C engine runs on moving Go goroutine stacks.  This file deliberately
 * uses raw Linux syscalls for filesystem/descriptor operations and crosses to
 * Go only for process creation/waiting and C-runtime-free float formatting.
 */
#include "../../third_party/quickjs-ng/quickjs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "qjs cli std/os bridge requires Linux/amd64"
#endif

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 00200000
#endif
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define GOC_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define GOC_AT_FDCWD (-100L)
#define GOC_FILE_PATH_LIMIT 65536u

/* Raw syscall wrappers: Linux returns -errno directly in RAX. */
static long goc_qjs_cli_syscall0(long nr) {
  long result;
  __asm__ volatile("syscall" : "=a"(result) : "a"(nr)
                   : "rcx", "r11", "memory");
  return result;
}
static long goc_qjs_cli_syscall1(long nr, long a1) {
  long result;
  __asm__ volatile("syscall" : "=a"(result) : "a"(nr), "D"(a1)
                   : "rcx", "r11", "memory");
  return result;
}
static long goc_qjs_cli_syscall2(long nr, long a1, long a2) {
  long result;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(nr), "D"(a1), "S"(a2)
                   : "rcx", "r11", "memory");
  return result;
}
static long goc_qjs_cli_syscall3(long nr, long a1, long a2, long a3) {
  long result;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                   : "rcx", "r11", "memory");
  return result;
}
static long goc_qjs_cli_syscall4(long nr, long a1, long a2, long a3, long a4) {
  long result;
  register long r10 __asm__("r10") = a4;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                   : "rcx", "r11", "memory");
  return result;
}

static size_t goc_qjs_cli_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}
static int goc_qjs_cli_write_all(int fd, const void *buffer, size_t length) {
  const uint8_t *p = (const uint8_t *)buffer;
  size_t done = 0;
  while (done < length) {
    size_t amount = length - done;
    if (amount > 0x7ffff000u) amount = 0x7ffff000u;
    long ret = goc_qjs_cli_syscall3(1, fd, (long)(p + done), (long)amount);
    if (ret == -EINTR) continue;
    if (ret < 0) return (int)ret;
    if (ret == 0) return -EIO;
    done += (size_t)ret;
  }
  return (int)done;
}
static JSValue goc_qjs_cli_tuple(JSContext *ctx, JSValue first, int32_t second) {
  if (JS_IsException(first)) return first;
  JSValue items[2] = { first, JS_NewInt32(ctx, second) };
  return JS_NewArrayFrom(ctx, 2, items);
}
static int goc_qjs_cli_set_error(JSContext *ctx, JSValueConst object,
                                 int32_t error) {
  if (JS_IsUndefined(object)) return 0;
  return JS_SetPropertyStr(ctx, object, "errno", JS_NewInt32(ctx, error));
}
static int32_t goc_qjs_cli_sys_error(long result) {
  return result < 0 ? (int32_t)result : 0;
}
static int32_t goc_qjs_cli_errno(long result) {
  return result < 0 ? (int32_t)-result : 0;
}
static long goc_qjs_cli_random_bytes(uint8_t *buffer, size_t length) {
  size_t done = 0;
  while (done < length) {
    long result = goc_qjs_cli_syscall3(318, (long)(buffer + done),
                                      (long)(length - done), 0);
    if (result == -EINTR) continue;
    if (result < 0) return result;
    if (result == 0) return -EIO;
    done += (size_t)result;
  }
  return (long)done;
}

/* ----- Go process bridge; layout mirrored by tests/qjscli/std_os.go. ----- */
typedef struct GocQjsCliProcessRequest {
  int32_t op;
  int32_t pid;
  int32_t block;
  int32_t options;
  int32_t use_path;
  int32_t stdin_fd;
  int32_t stdout_fd;
  int32_t stderr_fd;
  uint32_t argc;
  uint32_t _pad0;
  const char **argv;
  uint32_t envc;
  uint32_t _pad1;
  const char **envp;
  const char *file;
  const char *cwd;
  uint32_t uid;
  uint32_t gid;
  uint32_t groups_len;
  int32_t uid_set;
  int32_t gid_set;
  int32_t groups_set;
  const uint32_t *groups;
  int32_t result;
  int32_t status;
} GocQjsCliProcessRequest;

typedef struct GocQjsCliFloatRequest {
  const char *format;
  double value;
  char *output;
  uint64_t capacity;
  int64_t length;
} GocQjsCliFloatRequest;

typedef struct GocQjsCliStrerrorRequest {
  int32_t code;
  uint32_t _pad;
  char *output;
  uint64_t capacity;
  int64_t length;
} GocQjsCliStrerrorRequest;

_Static_assert(sizeof(GocQjsCliProcessRequest) == 120 &&
               offsetof(GocQjsCliProcessRequest, argv) == 40 &&
               offsetof(GocQjsCliProcessRequest, groups) == 104 &&
               offsetof(GocQjsCliProcessRequest, result) == 112,
               "Go process bridge layout mismatch");
_Static_assert(sizeof(GocQjsCliFloatRequest) == 40 &&
               offsetof(GocQjsCliFloatRequest, output) == 16,
               "Go float bridge layout mismatch");
_Static_assert(sizeof(GocQjsCliStrerrorRequest) == 32 &&
               offsetof(GocQjsCliStrerrorRequest, output) == 8,
               "Go strerror bridge layout mismatch");

static void goc_qjs_cli_call_go_process(GocQjsCliProcessRequest *request) {
  __asm__ volatile(
      "movq %0, %%rax\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoQjsCliProcess.goabi"
      : : "m"(request)
      : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
        "r10", "r11", "r12", "r13", "r14", "r15",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
        "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15", "cc", "memory");
}
static void goc_qjs_cli_call_go_float(GocQjsCliFloatRequest *request) {
  __asm__ volatile(
      "movq %0, %%rax\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoQjsCliFloat.goabi"
      : : "m"(request)
      : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
        "r10", "r11", "r12", "r13", "r14", "r15",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
        "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15", "cc", "memory");
}
static void goc_qjs_cli_call_go_strerror(GocQjsCliStrerrorRequest *request) {
  __asm__ volatile(
      "movq %0, %%rax\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoQjsCliStrerror.goabi"
      : : "m"(request)
      : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
        "r10", "r11", "r12", "r13", "r14", "r15",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
        "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15", "cc", "memory");
}

/* ----- qjs:std formatted output and FILE objects. ----- */
typedef struct GocQjsCliFile {
  int fd;
  bool stdio;
  bool popen;
  bool eof;
  bool error;
  int32_t pid;
} GocQjsCliFile;

typedef struct GocQjsCliBuffer {
  JSContext *ctx;
  uint8_t *data;
  size_t length;
  size_t capacity;
} GocQjsCliBuffer;

static JSClassID goc_qjs_cli_file_class_id;

static int goc_qjs_cli_buffer_reserve(GocQjsCliBuffer *buffer, size_t extra) {
  if (extra > SIZE_MAX - buffer->length) return -1;
  size_t needed = buffer->length + extra;
  if (needed <= buffer->capacity) return 0;
  size_t capacity = buffer->capacity ? buffer->capacity : 128;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
    capacity *= 2;
  }
  uint8_t *data = js_realloc(buffer->ctx, buffer->data, capacity);
  if (!data) return -1;
  buffer->data = data;
  buffer->capacity = capacity;
  return 0;
}
static int goc_qjs_cli_buffer_append(GocQjsCliBuffer *buffer,
                                     const void *data, size_t length) {
  if (goc_qjs_cli_buffer_reserve(buffer, length) < 0) return -1;
  const uint8_t *source = (const uint8_t *)data;
  for (size_t i = 0; i < length; i++)
    buffer->data[buffer->length + i] = source[i];
  buffer->length += length;
  return 0;
}
static int goc_qjs_cli_buffer_repeat(GocQjsCliBuffer *buffer,
                                     uint8_t byte, size_t count) {
  if (goc_qjs_cli_buffer_reserve(buffer, count) < 0) return -1;
  for (size_t i = 0; i < count; i++) buffer->data[buffer->length++] = byte;
  return 0;
}
static void goc_qjs_cli_buffer_free(GocQjsCliBuffer *buffer) {
  js_free(buffer->ctx, buffer->data);
  buffer->data = NULL;
  buffer->length = buffer->capacity = 0;
}
static int goc_qjs_cli_is_digit(uint8_t c) { return c >= '0' && c <= '9'; }
static int goc_qjs_cli_parse_number(const uint8_t **cursor, int *value) {
  int result = 0;
  const uint8_t *p = *cursor;
  while (goc_qjs_cli_is_digit(*p)) {
    int digit = *p++ - '0';
    if (result > (INT_MAX - digit) / 10) return -1;
    result = result * 10 + digit;
  }
  *cursor = p;
  *value = result;
  return 0;
}
static int goc_qjs_cli_utf8(GocQjsCliBuffer *buffer, uint32_t c) {
  uint8_t out[4];
  size_t length;
  if (c <= 0x7f) { out[0] = (uint8_t)c; length = 1; }
  else if (c <= 0x7ff) {
    out[0] = (uint8_t)(0xc0 | (c >> 6));
    out[1] = (uint8_t)(0x80 | (c & 0x3f)); length = 2;
  } else if (c <= 0xffff && !(c >= 0xd800 && c <= 0xdfff)) {
    out[0] = (uint8_t)(0xe0 | (c >> 12));
    out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
    out[2] = (uint8_t)(0x80 | (c & 0x3f)); length = 3;
  } else if (c <= 0x10ffff) {
    out[0] = (uint8_t)(0xf0 | (c >> 18));
    out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (uint8_t)(0x80 | (c & 0x3f)); length = 4;
  } else {
    out[0] = 0xef; out[1] = 0xbf; out[2] = 0xbd; length = 3;
  }
  return goc_qjs_cli_buffer_append(buffer, out, length);
}
static uint32_t goc_qjs_cli_first_codepoint(const uint8_t *s) {
  uint32_t c = s[0];
  if (c < 0x80 || c == 0) return c;
  if ((c & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80)
    return ((c & 0x1f) << 6) | (s[1] & 0x3f);
  if ((c & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 &&
      (s[2] & 0xc0) == 0x80)
    return ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
  if ((c & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
      (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80)
    return ((c & 7) << 18) | ((s[1] & 0x3f) << 12) |
           ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
  return 0xfffd;
}
static int goc_qjs_cli_add_integer(GocQjsCliBuffer *output, int64_t value,
                                   uint32_t raw, bool is_signed,
                                   bool is_long, char conversion,
                                   bool left, bool plus, bool space,
                                   bool alternate, bool zero, int width,
                                   int precision) {
  unsigned base = (conversion == 'o') ? 8 :
                  (conversion == 'x' || conversion == 'X') ? 16 : 10;
  bool upper = conversion == 'X';
  uint64_t magnitude;
  bool negative = false;
  if (is_signed) {
    int64_t number = is_long ? value : (int32_t)raw;
    negative = number < 0;
    magnitude = negative ? (uint64_t)(-(number + 1)) + 1 : (uint64_t)number;
  } else {
    magnitude = is_long ? (uint64_t)value : (uint32_t)raw;
  }
  char digits[65];
  int digit_count = 0;
  const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (magnitude == 0) {
    if (precision != 0) digits[digit_count++] = '0';
  } else {
    while (magnitude != 0) {
      digits[digit_count++] = alphabet[magnitude % base];
      magnitude /= base;
    }
  }
  int min_digits = precision < 0 ? 1 : precision;
  if (conversion == 'o' && alternate &&
      (digit_count == 0 || digits[digit_count - 1] != '0') && min_digits <= digit_count)
    min_digits = digit_count + 1;
  bool prefix_hex = alternate && base == 16 && digit_count != 0;
  char sign = negative ? '-' : plus ? '+' : space ? ' ' : 0;
  int zeros = min_digits > digit_count ? min_digits - digit_count : 0;
  int body = digit_count + zeros + (prefix_hex ? 2 : 0) + (sign != 0);
  int padding = width > body ? width - body : 0;
  if (zero && !left && precision < 0) {
    zeros += padding;
    padding = 0;
  }
  if (!left && goc_qjs_cli_buffer_repeat(output, ' ', (size_t)padding) < 0) return -1;
  if (sign && goc_qjs_cli_buffer_append(output, &sign, 1) < 0) return -1;
  if (prefix_hex) {
    const char prefix[2] = { '0', upper ? 'X' : 'x' };
    if (goc_qjs_cli_buffer_append(output, prefix, 2) < 0) return -1;
  }
  if (goc_qjs_cli_buffer_repeat(output, '0', (size_t)zeros) < 0) return -1;
  while (digit_count > 0) {
    uint8_t c = (uint8_t)digits[--digit_count];
    if (goc_qjs_cli_buffer_append(output, &c, 1) < 0) return -1;
  }
  if (left && goc_qjs_cli_buffer_repeat(output, ' ', (size_t)padding) < 0) return -1;
  return 0;
}
static int goc_qjs_cli_add_float(JSContext *ctx, GocQjsCliBuffer *output,
                                 double value, char conversion,
                                 bool left, bool plus, bool space,
                                 bool alternate, bool zero, int width,
                                 int precision) {
  if (width > 65536 || precision > 65536) return -3;
  char format[64];
  size_t at = 0;
  format[at++] = '%';
  if (left) format[at++] = '-';
  if (plus) format[at++] = '+';
  else if (space) format[at++] = ' ';
  if (alternate) format[at++] = '#';
  if (zero && !left) format[at++] = '0';
  if (width >= 0) {
    int n = width;
    char reversed[16]; int count = 0;
    do { reversed[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (count) format[at++] = reversed[--count];
  }
  if (precision >= 0) {
    format[at++] = '.';
    int n = precision;
    char reversed[16]; int count = 0;
    do { reversed[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (count) format[at++] = reversed[--count];
  }
  format[at++] = conversion == 'a' ? 'x' : conversion == 'A' ? 'X' : conversion;
  format[at] = '\0';
  size_t capacity = (size_t)(width > 0 ? width : 0) +
                    (size_t)(precision > 0 ? precision : 0) + 512;
  char *rendered = js_malloc(ctx, capacity);
  if (!rendered) return -1;
  GocQjsCliFloatRequest request = { format, value, rendered, capacity, -1 };
  goc_qjs_cli_call_go_float(&request);
  int ret = 0;
  if (request.length < 0 || (uint64_t)request.length > capacity) {
    ret = -1;
  } else if (goc_qjs_cli_buffer_append(output, rendered,
                                         (size_t)request.length) < 0) {
    ret = -1;
  }
  js_free(ctx, rendered);
  return ret;
}
static JSValue goc_qjs_cli_format(JSContext *ctx, int argc,
                                  JSValueConst *argv, int fd) {
  GocQjsCliBuffer output = { ctx, NULL, 0, 0 };
  const char *format = NULL;
  size_t format_length = 0;
  if (argc > 0) {
    format = JS_ToCStringLen(ctx, &format_length, argv[0]);
    if (!format) return JS_EXCEPTION;
  }
  const uint8_t *cursor = (const uint8_t *)format;
  const uint8_t *end = cursor ? cursor + format_length : cursor;
  int argument = 1;
  while (cursor && cursor < end) {
    const uint8_t *literal = cursor;
    while (cursor < end && *cursor != '%') cursor++;
    if (goc_qjs_cli_buffer_append(&output, literal,
                                  (size_t)(cursor - literal)) < 0)
      goto oom;
    if (cursor == end) break;
    cursor++;
    if (cursor < end && *cursor == '%') {
      if (goc_qjs_cli_buffer_append(&output, "%", 1) < 0) goto oom;
      cursor++;
      continue;
    }
    bool left = false, plus = false, space = false, alternate = false;
    bool zero = false;
    int width = -1, precision = -1;
    for (;;) {
      if (cursor >= end) goto invalid;
      switch (*cursor) {
        case '0': zero = true; cursor++; break;
        case '-': left = true; cursor++; break;
        case '+': plus = true; cursor++; break;
        case ' ': space = true; cursor++; break;
        case '#': alternate = true; cursor++; break;
        case '\'': cursor++; break;
        default: goto flags_done;
      }
    }
  flags_done:
    if (cursor < end && *cursor == '*') {
      if (argument >= argc) goto missing;
      int32_t n;
      if (JS_ToInt32(ctx, &n, argv[argument++])) goto fail;
      if (n < 0) { left = true; if (n == INT32_MIN) goto invalid; n = -n; }
      width = n;
      cursor++;
    } else if (cursor < end && goc_qjs_cli_is_digit(*cursor)) {
      if (goc_qjs_cli_parse_number(&cursor, &width) < 0 || width > 65536)
        goto invalid;
    }
    if (cursor < end && *cursor == '.') {
      cursor++;
      precision = 0;
      if (cursor < end && *cursor == '*') {
        if (argument >= argc) goto missing;
        int32_t n;
        if (JS_ToInt32(ctx, &n, argv[argument++])) goto fail;
        precision = n < 0 ? -1 : n;
        cursor++;
      } else if (cursor < end && goc_qjs_cli_is_digit(*cursor)) {
        if (goc_qjs_cli_parse_number(&cursor, &precision) < 0 || precision > 65536)
          goto invalid;
      }
    }
    if (width > 65536 || precision > 65536) goto invalid;
    bool is_long = false;
    if (cursor < end && *cursor == 'l') { is_long = true; cursor++; }
    if (cursor >= end) goto invalid;
    char conversion = (char)*cursor++;
    if (conversion == 'c') {
      if (argument >= argc) goto missing;
      uint32_t codepoint;
      if (JS_IsString(argv[argument])) {
        const char *s = JS_ToCString(ctx, argv[argument++]);
        if (!s) goto fail;
        codepoint = goc_qjs_cli_first_codepoint((const uint8_t *)s);
        JS_FreeCString(ctx, s);
      } else {
        int32_t n;
        if (JS_ToInt32(ctx, &n, argv[argument++])) goto fail;
        codepoint = (uint32_t)n;
      }
      if (codepoint > 0x10ffff) codepoint = 0xfffd;
      if (goc_qjs_cli_utf8(&output, codepoint) < 0) goto oom;
    } else if (conversion == 'd' || conversion == 'i' ||
               conversion == 'u' || conversion == 'o' ||
               conversion == 'x' || conversion == 'X') {
      if (argument >= argc) goto missing;
      int64_t number;
      if (JS_ToInt64Ext(ctx, &number, argv[argument++])) goto fail;
      uint32_t low = (uint32_t)number;
      bool signed_conversion = conversion == 'd' || conversion == 'i';
      if (goc_qjs_cli_add_integer(&output, number, low,
                                  signed_conversion, is_long, conversion,
                                  left, plus && signed_conversion,
                                  space && signed_conversion,
                                  alternate, zero, width, precision) < 0)
        goto oom;
    } else if (conversion == 's') {
      if (argument >= argc) goto missing;
      size_t length;
      const char *s = JS_ToCStringLen(ctx, &length, argv[argument++]);
      if (!s) goto fail;
      if (precision >= 0 && length > (size_t)precision)
        length = (size_t)precision;
      size_t padding = width > 0 && (size_t)width > length ?
                       (size_t)width - length : 0;
      int failed = (!left && goc_qjs_cli_buffer_repeat(&output, ' ', padding) < 0) ||
                   goc_qjs_cli_buffer_append(&output, s, length) < 0 ||
                   (left && goc_qjs_cli_buffer_repeat(&output, ' ', padding) < 0);
      JS_FreeCString(ctx, s);
      if (failed) goto oom;
    } else if (conversion == 'e' || conversion == 'E' ||
               conversion == 'f' || conversion == 'F' ||
               conversion == 'g' || conversion == 'G') {
      if (argument >= argc) goto missing;
      double number;
      if (JS_ToFloat64(ctx, &number, argv[argument++])) goto fail;
      int ret = goc_qjs_cli_add_float(ctx, &output, number, conversion,
                                      left, plus, space, alternate, zero,
                                      width, precision);
      if (ret == -3) goto invalid;
      if (ret < 0) goto oom;
    } else {
      goto invalid;
    }
  }
  if (format) JS_FreeCString(ctx, format);
  if (fd >= 0) {
    int count = goc_qjs_cli_write_all(fd, output.data, output.length);
    goc_qjs_cli_buffer_free(&output);
    return JS_NewInt32(ctx, count);
  }
  JSValue result = JS_NewStringLen(ctx, output.data ? (const char *)output.data : "",
                                   output.length);
  goc_qjs_cli_buffer_free(&output);
  return result;
missing:
  JS_ThrowReferenceError(ctx, "missing argument for conversion specifier");
  goto fail;
invalid:
  JS_ThrowTypeError(ctx, "invalid conversion specifier in format string");
fail:
  if (format) JS_FreeCString(ctx, format);
  goc_qjs_cli_buffer_free(&output);
  return JS_EXCEPTION;
oom:
  if (format) JS_FreeCString(ctx, format);
  goc_qjs_cli_buffer_free(&output);
  return JS_ThrowOutOfMemory(ctx);
}

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
static const JSCFunctionListEntry goc_qjs_cli_std_funcs[] = {
  JS_CFUNC_DEF("loadFile", 1, goc_qjs_cli_std_load_file),
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
