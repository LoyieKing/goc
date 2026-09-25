# P5b：如何构造真 X86 MachineInstrs + 二进制 goobj + MIR liveness

**日期：** 2026-09-21（Asia/Shanghai）

## 1. MachineInstrs（`GocInsertStackCheck`）

Distro `llvm-19-dev` **不安装** `llvm/lib/Target/X86/X86InstrInfo.h`；本树用 `_deps/llvm-project-19.1.7` + `X86CommonTableGen` 提供真头（Lite 仅 fallback）。本 pass **不**走 INLINEASM / ANNOTATION_LABEL。

做法：

1. 从 `MF.getSubtarget().getInstrInfo()` 取得 `TargetInstrInfo`（亦即 `MCInstrInfo`）。
2. 用 `MCInstrInfo::getName(opc)` **按名解析** opcode：`MOV64rm`、`CMP64rm`、`JCC_1`、`CALL64pcrel32`、`JMP_1`。
3. 用 `TargetRegisterInfo` / `MCRegisterInfo::getName` 解析 `RAX`、`RSP`、`FS`。
4. `BuildMI` 插入真实 MI：

| 步骤 | MI | 含义 |
|------|-----|------|
| 1 | `MOV64rm RAX, FS:-8` | Go linux/amd64 TLS → `g` |
| 2 | `CMP64rm RSP, 16(RAX)` | `SP` vs `g.stackguard0`（offset **16**） |
| 3 | `JCC_1 morestack, CondCode=6 (BE)` | **JBE** → morestack |
| 4 | `CALL64pcrel32 runtime.morestack_noctxt` | 扩栈 |
| 5 | `JMP_1 check` | 重入检查块 |

CFG：`bb.check` → (`JBE`) `bb.morestack` → `JMP` 回 check；fallthrough → `bb.ok`（原函数体）。

产物：`build/pass-out/goc.mir`、`stackcheck.recipe.txt`（含 `mi_path=real_x86_opcodes`）。

## 2. Write barrier MIs（`GocExpandStoreGptr`）

同样按名解析，**无 INLINEASM**：

| 步骤 | MI | 含义 |
|------|-----|------|
| 1 | `MOV64rm R14, FS:-8` | `g`（gcWriteBarrier2 要求 R14） |
| 2 | `CMP32mi runtime.writeBarrier, $0` | 读 `writeBarrier.enabled` |
| 3 | `JCC_1 dowrite, CondCode=4 (E)` | **JE** 关屏障 |
| 4 | `ADD64mi8 main.wbPathHits, $1` | harness 命中计数 |
| 5 | `CALL64pcrel32 runtime.gcWriteBarrier2` | R11←buf |
| 6–8 | `MOV64mr` buf[0]=new、load old、buf[1]=old | Demo C 合同 |
| 9 | `MOV64mr (CX), AX` | dowrite 存槽 |

recipe：`mi_path=real_x86_opcodes_wb`。

**二进制体：** `goobj/binwriter` 发等价 `obj.Prog`（TLS→R14 或汇编器折叠为 `FS:-8`），`TEXT main.StoreGptrWB`，**不**再放在 `stubs_amd64.s`。

## 3. MIR liveness → pointer maps（`GocEmitPointerMaps`）

### 算法

1. **发现 gptr 槽：** MIR store 带 `MachineMemOperand` 指向 **指针类型 alloca**（或 IR 中 `alloca ptr` 关联的 FI+disp）。  
2. **Go SP offset：** 单 locals blob（`goc-frame-locals-bytes=24`）→ offset = store **disp**（hold 合同 SP+16）；多 FI 时 creation-order rank×8 + disp。  
3. **Forward dataflow：** 每 BB 维护 live `GoSpOff` 集；store 置位；**CALL**（含 morestack）为 safepoint，快照并入函数级 union。  
4. **Args：** 扫描 IR 形参，`ptr` → 对应 word bit。  
5. 写出 `locals_map.bin` / `args_map.bin` / `maps.txt`（`locals_source mir_liveness`，`args_source mir_ir_args`）。

### 限制

- 真 `LiveIntervals`（`LiveIntervalsWrapperPass`）；safepoint 前 spill gptr vreg → Locals maps；不发射 regmap 位。  
- 循环靠单调并集收敛（有限迭代）。  
- `goc-live-gptr-sp-offs` **仅** `goc-live-gptr-debug-override=1` 时覆盖。

