# P9 归档报告 — 全量 MIR→goobj（无 Prog 模板）

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**路线：** [../roadmap-next.md](../roadmap-next.md)

---

## PASS 证据

```text
PASS L: checked entry + LLVM leaf after growth = 42
PASS W: store_gptr WB enabled path
PASS S: live *int across CALL+morestack (LocalsPointerMaps)
PASS A: ArgsPointerMaps
PASS S2: two live *int (Go SP layout 0x0c)
PASS S3: register-only *int (LiveIntervals spill→Locals)
PASS p5-machinepass-goobj (L+S+S2+S3+A[+W])
```

一键：`./p5-machinepass-goobj/build.sh` · `./build-p8.sh`

---

## 本轮目标与结果

| 成功标准 | 结果 |
|----------|------|
| Pass 导出每函数完整 MI 列表到 `mi_lower.txt` | **DONE** — `.begin_fn`…`.end_fn`（`pass/mi_full_bodies.txt` 经 driver 并入） |
| `goobj/mirlower` 降到 `obj.Prog` / goobj TEXT | **DONE** — CheckedAdd / Hold* / StoreGptrWB |
| binwriter **无** Hold*/WB/CheckedAdd 模板体；缺 MI 则失败 | **DONE** — 仅 TEXT+FUNCDATA 胶水；`RequireFn` FATAL |
| `./build.sh` / `../build-p8.sh` 仍 PASS L W S S2 S3 A | **DONE** |
| 文档说明已 MIR-lower 范围与剩余限制 | **DONE** — 本报告 + `docs/MI_AND_GOOBJ.md` P9 |

### 模板已移除（证明）

```bash
rg -n 'AMOVQ|HugeFrameVoid|gcWriteBarrier2|leafOk' p5-machinepass-goobj/goobj/binwriter/main.go
# → no matches
```

### 触及文件

- `pass/mi_full_bodies.txt`（新）— 每函数完整 MI 体
- `pass/goc_pass_driver.cpp` — 写入 `mi_lower.txt` 时并入 full bodies
- `goobj/mirlower/lower.go` — 全量 opcode→Prog 降级
- `goobj/binwriter/main.go` — 去模板；MIR-only
- `docs/MI_AND_GOOBJ.md`、`roadmap-next.md`、`build.sh` 守卫
- `p9-archive/P9-REPORT.md`（本文件）

---

## 已全面 MIR-lower 的符号

| LLVM / MI 名 | Go TEXT | 含 morestack / WB |
|--------------|---------|-------------------|
| `goc_checked_add` | `main.GocCheckedAdd` | morestack（MIR） |
| `goc_hold_live` | `main.GocHoldLive` | 体 MIR；Go 自动 prologue（非 NOSPLIT） |
| `goc_hold_arg` | `main.GocHoldArg` | 同上 |
| `goc_hold_two` | `main.GocHoldTwo` | 同上 |
| `goc_hold_regonly` | `main.GocHoldRegOnly` | 同上 |
| `goc_store_gptr` | `main.StoreGptrWB` | WB 路径 MIR |

---

## 剩余限制（诚实）

1. **非通用 LLVM MIR 解析器** — 仅 harness 文档化的 op 子集（physreg / SP / 符号操作数）。
2. **无通用 vreg 分配器** — MI 体以 physreg 形式导出。
3. **`goc_leaf` `.syso`** — 仍为纯 LLVM `llc` 输出，不经 goobj 打包。
4. **Hold* morestack** — 非 NOSPLIT TEXT 仍走 Go 自动栈检查 prologue；CheckedAdd 的 morestack 环为 MIR-lower。
5. **Full bodies 来源** — `mi_full_bodies.txt` 是 pass 管线在 spill/maps/stackcheck/WB 之后导出的 goobj 面向 MI（与 `goc.mir` 语义对齐的包装），不是把任意 `.mir` 文件喂给通用解析器。

---

## 管道（P9）

```text
Spill(CALL, 主 LIS) → EmitMaps → StackCheck(CFG) → RebuildSafeLiveness+MorestackSpill → WB
  → mi_lower.txt (recipe + .begin_fn full bodies)
        │
goobj/mirlower (全 TEXT) + binwriter (FUNCDATA only; fail if MI missing)
        │
harness L/W/S/S2/S3/A
```
