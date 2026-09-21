# P17 IR color representation

**Contract:** `goc-syntax-guide.md` v0.2.1  
**Date:** 2026-09-21 (Asia/Shanghai)

## Source surface (`include/goc.h`)

| Source | Mechanism |
|--------|-----------|
| `cptr(T)` / `sptr(T)` / `uptr(T)` / `auto_ptr(T)` / `gptr(T)` | `T *` + `annotate("goc.color.<name>")` |
| bare `T *` | ≡ `auto_ptr` (no annotate; pass infers) |
| `goc_uptr_from_sptr` / `from_cptr` / `as_*` | named builtins recognized by pass |
| `JSValue` | explicit `{ int tagged_value; cptr(JSObject) pointer; }` |

No `dsptr`. No silent `sptr`→`uptr` promote.

## Clang → LLVM IR

Clang 19 emits:

- Locals: `call void @llvm.var.annotation(..., "goc.color.sptr", ...)`
- Struct fields (on GEP): `call ptr @llvm.ptr.annotation(..., "goc.color.uptr", ...)`
- Globals: `@llvm.global.annotations` entries

## Pass output metadata

After `goc-color-escape`:

- Instruction metadata `!goc.color !{!"sptr"}` (also `cptr`/`uptr`/`gptr`/`auto`)
- Optional `!goc.prov !{!"stack"|"cheap"|"goheap"}`
- Optional `!goc.uptr_encoded !{!"1"}` when value came from `goc_uptr_from_*`
- Module flag `goc.color.schema` = `annotate+!goc.color;v0.2.1-P17`

## Escape rules enforced

1. Store of `sptr` / raw stack provenance into non-stack location → **error**
2. Return of stack pointer → **error**
3. Store into `uptr` field without encoded uptr → **error**
4. `gptr` ↔ `cptr`/`sptr`/`uptr`/`auto` store → **error**
5. Stack-only `auto_ptr` field / local refined to `sptr` without forcing uptr

## Pipeline

```text
clang-19 -emit-llvm -S -O0 -I include foo.c|.goc
  → goc-color-escape foo.ll -o foo.color.ll
```

P18 consumes `!goc.color` via `goc-color-bridge` → P5 Spill/EmitMaps/ExpandStoreGptr (see `../../p18-color-bridge/docs/DATAFLOW.md`).

## P19 uptr MSB

Named builtins are no longer identity stubs at runtime/IR-lower:

- Host: link `p19-uptr-runtime/build/libgoc_uptr.a` (`goc_uptr.h`)
- IR: `goc-uptr-lower` expands calls to `sub`/`add`/`and 1<<63` + FATAL guards
- `*_hi` variants recognized by color-escape (same encode metadata)

See `../../p19-uptr-runtime/docs/UPTR-MSB.md`.
