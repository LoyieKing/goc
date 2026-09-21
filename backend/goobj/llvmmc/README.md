# LLVM MC encoding path (P12–P15)

**Instruction encoding is LLVM’s**, not a goc opcode whitelist.

## Pipeline (P15)

```text
goc-pass-driver (printMIR)
        │
build/pass-out/harness.mir       Pass-exported llc-19-ready YAML MIR
build/pass-out/harness.meta.json sidecar: go_sym / frame / flags / calls[]
pass/harness.mir                 golden/reference only (not hot path)
        │
   mirguard.py            identity check only; FATAL if dialect leftover
        │                 (no MIR body rewrite; cmp -s input == out)
        │
   llc-19 -O0 -relocation-model=pic -filetype=obj   ← DIRECT on Pass MIR
        │              X86 AsmPrinter / MCCodeEmitter / AsmBackend
        ▼
   harness.llc.o  (ELF64)
        │
   goobj/elfpack  →  goc_funcs.o + FUNCDATA + dense PCDATA at CALLs
```

## Dialect transforms: ZERO (P14)

Previously (P13) mircanon still rewrote `&`→`@`, `CMP64ri`→`CMP64ri32`, stripped
`goc.*` / `GOC_PCDATA1`, injected `$rip` / CALL implicits / IR stubs.

**Now the Pass MIR producer emits llc-native forms**; metadata is out-of-band.
`mircanon.py` is an identity shim → `mirguard.py`. Build FATALS if any rewrite
def returns (`rewrite_*`, `split_morestack_cfg`, `inject_frame`, …).

| Former transform | Producer fix |
|------------------|--------------|
| `goc.*` / `GOC_PCDATA1` | `build/pass-out/harness.meta.json` (Pass) |
| `&sym` | emit `@sym` |
| noreg abs PIC | emit `$rip, …, @sym` |
| `CMP64ri` | emit `CMP64ri32` |
| CALL rsp implicits | emit full `implicit-def $rsp/$ssp` |
| `bb.N.name:` | emit `bb.N:` |
| IR stubs + `no_callee_saved_registers` | embedded in `harness.mir` header |

## Float / Go ABIInternal (amd64)

| | |
|--|--|
| Float args/results | **X0–X14** (X15 = zero) |
| Integer args | AX, BX, CX, DI, SI, R8, R9 |
| `goc_fadd64` | `ADDSDrr $xmm0, $xmm1` → Go `GocFadd64(a,b float64) float64` |
| `goc_fadd32` | `ADDSSrr` → `GocFadd32` |
| x87 | **unsupported FATAL** on Go-callable / pack path |
| AVX | llc encode + elfpack smoke; scalar Go PASS is SSE2 |

Ref: `cmd/compile/abi-internal.md` (amd64).

## Regenerate / run

```bash
./goobj/llvmmc/run_llvmmc.sh
./goobj/llvmmc/check_fixtures.sh
```
