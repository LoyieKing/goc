/* P29 north-star: freestanding libc surface for quickjs-ng on goroutine stacks.
 *
 * goc-compiled C must not enter libc: libc may switch stacks or use TLS in
 * ways Go's stack discipline cannot describe. This shim provides QuickJS's
 * C heap (coalescing fixed arena plus mmap slabs), memory/string/formatting,
 * raw clocks and Go-backed numeric/timezone conversions. Unsupported I/O
 * deliberately traps instead of faking values.
 * Pthread operations below are single-thread-only shims, not concurrency.
 *
 * Compiled with --goabi: QJS's SysV calls bind to the .impl bodies; Go uses
 * the separate ABIInternal entry thunks.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include "../../third_party/quickjs-ng/dtoa.h"

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
  size_t copied = n < h->size ? n : h->size;
  goc_memcpy(np, p, copied);
  goc_free(p);
  return np;
}

size_t goc_malloc_usable_size(void *p) {
  return p ? ((GocAllocHeader *)p - 1)->size : 0;
}

/* ---------- string / memory ops ---------- */
/* Small sizes (<= 64 bytes) use overlapping unaligned 8/4/2/1-byte accesses:
 * every load happens before any store, so the same helper serves memmove.
 * Fixed-size __builtin_memcpy lowers to a single load/store, never a call.
 * Larger sizes use rep movsb/stosb (ERMS). */
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

void *goc_memcpy(void *d, const void *s, size_t n) {
  /* A C loop is recognized as a memcpy libcall at -O3 and recursively
   * enters this freestanding implementation. */
  void *result = d;
  if (n <= 64) {
    goc_copy_small((unsigned char *)d, (const unsigned char *)s, n);
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
  } else {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  }
  return result;
}

void *goc_memset(void *d, int c, size_t n) {
  void *result = d;
  unsigned char byte = (unsigned char)c;
  if (n <= 64) {
    unsigned char *p = (unsigned char *)d;
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
    return result;
  }
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(byte) : "memory");
  return result;
}

int goc_memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  for (size_t i = 0; i < n; i++)
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

/* ---------- stdio: minimal formatter + raw write(2) syscall ---------- */
typedef struct FILE FILE;
struct FILE {
  int fd;
};
static FILE g_stdout = {1};
static FILE g_stderr = {2};
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

static long goc_write(int fd, const char *buf, unsigned long n) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(1), "D"((long)fd), "S"(buf), "d"(n)
                   : "rcx", "r11", "memory");
  return ret;
}

static int goc_write_all(FILE *f, const char *buf, size_t n) {
  if (!f)
    return -1;
  while (n) {
    long wrote = goc_write(f->fd, buf, n);
    if (wrote == -4) /* EINTR */
      continue;
    if (wrote <= 0)
      return -1;
    buf += wrote;
    n -= (size_t)wrote;
  }
  return 0;
}

typedef struct {
  char *buf;
  size_t cap, len, pending;
  FILE *file;
  char chunk[256];
  int error;
} GocFormatOut;

static void goc_format_char(GocFormatOut *o, char c) {
  if (o->error)
    return;
  if (o->file) {
    o->chunk[o->pending++] = c;
    if (o->pending == sizeof(o->chunk)) {
      o->error = goc_write_all(o->file, o->chunk, o->pending);
      o->pending = 0;
    }
  } else if (o->cap && o->len < o->cap - 1) {
    o->buf[o->len] = c;
  }
  if (o->len == 0x7fffffffUL)
    o->error = -1;
  else
    ++o->len;
}

static void goc_format_field(GocFormatOut *o, const char *text, size_t len,
                             int width, int left, int zero, size_t prefix) {
  int pad = width > 0 && (size_t)width > len ? width - (int)len : 0;
  if (!left && zero && prefix) {
    for (size_t i = 0; i < prefix; i++)
      goc_format_char(o, text[i]);
    for (int i = 0; i < pad; i++)
      goc_format_char(o, '0');
    for (size_t i = prefix; i < len; i++)
      goc_format_char(o, text[i]);
    return;
  }
  for (int i = 0; !left && i < pad; i++)
    goc_format_char(o, zero ? '0' : ' ');
  for (size_t i = 0; i < len; i++)
    goc_format_char(o, text[i]);
  for (int i = 0; left && i < pad; i++)
    goc_format_char(o, ' ');
}

