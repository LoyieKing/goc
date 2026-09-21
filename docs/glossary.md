# goc 术语表（glossary）

**范围：** go1.24.4 linux/amd64；与本目录 P0–P8 实验一致。  
**约定：** 每个术语只在此给**一次**精确定义；其它文档用链接回指。

相关文档：

| 文档 | 内容 |
|------|------|
| [llvm-ir-go-runtime-hooks.md](./llvm-ir-go-runtime-hooks.md) | morestack / 写屏障 / stackmap 设计 |
| [p2-llvm-goobj-plan.md](./p2-llvm-goobj-plan.md) | P2 可执行计划 |
| [p0-llvm-go-stack/](./p0-llvm-go-stack/) | 同栈 CALL + `.syso` + ABI0 trampoline |
| [p1-runtime-hooks/](./p1-runtime-hooks/) | Demo A/B/C 汇编模板（PASS） |
| [p2-llvm-lowering/](./p2-llvm-lowering/) | prologue / 写屏障模板注入管道 |
| [p2-goobj-min/](./p2-goobj-min/) | 最小 FUNCDATA / goobj 发射 |
| [p3-auto-stackmap-plan.md](./p3-auto-stackmap-plan.md) | P3 自动 stackmap 计划 |
| [p3-auto-stackmap/](./p3-auto-stackmap/) | lowering → 自动 gclocals·* / FUNCDATA（已并入 P4；保留回归） |
| [p4-unified-lower-plan.md](./p4-unified-lower-plan.md) | P4 统一 goc_lower 计划 |
| [p4-unified-lower/](./p4-unified-lower/) | **规范**统一 lowerer：stackcheck + store_gptr + live_gptr |
| [p5-machinepass-goobj/](./p5-machinepass-goobj/) | **主路径** MachineFunctionPass（真 X86 MI 栈检查+WB）+ MIR liveness Args/Locals + 二进制 goobj（P4 asm 缝合弃用） |
| [p6-plan.md](./p6-plan.md) | **P6** 寄存器级 LiveIntervals + safepoint spill；多 FI 真 Go SP 偏移；独立 goobj（无 GOROOT overlay） |
| [roadmap-next.md](./roadmap-next.md) | **P8 推荐顺序**：LIS after StackCheck → MIR→goobj → ABIInternal → `goc` CLI；5–6 仅文档 |
| [p8-compiler-pipeline/](./p8-compiler-pipeline/) | P8 CLI / 管道入口（扩展 p5） |
| [p8-archive/P8-REPORT.md](./p8-archive/P8-REPORT.md) | P8 归档：PASS 证据、路径、缺口 |

---

## 产品与路径

### goc
本项目目标编译器：吃近似 C 的前端（Clang/LLVM），产物跑在 **goroutine 用户栈**，经 **goobj / `cmd/link`** 与 Go 运行时同链接；不是「普通 ELF + cgo」。见 [goc-compiler-path-revised.md](./goc-compiler-path-revised.md)。

### same-stack（同栈）
goc 函数与调用它的 Go 代码共用**同一** goroutine 栈；扩栈 / GC 搬栈必须看到 goc 帧里的活指针。对比：cgo 把 C 放到独立系统栈。

### cgo tax
走 cgo / 跨栈时的额外成本（栈切换、逃逸、调用约定、不可抢占窗口等）。goc 主路径刻意避开。

### cmd/link
Go 链接器：把 `.o` / goobj / `.syso` 合成可执行文件，写入 pclntab、FUNCDATA 等。goc 最终产物必须被其接受。

### .syso
Go 构建系统会把包目录下的 `*_GOARCH.syso`（或 `*.syso`）当**额外目标文件**链进包。P0/P1 用它承载 LLVM `llc` 产出的 ELF `.o`（SysV 叶子），再由 Go 汇编 trampoline 调用。**不要**把 `*.llc.s` 放进包目录（会被当成 Go 汇编）。

### goobj
Go 工具链的包目标文件格式（`cmd/internal/goobj`）。相对普通 ELF，多了 pclntab、FUNCDATA/PCDATA、ABI 信息等。完整 LLVM→goobj 是 P2+ 目标；见 [p2-goobj-min/FORMAT.md](./p2-goobj-min/FORMAT.md)。

---

## 指针色（语言合同）

