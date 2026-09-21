# P18 dataflow: color.ll → maps / WB → goobj

**Date:** 2026-09-21 (Asia/Shanghai)  
**Contract:** `goc-syntax-guide.md` v0.2.1

```text
P17: .c|.goc + goc.h
  → clang-19 -emit-llvm
  → goc-color-escape
  → *.color.ll   (!goc.color / !goc.prov / !goc.uptr_encoded)

P18 bridge:
  → goc-color-bridge
  → *.bridged.ll + *.bridge.txt
     attrs:
       goc-color-driven=1
       goc-store-gptr / goc-color-wb     ← gptr (goheap) stores
       goc-spill-gptrs / goc-emit-maps   ← sptr|stack map-ptrs live across CALL
       goc-color-cptr-only               ← cptr/cheap; no WB
       goc-arg-ptr-colors=gptr|sptr|cptr|…

P18 machine (reuse P5 passes):
  → goc-p18-driver seeds MIR (hold-across-CALL / minimal)
  → GocSpillGptrsAtSafepoints   (R5 color-driven ID)
  → GocEmitPointerMaps          (args bits from goc-arg-ptr-colors)
  → GocExpandStoreGptr          (WB when goc-store-gptr|goc-color-wb)

Downstream (unchanged P5–P16 path when packing goobj):
  → recipe / maps.bin → elfpack FUNCDATA → goobj
```

## Color → behavior

| Color / prov | WB | Spill + Locals/Args maps |
|--------------|----|---------------------------|
| `gptr` / goheap store | yes | only if live across CALL |
| `sptr` / stack across CALL | no | yes |
| `cptr` / cheap | no | no (unless value truly in stack range; not this slice) |
| `uptr` encoded | no auto WB | MSB encode: **P19** `libgoc_uptr` / `goc-uptr-lower` (see `../../p19-uptr-runtime/docs/UPTR-MSB.md`) |

Illegal `sptr`→heap remains a **P17 frontend error**; P18 does not auto-promote.
