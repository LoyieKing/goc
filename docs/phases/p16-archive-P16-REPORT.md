# P16 归档报告 — 后端收口：分析 MF → Go-frame physreg → printMIR

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**前端 color/QJS：** 仍 OUT OF SCOPE

## 后端完成定义（本轮）

1. **单一 body 路径：** TEXT 字节由 llc 编码，来源是经分析 PassManager（Spill → EmitMaps → StackCheck → RebuildLIS → WB）处理后的 MachineFunction，再经 Go-frame physreg lower，最后 `llvm::printMIR` 导出。  
   **禁止：** `gocEmitLlcReadyMir`（或等价物）另建一套从未经过 Spill/StackCheck 的并行 `seed*` post-RA 体。
2. Seed 只允许作为**管线输入**（PM 之前写入同一批 MF）。
3. `build/pass-out/harness.mir` (+ meta) 由上述 post-pipeline MF 产出；热路径不变：mirguard identity → llc-19 DIRECT → elfpack。
4. 全量 harness：`PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])`。
5. AVX / x87 / EH：**合同关闭**（Go-callable 路径 permanent unsupported / FATAL；不假装支持）。
6. 本报告明确：后端完成；剩余产品工作 = 前端。

## PASS 证据

```text
P16: harness.mir from analysis MF after Go-frame lower (no parallel seed export)
PASS-DRIVER: wrote .../harness.mir + harness.meta.json (printMIR llc-ready export)
P16: analysis MF → Go-frame lower → printMIR harness OK (no parallel seed export)
...
PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])
```

一键：`./p5-machinepass-goobj/build.sh`

## 统一如何工作

```text
goc-pass-driver（同一 Module + MMI）
  ├─ 管线前 seed：hold_* / checked / store / leaf / fadd（分析形态 vreg 或空）
  ├─ Spill → EmitMaps → StackCheck → RebuildLIS → WB
  ├─ GocDumpMirPass → build/pass-out/goc.mir   （分析态：vreg / %stack.N / morestack）
  └─ GocLowerGoFrameEmitPass
       ├─ 证明：stackcheck 函数在分析 MF 上已有 morestack
       ├─ deleteMachineFunctionFor + 新建 MF（避开 LIS/%stack 残留）
       ├─ Go-frame physreg lower（原 seed* 体作为 Lower 构建器，消费分析契约）
       ├─ MIR 文件 IR 段写 void() stub（避免 llc 按 ptr 形参插 home spill）
       └─ printMIR(MF) → build/pass-out/harness.mir + meta
```

P15 的并行 Module（`goc_p15_export` + 独立 seed 表）已删除；编码体与 maps/recipe 共享同一组函数名与分析 PM。

## 证明：无并行 seed 导出

| 证据 | 含义 |
|------|------|
| harness 头注释 `no parallel seed export` | 生产者自证 |
| log `P16: harness.mir from analysis MF after Go-frame lower` | 驱动日志 |
| `goc.mir` 含 `%stack` / morestack；`harness.mir` 无 `%stack`、physreg | 分析→lower 前后指纹 |
| Module/函数名仍为分析侧 `goc_*`（非第二套 export Module） | 同一管线 |
| recipe/maps 函数名与 harness TEXT 对齐 | 无双轨命名 |

## 合同排除（永久，非缺口）

| 项 | 状态 |
|----|------|
| AVX Go-callable harness | unsupported（标量 SSE2 float = PASS F） |
| x87 Go-callable | permanent unsupported FATAL |
| EH landingpad | unsupported FATAL |

## 剩余工作（非后端）

- IR 上 color / escape 分析  
- QJS / 前端接入  
- 产品级 CLI 体验与中端 O2（文档已延后）

**结论：后端完成**（上表合同排除除外）。

## 触及文件

- `pass/goc_mir_export.cpp` / `.h` — `GocLowerGoFrameEmitPass`；删除并行 `gocEmitLlcReadyMir` Module
- `pass/goc_pass_driver.cpp` — 补齐 hold_arg/leaf/fadd seed；PM 末挂 Lower+Emit
- `build.sh` — P16 证明
- `PLAN.md` / `README.md` / `../roadmap-next.md`
