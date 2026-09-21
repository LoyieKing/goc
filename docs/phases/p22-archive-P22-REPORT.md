# P22 归档报告 — 生产态 g→stack.hi TLS 供 uptr

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1 §3.3  
**实现树：** [`../p22-uptr-tls/`](../p22-uptr-tls/) · runtime 扩展 [`../p19-uptr-runtime/`](../p19-uptr-runtime/)  
**入口：** `./bin/goc uptr --tls` · `./bin/goc test --p22` · `./p22-uptr-tls/build.sh`

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| `goc_uptr_from_sptr` / `as_sptr`（默认）读当前 g `stack.hi`（TLS） | ✅ `GOC_UPTR_HAVE_TLS` inline `FS:-8` |
| 证明：encode 用 live hi；栈增长 / hi 变化后同字 decode | ✅ **真实 stack growth** `stack-grew-same-enc-new-hi` |
| 接入 `bin/goc uptr` / `goc test`；不破坏 P19 host `*_hi` | ✅ |
| 归档 + roadmap；诚实缺口 | ✅ |
| 复用 P1/P5 TLS 约定（非第二套） | ✅ |
| 无 dsptr；无 sptr auto-promote | ✅ |

## TLS 约定（与 P1/P5 相同）

```text
FS:-8 → g
g.stack.lo @ 0
g.stack.hi @ 8
g.stackguard0 @ 16
```

Go 内部链接对 clang `.syso` 的 PLT/GOT/.bss reloc 不可靠 → P22 在
`GOC_UPTR_HAVE_TLS` 下 **inline** `%%fs:-8`（见 `docs/TLS-HI.md`）。

## 用法

```bash
./p22-uptr-tls/build.sh          # TLS harness + P19 regress
./bin/goc uptr                   # P19 host MSB（不变）
./bin/goc uptr --tls             # P22 TLS harness
./bin/goc test --p22             # P22 only
./bin/goc test                   # P17–P19 + P21 + P22
```

## PASS 证据

```text
P22 results (2026-09-21 Asia/Shanghai)
PASS tls-hi-read
PASS tls-hi-agree-go-asm
PASS tls-local-below-hi
PASS live-tls-from_sptr-MSB
PASS live-tls-as_sptr-roundtrip
PASS live-enc-eq-explicit-hi
PASS stack-grew-same-enc-new-hi
PASS p22-uptr-tls (production g->stack.hi TLS)
PASS P22-syso (inline TLS + freestanding uptr)
PASS P22-harness (TLS hi + encode/decode)
PASS P22-p19-regress (host *_hi unchanged)
```

一键产物：`p22-uptr-tls/build/p22_uptr_tls` · [`PASS-LINES.txt`](./PASS-LINES.txt)

## 触及文件

**新建**
- `p22-uptr-tls/**`（harness / build / docs）
- `p19-uptr-runtime/runtime/goc_uptr_tls_amd64.S`（文档叶）
- `p22-archive/P22-REPORT.md` · `PASS-LINES.txt`

**修改**
- `p19-uptr-runtime/runtime/goc_uptr_runtime.c` — `GOC_UPTR_HAVE_TLS` inline TLS
- `p19-uptr-runtime/include/goc_uptr.h` · `docs/UPTR-MSB.md` · `README.md`
- `bin/goc` — `uptr --tls` / `test --p22`
- `roadmap-next.md`

## 诚实缺口

| 项 | 说明 |
|----|------|
| QJS | 未接；仍 OUT OF SCOPE |
| Interprocedural color/escape | 未做 |
| 通用 SelectionDAG / 任意 C→goobj | P21 仅为证明切片 |
| Cross-g FATAL | 仍为引擎纪律；非 runtime 自动检测 |
| Host 默认库无 TLS | `libgoc_uptr.a` 仍为 host 形态；生产需 `HAVE_TLS` 重编进 Go |
| 非 amd64 / 非 linux Go TLS | 未做 |

**结论：P22 已交付** — 生产态 `g->stack.hi` TLS 驱动默认 uptr encode/decode，并在真实栈搬迁下证明同字不变式；P19 host `*_hi` 回归全绿。