Driver 为 `goc_hold_live` 播种：IR `ptr %p` + `alloca ptr`，MIR `MOV64mr FI+16` + `CALL HugeFrameVoid`，再跑栈检查与本分析。

## 4. 二进制 goobj（`goobj/binwriter`）

**主路径不是** 生成 `.s` + `go tool asm`。

1. `goobj/run_binwriter.sh` 建 **只读 GOROOT overlay**，在 `cmd/gocbinwrite` 下编译，以便合法 `import cmd/internal/obj` / `obj/x86` / `objabi`（不改系统 GOROOT，不 git clone）。  
2. binwriter 在内存里构造 `obj.Prog` 链（`ATEXT` / `AMOVQ` / `ACMPQ` / `AJHI` / `ACALL` / `AFUNCDATA` / **StoreGptrWB：ACMPL/AJEQ/gcWriteBarrier2** / …）。  
3. `obj.Flushplist` → `obj.WriteObjFile` → `build/goobj/goc_funcs.o`（`go object …` + `\x00go120ld`）。

## 5. 链接隔离

- `harness/stubs_amd64.s`：仅 helpers（GetSP / MorestackHits / HugeFrame* / WBPathHits / WBEnabled / 计数器）。  
- `harness/goc_leaf_amd64.syso`：LLVM leaf。  
- goc TEXT（`GocCheckedAdd` / `GocHoldLive` / `GocHoldArg` / **`StoreGptrWB`**）**只**来自 `goc_funcs.o`。  
- `tools/toolexec_pack_goobj.sh`：`asm -gensymabis` 后追加 ABI0 defs（含 StoreGptrWB）；`compile -pack` 后 `pack r` 注入二进制 goobj。

Harness 顺序 **L→W→S→A**。


## P6 note (2026-09-21 Asia/Shanghai)

- Real `#include "X86InstrInfo.h"` via `_deps/llvm-project-19.1.7` + `X86CommonTableGen`.
- `LiveIntervalsWrapperPass` via legacy `PassManager` (spill/maps before stackcheck).
- Lite fallback removed; real `X86InstrInfo.h` is required (missing headers fail the build).

## P6.1 (2026-09-21 Asia/Shanghai)

### Gptr identification (GocSpillGptrsAtSafepoints)

Not every GR64. Rules:

1. **R1** — fn attr `goc-gptr-vregs` (explicit indices)
2. **R2** — def = COPY from physreg of an IR **pointer** argument (seed ABI: ptr0→RAX, ptr1→RCX, …)
3. **R3** — def = load from ptr alloca FI / MMO; COPY from known gptr
4. **R4** — MRI LLT pointer when set

Reject: `MOV*ri` immediates, unknown GR64.

### Safepoints + LIS order

- Safepoints: **CALL** and **morestack_noctxt**
- Pipeline: `Spill(LIS)` → `EmitMaps` → `StackCheck` → `WB`
- Primary LIS on straight-line MIR; **no** LIS recompute on StackCheck’s cyclic morestack CFG
- StackCheck spills ptr-arg physregs into the same Locals-mapped FIs (spill registry) before `CALL morestack_noctxt`
- Scratch for `g` is **R11** (preserve RAX/RCX args)

### S3 register-only

`goc_hold_regonly` / `main.GocHoldRegOnly`: no IR alloca; MIR keeps heap gptr in a vreg across CALL (+ integer decoy). Spill → `hold_regonly/maps.txt` (`locals_hex …0404`). Harness **PASS S3**.


## P8 (2026-09-21 Asia/Shanghai)

### LIS after StackCheck

- Pipeline: `Spill(LIS)` → `EmitMaps` → `StackCheck(CFG only)` → `GocRebuildLISAfterStackCheck` → `WB`
- Safe rebuild: iterative live-in/out fixed-point (not blind `LiveIntervals` on the cycle)
- Morestack spills use rebuilt liveness + spill-registry FIs (same Go SP as CALL)

### MIR→goobj

- Pass emits `mi_lower.txt` (`format goc-mi-lower-1`)
- `goobj/mirlower` lowers stackcheck MI recipe → `obj.Prog`
- Hold*/WB bodies: **P9** full MIR-lower (see below); FUNCDATA/PCDATA from real maps

### ABIInternal

- Seed / spill / rebuild: AX,BX,CX,DI,… among int/ptr args
- binwriter TEXT via `LookupABI(..., ABIInternal)`; ret in AX
- toolexec appends `ABIInternal` symabis (no ABI0 wrapper in this harness)


## P9 (2026-09-21 Asia/Shanghai)

