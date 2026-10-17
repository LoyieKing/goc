/* Part of cli_std_os.c, which includes it; not compiled on its own.
 * qjs:std printf formatting. */

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
