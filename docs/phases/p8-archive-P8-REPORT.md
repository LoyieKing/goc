# P8 归档报告

**日期：** 2026-09-21（Asia/Shanghai）  
**路线：** [../roadmap-next.md](../roadmap-next.md)  
**实现：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/) · [../p8-compiler-pipeline/](../p8-compiler-pipeline/)

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

一键：

```bash
./p5-machinepass-goobj/build.sh
# 或
./build-p8.sh          # 构建 bin/goc 并跑 demo
./bin/goc demo
```

### 关键产物路径

| 产物 | 路径 |
|------|------|
| Pass 输出 | `p5-machinepass-goobj/build/pass-out/`（`goc.mir`, `stackcheck.recipe.txt`, **`mi_lower.txt`**, maps） |
| goobj | `p5-machinepass-goobj/build/goobj/goc_funcs.o` |
| harness | `p5-machinepass-goobj/build/p5_machinepass_goobj` |
| CLI | `bin/goc` |

### Recipe / 策略标记（抽查）

- `lis_policy=safe_recompute_after_stackcheck; … morestack_spill_via_rebuilt_lis`
- `stackcheck_mode=cfg_only`
- `morestack_spill_rebuilt_lis fi=…`
- `abi=amd64_ABIInternal_AX_BX_CX_DI_SI_R8_R9`
- `mi_lower.txt`: `format goc-mi-lower-1` / `abi amd64_ABIInternal`

---

## 本轮实现对照

| 序 | 项 | 结果 |
|----|-----|------|
| 1 | StackCheck 后安全重建 liveness；morestack spill | **DONE** — `GocRebuildLISAfterStackCheck`；StackCheck 仅 CFG |
| 2 | MIR→goobj | **PARTIAL** — `goobj/mirlower` 降 stackcheck；Hold*/WB 体仍模板 + 真 FUNCDATA/maps |
| 3 | ABIInternal | **DONE** — callee AX/BX/…；ret AX；symabis ABIInternal；无 ABI0 wrapper（harness 直调） |
| 4 | `goc` CLI | **DONE** — `bin/goc demo\|build\|version` |
| 5 | color/escape | **文档 only** — 见 roadmap §5 |
| 6 | O2/GC | **文档 only，不优先** — 见 roadmap §6 |

---

## 诚实缺口

1. **真 LIS 环上重算**：仍回避；用固定点 live-in/out **等价**重建（已知盲重跑不收敛）。
2. **MIR→goobj**：无完整 `.mir` 解析 / MC bytes+relocs；Hold 函数体仍部分手写 Prog（maps/FUNCDATA 真）。
3. **goc_leaf**：仍为 SysV `llc` `.syso`；CheckedAdd 内 AX/BX→DI/SI 转调。
4. **StoreGptrWB**：ABIInternal 入口后转为 Demo C（CX=slot, AX=new）。
5. **Caller 侧**：未单独实现跨包 ABIInternal→ABI0 转换器；本 harness 同包 ABIInternal 足够。
6. **Color/escape / O2/GC**：未做。

---

## 管道（P8）

```text
Spill(CALL, 主 LIS)
  → EmitMaps
  → StackCheck(CFG only)
  → RebuildSafeLiveness + MorestackSpill
  → WB expand
  → mi_lower.txt + recipe
        │
goobj/mirlower (stackcheck) + binwriter (ABIInternal TEXT + FUNCDATA)
        │
harness + toolexec ABIInternal + pack → L/W/S/S2/S3/A
```
