# Architecture

Compile pointer-colored C so it runs on the goroutine stack and participates
in Go stack growth, GC stackmaps, and write barriers for `gptr`.

## Pipeline

```text
.c / .goc
    │
    ▼
Clang 19  (Attr.td + SemaGocColors)
    │  stack pointer stored to a non-stack T*  →  uptr encode on store,
    │  decode on load
    │  returning a raw sptr is a hard error
    ▼
LLVM IR  (!goc.color)
    │
    ▼
frontend/color-escape
    │
    ▼
opt O$GOC_OPT_LEVEL (QuickJS: O2), then goc-stackmap / goc-reanchor
    │  frame addresses used after a safepoint are re-derived as plain GEPs
    │  (goc.fa; GOC_FRAMEADDR_MODE=asm keeps the legacy opaque leaq)
    │  hot uptr helpers are inlined to a volatile FS:-8 load
    ▼
goc-llc (llc-19 + GocStackmapPlacement + post-RA GocFrameAddrFix)  →  ELF
    │  frame addresses still held in a callee-saved register or spill slot
    │  across a call are re-derived from rbp (or rebased by the rbp delta)
    │  right after the call; stack moves only happen inside calls
    │
    ▼
elfpack  →  goobj
    │
    ▼
Go link
```

`backend/realbody/` is this path. It does not use seed MIR templates.

## Pieces

| Path | Role |
|------|------|
| `include/goc.h` | `cptr` / `sptr` / `uptr` / `auto_ptr` / `gptr` |
| `clang/patches`, `clang/sema` | In-tree Sema for LLVM 19.1.7 |
| `clang/plugin` | Same checks as an out-of-tree plugin |
| `frontend/color-escape` | Escape refine; lowers dynamic `alloca`; inserts uptr encode/decode |
| `backend/pass` | Stack maps, safepoint spills, morestack insertion |
| `backend/goobj/elfpack` | ELF bytes and relocations → goobj |
| `runtime/uptr` | MSB encode/decode; `g` from `FS:-8`; `goc_dynalloc` / `goc_dynrelease` |

Drivers pick clang via `GOC_CLANG`, then
`third_party/llvm-*-clang-build`, then system `clang-19` plus the plugin.

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
- linux/arm64. The product backend is amd64. Next version: [todo.md](todo.md).