> **规范全文：** [goc-syntax-guide.md](./goc-syntax-guide.md) **v0.2（2026-09-21）**。下列为速查；与指南冲突时以指南为准。  
> **无 `dsptr`：** 已移出语言表面；堆上栈引用用 `uptr`。

### cptr
指向**非栈**对象（C 堆 / 全局 / arena 等），**不是** Go 堆。直接寻址；无 Go 写屏障。指针*字*可在栈、寄存器或堆。

### sptr
指向**当前 g 栈**对象。直接寻址；指针*字*只许在栈槽/寄存器，**禁止进堆字段**；跨 morestack 须 spill 到 stackmap 槽。

`sptr` 逃逸（写入堆字段/全局/返回）→ **编译错误**；不得自动升格为 `uptr`。入库须显式 `uptr` 或经 `auto_ptr` 预收。

### uptr
`cptr|sptr` 编码并集；可存堆字段。MSB=0 → 绝对 cptr；MSB=1 → 相对 `g.stack.hi` 的 int64 offset。**不可直接解引用**；解码仅用 owner g。生产态 `stack.hi` 经 Go TLS `FS:-8`（P22）；host 证明仍可用 `*_hi` / `goc_test_set_stack_hi`（P19）。

### auto_ptr
推断色；`T *` ≡ `auto_ptr<T>`。逃逸敏感：优先 `cptr`/`sptr`，须入库则**一开始**收成 `uptr`。已钉成 `sptr` 后若逃逸 → **编译错误**，禁止自动升格。ABI（函数指针/导出/虚表）禁止裸 `auto_ptr`。

### gptr
「Go 堆」指针：与 `cptr`/`sptr`/`uptr`/`auto_ptr` **无隐式转换**；GC 扫描；写入 Go 堆槽走写屏障；栈上活 gptr 进 stackmap；禁止算术。

### dsptr（已废除）
v0.1 曾用的栈相对表面色；**v0.2 删除**。语义由 `uptr`（MSB=1）覆盖。历史文档若仍写 `dsptr`，视为过时。

### MIR liveness / LiveIntervals（goc / P5→P8）
- **P5：** `GocEmitPointerMaps` 对 **栈槽 FI** 做 forward dataflow，在 CALL safepoint 快照；**不**覆盖寄存器-only gptr；`goc-live-gptr-sp-offs` 仅调试覆盖。
- **P6：** 对齐 Go `cmd/compile`：用 **LiveIntervals** 跟踪 **gptr vreg**；CALL/morestack 前 spill→Locals；maps 真源 liveintervals。
- **P6.1：** gptr 识别收紧（R1–R4，非每个 GR64）；寄存器-only 硬例 **S3**；主 LIS 在 StackCheck 前。
- **P8：** StackCheck **之后**安全重建 liveness（等价 LIS；回避环上盲重跑）；morestack 慢路径 spill 用重建结果，减少 registry-only。见 [roadmap-next.md](./roadmap-next.md)。

---


### StoreGptrWB（P5）
goc 演示用的「写 gptr 槽」TEXT：`CX=slot, AX=new`，按 Demo C 合同调 `runtime.gcWriteBarrier2`。P5b **主路径**由 `goobj/binwriter` 发 Prog → 二进制 goobj；`GocExpandStoreGptr` 发对应真 X86 MI。`harness/stubs_amd64.s` 不再承载该 TEXT。

## 栈增长（morestack）

### morestack
运行时扩栈 / 协作抢占入口族：`runtime.morestack`、`morestack_noctxt`、`morestackc`。prologue 发现栈不够时 `CALL` 之；返回后必须 **JMP 回函数入口重跑**（重入），不是继续 cold 路径后面。Demo A：[`p1-runtime-hooks/a_morestack`](./p1-runtime-hooks/a_morestack/)。

### stackguard0
`runtime.g` 字段，amd64 上 **offset = 16**（`stack` 结构 16 字节之后；本机已用 `unsafe.Offsetof` 核对）。prologue 用 `CMPQ SP, g.stackguard0`（小帧）判断是否要 morestack。`stackguard1` 用于 systemstack / C 栈路径。

### NOSPLIT
函数属性：保证静态栈预算内不触发 morestack；可省略 prologue 检查。叶子小帧常用。违反预算会在运行时炸。对应汇编 `NOSPLIT`。

