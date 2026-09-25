# Status — what works / what does not

**As of:** 2026-09-25 · Phase **P29**  
**Honesty first:** this is a research compiler, not a production toolchain.

## Works

| Area | Evidence |
|------|----------|
| In-tree Clang Sema pointer colors | `tests/sema/01_ok_*.c` with patched clang, no `-fplugin` |
| Pointer storage contract | Bare `T *` cptr/auto heap/global slots are promoted to encoded `uptr` on stack-pointer stores; ineligible escapes and raw `sptr` returns remain errors (`frontend/color-escape/build.sh`, 10 PASS) |
| Real-body goobj (not P21 seed MIR) | `tests/realbody/03_ok_realbody_goobj.c` → magic `0x28C0DE42` in TEXT; meta `encoding=clang-real-isel` |
| Out-of-tree plugin fallback | `clang/plugin` + `./cmd/goc test --p27` |
| Go amd64 **ABIInternal** entry thunks (supported scalar and aggregate subset) | `scripts/test-p29-goabi.sh`: integers/floats, stack arguments, mixed-class results, register and stack pointer arguments across growth |
| Multi-function TU → goobj (`--all`) | `goc build --all`; frames derived per function from the llc prologue |
| Backend P5–P16 pipeline on assertions-enabled LLVM | `backend/build.sh` → `PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])` |
| quickjs-ng **coloring** (default `cptr` + explicit overrides) | `GOC_DEFAULT_PTR_COLOR=cptr` compiles all four TUs; the historical 17 diagnostics are not 17 actual escapes. The IR pass encodes `JSRuntime.parent_promise`/`current_stack_frame` without modifying their source-level pointer fields; stack checks and regexp rollback require the reproducible `scripts/qjs-gstack.patch`. |
| `goc build` over quickjs-ng (4 TUs) → goobj | `scripts/qjs-build.sh`: quickjs/libregexp/libunicode/dtoa all emit TEXT + data/GOT symbols |
| QuickJS `-O3` IR build | `scripts/qjs-cli-build.sh` defaults to `GOC_OPT_LEVEL=3`: Clang emits raw `-O3` IR, color analysis and stackmap insertion precede LLVM `default<O3>` (without `argpromotion`/`globalopt`, which invalidate precomputed SysV call maps). `goc-reanchor` rematerializes alloca+constant addresses. `goc-pin-i64` then stores SROA-split JSValue integer halves in dedicated non-pointer allocas and reloads them after safepoints, so llc cannot reuse those spill slots for `g`. With pointer maps enabled, llc still uses `-O0` machine lowering because its `-O3` stack-slot coalescing breaks frame/argument map proofs. This is **not** identical to native GCC `-O3` code generation. |
| QJS execution on a **goroutine stack** | `JS_GetVersion`, `JS_NewRuntime`, `JS_Eval` (`7`), Promise eval (`7`), and Promise parent hook execute via goc thunks with call-bearing functions inlined (`QJS_EVAL=1 QJS_PROMISE=1`, quickjs.c rebuilt 2026-09-25) |
| Go-style **split prologue + morestack stub** in goc TEXT | `GOC_MORESTACK=1`: check loads g from the Go TLS (R14 is an ordinary callee-saved register in LLVM-compiled bodies, so the R14==g invariant does not hold) + `CALL runtime.morestack_noctxt` (its ABI0 form loads g itself) + `JMP entry`; `AttrNoSplit` cleared |
| Whole QJS program **links** (no nosplit/undefined refs) | `scripts/qjs-build.sh` step 3 packs all four TUs plus shim/uptr/probe; split preambles pass Go's nosplit budget |
| llc-baked calls handed to the linker | calls to local `.text` symbols carry no relocation; elfpack decodes the displacement at the meta's disassembly offsets and emits `R_CALL` (by address, so TU-qualified static targets resolve) |
| pcsp matches Go's unwinder contract | fixed C frames and split stubs carry exact SP transitions; dynamically sized `alloca` is lowered to scoped non-stack storage before coloring, so C body SP remains describable |
| Fixed C frame geometry without dynamic realignment | `override-stack-alignment=8` and unaligned SSE moves avoid compiler-generated SP realignment that Go pcsp cannot describe; thunks reserve a fixed caller frame |
| **quickjs-ng running on the Go runtime** | With real pointer maps enabled: `PASS qjs-version`, `qjs-newruntime`, `qjs-link-copy-after-growth`, both mixed heap/stack frame-chain probes, `qjs-eval`, `qjs-promise-hook`, `qjs-freeruntime` |
| Go-driven QuickJS CLI | `scripts/qjs-cli-build.sh` links the four colored TUs. The current `-O3` CLI yielded 115 PASS, 0 FAIL, 1 UNSUPPORTED of 116 tests selected by upstream `tests.conf` (9 exclusions), including `wasi-stack-limit.js`. The runner still exits nonzero because `bug1468.js` (2 GiB) is unsupported, not failed. |
| Reclaimable C allocator and real numeric/time bridges | QuickJS uses a coalescing 64 MiB arena plus demand-paged mmap slabs, Go math/strconv/localtime helpers (including logarithms and trigonometry); upstream `test_builtin.js` passes including 331072 nested Proxies and regexp rollback. The `scripts/qjs-gstack.patch` records upstream moving-stack and regexp capture provenance changes. |
| Per-call sptr locals maps and SysV split-stub maps | `GOC_SPTR_MAPS=1`: IR root allocas, aggregate-field copies and outgoing stack arguments resolve to post-PEI frame offsets; the stub saves GPR/XMM arguments and adjusts pointer-typed register args on growth. GDB observed `JS_CallInternal` stack copies to 64/128 KiB during Promise evaluation |
| goobj / MIR research backend | `backend/` (P5–P16 path; harness goldens historically L/S/S2/S3/A/F/W) |
| `uptr` MSB + TLS stack.hi helpers | `runtime/` |
| Language contract (no `dsptr`) | [syntax-guide.md](syntax-guide.md) |