static int goc_vformat(GocFormatOut *o, const char *fmt, va_list ap) {
  while (*fmt) {
    if (*fmt != '%') {
      goc_format_char(o, *fmt++);
      continue;
    }
    ++fmt;
    int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
    for (;;) {
      if (*fmt == '-') left = 1;
      else if (*fmt == '+') plus = 1;
      else if (*fmt == ' ') space = 1;
      else if (*fmt == '#') alt = 1;
      else if (*fmt == '0') zero = 1;
      else break;
      ++fmt;
    }
    int width = 0, precision = -1;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      ++fmt;
      if (width == (-2147483647 - 1)) return -1;
      if (width < 0) { left = 1; width = -width; }
    } else {
      while (*fmt >= '0' && *fmt <= '9') {
        if (width > 1000000) return -1;
        width = width * 10 + *fmt++ - '0';
      }
    }
    if (*fmt == '.') {
      ++fmt;
      precision = 0;
      if (*fmt == '*') { precision = va_arg(ap, int); ++fmt; }
      else while (*fmt >= '0' && *fmt <= '9') {
        if (precision > 1000000) return -1;
        precision = precision * 10 + *fmt++ - '0';
      }
      if (precision < 0) precision = -1;
    }
    int length = 0;
    if (*fmt == 'h') { length = 1; ++fmt; if (*fmt == 'h') { length = 2; ++fmt; } }
    else if (*fmt == 'l') { length = 3; ++fmt; if (*fmt == 'l') { length = 4; ++fmt; } }
    else if (*fmt == 'z') { length = 5; ++fmt; }
    else if (*fmt == 't') { length = 6; ++fmt; }
    else if (*fmt == 'j') { length = 7; ++fmt; }
    char conv = *fmt++;
    char tmp[512];
    const char *field = tmp;
    size_t n = 0, prefix = 0;
    if (conv == 's') {
      field = va_arg(ap, const char *);
      if (!field) field = "(null)";
      while (field[n] && (precision < 0 || n < (size_t)precision)) ++n;
    } else if (conv == 'c') {
      tmp[n++] = (char)va_arg(ap, int);
    } else if (conv == '%') {
      tmp[n++] = '%';
    } else if (conv == 'd' || conv == 'i' || conv == 'u' ||
               conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') {
      int signed_number = conv == 'd' || conv == 'i';
      uint64_t u;
      if (conv == 'p') u = (uintptr_t)va_arg(ap, void *);
      else if (length >= 3) {
        if (length == 3 || length == 5) {
          if (signed_number) u = (uint64_t)va_arg(ap, long);
          else u = length == 5 ? va_arg(ap, size_t) : va_arg(ap, unsigned long);
        } else if (length == 6) {
          if (signed_number) u = (uint64_t)va_arg(ap, ptrdiff_t);
          else u = va_arg(ap, size_t);
        } else if (length == 7) {
          if (signed_number) u = (uint64_t)va_arg(ap, intmax_t);
          else u = va_arg(ap, uintmax_t);
        } else {
          if (signed_number) u = (uint64_t)va_arg(ap, long long);
          else u = va_arg(ap, unsigned long long);
        }
      } else {
        if (signed_number) {
          int v = va_arg(ap, int);
          u = length == 2 ? (uint64_t)(int8_t)v :
              length == 1 ? (uint64_t)(int16_t)v : (uint64_t)(int64_t)v;
        } else {
          unsigned v = va_arg(ap, unsigned);
          u = length == 2 ? (uint8_t)v : length == 1 ? (uint16_t)v : v;
        }
      }
      int neg = signed_number && (int64_t)u < 0;
      if (neg) u = 0 - u;
      unsigned base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : 10;
      char digits[64];
      size_t count = 0;
      do {
        unsigned d = (unsigned)(u % base);
        digits[count++] = (char)(d < 10 ? '0' + d : (conv == 'X' ? 'A' : 'a') + d - 10);
        u /= base;
      } while (u);
      if (signed_number) {
        if (neg) tmp[n++] = '-';
        else if (plus) tmp[n++] = '+';
        else if (space) tmp[n++] = ' ';
      }
      if (conv == 'p' || (alt && (conv == 'x' || conv == 'X'))) {
        tmp[n++] = '0'; tmp[n++] = conv == 'X' ? 'X' : 'x';
      } else if (alt && conv == 'o' && digits[count - 1] != '0') {
        tmp[n++] = '0';
      }
      prefix = n;
      if (precision > (int)sizeof(tmp) - 65) return -1;
      for (int j = precision - (int)count; j > 0; --j) tmp[n++] = '0';
      while (count) tmp[n++] = digits[--count];
      if (precision >= 0) zero = 0;
    } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
               conv == 'g' || conv == 'G') {
      int p = precision < 0 ? 6 : precision;
      if ((conv == 'g' || conv == 'G') && p == 0) p = 1;
      if (p > JS_DTOA_MAX_DIGITS - 1) return -1;
      int flags = (conv == 'f' || conv == 'F') ?
          JS_DTOA_FORMAT_FRAC | JS_DTOA_EXP_DISABLED :
          JS_DTOA_FORMAT_FIXED | ((conv == 'e' || conv == 'E') ? JS_DTOA_EXP_ENABLED : JS_DTOA_EXP_AUTO);
      double d = va_arg(ap, double);
      int digits = (conv == 'e' || conv == 'E') ? p + 1 : p;
      if (js_dtoa_max_len(d, 10, digits, flags) >= (int)sizeof(tmp) - 1) return -1;
      JSDTOATempMem mem;
      n = (size_t)js_dtoa(tmp, d, 10, digits,
                          flags | JS_DTOA_MINUS_ZERO, &mem);
      if (n >= sizeof(tmp)) return -1;
      if (tmp[0] != '-' && (plus || space)) {
        for (size_t j = n; j > 0; --j) tmp[j] = tmp[j - 1];
        tmp[0] = plus ? '+' : ' ';
        ++n;
      }
      if (conv == 'E' || conv == 'G')
        for (size_t j = 0; j < n; j++) if (tmp[j] == 'e') tmp[j] = 'E';
      prefix = tmp[0] == '-' || tmp[0] == '+' || tmp[0] == ' ';
      (void)alt;
    } else {
      return -1; /* not a fake success for unsupported printf conversions */
    }
    if (n >= sizeof(tmp) && field == tmp) return -1;
    goc_format_field(o, field, n, width, left, zero && precision < 0, prefix);
    if (o->error) return -1;
  }
  if (o->file && o->pending && goc_write_all(o->file, o->chunk, o->pending) < 0)
    return -1;
  if (!o->file && o->cap)
    o->buf[o->len < o->cap ? o->len : o->cap - 1] = 0;
  return o->error ? -1 : (int)o->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
  GocFormatOut out = { .buf = buf, .cap = n };
  return goc_vformat(&out, fmt, ap);
}

