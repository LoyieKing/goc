# P20 归档报告 — 统一 `bin/goc` 驱动

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1  
**入口：** [`../bin/goc`](../bin/goc)（薄 shell，编排既有 `build.sh` / 工具）  
**前置：** P17 [`../p17-archive/P17-REPORT.md`](../p17-archive/P17-REPORT.md) · P18 [`../p18-archive/P18-REPORT.md`](../p18-archive/P18-REPORT.md) · P19 [`../p19-archive/P19-REPORT.md`](../p19-archive/P19-REPORT.md) · P8 demo [`../bin/goc-p8`](../bin/goc-p8)

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| 单一 `bin/goc`：`fe` / `bridge` / `uptr` / `test` | ✅ |
| `goc fe`：P17 clang → color-escape | ✅ |
| `goc bridge`：P18 color→maps/WB（`p18-color-bridge/build.sh`） | ✅ |
| `goc uptr`：P19 MSB runtime（`p19-uptr-runtime/build.sh`） | ✅ |
| `goc test` / `pipeline`：P17+P18+P19 聚合 PASS、exit 0 | ✅ |
| `goc demo`：保留 P8/P5 后端 smoke（经 `goc-p8`） | ✅ |
| 短 help / version | ✅ |
| 旧 `goc-fe` / `goc-p18` / `goc-p19` 作别名 | ✅ |
| 不破坏各 `p17|p18|p19/build.sh` | ✅（heredoc 改为写别名） |
| 诚实缺口文档化 | ✅ 见下 |

## 用法

```bash
# from goc-docs/
./bin/goc help
./bin/goc version

./bin/goc fe [--emit-ir out.ll] file.c     # P17 color-escape
./bin/goc bridge                           # P18 proofs
./bin/goc uptr                             # P19 proofs
./bin/goc test                             # P17 + P18 + P19 → aggregate PASS
./bin/goc demo                             # P8 backend smoke

# compat aliases
./bin/goc-fe …     # → goc fe …
./bin/goc-p18 …    # → goc bridge …
./bin/goc-p19 …    # → goc uptr …
./bin/goc-p8 demo  # legacy Go CLI
```

聚合日志：[`PASS-LINES.txt`](./PASS-LINES.txt)（由 `goc test` 写出）。

## 架构（薄包装，不重写 pass）

```text
bin/goc
  fe      → clang-19 -emit-llvm + p17-frontend/build/goc-color-escape
  bridge  → p18-color-bridge/build.sh
  uptr    → p19-uptr-runtime/build.sh
  test    → P17 build.sh → P18 build.sh → P19 build.sh → aggregate PASS
  demo    → bin/goc-p8 → p5-machinepass-goobj/build.sh
```

`build-p8.sh` 现安装 **`bin/goc-p8`**，不再覆盖统一 `bin/goc`。

## 诚实缺口（仍非本轮）

- 无完整 **color.ll → goobj** 无 MIR seed 的端到端产品管线（P18 停在 maps/WB recipe + seed MIR）。
- **QJS** 未接入。
- 生产 **`stack.hi` TLS / `getg()`** 路径未落地（P19 host 证明 + IR lower；运行时绑定仍演示级）。
- **过程间** escape / 色传播未做。
- 统一 CLI **不**编译任意 `.c` 为可链接 Go 目标；`fe` 只到 color IR。

## PASS 证据

`./bin/goc test` → exit 0（2026-09-21 22:28 CST）。完整行见 [`PASS-LINES.txt`](./PASS-LINES.txt)（约 81 条 `PASS`，含 P18 重跑的 P17）。摘录：

```text
PASS 01_ok_outparam_stack … PASS 07_err_return_sptr
PASS P18-bridge … PASS P18-cptr … PASS p18-color-bridge
PASS cptr-encode-MSB-clear … PASS p19-uptr-runtime
PASS P20-aggregate (P17+P18+P19; 81 PASS lines)
PASS P20-goc-test (exit 0)
goc test: ALL PASS (P17 + P18 + P19)
```

一键：`./bin/goc test` · 日志副本：[`goc-test.log`](./goc-test.log)
