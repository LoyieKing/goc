# P21 归档报告 — color.ll → goobj 垂直路径

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1  
**实现树：** [`../p21-color-vertical/`](../p21-color-vertical/)  
**入口：** [`../bin/goc`](../bin/goc) `vertical` / `build` / `test [--p21]`  
**前置：** P17–P20 · P5–P16 encode（printMIR / llc / elfpack）

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| 小 colored `.c` / color.ll → **goobj**（无手写 harness.mir 作唯一真相） | ✅ |
| 至少一条：stackmap（sptr 跨 CALL）或 WB（gptr store） | ✅ **两条都证明** |
| color → bridge → lower → P16 export 路径 | ✅ `printMIR` → llc → elfpack |
| `bin/goc vertical` / `build`；`goc test` / `goc test --p21` | ✅ |
| TEXT/maps/WB 来自 color 管线的证据 | ✅ bridge→recipe→meta `color_fp`→nm |
| 诚实列出泛化缺口；不拿无关 P5 harness 冒充 color 体 | ✅ |
| 不破坏 P16 后端合同 | ✅ elfpack 仅加性 meta 字段 |

## 用法

```bash
# from goc-docs/
./bin/goc vertical              # 或 ./bin/goc build
./bin/goc test --p21            # 仅 P21
./bin/goc test                  # P17+P18+P19+P21 聚合

# 直接
./p21-color-vertical/build.sh
```

产物：`p21-color-vertical/build/p21-out/p21_funcs.o`  
PASS 行：[`PASS-LINES.txt`](./PASS-LINES.txt)

## 数据流（精确）

```text
fixtures/*.color.ll  或  tests/*.c → P17 color-escape
  → goc-color-bridge
       need_wb / need_spill_maps + bridged.ll attrs
  → goc-p21-vertical
       MIR seed GENERATED from attrs  ✗ NOT pass/harness.mir
       → Spill / EmitMaps / ExpandStoreGptr
       → p21.analysis.mir + p21_sptr_across_call/maps*
       → Go-frame physreg（按 color attrs 选择 WB / spill 模板）
       → vertical.mir + vertical.meta.json (color_fp)
  → mirguard identity → llc-19 → elfpack
  → p21_funcs.o
       T main.P21GptrStoreWB
       T main.P21SptrAcrossCall
       R gclocals.p21SptrLocals / p21SptrArgs
```

详述：[`../p21-color-vertical/docs/VERTICAL.md`](../p21-color-vertical/docs/VERTICAL.md)

## 证据指纹链

| 环节 | 证据 |
|------|------|
| Bridge | `need_wb=1` / `need_spill_maps=1` in `*.bridge.txt` |
| Analysis | `p21.analysis.mir` 含 `gcWriteBarrier2`；maps `color_driven 1` |
| Recipe | `fn p21_gptr_store … wb=1`；`fn p21_sptr_across_call … spill_maps=1` |
| Meta | `color_fp`；`not_source: …/pass/harness.mir`；`producer: goc-p21-vertical/printMIR` |
| goobj nm | `main.P21GptrStoreWB` / `main.P21SptrAcrossCall` / `gclocals.p21Sptr*` |
| 非 P5 harness | vertical.mir **无** `goc_checked_add` / `goc_hold_live` / `goc_store_gptr`；nm **无** `GocHoldLive` / `StoreGptrWB` |

## PASS 证据

```text
P21 results (2026-09-21 22:32 CST)
PASS P21-bridge (gptr→WB attrs, sptr→maps attrs)
PASS P21-vertical-fe (C→color→bridge)
PASS P21-vertical-mir (color-seeded → Spill/Maps/WB → printMIR; ≠harness.mir)
PASS P21-goobj (TEXT+maps from color vertical; nm fingerprints)
PASS P21-fingerprint (bridge→recipe→meta→goobj nm)
PASS p21-color-vertical (color.ll→goobj proven slice)
```

一键：`./bin/goc vertical` 或 `./bin/goc test --p21`

## 触及文件

**新建**
- `p21-color-vertical/pass/goc_p21_vertical.cpp` · `Makefile` · `build.sh`
- `p21-color-vertical/fixtures/*.color.ll` · `tests/*.c`
- `p21-color-vertical/docs/VERTICAL.md` · `README.md`
- `p21-archive/P21-REPORT.md`（本文件）

**修改（加性）**
- `bin/goc` — `vertical`/`build`；`test` 含 P21；`test --p21`
- `p5-machinepass-goobj/goobj/elfpack/main.go` — 可选 `color_fp` / `maps_subdir` / `args_map` / `locals_map`
- `roadmap-next.md` · `goc-product-plan.md`（见对应 diff）

## 诚实缺口

| 项 | 说明 |
|----|------|
| 通用 color.ll SelectionDAG / ISel | **未做**；本轮是 1–2 函数 proven slice |
| Go-frame 体 | 仍为 **attrs 选择的模板重建**（与 P16 lower 同诚实级别）；seed+Spill/Maps/WB+符号名属 color 管线 |
| 任意 `.c` → 可链接 Go 产品 | 仍非产品级；仅垂直 demo |
| QJS | OUT OF SCOPE |
| 过程间 escape / 色传播 | 未做 |
| 生产 `stack.hi` TLS / `getg()` | **P22 已交付** |
| dsptr / sptr auto-promote | 无（合同禁止） |

**结论：P21 已交付** — 诚实的 color 驱动垂直路径打到 goobj；泛化缺口文档化。
