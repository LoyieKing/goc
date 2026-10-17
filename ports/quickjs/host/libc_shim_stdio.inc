/* Part of libc_shim.c, which includes it; not compiled on its own.
 * Stdio, time, pthread, assert. */

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

typedef struct { int64_t sec; int64_t nsec; } goc_go_timespec;
__go_extern("main.gocGoClockGettime")
goc_go_timespec goc_x_ClockGettime(int64_t clk);

int goc_clock_gettime(int clk, struct timespec_s *ts) {
  /* CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) go through Go's vDSO clocks.
   * A raw syscall is about 100 ns slower. Other clocks stay on syscall 228. */
  if (clk == 0 || clk == 1) {
    goc_go_timespec p = goc_x_ClockGettime((int64_t)clk);
    ts->tv_sec = p.sec;
    ts->tv_nsec = p.nsec;
    return 0;
  }
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

__go_extern("main.gocGoLocaltime")
void goc_x_Localtime(int64_t seconds, struct tm *out, char *zone);

struct tm *goc_localtime_r(const time_t *t, struct tm *tm) {
  goc_x_Localtime((int64_t)(*t), tm, goc_localtime_zone);
  return tm;
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
