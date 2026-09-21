# P11 归档报告 — 通用 LLVM MIR 解析器

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**路线：** [../roadmap-next.md](../roadmap-next.md)  
**前端：** 仍延后（本轮不做）

---

## PASS 证据

```text
PASS L: checked entry + MIR/goobj leaf after growth = 42
PASS W: store_gptr WB enabled path
PASS S: live *int across CALL+morestack (LocalsPointerMaps)
PASS A: ArgsPointerMaps
PASS S2: two live *int (Go SP layout 0x0c)
PASS S3: register-only *int (LiveIntervals spill→Locals)
PASS p5-machinepass-goobj (L+S+S2+S3+A[+W])
```

一键：`./p5-machinepass-goobj/build.sh`

---

## 本轮目标与结果

| 成功标准 | 结果 |
|----------|------|
| 通用 LLVM MIR 解析（YAML `name:`/`body: \|` + MF dump） | **DONE** — `goobj/mirparse` |
| 集成：MIR → mirlower → goobj；缺 fn FATAL | **DONE** — `cmd/mir2lower`；`RequireFn` |
| Hold*/CheckedAdd/WB/leaf 默认走 MIR | **DONE** — `pass/harness.mir` 主路径 |
| `mi_full_bodies` 仅 fallback + 文档 | **DONE** |
| 单测 + golden mutate + `./build.sh` L W S S2 S3 A | **DONE** |
| Docs / archive；诚实缺口 | **DONE** |

---

## 解析的格式

1. **YAML-ish LLVM MIR**（LLC / 手写夹具）  
   - `---` / `name:` / `body: |` / `bb.N[.label]:`  
   - `$rax` physregs、`%0:gr64` vregs、`%bb.N`、`%stack.N`  
   - X86 五元组内存操作数；`&sym` / `@sym`  
   - `successors:` / `; predecessors:`  
   - 扩展键：`goc.go_sym` / `goc.frame` / `goc.flags`  
   - 伪指令：`GOC_PCDATA1`

2. **MF dump**（`goc.mir` / `MachineFunction::print`）  
   - `# Machine code for function NAME:` … `# End machine code`  
   - `fi#N: … at location [SP+N]`

---

## 管道接线

```text
Spill → Maps → StackCheck → RebuildLIS → WB   (pass; 仍写 goc.mir + recipe)
        │
pass/harness.mir  ──mirparse/mir2lower──►  build/pass-out/mi_lower.txt
        │                                    (recipe 元数据 + 全 TEXT bodies)
        │
goobj/mirlower → binwriter → goc_funcs.o → harness L/W/S/S2/S3/A

fallback: pass/mi_full_bodies.txt（仅 -fallback-bodies / 文档）
```

`build.sh` 步骤 `[1b]` 强制用 `harness.mir` 覆盖 pass 驱动写出的 bodies。

---

## 覆盖 vs 缺口

| 覆盖 | 缺口（诚实） |
|------|----------------|
| 多函数 MIR 文件 | Bundle |
| Physreg + COPY→vreg 别名 | 通用 regalloc / 任意预分配 vreg |
| MBB、successors/predecessors | 完整 CFI 发射 |
| MOV/ADD/SUB/CMP/LEA/JMP/JCC/CALL/RET | AVX / x87 / EH landingpads |
| Spill/reload（MOV 至 SP/FI） | TEST 仅近似为 CMP |
| FS/GS TLS、符号、FI→SP | 与 pass `goc.mir` 自动对齐（夹具仍手写 Go ABI 合同） |
| Golden：改 imm → goobj 字节变 | 前端 color/escape/QJS（延后） |

---

## 触及文件

- `goobj/mirparse/` — parse + convert + tests  
- `goobj/cmd/mir2lower/` — CLI  
- `pass/harness.mir` — 主夹具  
- `pass/mi_full_bodies.txt` — fallback 标记  
- `build.sh` — `[1b]` MIR 主路径  
- `docs/MI_AND_GOOBJ.md` §P11、`PLAN.md`、`roadmap-next.md`  
- `p11-archive/P11-REPORT.md`（本文件）