int goc_vfprintf(FILE *f, const char *fmt, va_list ap) {
  if (!f) return -1;
  GocFormatOut out = { .file = f };
  return goc_vformat(&out, fmt, ap);
}

int goc_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = goc_vfprintf(f, fmt, ap);
  va_end(ap);
  return ret;
}

int goc_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = goc_vfprintf(stdout, fmt, ap);
  va_end(ap);
  return ret;
}

int goc_snprintf(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return ret;
}

int goc_puts(const char *s) {
  size_t n = goc_strlen(s);
  if (goc_write_all(stdout, s, n) < 0 || goc_write_all(stdout, "\n", 1) < 0) return -1;
  return (int)n + 1;
}
int putc(int c, FILE *f) {
  char ch = (char)c;
  return goc_write_all(f, &ch, 1) == 0 ? (unsigned char)ch : -1;
}
int fputc(int c, FILE *f) { return putc(c, f); }
int putchar(int c) { return putc(c, stdout); }
int goc_fputs(const char *s, FILE *f) { return goc_write_all(f, s, goc_strlen(s)); }
int goc_fflush(FILE *f) { (void)f; return 0; }
size_t goc_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
  if (sz && n > SIZE_MAX / sz) return 0;
  return goc_write_all(f, p, sz * n) == 0 ? n : 0;
}

/* ---------- time (raw clock syscalls and Go's local timezone database) ---------- */
struct timespec_s {
  long tv_sec;
  long tv_nsec;
};

int goc_clock_gettime(int clk, struct timespec_s *ts) {
  long ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(228), "D"((long)clk), "S"(ts) : "rcx", "r11", "memory");
  return (int)ret;
}

int goc_gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  struct timespec_s ts;
  int result = goc_clock_gettime(0, &ts);
  if (result < 0)
    return result;
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;
  return 0;
}

_Static_assert(sizeof(struct tm) == 56 && offsetof(struct tm, tm_gmtoff) == 40,
               "Go localtime bridge requires Linux amd64 struct tm");
static char goc_localtime_zone[64];

struct tm *goc_localtime_r(const time_t *t, struct tm *tm) {
  struct tm *result = tm;
  long seconds = *t;
  char *zone = goc_localtime_zone;
  /* Go spills register args just above the return address so traceback can
   * recover them. A raw call from C does not reserve that area, so the spill
   * overwrites this frame and a later uptr check traps. 64 bytes covers this
   * 3-word signature. */
  __asm__ volatile(
      "subq $64, %%rsp\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoLocaltime.goabi\n\t"
      "addq $64, %%rsp"
      : "+a"(seconds), "+b"(tm), "+c"(zone)
      : : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
        "r12", "r13", "r14", "r15", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
        "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15", "cc", "memory");
  return result;
}

