# Architecture

## Goals

Compile a pointer-colored C dialect so functions can execute on the **goroutine
user stack**, participating in Go’s:

- stack growth (`morestack`)
- GC stackmaps (args / locals pointer maps)
- write barriers for `gptr` stores

## High-level pipeline

```text
  .c / .goc
      │
      ▼
  Clang 19  (+ goc Attr.td + SemaGocColors)
      │   native attrs → AnnotateAttr "goc.color.*"
      │   Sema + IR: stack pointer stored to heap/global T* → uptr;
      │   raw sptr return / unsupported escape = hard error
      ▼
  LLVM IR   (!goc.color metadata / annotations)
      │
      ├─► optional: frontend/color-escape  (IR refine)
      ├─► optional: frontend/color-bridge  (WB / spill recipes)
      │
      ▼
  P28 product lower (backend/realbody)
      Clang IR ──llc ISel──► ELF .o ──elfpack──► goobj .o
      meta.encoding = "clang-real-isel"   (NOT P21 seedMIR)
      │
      ▼
  Go toolchain links / calls TEXT symbols
```

## Frontend

| Component | Role |
|-----------|------|
| `include/goc.h` | Public macros: `cptr`/`sptr`/`uptr`/`auto_ptr`/`gptr` |
| `clang/patches` | Attr.td colors + Sema wiring for LLVM **19.1.7** |
| `clang/sema/SemaGocColors.cpp` | Drop-in Sema implementation |
| `clang/plugin` | Out-of-tree plugin (P27 legacy) when in-tree clang absent |

Drivers resolve clang via `GOC_CLANG`, then `third_party/llvm-*-clang-build`,
then system `clang-19` + plugin.

## Backend (research)

Under `backend/` (evolved from the P5–P16 machine-pass / goobj work):

- MIR passes: spill gptrs at safepoints, emit pointer maps, insert stack check /
  morestack, rebuild safe liveness, expand `store_gptr` WB
- `goobj/` encoder + `elfpack` to produce Go-linkable objects
- `backend/realbody/` — P28 path that **rejects** seed-MIR templates

Contract (P16): AVX / x87 / EH are unsupported on the Go-callable path.

## Runtime helpers

`runtime/` holds `uptr` MSB encode/decode and TLS `stack.hi` helpers (P19/P22).
These are building blocks, not a full language runtime.

## What is intentionally out of scope (for now)

- Full ISO C / libc
- Complete QuickJS-ng coloring and embed
- Generic “speed up all cgo”
- O2 / advanced GC integration as a priority

See [status.md](status.md) and [roadmap.md](roadmap.md).
