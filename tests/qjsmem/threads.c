/* Multi-instance memory probe for native QuickJS (quickjs-ng or Bellard).
 *
 *   qjsmem-threads N script.js
 *
 * Starts N pthreads. Each owns one JSRuntime + JSContext, evaluates the
 * script, then blocks with the runtime alive. Threads are started one after
 * another (the next starts once the previous has finished evaluating), the
 * same order as the goc harness (tests/qjscli/mem_instances.go). Thread stack
 * size: QJSMEM_STACK_KB (default 1024 KiB); RSS only counts touched pages.
 * JS_SetMaxStackSize is set to half of the thread stack.
 * When all N are parked it prints one JSON line with VmRSS / VmHWM from
 * /proc/self/status, then again after malloc_trim(0).
 * Built by scripts/bench-mem.sh against each engine's own objects.
 */
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "quickjs.h"

static char *src;
static size_t src_len;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready_count, failed, released;
static size_t stack_bytes;
static char first_result[64];

static long status_kb(const char *field) {
  FILE *f = fopen("/proc/self/status", "r");
  char line[256];
  long v = -1;
  size_t n = strlen(field);
  while (f && fgets(line, sizeof line, f))
    if (!strncmp(line, field, n) && line[n] == ':') { v = atol(line + n + 1); break; }
  if (f) fclose(f);
  return v;
}

static void *worker(void *arg) {
  (void)arg;
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
  int ok = 0;
  if (ctx) {
    JS_SetMaxStackSize(rt, stack_bytes / 2);
    JSValue v = JS_Eval(ctx, src, src_len, "instance.js", JS_EVAL_TYPE_GLOBAL);
    ok = !JS_IsException(v);
    if (ok && !first_result[0]) {
      const char *s = JS_ToCString(ctx, v);
      if (s) { snprintf(first_result, sizeof first_result, "%s", s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
  }
  pthread_mutex_lock(&mu);
  if (!ok) failed = 1;
  ready_count++;
  pthread_cond_broadcast(&cv);
  while (!released) pthread_cond_wait(&cv, &mu); /* runtime stays alive */
  pthread_mutex_unlock(&mu);
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: %s N script.js\n", argv[0]); return 2; }
  int n = atoi(argv[1]);
  FILE *f = fopen(argv[2], "rb");
  if (!f) { perror(argv[2]); return 2; }
  fseek(f, 0, SEEK_END); src_len = ftell(f); fseek(f, 0, SEEK_SET);
  src = calloc(1, src_len + 1);
  if (fread(src, 1, src_len, f) != src_len) return 2;
  fclose(f);
  const char *kb = getenv("QJSMEM_STACK_KB");
  stack_bytes = (size_t)(kb ? atol(kb) : 1024) * 1024;
  long base_rss = status_kb("VmRSS");
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack_bytes);
  pthread_t *tids = calloc(n ? n : 1, sizeof *tids);
  for (int i = 0; i < n; i++) {
    if (pthread_create(&tids[i], &attr, worker, NULL)) { fprintf(stderr, "pthread_create %d failed\n", i); return 1; }
    pthread_mutex_lock(&mu);
    while (ready_count <= i) pthread_cond_wait(&cv, &mu);
    int bad = failed;
    pthread_mutex_unlock(&mu);
    if (bad) { fprintf(stderr, "instance %d failed\n", i); return 1; }
  }
  if (n) fprintf(stderr, "instance 0 result: %s\n", first_result);
  long live_rss = status_kb("VmRSS"), live_hwm = status_kb("VmHWM");
  malloc_trim(0);
  long trim_rss = status_kb("VmRSS");
  const char *arena = getenv("MALLOC_ARENA_MAX");
  printf("MEMINST {\"engine\":\"%s\",\"n\":%d,\"stack_kb\":%zu,\"malloc_arena_max\":\"%s\","
         "\"base\":{\"VmRSSKB\":%ld},\"live\":{\"VmRSSKB\":%ld,\"VmHWMKB\":%ld},"
         "\"after_trim\":{\"VmRSSKB\":%ld}}\n",
         QJSMEM_ENGINE, n, stack_bytes / 1024, arena ? arena : "", base_rss, live_rss, live_hwm, trim_rss);
  fflush(stdout);
  _exit(0); /* do not join: the parked threads never return */
}
