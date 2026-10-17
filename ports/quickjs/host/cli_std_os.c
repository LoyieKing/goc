/* Linux/amd64 std/os surface for the goc-built QuickJS CLI.
 *
 * The C engine runs on moving Go goroutine stacks.  This file deliberately
 * uses raw Linux syscalls for filesystem/descriptor operations and crosses to
 * Go only for process creation/waiting and C-runtime-free float formatting.
 */
#include "../../../include/goc.h"
#ifdef GOC_QJS_BELLARD
#include "../../../third_party/quickjs-bellard/quickjs.h"
#include "bellard_compat.h"
#else
#include "../../../third_party/quickjs-ng/quickjs.h"
#endif

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

/* ----- Go process bridge; layout mirrored by ports/quickjs/cli/std_os.go. ----- */
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

__go_extern("main.gocGoQjsCliProcess")
void goc_x_QjsCliProcess(GocQjsCliProcessRequest *request);

static void goc_qjs_cli_call_go_process(GocQjsCliProcessRequest *request) {
  goc_x_QjsCliProcess(request);
}
__go_extern("main.gocGoQjsCliFloat")
void goc_x_QjsCliFloat(GocQjsCliFloatRequest *request);

static void goc_qjs_cli_call_go_float(GocQjsCliFloatRequest *request) {
  goc_x_QjsCliFloat(request);
}
__go_extern("main.gocGoQjsCliStrerror")
void goc_x_QjsCliStrerror(GocQjsCliStrerrorRequest *request);

static void goc_qjs_cli_call_go_strerror(GocQjsCliStrerrorRequest *request) {
  goc_x_QjsCliStrerror(request);
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

/* The rest of this translation unit, split by topic: */
#include "cli_std_format.inc"
#include "cli_std_file.inc"
#include "cli_os.inc"
