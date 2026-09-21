# P21 vertical dataflow

**Date:** 2026-09-21 (Asia/Shanghai)  
**Contract:** `goc-syntax-guide.md` v0.2.1

```text
.c / color.ll (!goc.color / !goc.prov)
  → goc-color-escape (P17)          [when starting from .c]
  → goc-color-bridge (P18)
       attrs: goc-store-gptr | goc-spill-gptrs | goc-color-driven
       + *.bridge.txt fingerprints
  → goc-p21-vertical
       MIR seed GENERATED from attrs (hold-across-CALL / minimal)
         ✗ NOT p5-machinepass-goobj/pass/harness.mir
       → GocSpillGptrsAtSafepoints / GocEmitPointerMaps / GocExpandStoreGptr
       → p21.analysis.mir + maps (color_driven)
       → Go-frame physreg lower selected by COLOR attrs
       → vertical.mir + vertical.meta.json (printMIR; color_fp)
  → mirguard identity → llc-19 → elfpack
  → p21_funcs.o
       TEXT: main.P21GptrStoreWB, main.P21SptrAcrossCall
       maps: gclocals.p21SptrLocals / p21SptrArgs
```

## What is proven

| Demo | Color input | Artifact evidence |
|------|-------------|-------------------|
| gptr store | `!goc.color !{!"gptr"}` store | bridge `need_wb=1` → recipe `color_wb` → MIR `gcWriteBarrier2` → nm `P21GptrStoreWB` |
| sptr across CALL | `!goc.color !{!"sptr"}` live across call | bridge `need_spill_maps=1` → Locals maps → nm `P21SptrAcrossCall` + `gclocals.p21Sptr*` |

## Honest gaps

- No general SelectionDAG / ISel from arbitrary color.ll.
- Go-frame bodies are **attr-selected** rebuilds (same honesty as P16 lower),
  but seed + Spill/Maps/WB + TEXT names are color-pipeline-owned.
- Interprocedural color / QJS / production `stack.hi` TLS still out of scope.
