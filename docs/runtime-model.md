# Runtime model

Compile pointer-colored C so it runs on the goroutine stack and participates
in Go stack growth, GC stackmaps, and write barriers for `gptr`.

The pipeline, the directory layout and what each tool and pass does are in
[ARCHITECTURE.md](../ARCHITECTURE.md). This page covers the runtime model the
generated code relies on.

Drivers pick the patched clang at `bin/clang`, then
`third_party/llvm-19.1.7-clang-build/bin/clang`. There is no path flag.
System `clang-19` is only the host used to build that Clang, and the
out-of-tree plugin test.

## uptr

One machine word.

- MSB 0: absolute address.
- MSB 1: `int64` offset from the owner goroutine's `g.stack.hi`.
  `abs = stack.hi + stored`.

`stack.lo` is `g+0`, `stack.hi` is `g+8`. A stack copy does not rewrite a
stored offset; the next decode adds the new `hi`.

## alloca

Color-escape rewrites dynamic `alloca` to `goc_dynalloc` / `goc_dynrelease`
before coloring. The result is a `cptr` that lives until the function
returns. Go's pcsp table cannot describe a mid-frame SP change, so this is
not a variable-length goroutine frame. Contract: syntax-guide §7.2.

## Not supported

- Full ISO C or a hosted libc.
- AVX, x87, and EH on the Go-callable path.
- `stacksave` / `stackrestore`, and a VLA that does not lower to `alloca i8` with alignment ≤ 16.
- Go ABIInternal for variadics and unsupported aggregates.
- Using `sptr` / `uptr`, or a context that holds them (`JSContext`), on a goroutine other than the owner. The current word has no goroutine pointer, so the offset is added to the wrong `stack.hi`. Contract: syntax-guide §8.3. A later opt-in switch can carry the goroutine pointer; the default stays one word. [todo.md](todo.md).
- linux/arm64 for QuickJS: the compiler and the Go side work under qemu, the QuickJS host does not yet. [todo.md](todo.md).
