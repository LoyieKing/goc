> **Public export:** this tree was `p5-machinepass-goobj/` in research.
> Real-body lower lives in `backend/realbody/`. Build with `GOC_ROOT` set.

# P5：MachineFunctionPass + 真 Args/Locals 图 + 二进制 goobj

**术语：** [../glossary.md](../glossary.md) · **计划：** [PLAN.md](./PLAN.md) · **格式：** [FORMAT.md](./FORMAT.md)  
**MI/goobj 构造：** [docs/MI_AND_GOOBJ.md](./docs/MI_AND_GOOBJ.md)  
**日期：** 2026-09-21（Asia/Shanghai）  
**工具链：** llvm-config-19 / clang++-19 / llc-19 · go1.24.4 · `CGO_ENABLED=0`

---

## P16 增量（2026-09-21 Asia/Shanghai）

| 项 | 状态 |
|----|------|
| 编码 MIR 来自分析 MF 经 Go-frame lower（非并行 seed） | ✅ |
| `GocLowerGoFrameEmitPass` 在同一 PM/MMI 内 lower+printMIR | ✅ |
| 热路径 mirguard identity → llc DIRECT → elfpack | ✅ |
| `./build.sh` L+W+S+S2+S3+A+F | ✅ |
| 前端 | **仍延后** |

归档：[../p16-archive/P16-REPORT.md](../p16-archive/P16-REPORT.md)

---

## P15 增量（2026-09-21 Asia/Shanghai）

