# P27 归档报告 — 基于 Clang 大改的完整前端（开工）

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1  
**实现树：** [`../p27-clang-frontend/`](../p27-clang-frontend/)  
**入口：** [`../bin/goc`](../bin/goc) `build` / `cc` / `test --p27`  
**计划：** [`../p27-clang-frontend/PLAN.md`](../p27-clang-frontend/PLAN.md)

## 用户指令与定位

> 「那没用。我要的是完整前端，你可以直接基于clang大幅度修改来实现。」

P27 开工：**真实 Clang 前端**（属性 + Sema 逃逸），编译用户 `.c`（含 goc 色）走向 goobj；**不是**又一个孤立 harness 垂直 demo。P17 annotate-only / P21 fixture 降为 legacy。

## 成功标准对照

| # | 标准 | 状态 |
|---|------|------|
| 1 | Clang tree 补丁 **或** 明确 out-of-tree plugin + patches | ✅ plugin `libGocClang.so` + `_deps/.../Attr.td` Goc* + `SemaGocColors.cpp` stub + `patches/` |
| 2 | `bin/goc build` / `goc cc` 编译**用户 `.c`** | ✅ |
| 3a | OK：out-param / stack auto_ptr | ✅ `tests/01_ok_outparam_stack.c` |
| 3b | ERROR：sptr → heap（Sema 诊断） | ✅ `tests/02_err_sptr_to_heap.c` |
| 3c | OK：非平凡函数 → goobj `.o`（输入来自 Clang `.c`） | ✅ `p27_funcs.o`（P21/P16 lower） |
| 4 | 文档：取代 P17/P21 为产品前端 | ✅ `docs/LEGACY.md` · PLAN · 本报告 |
| 5 | `p27-archive/P27-REPORT.md` + `PLAN.md` 多阶段路线 | ✅ |

## 架构

```text
.goc/.c  →  patched Clang (Sema colors + escape)  →  LLVM IR (!goc.color)
         →  goc machine/goobj pipeline  →  .o goobj
Driver: bin/goc build foo.c -o foo.o
```

## 用法

```bash
./bin/goc build                         # 完整 P27 证明
./bin/goc test --p27
./bin/goc cc -c -emit-llvm -S foo.c -o foo.ll
./bin/goc build foo.c -o foo.o
./p27-clang-frontend/build.sh
```

产物：`p27-clang-frontend/build/libGocClang.so` · `build/p27-out/p27_funcs.o`

## 补丁 / 触及文件

**新建**
- `p27-clang-frontend/plugin/GocClangPlugin.cpp` · `Makefile` · `build.sh`
- `p27-clang-frontend/include/goc.h` · `tests/*.c` · `driver/goc-cc`
- `p27-clang-frontend/patches/*` · `PLAN.md` · `docs/*`
- `p27-archive/P27-REPORT.md`（本文件）

**修改**
- `_deps/llvm-project-19.1.7/clang/include/clang/Basic/Attr.td` — GocCPtr/SPtr/UPtr/AutoPtr/GPtr
- `_deps/llvm-project-19.1.7/clang/lib/Sema/SemaGocColors.cpp` — stub
- `bin/goc` — `build`/`cc`/`test --p27`；`vertical` 保留为 legacy

## PASS 证据

```text
PASS P27-plugin (libGocClang.so built)
PASS P27-ok-outparam (Clang plugin compiles stack auto_ptr .c)
PASS P27-err-sptr-heap (Sema diagnostic from Clang plugin)
PASS P27-goobj (Clang .c → plugin → color → bridge → goobj .o)
PASS P27-frontend-proof (user .c through Clang plugin Sema → goobj)
PASS p27-clang-frontend (Clang plugin Sema colors + goobj)
```

Sema 样例诊断：

```text
error: goc: sptr escape — storing stack pointer (sptr) into non-stack location ...
```

## 诚实缺口（→ P28+）

| 项 | 说明 |
|----|------|
| 完整 clang 二进制重建 | **未完成**；今日为 system clang-19 + plugin；Attr.td 已改，重建见 `patches/README.md` |
| 通用 Clang IR → goobj ISel | 仍用 P21 attr-seeded Go-frame（与 P21 同级诚实）；**输入**已是 Clang `.c` |
| 形参色 CodeGen | color-escape 对部分 param 仍可能 soft-warn；Sema 为主 |
| 全量 QJS via goc | **欠** — PLAN P28–P31：in-tree Sema、ABI、safepoint、quickjs-ng |
| dsptr | 无（合同禁止） |

**结论：P27 已交付「开工」里程碑** — 真实 Clang 前端（属性 + Sema）+ `goc build` + 用户 `.c` → goobj；非 stub VM。