### Full MIR→goobj (no Prog templates)

- Pass exports **per-function full MI lists** in `mi_lower.txt` (`.begin_fn` … `.end_fn`), after spill / maps / stackcheck / rebuild-LIS / WB.
- `goobj/mirlower` lowers those lists to `obj.Prog` for **every** harness goc TEXT: `GocCheckedAdd`, `GocHoldLive`, `GocHoldArg`, `GocHoldTwo`, `GocHoldRegOnly`, `StoreGptrWB` (including morestack / WB paths).
- `goobj/binwriter` attaches only TEXT + FUNCDATA from maps; **build fails** if `mi_lower` lacks a required fn (no silent template fallback).
- Proof: `rg` finds no `AMOVQ`/`HugeFrameVoid`/`gcWriteBarrier2` bodies in `binwriter/main.go`.

### Still not a general MIR parser

- Operand model: physreg / SP / symbol only (no general vreg allocator).
- Not a general LLVM `.mir` parser for arbitrary IR — harness op subset only.
- ~~`goc_leaf` `.syso`~~ **P10:** leaf is MIR→goobj TEXT (no harness `.syso`).
- ~~Hold* Go auto stack-prologue~~ **P10:** Hold* are NOSPLIT with MIR-owned morestack (same as CheckedAdd).


## P10 (2026-09-21 Asia/Shanghai)

### Hold* MIR-owned morestack (no Go auto dual-track)

- All Hold* TEXTs are `flags nosplit` and include the same morestack MI recipe as `GocCheckedAdd`:
  `MOV64rm R11, FS:-8` → `CMP64rm RSP, 16(R11)` → `JCC_1 HI ok` → (spill ptr args to SP locals) → `CALL morestack_noctxt` → `JMP entry`.
- Go assembler **must not** supply a second stack-check prologue; NOSPLIT suppresses it.
- Proof: `build/hold_objdump.txt` shows FS/R11 morestack, not only `CMPQ SP, 0x10(R14)` + `JBE` auto prologue.

### NOSPLIT vs checked policy

| Kind | Flag | Stack check |
|------|------|-------------|
| `GocCheckedAdd`, Hold* | NOSPLIT | MIR-owned morestack (required) |
| `goc_leaf` | NOSPLIT | none (frame 0 leaf) |
| `StoreGptrWB` | NOSPLIT | none (WB only) |

**Forbidden:** non-NOSPLIT goc TEXT that relies on the Go assembler’s automatic stack-check prologue (P9 dual-track — removed).

### goc_leaf same pipeline

- Exported as `.begin_fn goc_leaf` → mirlower → goobj TEXT `goc_leaf` (**ABI0**, SysV-ish DI/SI→AX).
- Harness **no longer** links `goc_leaf_amd64.syso` (llc). Optional `build/goc_leaf.llc.s` is reference-only.

### Broader MI + golden

- mirlower: ADD64ri/SUB64ri, LEA64, MOV64rm from symbol(SB); CALL NAME_EXTERN relocs.
- Golden: mutate `imm=42`→`43` in `mi_lower` → `goc_funcs.o` bytes **must** change; `RequireFn` FATAL if MI missing; binwriter must not regain `x86.A*` body templates.
- Frontend color/escape/QJS: **deferred**.


## P11 (2026-09-21 Asia/Shanghai)

### General LLVM MIR parser (`goobj/mirparse`)

Primary path for Hold*/CheckedAdd/WB/leaf TEXT bodies:

```text
pass/harness.mir  (LLVM MIR YAML-ish: name:, body: |, bb.N, $rax, %0, FI)
        │
goobj/mirparse  →  goc-mi-lower-1 (op=… physreg/SP/symbol)
        │
goobj/mirlower  →  obj.Prog  →  binwriter goobj
```

- Also parses MF dump form (`# Machine code for function` / `goc.mir`).
- VRegs: only via `COPY` from physreg (no general regalloc).
- `GOC_PCDATA1` pseudo for safepoint PCDATA.
- Optional `goc.go_sym` / `goc.frame` / `goc.flags` YAML keys.

CLI: `go run ./goobj/cmd/mir2lower -in pass/harness.mir -out mi_lower.txt`.

### Fallback

`pass/mi_full_bodies.txt` remains for `-fallback-bodies` if MIR parse fails. Build prefers MIR; tests prefer MIR path.

### Honest limits

