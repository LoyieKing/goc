/* Freestanding libc surface for quickjs-ng on goroutine stacks.
 *
 * goc-compiled C must not enter libc for allocation, I/O, or TLS: libc may
 * switch stacks in ways Go's stack discipline cannot describe. This shim
 * provides QuickJS's C heap (coalescing fixed arena plus mmap slabs),
 * memory/string/formatting, vDSO clocks, and timezone conversions.
 * libm is the exception. sqrt/floor/ceil/trunc/round are inline, and the
 * transcendentals are a direct glibc call (ports/quickjs/host/asm/goc_libm_glibc.S) so
 * the result stays bit-identical to glibc, including Math.sin(1).
 * Unsupported I/O deliberately traps instead of faking values.
 * Pthread operations below are single-thread-only shims, not concurrency.
 *
 * QJS's SysV calls bind to the .impl bodies. Go uses the separate
 * ABIInternal entry thunks. C calls Go through __go_extern stubs
 * (spill, g, xmm15, pcsp); no handwritten goabi asm.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include "../../../third_party/quickjs-ng/dtoa.h"
#include "../../../include/goc.h"

/* ---------- memory: fixed C arena with reusable, coalescing blocks ---------- */
#define ARENA_SIZE (64u * 1024u * 1024u)
typedef struct GocAllocHeader {
  size_t size;
  size_t capacity;
  struct GocAllocHeader *prev_phys, *next_phys;
  struct GocAllocHeader *prev_free, *next_free;
  size_t is_free;
  size_t padding;
} GocAllocHeader;
_Alignas(16) static unsigned char g_arena[ARENA_SIZE];
static size_t g_arena_off;
static GocAllocHeader *g_last_block, *g_free_blocks;

/* Size-class cache for small blocks. A freed block with capacity <= 512 goes
 * on the LIFO list for capacity/16 instead of the coalescing first-fit list,
 * and a small malloc pops that list first (exact fit, O(1)). Cached blocks
 * are marked GOC_BLOCK_CACHED: not free for coalescing (neighbours skip them),
 * but a second free() still traps. Every block keeps the full header, so
 * free/realloc/malloc_usable_size and 16-byte alignment are unchanged.
 * Single-goroutine only, like the rest of this heap: QuickJS runs one
 * runtime per goroutine and the shim has no locks (see pthread shims). */
#define GOC_BLOCK_FREE 1u
#define GOC_BLOCK_CACHED 2u
#define GOC_SMALL_MAX 512u
static GocAllocHeader *g_small_free[GOC_SMALL_MAX / 16u + 1u];

static int goc_in_static_arena(const void *p) {
  uintptr_t address = (uintptr_t)p;
  return address >= (uintptr_t)g_arena &&
         address - (uintptr_t)g_arena < ARENA_SIZE;
}

static void goc_unlink_free(GocAllocHeader *h) {
  if (h->prev_free)
    h->prev_free->next_free = h->next_free;
  else
    g_free_blocks = h->next_free;
  if (h->next_free)
    h->next_free->prev_free = h->prev_free;
  h->prev_free = h->next_free = NULL;
  h->is_free = 0;
}

static void goc_link_free(GocAllocHeader *h) {
  h->is_free = GOC_BLOCK_FREE;
  h->prev_free = NULL;
  h->next_free = g_free_blocks;
  if (g_free_blocks)
    g_free_blocks->prev_free = h;
  g_free_blocks = h;
}

static void goc_split_block(GocAllocHeader *h, size_t capacity) {
  if (h->capacity < capacity + sizeof(GocAllocHeader) + 16)
    return;
  GocAllocHeader *rest = (GocAllocHeader *)((unsigned char *)(h + 1) + capacity);
  rest->size = 0;
  rest->capacity = h->capacity - capacity - sizeof(GocAllocHeader);
  rest->prev_phys = h;
  rest->next_phys = h->next_phys;
  if (rest->next_phys)
    rest->next_phys->prev_phys = rest;
  else if (goc_in_static_arena(h))
    g_last_block = rest;
  h->next_phys = rest;
  h->capacity = capacity;
  goc_link_free(rest);
}

