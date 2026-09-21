# P5→P6 计划：MachineFunctionPass + LiveIntervals-eq + 独立 goobj

**日期：** 2026-09-21（Asia/Shanghai）  
**术语：** [../glossary.md](../glossary.md) · **P6 计划：** [../p6-plan.md](../p6-plan.md)

## 成功标准（已达成）

| # | 标准 | 证据 |
|---|------|------|
| P6-1 | LiveIntervals-eq spill；maps `locals_source liveintervals` | `build/pass-out/maps.txt`；recipe `mi_path=liveintervals_eq_spill_gptr` |
| P6-2 | 多 FI 真 Go SP；`hold_two` byte=`0x0c`（非 FI×8） | `build/pass-out/hold_two/maps.txt`；harness **PASS S2** |
| P6-3 | X86InstrInfoLite + 独立 goobj（无 overlay） | `pass/vendor/X86InstrInfoLite.h`；`goobj/enc/`；`run_binwriter.sh` plain `go build` |
| P6-4 | `./build.sh` L/W/S/S2/S3/A PASS | `PASS p5-machinepass-goobj (L+S+S2+S3+A[+W])` |
| P6.1-gptr | 收紧 gptr ID（非每个 GR64） | recipe `gptr_id_rules=...; reject=every_GR64` |
| P6.1-S3 | 寄存器-only 硬例 | `hold_regonly` maps + **PASS S3** |
| P6.1-LIS | morestack 覆盖；不在环上重算 LIS | `lis_policy=...; spill_ptr_args_before_morestack` |

## 有序工作表（已完成）

1. 寄存器级 liveness + safepoint spill（Go 对齐：活 gptr spill→Locals）  
2. 多 FI Go SP 偏移（禁 FI 序号×8）  
3. X86InstrInfoLite + vendored goobj enc（无 GOROOT overlay）

## 诚实缺口

- llvm::LiveIntervals 构造函数在 PassManager 外为 private → 使用 SlotIndexes + 等价 dataflow（文档标明）  
- Distro 仍无 `X86InstrInfo.h` → Lite 按名解析  
- 不发射寄存器 pointer-map 位（对齐 Go spill→Locals 主路径）


## P8（2026-09-21）

见 [../roadmap-next.md](../roadmap-next.md) 与 [../p8-archive/P8-REPORT.md](../p8-archive/P8-REPORT.md)：
StackCheck 后安全重建 liveness、MIR→goobj（stackcheck）、ABIInternal、`bin/goc`。


## P9（2026-09-21）

全量 MIR→goobj：见 [../p9-archive/P9-REPORT.md](../p9-archive/P9-REPORT.md)。Hold*/WB/CheckedAdd 体不再手写 Prog；`mi_lower.txt` 含 `.begin_fn` 完整 MI。


## P10（2026-09-21）

完整后端收尾：Hold* MIR morestack（NOSPLIT）、goc_leaf→goobj、golden mutate、无 .syso。
前端 color/escape/QJS 延后。见 [../p10-archive/P10-REPORT.md](../p10-archive/P10-REPORT.md)。


## P11（2026-09-21）

通用 LLVM MIR 解析：`goobj/mirparse` + `pass/harness.mir` 为主路径；`mi_full_bodies` 仅 fallback。
见 [../p11-archive/P11-REPORT.md](../p11-archive/P11-REPORT.md)。

## P12（2026-09-21）

LLVM MC encoding：`goobj/llvmmc`（mircanon→llc→elfpack）。禁止 mirlower demo whitelist 作为主路径。
见 [../p12-archive/P12-REPORT.md](../p12-archive/P12-REPORT.md)。


## P13 (2026-09-21)

Eliminate mircanon CFG/frame rewrite; dense per-CALL PCDATA; elfpack reloc/pcsp checks; AVX/x87/EH contracts.
见 [../p13-archive/P13-REPORT.md](../p13-archive/P13-REPORT.md)。

## P14 (2026-09-21)

Zero dialect strip (llc consumes harness.mir directly); float64/32 through Go ABIInternal (X0/X1).
见 [../p14-archive/P14-REPORT.md](../p14-archive/P14-REPORT.md)。


## P15 (2026-09-21)

Pass 自动导出标准 MIR：`llvm::printMIR` → `build/pass-out/harness.mir` (+ meta)；热路径不再以手写 `pass/harness.mir` 为唯一真相。
见 [../p15-archive/P15-REPORT.md](../p15-archive/P15-REPORT.md)。

## P16 (2026-09-21)

Backend 收口：分析 MF → Go-frame physreg lower → printMIR 统一导出；删除并行 seed 导出。
见 [../p16-archive/P16-REPORT.md](../p16-archive/P16-REPORT.md)。
前端已于 P17 开工（树外 `p17-frontend/`）。

## P17 note (2026-09-21 Asia/Shanghai)

Frontend color/escape: [`../p17-frontend/`](../p17-frontend/). P18 wires `!goc.color` → Spill R5 / EmitMaps / ExpandStoreGptr via [`../p18-color-bridge/`](../p18-color-bridge/); see [`../p18-archive/P18-REPORT.md`](../p18-archive/P18-REPORT.md).

## P18 (2026-09-21)

Color-driven hooks (additive): Spill R5 / maps Args colors / WB `goc-color-wb`.
Driver+fixtures live in `../p18-color-bridge/`. Backend L+W+S+S2+S3+A+F still PASS.

## P19 (2026-09-21)

uptr MSB runtime encode/decode: [`../p19-uptr-runtime/`](../p19-uptr-runtime/); see [`../p19-archive/P19-REPORT.md`](../p19-archive/P19-REPORT.md).