### StackSmall / StackBig
`internal/abi`：`StackSmall = 128`，`StackBig = 4096`。决定 prologue 形态：
- 帧 ≤ StackSmall：直接 `CMPQ SP, stackguard0`
- ≤ StackBig：`SP-(framesize-StackSmall)` 再比
- ≥ StackBig：先防下溢再比

### prologue template（prologue 模板）
固定的**机器级**入口指令序列（TLS→g、CMP guard、CALL morestack、JMP entry），按帧大小分档。不适合只用 portable IR `call` 表达。P1 Demo A 手写小帧模板；P2 在降级路径注入。见 [p2-llvm-lowering](./p2-llvm-lowering/)。

### reentry（重入）
morestack 成功后从**函数入口**重新执行 prologue；因此不能假设「prologue 只跑一次」，参数须在调用者 spill 区可恢复。

### spill slots
ABIInternal 下，调用者帧里给寄存器参数预留的栈槽；morestack 路径把寄存器 spill 进去，重入后再 reload。

### TLS / g
线程局部存储中的当前 `g`（goroutine）。amd64 用户代码：`MOVQ TLS, CX; MOVQ 0(CX)(TLS*1), AX`，或已缓存时用 `R14`。写屏障路径常要求 `R14=g`。

---

## ABI 与调用约定

### ABI0
Go 汇编默认：参数 / 结果全在栈上（`a+0(FP)` 等）。用户包 `.s` 默认 ABI0。

### ABIInternal
Go 编译器生成代码使用的寄存器约定（amd64：部分参数进 AX/BX/…）。runtime 许多符号是 ABIInternal；链接器可插 `.abi0` 包装。

### SysV vs Go ABI
- **SysV**（Linux amd64）：C/LLVM 默认（DI、SI、DX… 传参，AX 返回）。
- **Go ABI0 / ABIInternal**：与 SysV 不同。P0/P1 用 ABI0 trampoline：从 FP 取参 → DI/SI → `CALL` LLVM `.syso` 符号。

### R11 buf ABI（gcWriteBarrier）
`runtime.gcWriteBarrierN` **不遵循** Go ABI：入口 stub 设 `R11 = N*8`（字节），出口 `R11 = buf`；fast path 不破坏通用整数寄存器。调用方写 `buf[0]=new`、`buf[1]=old`（N=2）等。见 Demo C。

---

## GC 元数据

### FUNCDATA
函数级元数据槽（pclntab）。重要索引（`runtime/funcdata.h` / `internal/abi`）：
- `0` ArgsPointerMaps
- `1` LocalsPointerMaps
- …

### PCDATA
按 PC 变化的元数据。重要：
- `0` UnsafePoint
- `1` **StackMapIndex**（当前用哪一份 pointer bitmap）

### StackMapIndex
PCDATA 值：选中 `ArgsPointerMaps` / `LocalsPointerMaps` 里第几张 bitmap。CALL / safepoint 前由编译器设置。

### LocalsPointerMaps / ArgsPointerMaps
栈帧局部 / 参数区的指针 bitmap 集合。运行时布局见 [p2-goobj-min/FORMAT.md](./p2-goobj-min/FORMAT.md)：`stackmap{n, nbit, bytedata…}`。

### S3（register-only gptr / P6.1）
回归：堆 gptr **仅**在寄存器跨越 CALL（MIR 无先验栈 store）；无 spill 则无 Locals maps → FAIL；有 spill → **PASS S3**。见 `goc_hold_regonly`。

### safepoint
可能停顿的点（CALL、morestack 等）：此处活 gptr 必须出现在对应 stackmap 中，否则搬栈 / GC 会漏改或漏扫。

P24 stub 解释器在后向边**手插** poll；**P25** 经 stub HIR 编译器在后向边**自动**插入 `OP_SAFEPOINT`（选项 B；非 LLVM pass）。C poll **不得** `CALL morestack`。见 [p25-auto-safepoint/docs/AUTO-SAFEPOINT.md](./p25-auto-safepoint/docs/AUTO-SAFEPOINT.md)。

**P26 / 真链 quickjs-ng：** 在 `JS_NAN_BOXING=0` 下链接真实 `quickjs.c`（显式 `struct JSValue`）；**不**等于全量 goc 着色或 Go 堆托管。见 [p26-qjs-real/docs/LAYOUT.md](./p26-qjs-real/docs/LAYOUT.md)。


