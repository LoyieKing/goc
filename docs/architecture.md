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
opt O3, then goc-stackmap / goc-reanchor
    │  frame addresses used after a safepoint are rematerialized from rbp
    │  hot uptr helpers are inlined to a volatile FS:-8 load
    ▼
llc ISel  →  ELF
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
| `frontend/color-escape` | Escape refine; inserts uptr encode/decode |
| `backend/pass` | Stack maps, safepoint spills, morestack insertion |
| `backend/goobj/elfpack` | ELF bytes and relocations → goobj |
| `runtime/uptr` | MSB encode/decode; `g` from `FS:-8` |

Drivers pick clang via `GOC_CLANG`, then
`third_party/llvm-*-clang-build`, then system `clang-19` plus the plugin.

## uptr

One machine word.

- MSB 0: absolute address.
- MSB 1: `int64` offset from the owner goroutine's `g.stack.hi`.
  `abs = stack.hi + stored`.

`stack.lo` is `g+0`, `stack.hi` is `g+8`. A stack copy does not rewrite a
stored offset; the next decode adds the new `hi`.

## Not supported

- Full ISO C or a hosted libc.
- AVX, x87, and EH on the Go-callable path.
- Go ABIInternal for variadics and unsupported aggregates.
- Decoding `uptr` with a goroutine other than the owner.