void *goc_malloc(size_t n) {
  if (n > SIZE_MAX - 15u)
    return NULL;
  size_t aligned = (n + 15u) & ~(size_t)15u;
  if (aligned <= GOC_SMALL_MAX) {
    GocAllocHeader *c = g_small_free[aligned >> 4];
    if (c) {
      g_small_free[aligned >> 4] = c->next_free;
      c->next_free = NULL;
      c->is_free = 0;
      c->size = n;
      return c + 1;
    }
  }
  for (GocAllocHeader *h = g_free_blocks; h; h = h->next_free) {
    if (h->capacity < aligned)
      continue;
    goc_unlink_free(h);
    goc_split_block(h, aligned);
    h->size = n;
    return h + 1;
  }
  if (g_arena_off > ARENA_SIZE - sizeof(GocAllocHeader) ||
      aligned > ARENA_SIZE - sizeof(GocAllocHeader) - g_arena_off) {
    if (aligned > SIZE_MAX - sizeof(GocAllocHeader) - 4095)
      return NULL;
    size_t bytes = (aligned + sizeof(GocAllocHeader) + 4095) & ~(size_t)4095;
    if (bytes < ARENA_SIZE)
      bytes = ARENA_SIZE;
    long address;
    register long flags __asm__("r10") = 0x22; /* MAP_PRIVATE | MAP_ANONYMOUS */
    register long fd __asm__("r8") = -1;
    register long offset __asm__("r9") = 0;
    __asm__ volatile("syscall" : "=a"(address)
                     : "a"(9L), "D"(0L), "S"(bytes), "d"(3L),
                       "r"(flags), "r"(fd), "r"(offset)
                     : "rcx", "r11", "memory");
    if ((unsigned long)address >= (unsigned long)-4095)
      return NULL;
    GocAllocHeader *mapped = (GocAllocHeader *)address;
    mapped->size = n;
    mapped->capacity = bytes - sizeof(GocAllocHeader);
    mapped->prev_phys = mapped->next_phys = NULL;
    mapped->prev_free = mapped->next_free = NULL;
    mapped->is_free = 0;
    goc_split_block(mapped, aligned);
    return mapped + 1;
  }
  GocAllocHeader *h = (GocAllocHeader *)&g_arena[g_arena_off];
  h->size = n;
  h->capacity = aligned;
  h->prev_phys = g_last_block;
  h->next_phys = NULL;
  h->prev_free = h->next_free = NULL;
  h->is_free = 0;
  if (g_last_block)
    g_last_block->next_phys = h;
  g_last_block = h;
  g_arena_off += sizeof(GocAllocHeader) + aligned;
  return h + 1;
}

void goc_free(void *p) {
  if (!p)
    return;
  GocAllocHeader *h = (GocAllocHeader *)p - 1;
  if (h->is_free)
    __builtin_trap();
  if (h->capacity <= GOC_SMALL_MAX) {
    /* capacity is a multiple of 16 (aligned request or split remainder). */
    h->is_free = GOC_BLOCK_CACHED;
    h->prev_free = NULL;
    h->next_free = g_small_free[h->capacity >> 4];
    g_small_free[h->capacity >> 4] = h;
    return;
  }
  if (h->prev_phys && h->prev_phys->is_free == GOC_BLOCK_FREE) {
    GocAllocHeader *prev = h->prev_phys;
    goc_unlink_free(prev);
    prev->capacity += sizeof(GocAllocHeader) + h->capacity;
    prev->next_phys = h->next_phys;
    if (h->next_phys)
      h->next_phys->prev_phys = prev;
    h = prev;
  }
  if (h->next_phys && h->next_phys->is_free == GOC_BLOCK_FREE) {
    GocAllocHeader *next = h->next_phys;
    goc_unlink_free(next);
    h->capacity += sizeof(GocAllocHeader) + next->capacity;
    h->next_phys = next->next_phys;
    if (h->next_phys)
      h->next_phys->prev_phys = h;
  }
  if (!h->next_phys && goc_in_static_arena(h))
    g_last_block = h;
  goc_link_free(h);
}

void *goc_memcpy(void *d, const void *s, size_t n);
void *goc_memset(void *d, int c, size_t n);

void *goc_calloc(size_t n, size_t sz) {
  if (sz && n > SIZE_MAX / sz)
    return NULL;
  size_t total = n * sz;
  unsigned char *p = (unsigned char *)goc_malloc(total);
  if (!p)
    return NULL;
  goc_memset(p, 0, total);
  return p;
}

void *goc_realloc(void *p, size_t n) {
  if (!p)
    return goc_malloc(n);
  if (n == 0) {
    goc_free(p);
    return NULL;
  }
  GocAllocHeader *h = (GocAllocHeader *)p - 1;
  if (n <= h->capacity) {
    h->size = n;
    return p;
  }
  unsigned char *np = (unsigned char *)goc_malloc(n);
  if (!np)
    return NULL;
  /* Callers may write up to usable size, which is the rounded capacity,
   * not the size passed to malloc. Copy that whole region. */
  size_t copied = h->capacity;
  goc_memcpy(np, p, copied);
  goc_free(p);
  return np;
}

