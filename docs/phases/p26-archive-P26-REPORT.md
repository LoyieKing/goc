# P26 归档报告 — 链接真实 quickjs-ng

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** goc-facing `JSValue` 为显式 struct（非 NaN-box）；指针字可日后着 `cptr`  
**实现树：** [`../p26-qjs-real/`](../p26-qjs-real/) · 上游 [`../quickjs-ng/`](../quickjs-ng/)  
**入口：** `./bin/goc qjs-ng` · `./bin/goc qjs --real` · `./bin/goc test --p26` · `./p26-qjs-real/build.sh`

> **诚实声明：这是「真链 quickjs.c + JS_NAN_BOXING=0」，不是完整 goc 着色 / Go 堆托管的 QuickJS。**  
> P23 stub 路径保留：`goc qjs` / `goc test --p23` 仍绿。

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| 用 CMake 构建真实 `libqjs`（`quickjs.c` 等） | ✅ `p26-qjs-real/build/qjs-static/libqjs.a` |
| `JS_NAN_BOXING=0` → 显式 `struct JSValue { union; tag }` | ✅ flags.make + layout probe `sizeof=16` |
| `JS_NewRuntime` / `JS_NewContext` / `JS_Eval("1+1")` | ✅ `p26_demo` → 2 |
| nm 证明来自真实 `quickjs.c`，非 P23 stub | ✅ `quickjs.c.o` ~1.4MB；`JS_Eval` in binary；无 `goc_qjs_go_eval` |
| stub 路径保留 | ✅ `p23-qjs-slice/` 不动；`goc qjs` 仍走 stub |
| `bin/goc test --p26` PASS | ✅ |
| 上游循环 safepoint | ❌ **跳过（gap）** |
| 全量内部着色 / Go 堆分配器 | ❌ **不做（gap）** |

## 布局（amd64，`JS_NAN_BOXING=0`）

```text
sizeof(JSValue)     = 16
offsetof(u)         = 0
offsetof(u.ptr)     = 0
offsetof(tag)       = 8
```

上游默认在 64-bit 已是 undef NAN_BOXING；P26 **显式** `-DJS_NAN_BOXING=0` 锁布局。  
与 P23 `goc_qjs.h`（tag 在前）字段序不同，但同为「可分离指针字」的显式 struct——满足 goc 合同意图；ABI 统一留给后续。

详见 [`../p26-qjs-real/docs/LAYOUT.md`](../p26-qjs-real/docs/LAYOUT.md)。

## 与 goc cptr 着色的关系

| | P26 |
|--|-----|
| 指针在独立机器字（`u.ptr`），日后可着 `cptr` | ✅ 布局前提 |
| 对 `quickjs.c` 跑 color-escape / 全量内部着色 | ❌ **本轮不要求、未做** |
| 堆字段禁裸 `sptr` 审计 | ❌ |
| `JSMallocFunctions` → Go 堆 / arena | ❌ 仍 libc malloc |

## 用法

```bash
./p26-qjs-real/build.sh
./bin/goc qjs-ng
./bin/goc qjs --real
./bin/goc test --p26
# stub 仍可用：
./bin/goc qjs
./bin/goc test --p23
```

## PASS 证据（摘要）

```text
PASS P26-libqjs-build (REAL quickjs.c, JS_NAN_BOXING=0)
PASS layout-explicit-struct
PASS P26-layout-probe
PASS P26-demo-link-real-symbols
PASS nan-boxing-off-explicit-struct
PASS jsvalue-struct-layout
PASS new-runtime
PASS new-context
PASS eval-1plus1
PASS new-object
PASS P26-host-demo (REAL JS_Eval 1+1)
PASS P26-stub-path-preserved (p23 tree intact)
PASS p26-qjs-real (REAL quickjs-ng linked — NOT full goc-colored QJS)
```

对比体积：`p23_demo` ~32KB · `p26_demo` ~1.3MB（内嵌真实引擎）。  
nm：[`nm-libqjs-symbols.txt`](./nm-libqjs-symbols.txt) · 一键 PASS：[`PASS-LINES.txt`](./PASS-LINES.txt)

## 布局树

```text
p26-qjs-real/
  build.sh
  demo/demo_main.c          REAL JS_Eval("1+1")
  demo/layout_probe.c       sizeof/offsetof 证明
  docs/LAYOUT.md            NAN_BOXING ↔ cptr
  docs/GAPS.md              诚实缺口
  build/qjs-static/libqjs.a CMake 产物
  build/p26_demo
```

## 诚实缺口（见 docs/GAPS.md）

| 缺口 | 说明 |
|------|------|
| **Go-framed morestack 深入 QJS C 帧** | 上游解释器无 stackmap/poll；搬栈不安全 |
| **全量着色 QJS 内部** | 未跑 color-escape 于 `quickjs.c` |
| **生产分配器上 Go 堆** | 仍默认 malloc |
| **上游循环 safepoint** | P26 可选 stretch — **已跳过** |
| **P23 `goc_qjs.h` 字段序统一** | tag-first stub vs union-first upstream |
| **CGO_ENABLED=0 全量 libqjs 进 Go** | 本轮仅 host demo |

**禁止宣称：**「完整 goc 着色的 QuickJS」或「QJS 已在 Go 栈上可 GC 抢占」。

## 触及文件

**新建**
- `p26-qjs-real/**`
- `p26-archive/P26-REPORT.md` · `PASS-LINES.txt` · `nm-libqjs-symbols.txt`

**修改**
- `bin/goc` — `qjs-ng` / `qjs --real` / `test --p26`；aggregate 含 P26
- `roadmap-next.md`
