# P13 归档报告 — 消除 mircanon CFG/frame、稠密 PCDATA、elfpack 硬化、AVX/x87/EH

**日期：** 2026-09-21（Asia/Shanghai）  
**实现树：** [../p5-machinepass-goobj/](../p5-machinepass-goobj/)  
**前端 color/QJS：** 仍 OUT OF SCOPE

## PASS 证据

```text
PASS L: checked entry + MIR/goobj leaf after growth = 42
PASS W: store_gptr WB enabled path hits=200
PASS S: live *int across CALL+morestack with LocalsPointerMaps from pass
PASS A: ArgsPointerMaps keep arg *int across CALL+morestack
PASS S2: two live *int across CALL+morestack (Go SP layout, not FI*8)
PASS S3: register-only *int across CALL+morestack (LiveIntervals spill→Locals)
PASS p5-machinepass-goobj (L+S+S2+S3+A[+W])
```

一键：`./p5-machinepass-goobj/build.sh`（含 `goobj/llvmmc/check_fixtures.sh`）

## 1. mircanon：已删除 CFG/frame 重写

### 从 mircanon 移除

| 已删除 | 说明 |
|--------|------|
| `split_morestack_cfg` | 原 harness 形「JCC 后同 BB 跟 morestack」CFG 修补 |
| `inject_frame` | 原自动插入 `PUSH BP` / `SUB $frame` |

`build.sh` / 单测若再出现 `def split_morestack_cfg` / `def inject_frame` → **FATAL**。

### Pass 侧

`pass/harness.mir` 现为 **llc-19 可直接消费** 的合法 CFG：

- morestack 在独立 MBB；JCC 为 terminator（无同 BB 后续 ops）
- `goc.frame>0` 时显式 Go 序言/尾声：`PUSH64r $rbp` / `SUB64ri8` / `ADD` / `POP64r`

### 残留 dialect-only（逐条文档）

| # | Transform | 证明 |
|---|-----------|------|
| 1 | Strip `goc.*` → meta.json | goobj 元数据 |
| 2 | Strip `GOC_PCDATA1` → meta `calls[]` | 稠密 PCDATA |
| 3 | `&sym` → `@sym` | LLVM 符号语法 |
| 4 | noreg 全局 → `$rip` | PIC |
| 5 | `CMP64ri` → `CMP64ri32` | X86 opcode 名 |
| 6 | CALL `$rsp/$ssp` implicits | llc verifier |
| 7 | `bb.N.name:` → `bb.N:` | 标签 |
| 8 | IR stubs + `no_callee_saved_registers` | Go ABIInternal |

### 回归

- harness dialect-only → `llc-19 -filetype=obj`（无 CFG repair）
- `clang-19` 小样本 → `llc-19`（完全无 mircanon）
- meta：`cfg_rewrite: false`, `frame_inject: false`

## 2. 稠密 PCDATA（每 CALL safepoint）

- meta `calls[].stackmap_index` 来自 `GOC_PCDATA1`
- elfpack 扫 `0xe8` CALL（经 ELF PLT32/PC32 reloc）→ 建 `PCDATA_StackMapIndex` pctab
- FUNCDATA Args/Locals 仍来自 pass maps；harness **S/S2/S3/A PASS**

### 证明

- **同 live set 共享 index：** `goc_hold_live` 的 morestack / HugeFrameVoid / goc_leaf 均为 **idx 0**（见 `pcdata_proof.txt`）
- **不同 live set 区分 index：** fixture `pass/fixtures/dense_pcdata.mir` → idx **0** 与 **1**（`check_fixtures.sh`）

## 3. elfpack pcsp / reloc 硬化

| 检查 | 行为 |
|------|------|
| PLT32 addend | 必须 `-4`（否则 FATAL） |
| PC32 addend | `-4` 或 `-5` |
| Go `R_CALL` addend | `A' = A+4` → 必须 `0` |
| pcsp | `frame>0` ⇒ `spdelta=frame+8`；TEXT 必须以 `0x55` (PUSH BP) 开头 |
| `-check-relocs` | build.sh 自动调用 |

## 4. AVX / x87 / EH

| 项 | 状态 |
|----|------|
| SSE | hand MIR `MOVAPS`+`ADDPS` → llc → elfpack；objdump `movaps`/`addps` |
| AVX | **clang** `_mm256_add_ps` → llc `+avx` → objdump **`vaddps`** |
| x87 | **clang** `long double` add → llc → objdump **`fldt`/`faddp`** |
| EH | **contract = unsupported**：`EH_LABEL` → mircanon **FATAL**（llc 会静默丢掉）；C++ `landingpad` IR 仅作对照，**不**打进 goobj |

说明：AVX/x87 smoke **不**链入 Go harness（避免破坏 Go 运行时 ABI）；证明停在 llc MC + objdump +（SSE）elfpack。

## 5. 保持

- Encoding = **LLVM llc MC**（无 demo opcode table）
- `./build.sh` → L W S S2 S3 A PASS

## 诚实剩余缺口

- dialect strip（上表 1–8）仍需要；**不是** CFG/ABI/frame 重写
- AVX/x87 未做 Go runtime 链接冒烟（有意：避免污染 ABIInternal / GC）
- EH：无支持路径；仅显式 FATAL
- 任意 clang 预 RA MIR（带 vreg）仍需 COPY→physreg；本 harness 为 post-RA
- pass C++ 仍不直接 dump llc-ready MIR；主路径手写/维护 `harness.mir`（结构已是标准 MIR）

## 触及文件

- `pass/harness.mir` — 合法 CFG + 显式 frame
- `goobj/llvmmc/mircanon.py` — dialect-only；删 CFG/frame；EH FATAL
- `goobj/llvmmc/check_fixtures.sh`、`pass/fixtures/**`
- `goobj/elfpack/main.go` — 稠密 PCDATA、reloc/pcsp 检查
- `goobj/mirparse` / `mirlower` — PUSH/POP（demoted 路径）
- `build.sh`、`docs/MI_AND_GOOBJ.md`、`PLAN.md`、`roadmap-next.md`