### writeBarrier.enabled
`runtime.writeBarrier` 结构首字段（`bool` + `pad[3]`）。用 **32-bit** `CMPL` 判断是否走 `gcWriteBarrierN`。运行中可变，不可在模块初始化时当常量折死。

### gcWriteBarrierN
批量写屏障入口（N=1…8）。见上文 R11 buf ABI；Demo C：[`p1-runtime-hooks/c_writebarrier`](./p1-runtime-hooks/c_writebarrier/)。

---

## 编译器管道

### MIR lowering
Machine IR / 选指令之后的降级阶段。goc 推荐在此插入 morestack prologue 与写屏障序列（而非只靠 portable IR）。

### gclocals·（符号前缀）
FUNCDATA 指向的 locals/args bitmap 符号必须带 `gclocals·` 或 `gclocals.` 前缀，链接器才放入载体 `go:func.*`。自定义名如 `·holdLocals` 会 link panic。P2/P3 demo：`gclocals·gocHold` / `gclocals·gocHoldLive`。见 [p2-goobj-min/FORMAT.md](./p2-goobj-min/FORMAT.md)。

### llvm.goc.live_gptr
P3 注解 intrinsic（方言）：`live_gptr(ptr, spill_sp_offset)` 声明「该 gptr 在 prologue 后某 SP 偏移的帧槽内跨 safepoint 保活」。Lowerer 用**同一**偏移写 spill/reload 与 bitmap 位。见 [p3-auto-stackmap/](./p3-auto-stackmap/)。

### auto stackmap / emit_funcdata
由降级工具（非 Go 编译器 plive）按帧布局生成 `stackmap{n,nbit,bytedata}` 字节并写成 `DATA gclocals·…` + `FUNCDATA_LocalsPointerMaps`。P3 起；**规范**：[p4-unified-lower](./p4-unified-lower/) `emit_funcdata.py` + 统一 `goc_lower.py`。

### prologue / WB template injector
P2/P4 实用路径：asm **模板缝合**（Python）。**P5 起主线弃用**；改由 MachineFunctionPass + goobj writer。P4 目录保留回归。

---

### goc_lower（统一降级器）
注解 IR → Go-asm 模板缝合器。P4 起单一入口；**P5 起主线弃用**（保留回归）。见 [p4-unified-lower](./p4-unified-lower/)。

### MachineFunctionPass（goc）
LLVM `llvm::MachineFunctionPass`：在 MIR 上插入 morestack 检查 / WB / spill / 发射 pointer map。P5–P6：`GocInsertStackCheck`、`GocExpandStoreGptr`、`GocSpillGptrsAtSafepoints`（LiveIntervals）、`GocEmitPointerMaps`，见 [p5-machinepass-goobj/pass](./p5-machinepass-goobj/pass/)。

### goobj writer（P5b→P6）
按 pass recipe/maps 在内存构造 Prog/`WriteObjFile` 写二进制 goobj（**主路径不经** `.s` + `go tool asm`）。
- **P5b：** 经 **GOROOT overlay** import `cmd/internal/obj`。
- **P6：** **独立** writer：vendored encode 包（`goobj/enc/`），普通 `go build`，**无** GOROOT overlay。见 [p6-plan.md](./p6-plan.md)。

### 真 X86 MachineInstr 栈检查 / WB（P5b→P6）
`GocInsertStackCheck` / `GocExpandStoreGptr` / spill 均 `BuildMI` 真 X86 MI（**无** INLINEASM 主路径）。Distro `llvm-19-dev` **不**带 `X86InstrInfo.h`（头在源码 `llvm/lib/Target/X86/`）。本树安装 llvm-project **19.1.7** + `ninja X86CommonTableGen`，`#include "X86InstrInfo.h"`，用 `X86::MOV64rm` 等；`pass/vendor/X86InstrInfoLite.h` 仅缺本地构建时 fallback。WB 体由独立 goobj writer 写入 `StoreGptrWB`。

### register-level liveness（P6）
在 **vreg / 物理寄存器** 粒度跟踪 gptr 是否跨越 safepoint 仍活；与仅跟踪栈 FI 的 P5 dataflow 相对。实现上依赖 LiveIntervals（或等价）。

