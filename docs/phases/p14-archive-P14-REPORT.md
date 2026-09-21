# P14 归档报告 — dialect strip → ZERO + float 进 Go ABI

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
PASS F: float64+float32 via llc→elfpack→goobj→Go (X0,X1→X0)
PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])
```

一键：`./p5-machinepass-goobj/build.sh`（含 `goobj/llvmmc/check_fixtures.sh`）

## 1. Dialect stripping → ZERO

### 已死掉的 transforms（改生产者，不再改写 MIR 体）

| # | 原 mircanon transform | P14 做法 |
|---|----------------------|----------|
| 1 | Strip `goc.*` → meta | `pass/harness.meta.json` sidecar；MIR 无 `goc.*` |
| 2 | Strip `GOC_PCDATA1` → calls[] | sidecar `calls[].stackmap_index`；MIR 无 PCDATA 伪指令 |
| 3 | `&sym` → `@sym` | Pass MIR 直接发 `@sym` |
| 4 | noreg abs → `$rip` | Pass MIR 直接发 `$rip, 1, $noreg, @sym` |
| 5 | `CMP64ri` → `CMP64ri32` | Pass MIR 直接发 `CMP64ri32` |
| 6 | CALL `$rsp/$ssp` implicits | Pass MIR 带全套 `implicit-def $rsp/$ssp` |
| 7 | `bb.N.name:` → `bb.N:` | 已是 `bb.N:` |
| 8 | IR stubs + `no_callee_saved_registers` | 嵌入 `harness.mir` 头（`--- |` IR 块） |

### 热路径

```text
mirguard.py   # identity：cmp 输入==输出；有方言残留 → FATAL
llc-19 … pass/harness.mir   # DIRECT，无 rewrite
elfpack -meta pass/harness.meta.json
```

`mircanon.py` 仅是 → `mirguard` 的 identity shim。任何 `rewrite_*` / `split_morestack_cfg` / `inject_frame` 定义 → FATAL。

### 证明

- `cmp -s pass/harness.mir build/pass-out/harness.canon.mir`
- meta：`mode=identity`, `transforms=[]`, `dialect_strip=false`
- fixtures：dense / SSE / harness 均 **llc DIRECT**

## 2. Float into Go ABI

### ABI（amd64 ABIInternal）

| | |
|--|--|
| Float 参数/返回 | **X0–X14**（**X15** = zero） |
| Integer 参数 | AX, BX, CX, DI, SI, R8, R9 |
| 参考 | `cmd/compile/abi-internal.md` |

### PASS F

| TEXT | MIR | Go |
|------|-----|-----|
| `main.GocFadd64` | `ADDSDrr $xmm0, $xmm1` | `GocFadd64(1.5, 2.25)==3.75` |
| `main.GocFadd32` | `ADDSSrr $xmm0, $xmm1` | `GocFadd32(1.5, 2.25)==3.75` |

路径：`llc → elfpack → goobj → harness`（toolexec 追加 `ABIInternal` symabis）。

### AVX / x87

| 项 | 状态 |
|----|------|
| SSE scalar float64/32 | ✅ Go harness **PASS F** |
| SSE pack (`MOVAPS`/`ADDPS`) | ✅ llc DIRECT + elfpack |
| AVX (`vaddps`) | ✅ clang→llc objdump；未做 Go harness 调用（无自然 Go 向量类型合同） |
| x87 | ✅ **Go-callable 永久 unsupported**：`mirguard` FATAL；clang `fldt`/`faddp` 仅 encode 对照 |

## 3. 保持

- Encoding = **LLVM llc MC**
- 稠密 per-CALL PCDATA + FUNCDATA
- elfpack reloc/pcsp 检查
- `./build.sh` → **L W S S2 S3 A F** PASS

## 诚实剩余缺口

- AVX 未做 Go harness 调用（标量 float 已覆盖 SSE2；AVX 停在 encode+elfpack）
- EH：仍无支持路径；仅 FATAL
- Pass C++ 仍不 dump llc-ready MIR；主路径维护 `harness.mir` + `harness.meta.json`
- 任意 clang 预 RA MIR（vreg）仍需 COPY→physreg；本 harness 为 post-RA

## 触及文件

- `pass/harness.mir` — llc-ready（含 IR stubs + fadd）
- `pass/harness.meta.json` — 元数据 sidecar
- `goobj/llvmmc/mirguard.py` — identity + FATAL
- `goobj/llvmmc/mircanon.py` — identity shim
- `goobj/llvmmc/run_llvmmc.sh` / `check_fixtures.sh`
- `pass/fixtures/dense_pcdata.*` / `avx_smoke.*`
- `harness/main.go` — PASS F
- `tools/toolexec_pack_goobj.sh` — Fadd symabis
- `goobj/elfpack` / `goobj/mirparse` — identity meta + SSE ops
- `build.sh`、`docs/MI_AND_GOOBJ.md`、`PLAN.md`、`roadmap-next.md`
