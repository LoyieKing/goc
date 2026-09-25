/* P19/P22: uptr MSB encode/decode against owner g stack.hi.
 *
 * Builds:
 *   Host (P19): default — test override + page approx; libc fatal.
 *   Go  (P22):  -DGOC_UPTR_FREESTANDING -DGOC_UPTR_HAVE_TLS
 *               inline FS:-8 → g→stack.hi (no cross-object relocs;
 *               Go internal linker does not apply clang PLT/GOT/.bss).
 */
#include "goc_uptr.h"

#ifndef GOC_UPTR_FREESTANDING
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef GOC_UPTR_WITH_GOC_H
#include "goc.h"
#endif

#ifndef GOC_UPTR_HAVE_TLS
static uintptr_t g_test_hi = 0;
static uintptr_t g_test_lo = 0;
static int g_test_hi_set = 0;
static int g_test_lo_set = 0;
#endif

void goc_uptr_fatal(const char *msg) {
#ifdef GOC_UPTR_FREESTANDING
  (void)msg;
  __builtin_trap();
  for (;;) {
  }
#else
  fprintf(stderr, "goc FATAL uptr: %s\n", msg ? msg : "(null)");
  fflush(stderr);
  abort();
#endif
}

void goc_test_set_stack_hi(uintptr_t hi) {
#ifdef GOC_UPTR_HAVE_TLS
  (void)hi; /* production TLS build: override disabled */
#else
  g_test_hi = hi;
  g_test_hi_set = 1;
#endif
}

void goc_test_set_stack_lo(uintptr_t lo) {
#ifdef GOC_UPTR_HAVE_TLS
  (void)lo;
#else
  g_test_lo = lo;
  g_test_lo_set = 1;
#endif
}

void goc_test_clear_stack_bounds(void) {
#ifndef GOC_UPTR_HAVE_TLS
  g_test_hi = 0;
  g_test_lo = 0;
  g_test_hi_set = 0;
  g_test_lo_set = 0;
#endif
}

/* linux/amd64 Go TLS — same convention as P1 morestack / P5 FS:-8. */
static inline uintptr_t goc_tls_getg(void) {
  uintptr_t g;
  __asm__ volatile("movq %%fs:-8, %0" : "=r"(g));
  return g;
}

static inline uintptr_t goc_tls_stack_hi_inline(void) {
  uintptr_t g = goc_tls_getg();
  uintptr_t hi;
  __asm__ volatile("movq 8(%1), %0" : "=r"(hi) : "r"(g));
  return hi;
}

static inline uintptr_t goc_tls_stack_lo_inline(void) {
  uintptr_t g = goc_tls_getg();
  uintptr_t lo;
  __asm__ volatile("movq 0(%1), %0" : "=r"(lo) : "r"(g));
  return lo;
}

uintptr_t goc_runtime_getg(void) { return goc_tls_getg(); }
uintptr_t goc_runtime_stack_hi(void) { return goc_tls_stack_hi_inline(); }
uintptr_t goc_runtime_stack_lo(void) { return goc_tls_stack_lo_inline(); }

uintptr_t goc_stack_hi(void) {
#ifdef GOC_UPTR_HAVE_TLS
  return goc_tls_stack_hi_inline();
#else
  if (g_test_hi_set)
    return g_test_hi;
  volatile char probe;
  uintptr_t approx = (uintptr_t)&probe;
  return (approx + 0x1000u) & ~(uintptr_t)0xfffu;
#endif
}

uintptr_t goc_stack_lo(void) {
#ifdef GOC_UPTR_HAVE_TLS
  return goc_tls_stack_lo_inline();
#else
  if (g_test_lo_set)
    return g_test_lo;
  return 0;
#endif
}

goc_uptr goc_uptr_from_cptr_hi(goc_cptr p, uintptr_t stack_hi) {
  (void)stack_hi;
  uintptr_t w = (uintptr_t)p;
  if (w & GOC_UPTR_MSB)
    goc_uptr_fatal("goc_uptr_from_cptr: address has MSB set (not encodeable as cptr)");
  return (goc_uptr)w;
}