| Covered | Not yet |
|---------|---------|
| Multi-fn YAML MIR + dump | Bundle / full CFI emission |
| Physregs, MBB, successors | General vreg allocator |
| MOV/ADD/CMP/JMP/CALL/RET/LEA/TEST≈CMP/PUSH/POP | AVX/x87 via llc fixtures (P13); EH FATAL |
| Mem 5-tuple, FS/GS, FI→SP, symbols | Arbitrary pre-RA MIR without COPY |
| Spill/reload as MOV64mr/rm | Blind LiveIntervals on cyclic CFG |

## P12 (2026-09-21 Asia/Shanghai) — LLVM MC encoding (no demo opcode table)

**Directive:** instruction selection/encoding must be **LLVM MC**, not a hand-grown mirlower whitelist / generated demo opcode table.

### Primary path

```text
pass/harness.mir
    → goobj/llvmmc/mircanon.py     (dialect normalize only; no encoding)
    → llc-19 -filetype=obj        (X86 AsmPrinter + MCCodeEmitter + AsmBackend)
    → build/pass-out/harness.llc.o (ELF64)
    → goobj/elfpack               (bytes+relocs → goobj; FUNCDATA from maps)
    → build/goobj/goc_funcs.o
```

### LLVM tools / APIs used

| Component | Role |
|-----------|------|
| `llc-19` | MIR parse + object emission |
| X86 AsmPrinter | machine instr → asm/object |
| MCCodeEmitter | opcode bytes |
| AsmBackend / ELF ObjectWriter | `R_X86_64_PLT32`, `R_X86_64_PC32` |
| IR attr `no_callee_saved_registers` | avoid SysV CSR push that breaks Go ABIInternal |

### Explicitly abandoned as primary

- Growing `mirlower` `switch { case "MOV64rr": ...}` for harness TEXT
- Generating a hand-maintained / TableGen-copied demo opcode→Go `obj.As` table as the encoder

`goobj/mirlower` is **demoted** (diagnostics / legacy binwriter only).

### Remaining FATALS / holes (honest) — superseded by P13

See §P13 below. CFG rewrite removed; dense PCDATA; AVX/x87/EH contracts documented.

## P13 (2026-09-21 Asia/Shanghai) — mircanon minimize + dense PCDATA + pack harden

### mircanon

**Removed:** `split_morestack_cfg`, `inject_frame`.  
**Pass emits** llc-consumable CFG + explicit Go frame in `pass/harness.mir`.  
**Remaining:** dialect-only strip/rename (documented in `goobj/llvmmc/README.md`).

### Dense PCDATA

elfpack builds per-CALL `PCDATA_StackMapIndex` from meta `calls[]` + ELF CALL sites.  
Same live set → shared index (hold_* idx 0); fixture `dense_pcdata` proves distinct 0/1.

### Pack checks

PLT32/PC32 addends validated; pcsp = frame+8 with PUSH BP proof; `check_fixtures.sh` in `build.sh`.

### AVX / x87 / EH

- SSE/AVX/x87: clang→llc objdump proofs (`addps` / `vaddps` / `fldt`+`faddp`); SSE also elfpack
- EH: mircanon FATAL on `EH_LABEL` etc.; no Go EH tables

### Encoding

Still **LLVM llc MC** only.


## P14 — zero dialect strip + float into Go ABI (2026-09-21 Asia/Shanghai)

### Dialect → ZERO

`pass/harness.mir` is **llc-19-ready** (IR stubs + `@sym` + `$rip` PIC + `CMP64ri32` + CALL implicits + `bb.N:`).
Metadata (`go_sym` / `frame` / `flags` / per-CALL stackmap indices) lives in `pass/harness.meta.json`.
Hot path: `mirguard` identity check → **`llc-19` on harness.mir directly** → `elfpack`.
`mircanon.py` is a no-op shim; any leftover dialect pattern is **FATAL**.

### Float ABIInternal (amd64)

| Go | MIR | Regs |
|----|-----|------|
| `GocFadd64(a, b float64) float64` | `ADDSDrr $xmm0, $xmm1` | args **X0,X1** → result **X0** |
| `GocFadd32(a, b float32) float32` | `ADDSSrr $xmm0, $xmm1` | same |

Integer args remain AX/BX/CX/DI/SI/R8/R9. Float regs X0–X14; X15 is zero (Go amd64).
**x87**: permanent unsupported on Go-callable path (`mirguard` FATAL). Clang long-double objdump is encode-only.
**AVX**: llc+elfpack encode smoke retained; harness PASS F uses SSE2 scalar (required on Go amd64).

Harness prints `PASS F: …` and final `PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])`.
