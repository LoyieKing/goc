# P18 归档报告 — 前端色注解接到后端 stackmap / 写屏障

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1  
**实现树：** [`../p18-color-bridge/`](../p18-color-bridge/)  
**前端：** [`../p17-frontend/`](../p17-frontend/)（P17，诊断不变）  
**后端 hooks：** [`../p5-machinepass-goobj/pass/`](../p5-machinepass-goobj/pass/)（R5 / color-aware maps / WB gate）

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| 消费 `!goc.color` / `!goc.prov`（及 uptr 标记） | ✅ `goc-color-bridge` |
| gptr store → WB 路径（复用 `GocExpandStoreGptr`） | ✅ |
| sptr / stack-prov 跨 CALL → Locals maps（spill） | ✅ |
| cptr non-stack：不进 Go WB；无虚假 maps | ✅ |
| P17 非法 sptr→heap 仍前端失败；无 auto-promote | ✅（P17 重跑绿） |
| 色驱动 goldens（maps/WB） | ✅ 5 PASS 行 |
| 一键含 P17 + P18 | ✅ `./p18-color-bridge/build.sh` |
| 归档 + roadmap/PLAN | ✅ |

## 数据流（精确）

```text
color.ll (!goc.color / !goc.prov / !goc.uptr_encoded)
  → goc-color-bridge
      attrs: goc-color-driven, goc-store-gptr|goc-color-wb,
             goc-spill-gptrs|goc-emit-maps, goc-color-cptr-only,
             goc-arg-ptr-colors, goc-colors-seen
      + *.bridge.txt
  → goc-p18-driver（seed MIR）
      → GocSpillGptrsAtSafepoints   (R5 color-driven)
      → GocEmitPointerMaps          (args from goc-arg-ptr-colors)
      → GocExpandStoreGptr          (WB if goc-store-gptr|goc-color-wb)
  → p18.recipe.txt / p18.mir / <fn>/maps.txt
  （打包 goobj 仍走既有 P5–P16 elfpack 路径；本轮垂直证明停在 maps/WB recipe）
```

详见 [`../p18-color-bridge/docs/DATAFLOW.md`](../p18-color-bridge/docs/DATAFLOW.md)。

## 色 → 行为

| 色 / 来源 | WB | Spill + Locals/Args |
|-----------|----|---------------------|
| gptr / goheap **store** | 是 | 仅当跨 CALL 也活 |
| sptr / stack 跨 CALL | 否 | 是 |
| cptr / cheap | 否 | 否 |
| uptr encoded 标记 | 透传 `goc-color-uptr-encoded` | MSB 运行时编码仍缺口 |

## PASS 证据

```text
P18 results (2026-09-21 22:20 CST)
PASS P18-bridge (gptr→WB attrs, sptr→maps attrs, cptr→no WB)
PASS P18-vertical-fe (C→color→bridge)
PASS P18-WB (gptr store → ExpandStoreGptr / gcWriteBarrier2)
PASS P18-maps (sptr across CALL → Locals maps)
PASS P18-cptr (no spurious Go WB)
PASS p18-color-bridge (P17-green + color→maps/WB)
```

P5 回归（色 hooks 为加性）：`PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])`。

一键：`./p18-color-bridge/build.sh` 或 `./bin/goc-p18`。

## 触及文件

**新建**
- `p18-color-bridge/pass/GocColorBridge.cpp`
- `p18-color-bridge/pass/goc_p18_driver.cpp`
- `p18-color-bridge/pass/Makefile` · `build.sh` · `README.md` · `docs/DATAFLOW.md`
- `p18-color-bridge/fixtures/*.color.ll` · `tests/p18_ok_*.c`
- `bin/goc-p18`
- `p18-archive/P18-REPORT.md`（本文件）

**修改（P5 加性）**
- `GocSpillGptrsAtSafepoints.cpp` — R5 color-driven；cptr-only 拒绝
- `GocExpandStoreGptr.cpp` — `goc-color-wb`；拒 `goc-color-cptr-only`；recipe 记 color
- `GocEmitPointerMaps.cpp` — `goc-arg-ptr-colors` Args 位；color-driven 分目录 maps

**文档**
- `roadmap-next.md` · `p5-machinepass-goobj/PLAN.md` · `p17-frontend/docs/IR-COLOR.md`

## 诚实缺口

| 项 | 说明 |
|----|------|
| uptr MSB 运行时编码 | **P19 已交付** — 见 [`../p19-archive/P19-REPORT.md`](../p19-archive/P19-REPORT.md) |
| 统一 `bin/goc` | P8 CLI 仍偏后端 smoke；前端 `goc-fe`、本轮 `goc-p18` 并行 |
| 整管线 color→goobj harness | 垂直证明到 maps/WB MIR+recipe；未把 P17 C 样例打进 Go harness smoke |
| Interprocedural / 间接调用 | 形参色仍粗（bridge 按函数内 MD + CALL 启发式） |
| QJS | OUT OF SCOPE |
| 全量 IR→MIR 降色 | 仍 seed MIR（与 P5 同风格）；非 Generic 从 color.ll SelectionDAG 降下来 |
| dsptr | 无（合同禁止） |

**结论：P18 已交付** — 色元数据驱动至少一条端到端垂直路径（bridge attrs → 既有 Spill/Maps/WB）。