goc_uptr goc_uptr_from_sptr_hi(goc_sptr p, uintptr_t stack_hi) {
  uintptr_t abs = (uintptr_t)p;
  int64_t off = (int64_t)abs - (int64_t)stack_hi;
  uintptr_t word = (uintptr_t)off;
  if ((word & GOC_UPTR_MSB) == 0)
    goc_uptr_fatal("goc_uptr_from_sptr: offset cleared MSB (abs not below stack.hi?)");
  return (goc_uptr)word;
}

goc_cptr goc_uptr_as_cptr_hi(goc_uptr u, uintptr_t stack_hi) {
  (void)stack_hi;
  uintptr_t w = (uintptr_t)u;
  if (w & GOC_UPTR_MSB)
    goc_uptr_fatal("goc_uptr_as_cptr: MSB set (expected absolute cptr encoding)");
  return (goc_cptr)w;
}

goc_sptr goc_uptr_as_sptr_hi(goc_uptr u, uintptr_t stack_hi) {
  uintptr_t w = (uintptr_t)u;
  if ((w & GOC_UPTR_MSB) == 0)
    goc_uptr_fatal("goc_uptr_as_sptr: MSB clear (expected stack-offset encoding)");
  int64_t off = (int64_t)w;
  uintptr_t abs = (uintptr_t)((int64_t)stack_hi + off);
  return (goc_sptr)abs;
}

goc_uptr goc_uptr_from_cptr(goc_cptr p) {
  return goc_uptr_from_cptr_hi(p, goc_stack_hi());
}

goc_uptr goc_uptr_from_sptr(goc_sptr p) {
  return goc_uptr_from_sptr_hi(p, goc_stack_hi());
}

goc_cptr goc_uptr_as_cptr(goc_uptr u) {
  return goc_uptr_as_cptr_hi(u, goc_stack_hi());
}

goc_sptr goc_uptr_as_sptr(goc_uptr u) {
  return goc_uptr_as_sptr_hi(u, goc_stack_hi());
}

goc_uptr goc_uptr_from_ptr(void *p) {
  uintptr_t addr = (uintptr_t)p;
  if (addr == 0)
    return (goc_uptr)0;
  uintptr_t lo = goc_stack_lo();
  uintptr_t hi = goc_stack_hi();
  if (lo != 0 && hi != 0 && addr >= lo && addr < hi)
    return goc_uptr_from_sptr_hi((goc_sptr)p, hi);
  return goc_uptr_from_cptr_hi((goc_cptr)p, hi);
}

void *goc_uptr_decode(goc_uptr u) {
  uintptr_t word = (uintptr_t)u;
  if ((word & GOC_UPTR_MSB) == 0)
    return (void *)word;
  int64_t off = (int64_t)word;
  uintptr_t abs = (uintptr_t)((int64_t)goc_stack_hi() + off);
  return (void *)abs;
}

void *goc_uptr_require_cptr(void *p) {
  uintptr_t addr = (uintptr_t)p;
  uintptr_t lo = goc_stack_lo();
  uintptr_t hi = goc_stack_hi();
  if (addr != 0 && lo != 0 && hi != 0 && addr >= lo && addr < hi)
    goc_uptr_fatal("sptr cannot be returned as cptr");
  return p;
}

typedef struct GocDynamicBlock {
  struct GocDynamicBlock *previous;
  size_t mapped_size;
} GocDynamicBlock;

#ifdef GOC_UPTR_FREESTANDING
/* QJS shim heap: cptr, 16-byte aligned, and recycled. A raw mmap per alloca
 * made every JS_CallInternal pay a syscall pair. */
extern void *goc_malloc(size_t n);
extern void goc_free(void *p);
#endif

#ifdef GOC_DYNALLOC_POOL
/*
 * Single-thread bump pool. The cursor is a process global, so two
 * goroutines calling goc_dynalloc at once corrupt each other. QuickJS on
 * one goroutine is the intended user; do not define this for a concurrent
 * caller.
 *
 * Size: JS_DEFAULT_STACK_SIZE is 1 MiB. Each JS_CallInternal frame is
 * 16*(args + vars + stack slots) + 8*var_refs (nan-boxing off). The compiler
 * caps one function at 65534 slots (~1 MiB of operand stack) but nesting is
 * the real cost: the goc CLI sets JS_SetMaxStackSize to 16 MiB
 * (--stack-size 16384 KiB) because EarleyBoyer and deep recursion exceed
 * 1 MiB of nested frames. The pool matches that limit so it does not trap
 * before QuickJS's own stack check. Override with -DGOC_DYNALLOC_POOL_SIZE=.
 */
