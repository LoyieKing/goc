# Roadmap

## Where we are — P28

**Done (experimental):**

1. In-tree Clang 19.1.7 Sema for goc pointer colors (`Attr.td` + `SemaGocColors.cpp`)
2. Product driver prefers patched clang without `-fplugin`
3. Real-body goobj path: Clang IR → llc ISel → elfpack (explicitly **not** P21 seed MIR)
4. Curated goldens: Sema OK / Sema error / realbody magic proof

## P29 gaps (honest next milestones)

1. **ABIInternal (amd64)** on arbitrary Clang-compiled functions (callee, then caller)
2. Run the full analysis MachineFunction pipeline (Spill → Maps → StackCheck /
   morestack → RebuildLIS → WB) on **real ISel MF**, not seed templates
3. Multi-function TUs / richer types / cross-file lowering
4. Full **quickjs-ng** coloring + `goc build` (NAN boxing off; stack discipline)

## Non-goals for the near term

- Pretending AVX / x87 / EH work on the Go-callable path
- Prioritizing O2 / fancy GC over correctness contracts
- Vendoring LLVM or quickjs-ng blobs into this repo

## Historical phases (summaries only)

See [phases/](phases/) for condensed P8–P28 reports. The private research tree
that produced them is not part of this public export.

| Band | Theme |
|------|--------|
| P0–P7 | LLVM ↔ Go stack experiments, early lowering |
| P8–P16 | Backend: MIR passes, goobj, stackmap/WB; P16 closes AVX/x87/EH |
| P17–P22 | Color IR, bridge, uptr MSB/TLS |
| P23–P26 | QJS slices / safepoints / real quickjs-ng smoke (not full product) |
| P27 | Out-of-tree Clang plugin Sema |
| P28 | In-tree Sema + real-body goobj |
| P29+ | ABIInternal + full MF pipeline + QJS via goc |
