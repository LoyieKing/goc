# P19 归档报告 — uptr MSB 运行时编码

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1 §3.3  
**实现树：** [`../p19-uptr-runtime/`](../p19-uptr-runtime/)  
**前端：** [`../p17-frontend/`](../p17-frontend/)（`*_hi` 识别；stubs 标明 IR-only）  
**桥：** [`../p18-color-bridge/`](../p18-color-bridge/)（`goc-color-uptr-encoded` 透传；消费说明见 UPTR-MSB）

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| Runtime helpers：`goc_uptr_from_sptr/cptr`、`as_sptr/cptr`（+ `*_hi`） | ✅ `libgoc_uptr.a` |
| MSB 协议相对 owner `stack.hi`；wrong-tag FATAL | ✅ |
| 前端 builtin 非 no-op：`goc-uptr-lower` 产出编码 IR | ✅ |
| cptr MSB clear round-trip | ✅ |
| sptr MSB set；`hi+enc==abs`；stack-move 同字 | ✅ |
| P17 裸 sptr→heap 仍错；无 auto-promote；无 dsptr | ✅ |
| P18 消费说明（轻量） | ✅ `docs/UPTR-MSB.md` |
| 一键 + 归档 + roadmap/PLAN | ✅ |

## 协议（amd64 user VA）

```text
MSB=0  → absolute cptr（字即地址）
MSB=1  → int64 补码 offset；abs = (uintptr_t)((int64_t)hi + (int64_t)stored)
栈向下长 → abs < hi → offset 为负 → MSB 自然为 1
搬栈：堆上 MSB=1 字不变；hi'=hi+δ 时同一 enc 仍得 abs'
```

## 布局

| 路径 | 角色 |
|------|------|
| `include/goc_uptr.h` | API |
| `runtime/goc_uptr_runtime.c` | 真编码 + `goc_uptr_fatal` |
| `pass/GocUptrLower.cpp` | LLVM 19 IR lower |
| `tests/test_msb_runtime.c` | host 证明 |
| `docs/UPTR-MSB.md` | 协议 + P18 消费注意 |
| `bin/goc-p19` | 一键驱动 |

## PASS 证据

```text
P19 results (2026-09-21 22:25 CST)
PASS cptr-encode-MSB-clear
PASS cptr-encode-not-stack-tag
PASS cptr-encode-word-eq-abs
PASS cptr-roundtrip
PASS sptr-encode-MSB-set
PASS sptr-encode-MSB-bit
PASS sptr-hi-plus-enc-eq-abs
PASS sptr-roundtrip
PASS stack-move-same-enc-yields-abs-prime
PASS stack-move-as_sptr-hi-prime
PASS current-g-from_sptr-MSB
PASS current-g-as_sptr-roundtrip
PASS wrong-tag-as_cptr-on-sptr-FATAL
PASS wrong-tag-as_sptr-on-cptr-FATAL
PASS from_cptr-rejects-MSB-FATAL
PASS P19-runtime (cptr/sptr/stack-move/wrong-tag)
PASS P19-ir-lower (sub/add/MSB mask / !goc.uptr_encoded)
PASS P19-c-to-lower (clang→color→MSB IR)
PASS P19-p17-sptr-escape (still compile error; no auto-promote)
PASS P19-p17-uptr-encode (color OK + MSB lower)
PASS p19-uptr-runtime (MSB encode/decode)
```

一键：`./p19-uptr-runtime/build.sh` 或 `./bin/goc-p19`。

## IR 证据（非注释）

`goc-uptr-lower` 将 `@goc_uptr_from_sptr_hi` 等替换为：

```llvm
%goc.uptr.off = sub i64 %goc.uptr.p2i, %hi
%goc.uptr.msb = and i64 %goc.uptr.off, -9223372036854775808  ; 1<<63
; icmp + br → goc_uptr_fatal / cont
%goc.uptr.i2p = inttoptr i64 %goc.uptr.off to ptr, !goc.uptr_encoded !1
; decode:
%goc.uptr.abs = add i64 %hi, %goc.uptr.p2i
```

完整样例：`p19-uptr-runtime/build/test-out/uptr_builtins.lowered.ll`。

## 触及文件

**新建**
- `p19-uptr-runtime/**`
- `bin/goc-p19`
- `p19-archive/P19-REPORT.md`（本文件）· `PASS-LINES.txt`

**修改**
- `p17-frontend/include/goc.h` — `*_hi` 声明 + P19 指针
- `p17-frontend/include/goc_stubs.c` — 标明 identity 仅 IR-only；真实现见 P19
- `p17-frontend/pass/GocColorEscape.cpp` — 识别 `*_hi`
- `p17-frontend/docs/IR-COLOR.md` · `p18-color-bridge/docs/DATAFLOW.md`
- `roadmap-next.md` · `p5-machinepass-goobj/PLAN.md`

## 诚实缺口

| 项 | 说明 |
|----|------|
| 统一 `bin/goc` | `goc-fe` / `goc-p18` / `goc-p19` 仍并行 |
| color.ll→完整 goobj | 未接 MIR seed / elfpack 垂直 harness |
| 生产 `g->stack.hi` | **P22 已交付**（TLS FS:-8）；本归档时为 test inject / approx |
| Interprocedural / QJS | OUT OF SCOPE |
| P18 全量回归在 P19 一键内 | 仅 P17 逃逸 + uptr encode；完整 `goc-p18` 未强制重跑（加性、未改 bridge） |
| dsptr | 无（合同禁止） |

**结论：P19 已交付** — uptr MSB 在 runtime 与 IR 两层可证明；栈搬迁不变式与 wrong-tag FATAL 覆盖合同核心。
