# P15 归档报告 — Pass 自动导出标准 MIR

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**前端 color/QJS：** 仍 OUT OF SCOPE

## PASS 证据

```text
PASS-DRIVER: wrote .../harness.mir + harness.meta.json (printMIR llc-ready export)
P15: Pass printMIR → build/pass-out/harness.mir (+ meta) OK
P15: hot path = build/pass-out/harness.mir (Pass printMIR); pass/harness.mir golden/reference only
PASS L: checked entry + MIR/goobj leaf after growth = 42
PASS W: store_gptr WB enabled path hits=200
PASS S: live *int across CALL+morestack with LocalsPointerMaps from pass
PASS A: ArgsPointerMaps keep arg *int across CALL+morestack
PASS S2: two live *int across CALL+morestack (Go SP layout, not FI*8)
PASS S3: register-only *int across CALL+morestack (LiveIntervals spill→Locals)
PASS F: float64+float32 via llc→elfpack→goobj→Go (X0,X1→X0)
PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])
```

一键：`./p5-machinepass-goobj/build.sh`

## 1. 热路径（P15）

```text
goc-pass-driver
  ├─ analysis PM: Spill → EmitMaps → StackCheck → Rebuild → WB
  │    └─ GocDumpMirPass: llvm::printMIR → build/pass-out/goc.mir
  └─ gocEmitLlcReadyMir (printMIR)
       ├─ build/pass-out/harness.mir      ← 编码主输入
       └─ build/pass-out/harness.meta.json
            │
       mirguard.py   # identity；方言残留 → FATAL
            │
       llc-19 … build/pass-out/harness.mir   # DIRECT，无 rewrite
            │
       elfpack -meta build/pass-out/harness.meta.json
```

- `pass/harness.mir` / `pass/harness.meta.json`：**golden/reference only**（手改会被文档标明覆盖；热路径不读它们作为唯一真相）。
- 证明：`PASS-DRIVER: wrote ... harness.mir`；`cmp -s build/pass-out/harness.mir build/pass-out/harness.canon.mir`；`mirguard --check-only` on Pass 输出。

## 2. MIRPrinter（非 MF.print）

| 项 | 做法 |
|----|------|
| API | `llvm::printMIR(OS, Module)` + `printMIR(OS, MF)`（`MIRPrinter.h`） |
| 分析 dump | `GocDumpMirPass` → `goc.mir`（YAML，非 debug text） |
| 编码导出 | `goc_mir_export.cpp`：post-RA physreg 体 + IR stubs（`no_callee_saved_registers`） |

方言合同（与 P14 相同，由生产者保证）：

| 规则 | Pass 导出 |
|------|-----------|
| `@sym` | `addGlobalAddress` |
| `$rip` PIC | mem 基址 `X86::RIP` |
| `CMP64ri32` | 直接发 |
| CALL implicits | desc ImpUse + 显式 ImpDef `$rsp/$ssp` |
| 无 `goc.*` / `GOC_PCDATA1` / `&sym` | meta sidecar |

## 3. 覆盖的 TEXT

`goc_checked_add`, `goc_hold_live`, `goc_hold_arg`, `goc_hold_two`, `goc_hold_regonly`, `goc_store_gptr`, `goc_leaf`, `goc_fadd64`, `goc_fadd32`  
（Go ABIInternal 形状与原 harness 对齐；maps 仍来自 analysis PM。）

## 4. mirparse

LLVM MIRPrinter 输出 `body:             |`（带填充空格）。`goobj/mirparse` 增加 `isBodyKey`，接受标准 printMIR 间距。

## 5. 诚实剩余缺口

| 项 | 状态 |
|----|------|
| AVX Go harness 调用 | 未做（标量 float SSE2 = PASS F） |
| x87 Go-callable | permanent unsupported FATAL |
| EH | unsupported FATAL |
| 导出体 vs analysis 管线 | 导出为独立 post-RA physreg 种子（`gocEmitLlcReadyMir`），**不是**把 Spill/StackCheck 的 vreg/%stack.N 直接 llc；maps/recipe 仍走 analysis PM。把分析 MF 自动降到 Go 帧 physreg 导出仍是后续工作。 |
| 预 RA vreg MIR | 仍需 COPY→physreg；本导出为 post-RA |
| `pass/harness.mir` | 仅 golden；与 Pass 导出在 verbose YAML 键上可能不同（printMIR 多字段），语义对齐 |

## 触及文件

- `pass/goc_mir_export.cpp` / `.h` — printMIR 导出全部 TEXT + meta
- `pass/goc_pass_driver.cpp` — Dump 改 printMIR；调用导出
- `pass/Makefile` — 链入 export
- `goobj/mirparse/parse.go` — `body:             |` 兼容
- `goobj/llvmmc/run_llvmmc.sh` / `README.md` — 热路径默认 Pass 输出
- `build.sh` — Pass MIR 主路径 + P15 证明
- `pass/harness.mir` — 标为 golden/reference
- `PLAN.md` / `README.md` / `../roadmap-next.md`