| 项 | 状态 |
|----|------|
| Pass `llvm::printMIR` 导出 llc-ready YAML MIR | ✅ `build/pass-out/harness.mir` |
| 热路径消费 Pass 导出（非手写唯一真相） | ✅ `run_llvmmc.sh` + `build.sh` |
| sidecar meta 由 Pass 写出 | ✅ `build/pass-out/harness.meta.json` |
| 回归 | `./build.sh` → L/W/S/S2/S3/A/**F** PASS |

归档：[../p15-archive/P15-REPORT.md](../p15-archive/P15-REPORT.md)

---

## P14 增量（2026-09-21 Asia/Shanghai）

| 项 | 状态 |
|----|------|
| dialect strip → ZERO；`llc-19` DIRECT on `harness.mir` | ✅ `mirguard` identity + sidecar `harness.meta.json` |
| float64/32 → Go ABIInternal（X0/X1→X0）PASS F | ✅ |
| x87 Go-callable | ✅ permanent unsupported FATAL |
| 回归 | `./build.sh` → L/W/S/S2/S3/A/**F** PASS |

归档：[../p14-archive/P14-REPORT.md](../p14-archive/P14-REPORT.md)

---

## P8 增量（2026-09-21 Asia/Shanghai）

| 项 | 状态 |
|----|------|
| StackCheck 后安全重建 liveness + morestack spill | ✅ `GocRebuildLISAfterStackCheck`；`lis_policy=safe_recompute_after_stackcheck` |
| StackCheck CFG-only（溢栈不再 registry-only） | ✅ `stackcheck_mode=cfg_only` |
| MIR→goobj | ✅ **P15** Pass `printMIR`→`build/pass-out/harness.mir`→`llvmmc`(`llc` MC)→`elfpack` |
| amd64 ABIInternal | ✅ AX/BX/… 形参；返回 AX；symabis ABIInternal |
| 单一 CLI | ✅ 仓库 `bin/goc`（`../p8-compiler-pipeline/`） |
| 回归 | `./build.sh` → L/W/S/S2/S3/A PASS |

路线：[../roadmap-next.md](../roadmap-next.md) · 归档：[../p8-archive/P8-REPORT.md](../p8-archive/P8-REPORT.md)

Pass 顺序：`Spill(LIS)` → `EmitMaps` → `StackCheck(CFG)` → **`RebuildLIS+MorestackSpill`** → `WB`。

---

## P6 增量（2026-09-21 Asia/Shanghai）

| 项 | 状态 |
|----|------|
| 真 `llvm::LiveIntervals`（`LiveIntervalsWrapperPass` + `SlotIndexesWrapperPass`，legacy `PassManager`） | ✅ spill 用 `LI.liveAt(CallIdx)`；maps `locals_source liveintervals` |
| 多 FI 真 Go SP 偏移（禁 FI×8） | ✅ `hold_two` locals byte=`0x0c`（SP+16|SP+24）；错误 `0x03` 会 FAIL |
| 真 `X86InstrInfo.h` | ✅ 本地 llvm-project **19.1.7** + `ninja X86CommonTableGen`；`-I` 见 `pass/Makefile`（`LLVM_X86_SRC_INCLUDE`） |
| Lite fallback | ❌ 已移除 — 强制真 `X86InstrInfo.h`（缺头/缺 `.inc` 时 `$(error)` 编译失败） |
| 独立 goobj | ✅ `goobj/enc/` vendored encode；`run_binwriter.sh` **无** GOROOT overlay |
| 收紧 gptr 识别（非每个 GR64） | ✅ R1 attr / R2 ptr-arg COPY / R3 ptr-load·alloca / R4 LLT；reject every_GR64 |
| 寄存器-only 硬例 **S3** | ✅ `goc_hold_regonly`：跨 CALL 仅 vreg 活；无 spill 则无 maps → FAIL；integer decoy 不 spill |
| morestack safepoint + LIS 策略 | ✅ CALL+morestack；Spill→Maps→StackCheck；不在环上重算 LIS；morestack 用 registry 溢入 Locals FI |
| 回归 | `./build.sh` → L/W/S/S2/S3/A PASS |

### LLVM X86 头安装（本机）

```text
$LLVM_SRC/          # 官方 19.1.7 源码树
$GOC_ROOT/third_party/llvm-19.1.7-build/            # CMake+Ninja，TARGETS=X86
  ninja X86CommonTableGen   # 生成 X86GenInstrInfo.inc 等（~17s，8 CPU）
X86InstrInfo.h = .../llvm/lib/Target/X86/X86InstrInfo.h
include = -I$SRC/llvm/lib/Target/X86 -I$BUILD/lib/Target/X86
link    = 系统 -lLLVM-19（无需重链 LLVMX86CodeGen）
```

Pass 顺序：`Spill(LIS)` → `EmitMaps` → `StackCheck` → `WB`。
主 LIS 只跑在直线 MIR；StackCheck 插入 morestack 环后**不**重算 LIS（已知不收敛）。
morestack safepoint：StackCheck 把 ptr 实参物理寄存器溢入 spill registry 的 Locals FI（与 CALL spill 同布局）。

Go 对齐：safepoint 活 gptr **spill 到栈**，Locals maps 描述之；**不**发射寄存器 pointer-map 位（主路径）。


## 1. 本轮目标（P5b，相对 P5a / P4）

| P5a（已替换） | **P5b（本目录，主路径）** |
|---------------|---------------------------|
| INLINEASM / ANNOTATION_LABEL 假装栈检查 | **真** X86 `MachineInstr`：`MOV64rm`/`CMP64rm`/`JCC_1`/`CALL64pcrel32`/`JMP_1` |
| goobj 经 `.s` + `go tool asm` | **二进制** goobj：`Prog` → `WriteObjFile`（GOROOT overlay） |
| harness 重汇编 writer `.s` 得到 goc TEXT | leaf `.syso` + **binary** `goc_funcs.o`；stubs 仅 helpers |
| WB / maps 靠 stubs 或属性 | **StoreGptrWB** 进 binwriter；**Locals/Args** 来自 MIR liveness |

P4 `goc_lower.py` asm 缝合仍弃用。

---

## 2. 管道

```text
pass/ GocSpillGptrsAtSafepoints（LiveIntervalsWrapperPass）
pass/ GocEmitPointerMaps（locals_source=liveintervals）
pass/ GocInsertStackCheck / GocExpandStoreGptr（真 X86InstrInfo opcodes）
  → goc.mir + stackcheck.recipe.txt + maps.txt
        │
goobj/binwriter（run_binwriter.sh；standalone enc；无 GOROOT overlay）
  Prog 链（含 StoreGptrWB）+ maps → WriteObjFile → build/goobj/goc_funcs.o
        │
harness/
  stubs_amd64.s（helpers only；无 goc TEXT）
  goc_leaf_amd64.syso（llc leaf）
  toolexec：symabis ABI0 + pack 注入 goc_funcs.o
  CGO_ENABLED=0 go build -a → harness 顺序 L→W→S→A（+S2+S3）
```

## 3. 一键跑

```bash
./build.sh
# 或
../build-p5.sh
```

期望：

```text
PASS L: ...
PASS W: ...
PASS S: ...
PASS A: ...
PASS p5-machinepass-goobj (L+S+S2+S3+A[+W])
```

---

## 4. PASS 标签

| 标签 | 含义 |
|------|------|
| **L** | morestack：真栈检查入口 + 扩栈后 LLVM leaf |
| **S** | live ptr **locals** map 跨 CALL+morestack（maps 自 LiveIntervals spill） |
| **S2** | 两活 gptr / 多 FI 真 Go SP（非 FI×8） |
| **S3** | **寄存器-only** 堆 gptr 跨 CALL（MIR 无先验栈 store）；依赖 spill→Locals |
| **A** | **ArgsPointerMaps** 自举正确（args 自 IR 指针形参） |
| **W** | store_gptr → WB（binary goobj `StoreGptrWB`；`GOC_P5_SKIP_W=1` 跳过） |

---

## 5. 诚实缺口

1. Distro `llvm-19-dev` 仍无 Target/X86 头；本树用 **本地 19.1.7 源码 + tablegen**。Lite 回退已删除，缺头则 `$(error)`。  
2. 独立 goobj：`goobj/enc/` vendored（**无** GOROOT overlay）。  
3. 真 `LiveIntervals` 经 PassManager（CALL spill）；合成 morestack 环上**不**盲重跑 LIS。P8：StackCheck 后 **安全固定点 liveness** 覆盖 morestack spill（`GocRebuildLISAfterStackCheck`）。
3b. gptr 识别收紧：见 spill pass 头注释 R1–R4（禁止「每个 GR64」）。  
4. Go SP：spill 分配真字节偏移；禁 FI-rank×8。  
5. `goc-live-gptr-sp-offs` **仅** debug override。  
6. `go build -a` + toolexec（GOCACHE）。  
7. harness 顺序 **L→W→S→A**（+S2+S3）。  
8. 不发射寄存器 pointer-map 位（Go spill→Locals 主路径）。

## 6. 与 P4 / P5a

- **P4** asm 缝合：弃用主路径，目录保留回归。  
- **P5a** INLINEASM + `go tool asm`：已被本 P5b 替换。
