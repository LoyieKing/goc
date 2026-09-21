# P22 — production `g->stack.hi` via TLS

**Date:** 2026-09-21 (Asia/Shanghai)  
**Contract:** `goc-syntax-guide.md` v0.2.1 §3.3 · P19 MSB protocol  
**Reuse:** same amd64 TLS convention as P1 morestack / P5 `FS:-8`

## Convention (linux/amd64 Go)

```text
movq %fs:-8, %rax     // g   (Go TLS slot; assembler may write FS:-8)
movq 8(%rax), %rax    // g.stack.hi   (stack.lo @ 0, stack.hi @ 8, stackguard0 @ 16)
```

`goc_stack_hi()` / `goc_uptr_from_sptr` / `goc_uptr_as_sptr` use this when built with
`-DGOC_UPTR_HAVE_TLS` (P22 Go harness `.syso`).

## Why inline asm (not a second .S + C call)

Go `CGO_ENABLED=0` internal linking includes `.syso` object code but **does not
reliably apply** clang `R_X86_64_PLT32` / `GOTPCREL` / `.bss` PC32 relocations.
Cross-object `call goc_runtime_stack_hi` from C therefore mis-resolves.

P22 inlines `%%fs:-8` in `goc_uptr_runtime.c` under `GOC_UPTR_HAVE_TLS` so the
`.syso` has **no PLT/GOT relocs**. The separate `goc_uptr_tls_amd64.S` remains as
a documented leaf matching P1/P5 for consumers that link via Go `CALL` (Go asm
relocs are fine).

## API resolution

| Build | `goc_stack_hi()` |
|-------|------------------|
| Host P19 (default) | `goc_test_set_stack_hi` → else page-approx |
| Go P22 (`HAVE_TLS`) | live `g->stack.hi` via FS:-8 |
| Explicit | always `*_hi(…, stack_hi)` |

## Stack move proof

Harness encodes a live stack local with **default** `goc_uptr_from_sptr` (TLS hi),
forces deep `hugeFrame` growth, then `goc_uptr_as_sptr` with the **new** TLS hi.
Same encoded word → `abs' = abs + (hi' - hi)`. If a run does not relocate, the
harness also proves the invariant with simulated `hi+δ` via `*_hi`.

## Non-goals

No dsptr. No sptr auto-promote. No QJS. No general SelectionDAG.
