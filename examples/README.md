# Examples

| File | Intent |
|------|--------|
| `hello/` | `goc go examples/hello` links `hello_add` into a Go binary |
| `hello_colors.c` | OK: `sptr` out-param + `cptr` loads |
| `sptr_escape_bad.c` | ERROR: returns a stack pointer (Sema). Storing `sptr` into a plain `T *` global is allowed and is encoded as `uptr` |

Color checks only. Linking into Go is [docs/quickstart.md](../docs/quickstart.md). The verified program is `tests/goabi`.

```bash
./cmd/goc cc -emit-llvm -S -o /tmp/hello.ll examples/hello_colors.c
./cmd/goc build examples/hello_colors.c -o /tmp/hello.o
# Expect failure:
./cmd/goc cc -c examples/sptr_escape_bad.c
```