#ifndef GOC_DYNALLOC_POOL_SIZE
#define GOC_DYNALLOC_POOL_SIZE (16u * 1024u * 1024u)
#endif

static unsigned char *goc_alloca_pool __attribute__((annotate("goc.color.cptr")));
static size_t goc_alloca_off;

static void goc_alloca_pool_init(void) {
  if (goc_alloca_pool)
    return;
#ifdef GOC_UPTR_FREESTANDING
  goc_alloca_pool = goc_malloc(GOC_DYNALLOC_POOL_SIZE);
#else
  goc_alloca_pool = malloc(GOC_DYNALLOC_POOL_SIZE);
#endif
  if (!goc_alloca_pool)
    goc_uptr_fatal("alloca pool allocation failed");
  goc_alloca_off = 0;
}

void *goc_dynalloc(size_t bytes, void **scope) {
  if (!scope || bytes > SIZE_MAX - 15u)
    goc_uptr_fatal("dynamic alloca size overflow");
  goc_alloca_pool_init();
  size_t aligned = (bytes + 15u) & ~(size_t)15u;
  if (aligned > GOC_DYNALLOC_POOL_SIZE - goc_alloca_off)
    goc_uptr_fatal("alloca pool exhausted");
  /* First bump in this function saves the watermark. A pool address is
   * never NULL, so a later alloca in the same function leaves it alone. */
  if (!*scope)
    *scope = goc_alloca_pool + goc_alloca_off;
  unsigned char *p __attribute__((annotate("goc.color.cptr")));
  p = goc_alloca_pool + goc_alloca_off;
  goc_alloca_off += aligned;
  if (bytes)
    __builtin_memset(p, 0, bytes);
  return p;
}

void goc_dynrelease(void **scope) {
  if (!scope)
    goc_uptr_fatal("missing dynamic alloca scope");
  if (!*scope)
    return;
  unsigned char *mark = (unsigned char *)*scope;
  if (!goc_alloca_pool || mark < goc_alloca_pool ||
      mark > goc_alloca_pool + goc_alloca_off)
    goc_uptr_fatal("alloca pool watermark corrupt");
  /* -size: drop every byte this function bumped. */
  goc_alloca_off = (size_t)(mark - goc_alloca_pool);
  *scope = NULL;
}
#else
void *goc_dynalloc(size_t bytes, void **scope) {
  if (!scope || bytes > SIZE_MAX - sizeof(GocDynamicBlock))
    goc_uptr_fatal("dynamic alloca size overflow");
  size_t size = bytes + sizeof(GocDynamicBlock);
  /* Off the goroutine stack either way: Go pcsp cannot describe a mid-frame
   * SP change, and the pointer must stay a cptr across stack growth. */
  GocDynamicBlock *block __attribute__((annotate("goc.color.cptr")));
#ifdef GOC_UPTR_FREESTANDING
  block = goc_malloc(size);
  if (!block)
    goc_uptr_fatal("malloc failed for dynamic alloca");
  /* The old mmap path returned zero-filled pages. Keep that. */
  __builtin_memset(block, 0, size);
#else
  block = malloc(size);
  if (!block)
    goc_uptr_fatal("malloc failed for dynamic alloca");
#endif
  block->previous = (GocDynamicBlock *)*scope;
  block->mapped_size = size;
  *scope = block;
  return block + 1;
}

void goc_dynrelease(void **scope) {
  if (!scope)
    goc_uptr_fatal("missing dynamic alloca scope");
  GocDynamicBlock *block = (GocDynamicBlock *)*scope;
  *scope = NULL;
  while (block) {
    GocDynamicBlock *previous = block->previous;
#ifdef GOC_UPTR_FREESTANDING
    goc_free(block);
#else
    free(block);
#endif
    block = previous;
  }
}
#endif
