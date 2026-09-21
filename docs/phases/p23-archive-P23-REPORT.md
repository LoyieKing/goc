# P23 归档报告 — QJS 第一刀（first slice）

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1 §6（显式 `JSValue`）  
**实现树：** [`../p23-qjs-slice/`](../p23-qjs-slice/)  
**入口：** `./bin/goc qjs` · `./bin/goc test --p23` · `./p23-qjs-slice/build.sh`

> **诚实声明：这是第一刀切片，不是完整 QuickJS 移植。**  
> Upstream `quickjs-ng/` 仍为 NaN-box；本切片提供 goc 面向的显式 struct 形状 + **STUB** VM。

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| goc 面向 `JSValue = {tagged_value; cptr pointer}`（非 NaN-box） | ✅ `include/goc_qjs.h` + host/Go 证明 `sizeof=16` / `pointer_offset=8` |
| 分配器入口：arena/cptr；堆字段无裸 sptr | ✅ host malloc→arena；Go 提供 buffer→arena（见 docs/ALLOCATOR.md） |
| 可运行 demo：Runtime/Context + NewInt32/NewObject + retain/release + `eval "1+1"` | ✅ host `p23_demo` + Go harness |
| Prefer vendored quickjs-ng；否则清晰标注 stub | ✅ 引用 `quickjs-ng/`；**STUB** 实现于 `stub/`（未链 `quickjs.c`） |
| `bin/goc qjs` / `goc test --p23`；PASS 行 | ✅ |
| 不破坏 P17–P22 | ✅ `goc test` 仍跑既有阶段；P23 追加 |
| 无 dsptr；无 sptr auto-promote；JSValue 非 NaN-box | ✅ color-escape 金测 |

## 用法

```bash
./p23-qjs-slice/build.sh     # host demo + color goldens + Go harness
./bin/goc qjs                # 同上
./bin/goc test --p23         # P23 only
./bin/goc test               # P17–P19 + P21 + P22 + P23
```

## PASS 证据（摘要）

```text
PASS jsvalue-explicit-struct-size
PASS eval-1plus1
PASS object-cptr-arena
PASS retain-release
PASS color-ok-jsvalue-cptr
PASS color-err-sptr-in-jsvalue-heap
PASS color-ok-uptr-stack-ref
PASS go-eval-1plus1
PASS p23-qjs-slice (first QJS knife — NOT full port)
```

一键产物：`p23-qjs-slice/build/p23_demo` · `p23-qjs-slice/build/p23_qjs_go` · [`PASS-LINES.txt`](./PASS-LINES.txt)

## 布局

```text
p23-qjs-slice/
  include/goc_qjs.h          goc-facing API（显式 JSValue）
  stub/                      STUB VM + arena + freestanding amalgam（Go .syso）
  demo/demo_main.c           host demo
  tests/10..12_*.c           color-escape 金测
  harness/                   CGO_ENABLED=0 Go + trampoline + .syso
  docs/ALLOCATOR.md · SLICE-SCOPE.md
```

## 触及文件

**新建**
- `p23-qjs-slice/**`
- `p23-archive/P23-REPORT.md` · `PASS-LINES.txt`

**修改**
- `bin/goc` — `qjs` / `test --p23`；aggregate 含 P23
- `roadmap-next.md`

## 完整 QJS 仍需（缺口）

| 项 | 说明 |
|----|------|
| Upstream NaN-box → 显式 struct 迁移 | `quickjs-ng` 仍 `uint64_t` / tagged-word；须改遍值槽与属性 |
| 解释器循环 safepoint | morestack / 抢占点未接入字节码循环 |
| 全量内部着色 | 仅切片源 + 金测过 color-escape；非整树 |
| 真链 `quickjs.c` | ~65kLOC；分配器/`JSMallocFunctions`、GC、模块、BigInt、async… |
| 过程间 color/escape | 未做 |
| Go `.syso` 与 C `.bss` PC32 | Go 内链不重定位 `.bss` PC32 → 本切片用 **Go 提供 arena buffer**（无 C BSS） |

**结论：P23 已交付** — goc 面向的 QJS 第一刀（显式 JSValue + cptr arena + stub eval + Go 栈 harness）；**不是**完整 QJS。
