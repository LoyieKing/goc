# P25 归档报告 — 编译器自动插入 safepoint

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** goc 跑在 goroutine 栈上；长跑解释器须 poll safepoint，以便 morestack / GC  
**实现树：** [`../p25-auto-safepoint/`](../p25-auto-safepoint/)  
**入口：** `./bin/goc qjs --auto-safepoint` · `./bin/goc test --p25` · `./p25-auto-safepoint/build.sh`

> **诚实声明：仍是 STUB VM，不是完整 QuickJS。**  
> 选择 **B（stub bytecode 编译器）**，不是 A（LLVM IR/MIR pass）。  
> Poll **不得**从 C 帧 `CALL morestack`。

## 机制选择

| 选项 | 结论 |
|------|------|
| **A. LLVM IR/MIR pass** | 适合未来 native C + `annotate("goc.interp")`；对本 stub VM 过重 |
| **B. Stub HIR→bytecode 编译器** | **已选。** 新循环写 HIR 即可在后向边自动得到 `OP_SAFEPOINT`，无需再手改 `qjs_interp.c` 各分支 |

P24 手写后向边 poll（`GOC_QJS_RUN_HAND_POLL`）保留为 **fallback**。  
P25 主路径：`HIR → goc-bc-compile → OP_SAFEPOINT → GOC_QJS_RUN_OPCODE_POLL`。

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| 自动插入路径（非仅手写 call site） | ✅ `goc-bc-compile` + `AUTO-INSERT` 日志 + hex 含 `05` |
| 新循环无需手改解释器分支 | ✅ `nested_loops.hir` → `auto_inserts=2` |
| 测试证明 auto polls 触发 | ✅ hand OFF + opcode ON → hits=10000；noauto 对照 hits=0 |
| P24 growth / Go-framed morestack 仍绿 | ✅ `p24-regress` |
| P23/P24 regress | ✅ 经 P24 build |
| `bin/goc test --p25` | ✅ |
| 无 dsptr；无 sptr auto-promote；无 C morestack | ✅ |

## 用法

```bash
./p25-auto-safepoint/build.sh
./bin/goc qjs --auto-safepoint
./bin/goc test --p25
./bin/goc-bc-compile -l out.log -H out.hex p25-auto-safepoint/hir/tight_loop.hir
```

## PASS 证据（摘要）

```text
PASS P25-compiler-build
PASS P25-compile-tight-auto          # AUTO-INSERT + EMIT OP_SAFEPOINT @ pc=…
PASS P25-compile-noauto-control      # 无 05 opcode
PASS P25-compile-nested-two-inserts  # AUTO-INSERT total=2
PASS auto-path-safepoint-hit         # hand_poll=OFF, hits=10000
PASS noauto-zero-hits-proves-auto-path
PASS p24-fallback-hand-poll
PASS p24-regress (P25 contract)
PASS p25-auto-safepoint (compiler auto-insert OP_SAFEPOINT — NOT full QJS)
```

产物：`p25-auto-safepoint/build/goc-bc-compile` · `p25_demo` ·  
[`PASS-LINES.txt`](./PASS-LINES.txt) · [`tight_loop-compile.log`](./tight_loop-compile.log) · [`tight_loop.hex`](./tight_loop.hex)

## 布局

```text
p25-auto-safepoint/
  compiler/goc_bc_compile.{c,h}  HIR parser + back-edge AUTO-INSERT
  compiler/main.c                goc-bc-compile CLI
  hir/*.hir                      无显式 safepoint 关键字
  demo/demo_main.c               auto vs noauto 对照 + nested
  docs/AUTO-SAFEPOINT.md
  build.sh

p23-qjs-slice/stub/
  qjs_interp.h/.c                + OP_SAFEPOINT + run_bytecode_ex flags
  qjs_freestanding_amalgam.c     同步（P24 Go harness）
```

## 自动插入证据（摘录）

```text
AUTO-INSERT safepoint before back-edge to .loop (insn 3)
EMIT OP_SAFEPOINT @ pc=14 (auto)
BYTECODE … 05 04 01 fd ff 00   # 05 = OP_SAFEPOINT
```

对照：`--no-auto` 同 HIR → 无 `05`；仅 `OPCODE_POLL` 运行 → hits=0。  
故计数器命中来自编译器插入的 opcode，而非 P24 手写 `SUB1_JNZ` 站点。

## 缺口

| 项 | 说明 |
|----|------|
| 完整 QJS | 仍欠 NaN-box / 真链 `quickjs.c` |
| LLVM pass (选项 A) | 未做；native C `goc.interp` 标注属后续 |
| In-loop 直接 morestack | 仍须 Go-framed；C poll 只 tick/hook |
| 生产密度 / 抢占 / GC 握手 | 演示级（后向边一票） |

**结论：P25 已交付** — stub 工具链自动在后向边插入 safepoint；P24 手写 poll 仍可用作 fallback；**不是**完整 QJS，**不是** LLVM 通用 pass。
