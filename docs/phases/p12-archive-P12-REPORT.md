# P12 归档报告 — LLVM MC encoding（禁止 demo opcode table）

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**前端：** 仍延后

## 用户硬性约束（已遵守）

- **Do NOT** take shortcuts with hand-grown / generated demo opcode tables as the encoder.
- Use **LLVM’s own MC / CodeGen** (`llc` → X86 AsmPrinter / MCCodeEmitter / AsmBackend).
- Pack into goobj with existing maps (FUNCDATA); Go `obj` libs OK for the **container**, not for instruction encoding.
- Aborted mid-flight custom `X86GenInstrInfo` → Go table generator path when steered to LLVM MC.

## PASS 证据

```text
PASS L: checked entry + MIR/goobj leaf after growth = 42
PASS W: store_gptr WB enabled path hits=200
PASS S: live *int across CALL+morestack with LocalsPointerMaps from pass
PASS A: ArgsPointerMaps keep arg *int across CALL+morestack
PASS S2: two live *int across CALL+morestack (Go SP layout, not FI*8)
PASS S3: register-only *int across CALL+morestack (LiveIntervals spill→Locals)
PASS p5-machinepass-goobj (L+S+S2+S3+A[+W])
```

一键：`./p5-machinepass-goobj/build.sh`

## Architecture

```text
pass/harness.mir
    → goobj/llvmmc/mircanon.py     (dialect normalize ONLY; no encoding)
    → llc-19 -O0 -relocation-model=pic -filetype=obj
         (X86 AsmPrinter + MCCodeEmitter + AsmBackend)
    → build/pass-out/harness.llc.o (ELF64)
    → goobj/elfpack               (bytes + ELF→goobj relocs + FUNCDATA from maps
                                   + pcsp rebuild for Go frame maps)
    → build/goobj/goc_funcs.o
```

### Generator / table status

**Aborted as primary.** No checked-in `x86_instr_table.gen.go` encoder.  
Opcode **bytes** come from LLVM MC via `llc-19`.

### LLVM APIs / tools used

| Tool / API | Role |
|------------|------|
| `llc-19` | MIR parse + object emission |
| X86 AsmPrinter | MachineInstr → object |
| MCCodeEmitter | raw opcode bytes |
| AsmBackend / ELF ObjectWriter | `R_X86_64_PLT32`, `R_X86_64_PC32` |
| IR attr `no_callee_saved_registers` | avoid SysV CSR pushes that break Go ABIInternal |
| `mircanon.py` | `&`→`@`, CFG split for morestack, PIC `$rip`, strip `goc.*`/`GOC_PCDATA1`, Go-style `PUSH BP`+`SUB $frame` |
| `goobj/elfpack` | ELF→goobj; reloc addend `A' = A+4`; FUNCDATA; constant pcsp (`frame+8` with BP) |

### Demoted

- `goobj/mirlower` per-opcode `switch` — **not** harness TEXT encoding path (legacy / diagnostics only)
- `goobj/binwriter` Prog-lowering path — replaced in `build.sh` by `llvmmc/run_llvmmc.sh`

## Honest remaining FATALS / holes

- `mircanon` CFG rewrite is harness-shaped; arbitrary clang `-O0` MIR may need more repair before `llc`
- Dense per-call PCDATA not rebuilt from ELF (function-wide stack-map index 0)
- AVX / x87 / EH: `llc` can encode; harness/goobj ABI+maps not validated
- Absolute null `MOV32mi` crash stub remains intentional
- DWARF file attribution may show `stubs_amd64.s`; provenance is `ENCODING.txt=llvm-llc-mc`

## Touched files

- `goobj/llvmmc/` — mircanon, run script, README, tests  
- `goobj/elfpack/` — ELF→goobj packer  
- `build.sh` — primary path → llvmmc  
- `docs/MI_AND_GOOBJ.md` §P12, `PLAN.md`, `roadmap-next.md`  
- `goobj/mirlower` — demoted Limits / package comment  
