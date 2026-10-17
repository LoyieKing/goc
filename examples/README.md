# Examples

| Path | Intent |
|------|--------|
| `hello/` | `goc go examples/hello` links `hello_add` into a Go binary (`goc check` builds it) |
| `colors/colors.c` | Pointer colors: an `sptr` out-parameter and `cptr` loads |

Linking into Go is [docs/quickstart.md](../docs/quickstart.md). The color
error cases (a returned stack pointer, a `gptr` stored as `cptr`) are lit
tests in `test/lit/Sema/` and `test/lit/ColorEscape/`.

```bash
goc cc -emit-llvm -S -o /tmp/colors.ll examples/colors/colors.c
goc build examples/colors/colors.c -o /tmp/colors.goobj
goc cc -c test/lit/Sema/sptr_escape_bad.c   # fails: goc: sptr escape
```