### LiveIntervals（goc / P6）
LLVM `llvm::LiveIntervals`（配合 `SlotIndexes`、`MachineDominatorTree`），经 legacy `PassManager` 的 `LiveIntervalsWrapperPass` / `SlotIndexesWrapperPass` 取得（ctor 对树外私有）。goc spill 用 `LI.liveAt(CallIdx)` 判定 CALL 前活 gptr vreg。合成 morestack 环 + 活 vreg 时 LIS 不收敛 → Spill/Maps 排在 StackCheck 前；morestack safepoint 用 spill registry 把 ptr 实参写入 Locals FI（不重算 LIS）。

### safepoint spill（Go 对齐）
在 CALL / morestack 前把活 gptr 从寄存器写入栈槽，使 **LocalsPointerMaps** 足以让 GC 看见它们。goc **不**依赖单独的寄存器 pointer map 位作为主路径（若将来发射 reg map，须在文档中显式说明）。

### Go SP offset / varp 布局
Go amd64 帧：`TEXT $N-argsize` 本地区 N 字节；扫描合同 `bit i → SP+i*8`（见 FRAME_LAYOUT）。**禁止**用 LLVM FI 创建序号×8 代替真实字节偏移。

### 独立 goobj writer（P6）
不通过 GOROOT overlay import `cmd/internal/*`；encode 逻辑在树内 `goobj/enc/`（从 Go 工具链 vendored 并改写 import），`go build` 链接 harness。

---

## 快速对照（P0 / P1 / P2 / P3 / P4 / P5 / P6）

| 阶段 | 证明了什么 |
|------|------------|
| P0 | 同栈 CALL LLVM leaf（`.syso` + ABI0 trampoline） |
| P1-A | 手写 morestack prologue + 重入 |
| P1-B | 活 gptr 跨 CALL/扩栈（**Go 编译器**发 FUNCDATA） |
| P1-C | `CMPL writeBarrier` + `gcWriteBarrier2` 序列 |
| P2-lowering | 模板注入管道可重复生成带 prologue / WB 的产物 |
| P2-goobj | 自控函数带 FUNCDATA bitmap，且能被 `cmd/link` 接受、扩栈后指针仍有效 |
| P3-auto-stackmap | **降级生成**的函数自动带 `gclocals·*`；活 gptr 跨 CALL+morestack PASS |
| P4-unified-lower | 一个 `goc_lower` + L/W/S（**主线弃用**，回归保留） |
| P5-machinepass-goobj | 真 X86 MI 栈检查+WB + MIR liveness maps + **二进制** goobj（含 StoreGptrWB）；L/W/S/A |
| P6 | 寄存器 LiveIntervals + safepoint spill；多 FI 真 Go SP；独立 goobj；L/W/S/A |

### P17（前端第一刀）
`p17-frontend/`：Clang annotate 色表面（`cptr`/`sptr`/`uptr`/`auto_ptr`/`gptr`）+ `goc-color-escape` IR 逃逸分析；`sptr` 逃逸 = 编译错误（无自动升 `uptr`）。见 [p17-archive/P17-REPORT.md](./p17-archive/P17-REPORT.md)。

`p23-qjs-slice/`：QJS **第一刀**（显式 `JSValue` struct + STUB VM + cptr arena；非完整移植）。见 [p23-archive/P23-REPORT.md](./p23-archive/P23-REPORT.md)。

## P27 — Clang 产品前端

**P27：** 基于 Clang 插件/补丁的 goc 产品前端：`goc_cptr|sptr|uptr|auto_ptr|gptr` 属性 + Sema 层 `sptr` 逃逸报错；`bin/goc build` 编译用户 `.c` → color IR → goobj。P17/P21 降为 legacy。见 `p27-clang-frontend/PLAN.md`。


## P28 — In-tree Clang + real-body goobj

**P28：** 从 patched LLVM/Clang 树重建 `clang`（`SemaGocColors` + Attr.td），产品路径**不再依赖** `-fplugin`；`sptr→heap` 仍为硬 Sema 错误。goobj 使用真实 Clang IR → llc ISel 函数体（禁止 P21 `seedMinimal`/`seedLiveAcrossCall`）。通用 ABIInternal / 任意 MF 上的 morestack+Spill+WB 属 **P29**。见 `p28-clang-intree/PLAN.md`。
