# Examples

| File | Intent |
|------|--------|
| `hello_colors.c` | OK: `sptr` out-param + `cptr` loads |
| `sptr_escape_bad.c` | ERROR: stores `sptr` to a global (Sema) |

Color checks only. Linking into Go is [docs/guide.md](../docs/guide.md). The verified program is `tests/goabi`.

```bash
export GOC_CLANG=/path/to/patched/clang
./cmd/goc cc -emit-llvm -S -o /tmp/hello.ll examples/hello_colors.c
./cmd/goc build examples/hello_colors.c -o /tmp/hello.o
# Expect failure:
./cmd/goc cc -c examples/sptr_escape_bad.c
```