/* ---------- pthread: single-threaded no-ops ---------- */
int goc_pthread_mutex_init(void *m, const void *a) { (void)m; (void)a; return 0; }
int goc_pthread_mutex_destroy(void *m) { (void)m; return 0; }
int goc_pthread_mutex_lock(void *m) { (void)m; return 0; }
int goc_pthread_mutex_unlock(void *m) { (void)m; return 0; }
int pthread_once(void *o, void (*f)(void)) { (void)o; if (f) f(); return 0; }
int goc_pthread_once(void *o, void (*f)(void)) { (void)o; if (f) f(); return 0; }
int goc_pthread_cond_init(void *c, const void *a) { (void)c; (void)a; return 0; }
int goc_pthread_cond_destroy(void *c) { (void)c; return 0; }
int goc_pthread_cond_signal(void *c) { (void)c; return 0; }
int goc_pthread_cond_broadcast(void *c) { (void)c; return 0; }
int goc_pthread_cond_wait(void *c, void *m) { (void)c; (void)m; return 0; }
int goc_pthread_cond_timedwait(void *c, void *m, const void *t) { (void)c; (void)m; (void)t; return 0; }
int goc_pthread_condattr_init(void *a) { (void)a; return 0; }
int goc_pthread_condattr_destroy(void *a) { (void)a; return 0; }
int goc_pthread_condattr_setclock(void *a, int c) { (void)a; (void)c; return 0; }

/* ---------- assert / goc_abort ---------- */
void goc_abort(void) { __builtin_trap(); }

void goc___assert_fail(const char *expr, const char *file, unsigned line, const char *fn) {
  (void)expr;
  (void)file;
  (void)line;
  (void)fn;
  __builtin_trap();
}

/* ---------- libm: call Go's math routines without entering libc ----------
 * The inline call keeps SP fixed (pcsp remains valid); LLVM saves SysV
 * callee-saved registers listed as clobbers, and R14 is loaded with the
 * current Go g before entering an ABIInternal Go function. */
#define GOC_GO_MATH_CLOBBERS \
  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", \
  "r12", "r13", "r14", "r15", "xmm0", "xmm1", "xmm2", "xmm3", \
  "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", \
  "xmm12", "xmm13", "xmm14", "xmm15", "cc", "memory"

#define GOC_GO_MATH1(name, go_name) \
  double name(double x) { \
    double out; \
    __asm__ volatile( \
        "movsd %1, %%xmm0\n\t" \
        "movq %%fs:-8, %%r14\n\t" \
        "pxor %%xmm15, %%xmm15\n\t" \
        "call " #go_name ".goabi\n\t" \
        "movsd %%xmm0, %0" \
        : "=m"(out) : "m"(x) : GOC_GO_MATH_CLOBBERS); \
    return out; \
  }

#define GOC_GO_MATH2(name, go_name) \
  double name(double x, double y) { \
    double out; \
    __asm__ volatile( \
        "movsd %1, %%xmm0\n\t" \
        "movsd %2, %%xmm1\n\t" \
        "movq %%fs:-8, %%r14\n\t" \
        "pxor %%xmm15, %%xmm15\n\t" \
        "call " #go_name ".goabi\n\t" \
        "movsd %%xmm0, %0" \
        : "=m"(out) : "m"(x), "m"(y) : GOC_GO_MATH_CLOBBERS); \
    return out; \
  }

