# Roadmap

## Where we are — P29

**Done (experimental):**

1. In-tree Clang 19.1.7 Sema for goc pointer colors (`Attr.td` + `SemaGocColors.cpp`)
2. Product driver prefers patched clang without `-fplugin`
3. Real-body goobj path: Clang IR → llc ISel → elfpack (explicitly **not** P21 seed MIR)
4. Curated goldens: Sema OK / Sema error / realbody magic proof
5. Go amd64 ABIInternal entry thunks (integer/floating-point, stack arguments,
   supported mixed aggregate results and literal aggregate parameter leaves)
6. Multi-function TUs (`goc build --all`): per-function frames from the llc prologue
7. quickjs-ng colored (default `cptr`) + built by `goc build` (4 TUs → goobj) and
   linked into a Go binary with a freestanding libc shim; `JS_Eval` and the
   Promise parent hook run on movable goroutine stacks with real pointer maps
   (`GOC_SPTR_MAPS=1 GOC_CRESERVE=8192 QJS_EVAL=1 QJS_PROMISE=1`).
8. Compiler-owned `T *` → `uptr` storage promotion, including first fields and
   phi-selected destinations; aggregate memcpy roots and SysV split-stub
   pointer arguments survive stack copies without manually changing QuickJS's
   pointer-field declarations. Stack checks and regexp rollback use a separate
   reproducible patch in `scripts/qjs-gstack.patch`.
9. Dynamic `alloca(i8,size)` lowered to scoped `goc_malloc` storage before coloring,
   because Go pcsp cannot describe variable C stack adjustments.
10. Call-bearing functions may inline. `goc-inline-gate` does not stamp
    `noinline`. Stack-pointer arguments are reloaded after safepoints, and a
    local that keeps `&s->token` is a frame-map root. V8-v7 median 791
    (785 and 797, 48 s) versus 1389 for the same-source native Release build.

## P29 gaps (honest next milestones)

1. **ABIInternal** for unsupported byval/identified/merged-class aggregates,
   variadics and other non-C-compatible signatures. Eligible cross-TU calls
   already resolve to the SysV `.impl` body.
2. Run the full analysis MachineFunction pipeline (Spill → Maps → StackCheck /
   morestack → RebuildLIS → WB) on **real ISel MF**, not seed templates
   (llc+elfpack currently combines IR stackmap records and post-PEI MIR offsets;
   the complete real-MF pass sequence remains separate).
3. The same-source gap to native QuickJS-ng is about 1.8×, concentrated in
   call-heavy suites (DeltaBlue 2.6×, Richards/EarleyBoyer ~2.1×). Splittable
   entries still pay a TLS stack check, tail calls and shrink-wrapping stay
   off so pcsp remains exact, and stack pointers are reloaded after safepoints.
   Numeric loops (NavierStokes 1.12×) are not the gap.
4. Extend host compatibility beyond the exercised `qjs:std` FILE/process,
   `qjs:os` filesystem/timers/cooperatively scheduled workers, and bjson paths.
   A coalescing C allocator with mmap growth passes `test_builtin.js`; the
   selected upstream suite has 115 PASS, zero FAIL, and one explicit
   UNSUPPORTED 2 GiB memory-stress case. Dynamic alloca lifetimes outside the
   supported form still fail closed.
5. GC/preemption safety across arbitrary JS programs is not claimed.
   `GOC_SPTR_MAPS=1` is the QJS default, not a hidden experimental switch.

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