## V8-v7 benchmark snapshot (2026-09-24, updated 2026-09-25)

The [official QuickJS extras archive](https://bellard.org/quickjs/) supplies
`tests/bench-v8/combined.js` (V8-v7, eight suites). On an AMD Ryzen 9 7900X,
two complete sequential runs per engine produced these **median scores**
(higher is faster):

| Runtime | Score | Median end-to-end time |
|---------|------:|-----------------------:|
| goc-built QuickJS-ng CLI (`-O0`) | 55.3 | 383.9 s |
| goc-built QuickJS-ng CLI (`-O3` IR + `goc_malloc` dynalloc) | 341 | 89.5 s (one run, not a median) |
| goc-built QuickJS-ng CLI (`-O3`, no pin-i64, nosplit leaves) | 403, 413, 414 | 77–79 s (three runs) |
| goc-built QuickJS-ng CLI (`-O3`, call-bearing inline, tagged `&s->token`) | 785, 797 | 48.0 s (two runs, median 791) |
| goc-built QuickJS-ng CLI (`-O3`, inlined uptr + alloca pool bump) | 1198, 1203 | 38.0–39.7 s (rerun, mean 1200.5) |
| Goja (`cfe4039cb6d77b297d8b637182f774fa4a54b7d5`, Go 1.24.4) | 402 | 87.7 s |
| Pristine native QuickJS-ng (`6d46d07d04041b40f4f49eaa7fdebe44c314c699`, GCC Release) | 1389 | 36.3 s |
| Original [Bellard QuickJS 2026-06-04](https://bellard.org/quickjs/) (GCC `-O2`) | 1768 | 30.6 s |

Native QuickJS-ng / Goja / Bellard QuickJS scores are
approximately 25.1× / 7.3× / 32.0× the complete `-O0` goc score.
The 341 row is **one** complete run after `goc-pin-i64` and after
`goc_dynalloc` stopped calling `mmap` on every JS call (exit 0, SCORE 341,
89.5 s), not a two-run median like the other rows. Against that single run,
native QuickJS-ng / Goja / Bellard are about 4.1× / 1.2× / 5.2×. The 414 row
is two later runs: `goc-pin-i64` is not in the pipeline (`-no-stack-slot-sharing`
already prevents the slot-reuse bug), reanchor reloads are coalesced inside a
safepoint-free stretch, and ABI0 leaves whose frame fits in `StackSmall` are
nosplit so they do not pay a TLS stack probe. Three runs scored 414, 403, and
413, all above the Goja median of 402. The 413 run rebuilt every QJS TU with
the leaf rule (Richards 237, DeltaBlue 223, Crypto 304, RayTrace 558,
EarleyBoyer 707, RegExp 136, Splay 1482, NavierStokes 664, 80.3 s). They are
still about 3.4× behind native QuickJS-ng (1389). The 2026-09-25 rebuild
allows call-bearing inline and tags slots that keep `&s->token`. Two
sequential runs of the same `bench-v8.js` scored 785 and 797 (median 791,
47.9–48.0 s), about 1.9× the 413 run and about 1.8× behind native
QuickJS-ng (1389). The per-suite table's 413 column is that earlier all-TU
run; the 791 column is the median of these two runs. The previous
single `-O3` run, still paying a `mmap`/`munmap` pair per call, scored 70.2
in 333.1 s. An earlier attempt faulted in RegExp after EarleyBoyer; that
partial print is not a score.
The 2026-09-25 follow-up inlines `goc_stack_hi` / `goc_uptr_decode` /
`goc_uptr_from_ptr` to a volatile TLS load, and (`GOC_INLINE_DYNALLOC=1`)
bumps the alloca pool in line without the extra memset. Two runs scored
1265 and 1239 on the first pair (38.3–38.5 s). The same binary rerun scored
1198 and 1203 (mean 1200.5, 37.95 s and 39.73 s): 3.0× Goja's 402, and 1.16×
behind the same-source native QuickJS-ng 1389. The per-suite column is that
rerun. Microcall on the rerun: 19556 vs native 23356 (1.19×); depth4 60 ms
vs 45 ms (1.33×).
Native QuickJS-ng built separately at `-O0` scored 385 in one control run.
This earlier goc snapshot was compiled at `-O0`, whereas native Release used
GCC `-O3`; these numbers compare the **complete builds**, not isolated goroutine-stack
overhead. Bellard QuickJS is a different codebase/version and uses `-O2`;
the pristine QuickJS-ng row is the same-source control. Goja uses a pinned
pre-Go-1.25 revision. A 16 MiB QuickJS stack limit was set in the goc and
native QuickJS-ng CLIs (`--stack-size 16384`, KiB); the goc CLI's
larger C frames overflow the default limit in EarleyBoyer. Bellard QuickJS
uses `--stack-size 16777216` in bytes. Native QuickJS-ng
loaded a one-line `console.log` adapter via `-I`, and the goc CLI prepended
that same adapter to the otherwise unchanged `combined.js`; Goja's CLI
and Bellard QuickJS already supply `console.log`. The benchmark's internal
`SCORE` excludes initial parsing and startup; end-to-end time does not.
No benchmark score is
reported for the first goc attempt: it reached an unimplemented `log` shim
and trapped; the Go-backed math functions now run the unmodified timed suites.
Per-run scores, exit codes, commands, and elapsed times from this workstation
are saved in the local `build/qjs/bench-v8-results.json` artifact.

| Suite | goc `-O0` | goc `-O3` pin | goc 413 | goc 791 | goc 1200.5 | Goja | native ng | Bellard |
|-------|----------:|-------------:|--------:|--------:|-----------:|-----:|----------:|--------:|
| Richards | 13.5 | 189 | 237 | 438 | 837.5 | 334 | 914 | 1170 |
| DeltaBlue | 9.23 | 178 | 223 | 360 | 846.5 | 400 | 948 | 1106 |
| Crypto | 90.0 | 265 | 304 | 736.5 | 804 | 166 | 994 | 1274 |
| RayTrace | 50.0 | 450 | 558 | 1014.5 | 1776 | 348 | 1915 | 2576 |
| EarleyBoyer | 60.3 | 585 | 707 | 1264.5 | 2262 | 711 | 2626 | 3132 |
| RegExp | 65.5 | 110 | 136 | 254.5 | 362 | 289 | 454 | 584 |
| Splay | 146 | 1215 | 1482 | 2609.5 | 3449 | 1568 | 4081 | 5038 |
| NavierStokes | 272 | 584 | 664 | 1549.5 | 1511 | 274 | 1733 | 2447 |
| **Overall** | **55.3** | **341** | **413** | **791** | **1200.5** | **402** | **1389** | **1768** |

The independent recursive Earley check that previously returned 43/133 now
returns 42/132 at sizes 6/7. That check is separate from the V8 score above.

## Does not work (yet)

| Gap | Notes / target |
|-----|----------------|
| Arbitrary QuickJS workloads beyond the exercised paths | Four-TU build, interpreter, Promise hook, and the selected JS suite pass except the unsupported 2 GiB stress. The freestanding libc shim is experimental and ordinary pthread callbacks are not real threading support. |
| Complete QuickJS host compatibility | The exercised std FILE/process, os filesystem/timers/serialized workers, bjson, and JSON/text/bytes import paths run. Worker runtimes are cooperatively scheduled on one goroutine, not native threads; other host APIs and 2 GiB `bug1468.js` stress are not established by this suite. |
| Full real-MF analysis pipeline | Arbitrary IR still uses llc+elfpack with LLVM stackmap records and MIR frame offsets, not the complete P5–P16 MachineFunction pipeline. Pointer maps are experimental and off by default in the generic driver; the QJS script opts in. Without `GOC_SPTR_MAPS=1`, sptr-bearing frames fail closed on growth |
| ABI coverage | Supported scalar and literal aggregate eightbyte cases work; byval/identified or merged-class aggregates, variadics and unsupported signatures remain SysV-only |
| Dynamic C frame limitations | `alloca(i8, size)` at alignment ≤16 lowers to a scoped `goc_dynalloc`/`goc_dynrelease` (not a mid-frame SP change). The QJS build defines `GOC_DYNALLOC_POOL`: one 16 MiB heap pool, then +size/−size. That pool is single-thread only. Without the macro each alloca is still `goc_malloc`/`goc_free`. `llvm.stacksave/stackrestore`, IR EH exits and other unmodelled dynamic allocation forms fail closed. Non-IR C `longjmp` across a live dynamic scope is not supported |
| Complete Go amd64 **ABIInternal** | Partial research only |
| AVX / x87 / EH on Go-callable path | Permanently unsupported (P16 contract) |
| Multi-TU / LTO-grade lowering | Not started |
| Stable releases / installers | `main` only |

## How to verify locally

```bash
export GOC_ROOT="$(pwd)"
export GOC_CLANG=/path/to/patched/clang
GOC_OPT_LEVEL=3 GOC_SPTR_MAPS=1 GOC_CRESERVE=8192 QJS_EVAL=1 QJS_PROMISE=1 ./scripts/qjs-build.sh
./scripts/qjs-cli-build.sh
./build/qjs/qjscli --stack-size 16384 -e 'print(Math.log(Math.E))' # stack size in KiB
./scripts/qjs-cli-tests.sh # O3: 115 pass, 0 fail, 1 unsupported; exits nonzero
./cmd/goc test --p28
```

Plugin-only (no patched clang):

```bash
./cmd/goc test --p27
```

## Phase summaries

Condensed reports: [phases/](phases/). Prefer those over archaeology of the
private research tree.