GOC_GO_MATH2(pow, main.gocGoMathPow)
GOC_GO_MATH1(sqrt, main.gocGoMathSqrt)
GOC_GO_MATH1(trunc, main.gocGoMathTrunc)
GOC_GO_MATH1(floor, main.gocGoMathFloor)
GOC_GO_MATH1(ceil, main.gocGoMathCeil)
GOC_GO_MATH2(hypot, main.gocGoMathHypot)
GOC_GO_MATH2(fmod, main.gocGoMathMod)
GOC_GO_MATH1(round, main.gocGoMathRound)
GOC_GO_MATH1(goc_round, main.gocGoMathRound)
GOC_GO_MATH1(acos, main.gocGoMathAcos)
GOC_GO_MATH1(acosh, main.gocGoMathAcosh)
GOC_GO_MATH1(asin, main.gocGoMathAsin)
GOC_GO_MATH1(asinh, main.gocGoMathAsinh)
GOC_GO_MATH1(atan, main.gocGoMathAtan)
GOC_GO_MATH2(atan2, main.gocGoMathAtan2)
GOC_GO_MATH1(atanh, main.gocGoMathAtanh)
GOC_GO_MATH1(cbrt, main.gocGoMathCbrt)
GOC_GO_MATH1(cos, main.gocGoMathCos)
GOC_GO_MATH1(cosh, main.gocGoMathCosh)
GOC_GO_MATH1(exp, main.gocGoMathExp)
GOC_GO_MATH1(expm1, main.gocGoMathExpm1)
GOC_GO_MATH1(log, main.gocGoMathLog)
GOC_GO_MATH1(log10, main.gocGoMathLog10)
GOC_GO_MATH1(log1p, main.gocGoMathLog1p)
GOC_GO_MATH1(log2, main.gocGoMathLog2)
GOC_GO_MATH1(sin, main.gocGoMathSin)
GOC_GO_MATH1(sinh, main.gocGoMathSinh)
GOC_GO_MATH1(tan, main.gocGoMathTan)
GOC_GO_MATH1(tanh, main.gocGoMathTanh)

long goc_lrint(double x) {
  long out;
  __asm__ volatile(
      "movsd %1, %%xmm0\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoMathRoundToEven.goabi\n\t"
      "movq %%rax, %0"
      : "=m"(out) : "m"(x) : GOC_GO_MATH_CLOBBERS);
  return out;
}

double frexp(double x, int *exponent) {
  double fraction;
  long power;
  __asm__ volatile(
      "movsd %2, %%xmm0\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoMathFrexp.goabi\n\t"
      "movsd %%xmm0, %0\n\t"
      "movq %%rax, %1"
      : "=m"(fraction), "=m"(power) : "m"(x) : GOC_GO_MATH_CLOBBERS);
  *exponent = (int)power;
  return fraction;
}

double scalbn(double x, int exponent) {
  double result;
  __asm__ volatile(
      "movsd %1, %%xmm0\n\t"
      "movl %2, %%eax\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoMathLdexp.goabi\n\t"
      "movsd %%xmm0, %0"
      : "=m"(result) : "m"(x), "m"(exponent) : GOC_GO_MATH_CLOBBERS);
  return result;
}

double goc_scalbn(double x, int exponent) { return scalbn(x, exponent); }

double modf(double x, double *integer) {
  double whole, fraction;
  __asm__ volatile(
      "movsd %2, %%xmm0\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoMathModf.goabi\n\t"
      "movsd %%xmm0, %0\n\t"
      "movsd %%xmm1, %1"
      : "=m"(whole), "=m"(fraction) : "m"(x) : GOC_GO_MATH_CLOBBERS);
  *integer = whole;
  return fraction;
}

double strtod(const char *input, char **end) {
  const char *p = input;
  while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
    p++;
  const char *number = p;
  if (*p == '-' || *p == '+')
    p++;
  const char *digits = p;
  while (*p >= '0' && *p <= '9')
    p++;
  int found_digit = p != digits;
  if (*p == '.') {
    p++;
    digits = p;
    while (*p >= '0' && *p <= '9')
      p++;
    found_digit |= p != digits;
  }
  if (!found_digit) {
    if (end)
      *end = (char *)input;
    return 0;
  }
  const char *exponent = p;
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '-' || *p == '+')
      p++;
    digits = p;
    while (*p >= '0' && *p <= '9')
      p++;
    if (digits == p)
      p = exponent;
  }
  if (end)
    *end = (char *)p;
  double result;
  size_t length = (size_t)(p - number);
  __asm__ volatile(
      "movq %1, %%rax\n\t"
      "movq %2, %%rbx\n\t"
      "movq %%fs:-8, %%r14\n\t"
      "pxor %%xmm15, %%xmm15\n\t"
      "call main.gocGoParseFloat.goabi\n\t"
      "movsd %%xmm0, %0"
      : "=m"(result) : "m"(number), "m"(length) : GOC_GO_MATH_CLOBBERS);
  return result;
}

double goc_fabs(double x) { return __builtin_fabs(x); }

/* ---------- bare-name aliases for compiler-emitted libcalls ----------
 * llc lowers llvm.memcpy/memset/memmove intrinsics (struct copies) to these
 * libcall names; -D macros cannot rename them, so provide the names here as
 * thin wrappers over the goc_-prefixed implementations. */
void *memcpy(void *d, const void *s, size_t n) { return goc_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return goc_memmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return goc_memset(d, c, n); }
