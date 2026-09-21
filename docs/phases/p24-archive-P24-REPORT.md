# P24 归档报告 — 解释器循环 safepoint / morestack

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** goc 跑在 goroutine 栈上；长跑解释器须 poll safepoint，以便 morestack / GC  
**实现树：** [`../p24-qjs-safepoint/`](../p24-qjs-safepoint/)（扩展 P23 stub）  
**入口：** `./bin/goc qjs --safepoint` · `./bin/goc test --p24` · `./p24-qjs-safepoint/build.sh`

> **诚实声明：仍是 STUB VM，不是完整 QuickJS。**  
> Upstream `quickjs-ng/` 未链接。Safepoint 密度/策略为演示级（后向边 + 可选每 N 条 opcode）。

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| stub 解释器主循环插入 safepoint poll | ✅ `qjs_interp.c` / amalgam；后向边必 poll |
| 复用 P1/P5 TLS 风格（`g` / `stackguard0`），非第三套 TLS | ✅ Go `SafepointPoll` 读 TLS→g→offset 16 |
| 紧循环最终命中 safepoint（计数/hook trace） | ✅ `go-safepoint-hit` / `host-safepoint-hit` |
| 强制 tiny stack / growth：解释器跨 safepoint 存活（或文档化 sim） | ✅ Go-framed `ForceMorestackOnce` + 增长后 interp；host sim morestack |
| P23 regress；color-escape 金测不变 | ✅ `p23-regress` + color OK/ERR |
| `bin/goc qjs --safepoint` / `goc test --p24` | ✅ |
| 无 dsptr；无 sptr auto-promote；显式 JSValue | ✅ |

## 用法

```bash
./p24-qjs-safepoint/build.sh   # P23 regress + host demo + Go harness
./bin/goc qjs --safepoint      # 同上
./bin/goc test --p24           # P24 only
./bin/goc qjs                  # 仍为 P23-only 切片
```

## PASS 证据（摘要）

```text
PASS p23-regress (demos + color goldens unchanged contract)
PASS host-safepoint-hit
PASS host-sim-growth-morestack
PASS go-safepoint-hit
PASS go-force-morestack-across-safepoint
PASS go-interp-survives-after-grow
PASS p24-qjs-safepoint (stub interp safepoint/morestack — NOT full QJS)
```

产物：`p24-qjs-safepoint/build/p24_demo` · `p24_qjs_go` · [`PASS-LINES.txt`](./PASS-LINES.txt)

## 布局

```text
p23-qjs-slice/stub/
  qjs_interp.h/.c          bytecode + tight_loop + safepoint tick API
  qjs_slice.c              JSRuntime.{safepoint_poll,hits}
  qjs_freestanding_amalgam.c  inlined P24 (static helpers; no PLT/jump-table)

p24-qjs-safepoint/
  demo/demo_main.c         host safepoint + sim growth
  stub/qjs_safepoint_host.c
  harness/                 CGO_ENABLED=0 + SafepointPoll / ForceMorestackOnce
  docs/SAFEPOINT.md
  build.sh
```

## 设计要点

1. **In-loop poll（C→hook）：** 计数 + TLS `stackguard0` 观察；**不**从 C 帧 `CALL morestack`（Go `copystack` 无法展开 C 帧）。
2. **Real morestack（Go-framed）：** `ForceMorestackOnce` / `StackCheckedLeaf` 同 P1；在解释器窗口之间/之后调用。
3. **Freestanding：** `-fno-jump-tables -mstackrealign`；helpers `static`（避免 PLT32 / `.rela.rodata` PC32）。

## 缺口

| 项 | 说明 |
|----|------|
| 完整 QJS | 仍欠 NaN-box 迁移、真链 `quickjs.c` |
| In-loop 直接 morestack | 需 Go-framed interp 或 poll 返回 Go 再 grow |
| 生产 safepoint 策略 | 密度/抢占/GC 握手未做 |
| 过程间 color | 未做 |

**结论：P24 已交付** — stub 解释器循环 safepoint poll + P1 morestack 合同（Go-framed growth）；**不是**完整 QJS。