size_t goc_malloc_usable_size(void *p) {
  return p ? ((GocAllocHeader *)p - 1)->capacity : 0;
}

/* ---------- string / memory ops ---------- */
/* Small sizes (<= 64 bytes) use overlapping unaligned 8/4/2/1-byte accesses:
 * every load happens before any store, so the same helper serves memmove.
 * Fixed-size __builtin_memcpy lowers to a single load/store, never a call.
 * 64 bytes through 4 KiB use an SSE2 movups loop (the goc path has no AVX).
 * Larger sizes use rep movsb/stosb (ERMS). The stack is 8-byte aligned, so
 * the loop must not emit movaps. */
static inline uint64_t goc_ld8(const unsigned char *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
static inline void goc_st8(unsigned char *p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
static inline uint32_t goc_ld4(const unsigned char *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline void goc_st4(unsigned char *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }

static inline void goc_copy_small(unsigned char *d, const unsigned char *s, size_t n) {
  if (n >= 16) {
    if (n <= 32) {
      uint64_t a = goc_ld8(s), b = goc_ld8(s + 8);
      uint64_t c = goc_ld8(s + n - 16), e = goc_ld8(s + n - 8);
      goc_st8(d, a); goc_st8(d + 8, b);
      goc_st8(d + n - 16, c); goc_st8(d + n - 8, e);
    } else {
      uint64_t a0 = goc_ld8(s), a1 = goc_ld8(s + 8), a2 = goc_ld8(s + 16), a3 = goc_ld8(s + 24);
      uint64_t b0 = goc_ld8(s + n - 32), b1 = goc_ld8(s + n - 24);
      uint64_t b2 = goc_ld8(s + n - 16), b3 = goc_ld8(s + n - 8);
      goc_st8(d, a0); goc_st8(d + 8, a1); goc_st8(d + 16, a2); goc_st8(d + 24, a3);
      goc_st8(d + n - 32, b0); goc_st8(d + n - 24, b1);
      goc_st8(d + n - 16, b2); goc_st8(d + n - 8, b3);
    }
  } else if (n >= 8) {
    uint64_t a = goc_ld8(s), b = goc_ld8(s + n - 8);
    goc_st8(d, a); goc_st8(d + n - 8, b);
  } else if (n >= 4) {
    uint32_t a = goc_ld4(s), b = goc_ld4(s + n - 4);
    goc_st4(d, a); goc_st4(d + n - 4, b);
  } else if (n) {
    unsigned char a = s[0], b = s[n >> 1], c = s[n - 1];
    d[0] = a; d[n >> 1] = b; d[n - 1] = c;
  }
}

/* The loop is asm so -O3 does not turn it back into a memcpy libcall. */
static void goc_copy_sse2(unsigned char *d, const unsigned char *s, size_t n) {
  size_t chunks = n >> 6;
  unsigned char *dst = d;
  const unsigned char *src = s;
  __asm__ volatile(
      "test %[chunks], %[chunks]\n\t"
      "jz L%=e\n\t"
      "L%=l:\n\t"
      "movups (%[src]), %%xmm0\n\t"
      "movups 16(%[src]), %%xmm1\n\t"
      "movups 32(%[src]), %%xmm2\n\t"
      "movups 48(%[src]), %%xmm3\n\t"
      "movups %%xmm0, (%[dst])\n\t"
      "movups %%xmm1, 16(%[dst])\n\t"
      "movups %%xmm2, 32(%[dst])\n\t"
      "movups %%xmm3, 48(%[dst])\n\t"
      "add $64, %[src]\n\t"
      "add $64, %[dst]\n\t"
      "dec %[chunks]\n\t"
      "jnz L%=l\n\t"
      "L%=e:\n\t"
      : [dst] "+r"(dst), [src] "+r"(src), [chunks] "+r"(chunks)
      :
      : "xmm0", "xmm1", "xmm2", "xmm3", "cc", "memory");
  if (n & 63)
    goc_copy_small(dst, src, n & 63);
}

void *goc_memcpy(void *d, const void *s, size_t n) {
  /* A C loop is recognized as a memcpy libcall at -O3 and recursively
   * enters this freestanding implementation. */
  void *result = d;
  if (n <= 64) {
    goc_copy_small((unsigned char *)d, (const unsigned char *)s, n);
    return result;
  }
  if (n <= 4096) {
    goc_copy_sse2((unsigned char *)d, (const unsigned char *)s, n);
    return result;
  }
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return result;
}

void *goc_memmove(void *d, const void *s, size_t n) {
  void *result = d;
  if (n <= 64) {
    goc_copy_small((unsigned char *)d, (const unsigned char *)s, n);
    return result;
  }
  if ((uintptr_t)d > (uintptr_t)s && (uintptr_t)d - (uintptr_t)s < n) {
    unsigned char *end = (unsigned char *)d + n - 1;
    const unsigned char *from = (const unsigned char *)s + n - 1;
    __asm__ volatile("std; rep movsb; cld"
                     : "+D"(end), "+S"(from), "+c"(n) : : "cc", "memory");
  } else if (n <= 4096) {
    goc_copy_sse2((unsigned char *)d, (const unsigned char *)s, n);
  } else {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  }
  return result;
}

/* always_inline: a real call to goc_memset from its own SSE2 tail is a
 * nosplit cycle (the body has no call, so goc-llc marks the small leaf
 * frame nosplit and the linker rejects the recursion). */
static inline __attribute__((always_inline)) void goc_memset_small(
    unsigned char *p, unsigned char byte, size_t n) {
  uint64_t v = (uint64_t)byte * 0x0101010101010101ull;
  if (n >= 16) {
    goc_st8(p, v); goc_st8(p + 8, v);
    goc_st8(p + n - 16, v); goc_st8(p + n - 8, v);
    if (n > 32) {
      goc_st8(p + 16, v); goc_st8(p + 24, v);
      goc_st8(p + n - 32, v); goc_st8(p + n - 24, v);
    }
  } else if (n >= 8) {
    goc_st8(p, v); goc_st8(p + n - 8, v);
  } else if (n >= 4) {
    goc_st4(p, (uint32_t)v); goc_st4(p + n - 4, (uint32_t)v);
  } else if (n) {
    p[0] = byte; p[n >> 1] = byte; p[n - 1] = byte;
  }
}

void *goc_memset(void *d, int c, size_t n) {
  void *result = d;
  unsigned char byte = (unsigned char)c;
  if (n <= 64) {
    goc_memset_small((unsigned char *)d, byte, n);
    return result;
  }
  if (n <= 4096) {
    size_t chunks = n >> 6;
    unsigned char *dst = (unsigned char *)d;
    uint64_t v = (uint64_t)byte * 0x0101010101010101ull;
    __asm__ volatile(
        "movq %[v], %%xmm0\n\t"
        "punpcklqdq %%xmm0, %%xmm0\n\t"
        "test %[chunks], %[chunks]\n\t"
        "jz L%=e\n\t"
        "L%=l:\n\t"
        "movups %%xmm0, (%[dst])\n\t"
        "movups %%xmm0, 16(%[dst])\n\t"
        "movups %%xmm0, 32(%[dst])\n\t"
        "movups %%xmm0, 48(%[dst])\n\t"
        "add $64, %[dst]\n\t"
        "dec %[chunks]\n\t"
        "jnz L%=l\n\t"
        "L%=e:\n\t"
        : [dst] "+r"(dst), [chunks] "+r"(chunks)
        : [v] "r"(v)
        : "xmm0", "cc", "memory");
    if (n & 63)
      goc_memset_small(dst, byte, n & 63);
    return result;
  }
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(byte) : "memory");
  return result;
}

int goc_memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t u = goc_ld8(x + i), v = goc_ld8(y + i);
    if (u != v) {
      /* First differing byte is the most significant after a bswap. */
      u = __builtin_bswap64(u);
      v = __builtin_bswap64(v);
      return u < v ? -1 : 1;
    }
  }
  for (; i < n; i++)
    if (x[i] != y[i])
      return x[i] < y[i] ? -1 : 1;
  return 0;
}

void *goc_memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  for (size_t i = 0; i < n; i++)
    if (p[i] == (unsigned char)c)
      return (void *)(p + i);
  return 0;
}

size_t goc_strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

int goc_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int goc_strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    if (!a[i])
      return 0;
  }
  return 0;
}

char *goc_strcpy(char *d, const char *s) {
  char *r = d;
  while ((*d++ = *s++))
    ;
  return r;
}

char *goc_strncpy(char *d, const char *s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; i++)
    d[i] = s[i];
  for (; i < n; i++)
    d[i] = 0;
  return d;
}

char *goc_strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return 0;
  }
}

char *goc_strrchr(const char *s, int c) {
  const char *last = 0;
  for (;; s++) {
    if (*s == (char)c)
      last = s;
    if (!*s)
      return (char *)last;
  }
}

char *goc_strstr(const char *h, const char *n) {
  if (!*n)
    return (char *)h;
  for (; *h; h++) {
    const char *a = h, *b = n;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (!*b)
      return (char *)h;
  }
  return 0;
}

/* The rest of this translation unit, split by topic: */
#include "libc_shim_stdio.inc"
#include "libc_shim_libm.inc"
