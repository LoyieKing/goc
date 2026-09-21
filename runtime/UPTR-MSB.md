# uptr MSB protocol (P19 / P22)

**Contract:** `goc-syntax-guide.md` v0.2.1 §3.3  
**Date:** 2026-09-21 (Asia/Shanghai)

## Word layout (amd64 user VA)

| MSB | Meaning | Decode |
|-----|---------|--------|
| 0 | Absolute `cptr` address | Use word as pointer |
| 1 | `int64` two's-complement offset from **owner g** `stack.hi` | `abs = (uintptr_t)((int64_t)hi + (int64_t)stored)` |

Stack grows down → typical `abs < hi` → offset negative → **MSB naturally 1** on two's-complement.

## API

| Function | Behavior |
|----------|----------|
| `goc_uptr_from_cptr` / `_hi` | Store abs; **FATAL** if MSB already set |
| `goc_uptr_from_sptr` / `_hi` | `off = abs - hi`; **FATAL** if MSB cleared (abs not below hi) |
| `goc_uptr_as_cptr` / `_hi` | Require MSB=0; else **FATAL** |
| `goc_uptr_as_sptr` / `_hi` | Require MSB=1; `abs = hi + off`; else **FATAL** |

`*_hi` takes explicit `stack_hi` (tests / IR lower / freestanding).

Non-`_hi` uses `goc_stack_hi()`:

1. `goc_test_set_stack_hi` (host unit tests) when not `GOC_UPTR_HAVE_TLS`
2. **P22:** live `g->stack.hi` via TLS `FS:-8` when built with `GOC_UPTR_HAVE_TLS`
3. Host page-approx fallback otherwise

See [`../../p22-uptr-tls/docs/TLS-HI.md`](../../p22-uptr-tls/docs/TLS-HI.md).

## Stack move

Heap-resident MSB=1 words are **invariant** under stack copy: if the runtime updates
live absolute pointers by `delta` and sets `hi' = hi + delta`, then the **same**
encoded word still satisfies `hi' + enc == abs'`.

## Cross-g

Decoding MSB=1 with a non-owner `stack.hi` is a logic error (wrong abs). Tag-mismatch
FATAL is separate; owner discipline is engine-level (one JS runtime ↔ one owner g).

## P18 consumer note (`goc-color-uptr-encoded`)

Treat the machine word as opaque MSB-tagged: do not put in Go pointer maps as an
absolute pointer; do not apply gptr write barrier; decode with owner `stack.hi`.

## Non-goals

No `dsptr`. No silent `sptr`→`uptr` promote. QJS / interproc / general SDAG — later.
