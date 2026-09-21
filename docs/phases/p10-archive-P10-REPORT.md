# P10 归档报告 — 完整后端收尾（Hold* MIR morestack + leaf + golden）

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**路线：** [../roadmap-next.md](../roadmap-next.md)  
**前端：** color / escape / QJS **延后**（本轮不做）

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

一键：`./p5-machinepass-goobj/build.sh` · `./build-p8.sh`（`bin/goc demo`）

---

## 本轮目标与结果

| 成功标准 | 结果 |
|----------|------|
| Hold* MIR-owned morestack（去 Go 自动 prologue 双轨） | **DONE** — `flags nosplit` + FS:-8/CMP/CALL morestack/JMP 进 `mi_full_bodies` → mirlower |
| Broader MI + golden（mutate → bytes change；禁模板回潮） | **DONE** — ADD/SUB/LEA/sym MOV；`go test` + build.sh mutate goobj |
| goc_leaf 进同一 MIR→goobj 管道；去掉 harness `.syso` | **DONE** — `goc_leaf` TEXT in `goc_funcs.o`；无 `.syso` |
| Docs + archive；前端延后声明 | **DONE** — 本报告 + `docs/MI_AND_GOOBJ.md` + `roadmap-next.md` |
| PASS L W S S2 S3 A (+ CLI) | **DONE**（见上） |

### Hold morestack 证明（objdump / recipe）

```text
# build/hold_objdump.txt (excerpt)
TEXT main.GocHoldLive(SB) goc_binwriter
  goc_binwriter:1	0x495680		55			PUSHQ BP				
  goc_binwriter:1	0x495681		4889e5			MOVQ SP, BP				
  goc_binwriter:1	0x495684		4883ec18		SUBQ $0x18, SP				
  goc_binwriter:1	0x495688		644c8b1c25f8ffffff	MOVQ FS:0xfffffff8, R11			
  goc_binwriter:1	0x495691		493b6310		CMPQ SP, 0x10(R11)			
  goc_binwriter:1	0x495695		770c			JA 0x4956a3				
  goc_binwriter:1	0x495697		4889442410		MOVQ AX, 0x10(SP)			
  goc_binwriter:1	0x49569c		e81f6cfdff		CALL runtime.morestack_noctxt.abi0(SB)	
  goc_binwriter:1	0x4956a1		ebe5			JMP 0x495688				
  goc_binwriter:1	0x4956a3		4889442410		MOVQ AX, 0x10(SP)			
  goc_binwriter:1	0x4956a8		e873020000		CALL main.HugeFrameVoid.abi0(SB)	
```

```text
# build/leaf_objdump.txt
TEXT goc_leaf(SB) goc_binwriter
  goc_binwriter:1	0x495760		4889f8			MOVQ DI, AX		
  goc_binwriter:1	0x495763		4801f0			ADDQ SI, AX		
  goc_binwriter:1	0x495766		c3			RET
```

`mi_lower.txt` / `pass/mi_full_bodies.txt`：每个 Hold* 含 `flags nosplit` + `mem=FS:-8` + `runtime.morestack_noctxt`。

### NOSPLIT vs checked 策略

| 类 | 标志 | morestack |
|----|------|-----------|
| Checked / Hold*（需扩栈） | `NOSPLIT` | **MIR 拥有**：compare/call/jmp 经 mirlower；禁止依赖 Go 汇编器自动栈检查 prologue |
| 小叶子（`goc_leaf`） | `NOSPLIT` | 无检查（帧 0，不增长） |
| StoreGptrWB | `NOSPLIT` | 无 morestack（WB 路径） |

### Leaf 路径

```text
mi_full_bodies goc_leaf → mi_lower.txt → mirlower → goobj TEXT goc_leaf (ABI0)
  → pack into harness .a
  （不再 llc→harness/*.syso）
```

`ir/goc_leaf.ll` 仍可作参考；`build/goc_leaf.llc.s` 可选生成但不链接。

### Golden test

- `goobj/mirlower/golden_test.go`：RequireFn Hold/leaf；NOSPLIT+FS:-8；mutate imm；binwriter 无 `x86.A*` 模板
- `build.sh`：`imm=42→43` 后重跑 binwriter，**goobj 字节必须变化**；恢复后继续

---

## 触及文件

- `pass/mi_full_bodies.txt` — Hold* morestack + goc_leaf
- `goobj/mirlower/lower.go` — ADD/SUB/LEA/sym addressing；Limits
- `goobj/mirlower/golden_test.go` — 新
- `goobj/binwriter/main.go` — goc_leaf TEXT（ABI0）
- `build.sh` — P10 守卫 / 无 .syso / golden / objdump 证明
- `docs/MI_AND_GOOBJ.md`、`roadmap-next.md`、`PLAN.md`
- `p10-archive/P10-REPORT.md`（本文件）

---

## 管道（P10）

```text
Spill → Maps → StackCheck → RebuildLIS → WB
  → mi_lower.txt（recipe + .begin_fn 含 Hold MIR morestack + goc_leaf）
        │
goobj/mirlower（全 TEXT）+ binwriter（FUNCDATA only；RequireFn FATAL）
        │
harness L/W/S/S2/S3/A（无 .syso）
```

---

## 剩余限制（诚实）

1. 仍非通用 LLVM MIR 解析器；physreg/SP/symbol 子集。
2. 无通用 vreg 分配器；`mi_full_bodies.txt` 为 pass 合同包装，非任意 `.mir` 喂入。
3. Hold* morestack 在 Go 插入的帧prologue之后执行（与 CheckedAdd 同模式：NOSPLIT 仍可有 BP/frame setup）。
4. **前端** color/escape/QJS：**延后**。
5. O2 / 更深 GC：**延后**。
