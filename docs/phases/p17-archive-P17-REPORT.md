# P17 归档报告 — 前端第一刀：指针色 + 逃逸分析

**日期：** 2026-09-21（Asia/Shanghai）  
**合同：** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2.1（`cptr`/`sptr`/`uptr`/`auto_ptr`/`gptr`；无 `dsptr`；`JSValue` 显式 struct）  
**实现树：** [`../p17-frontend/`](../p17-frontend/)  
**后端（不动）：** [`../p5-machinepass-goobj/`](../p5-machinepass-goobj/) P0–P16 已完成

## 目标与完成度

| 成功标准 | 状态 |
|----------|------|
| `goc.h` + ≥5 golden（含 sptr→heap ERROR、uptr 显式编码 OK、auto 栈 struct OK、gptr 不混 cptr） | ✅ 7/7 |
| 诊断明文提及 color/escape 规则 | ✅ |
| IR 带 `!goc.color` / provenance 元数据 | ✅ |
| 一键 `./build.sh` exit 0 + PASS 行 | ✅ |
| 诚实缺口（P18+） | ✅ 见下 |

## 架构选型

**Clang 19 作解析器** + **独立 LLVM IR 驱动** `goc-color-escape`（与 P5 `goc-pass-driver` 同风格，链 `libLLVM-19`）。

未做完整 Clang fork / 自研 C11 解析。色表面用 `clang annotate`（非 addrspace：Clang 禁止把 AS 打在自动变量上）。

```text
.goc/.c + include/goc.h
    → clang-19 -emit-llvm -S -O0
    → goc-color-escape (seed annotate + provenance + escape/mix checks)
    → *.color.ll  (!goc.color / !goc.prov / !goc.uptr_encoded)
```

驱动：`bin/goc-fe`。

## 源色与 IR 表示

### 源（`include/goc.h`）

```c
cptr(T) / sptr(T) / uptr(T) / auto_ptr(T) / gptr(T)
  → T * __attribute__((annotate("goc.color.<name>")))
T *                         → auto_ptr（推断）
goc_uptr_from_sptr/cptr …   → 具名内建（pass 识别）
JSValue { int tagged_value; cptr(JSObject) pointer; }
```

### IR

| 机制 | 用途 |
|------|------|
| `llvm.var.annotation` / `llvm.ptr.annotation` / `llvm.global.annotations` | 携带 `goc.color.*` |
| `!goc.color !{!"sptr"\|"cptr"\|"uptr"\|"gptr"\|"auto"}` | 精化后注解 |
| `!goc.prov !{!"stack"\|"cheap"\|"goheap"}` | 来源 |
| `!goc.uptr_encoded` | 经 `goc_uptr_from_*` |
| module flag `goc.color.schema` | `annotate+!goc.color;v0.2.1-P17` |

详见 [`../p17-frontend/docs/IR-COLOR.md`](../p17-frontend/docs/IR-COLOR.md)。

## 逃逸 / 精化规则（已实现）

1. alloca / 栈来源 → 可精化为 `sptr`（若指针字不入库）。
2. 存入全局 / 非栈位置的 **绝对栈指针 / 已钉 `sptr`** → **error**（禁止自动升 `uptr`）。
3. 显式 `goc_uptr_from_sptr` 后再写入 `uptr` 字段 → OK。
4. 仅活在栈上的 struct 实例的 `auto_ptr` 字段可持有 `&local` → OK（不强制整类型升 `uptr`）。
5. `gptr` 与 `cptr`/`sptr`/`uptr`/`auto` 存储互转 → **error**。
6. `return` 栈指针 → **error**。

## Golden 结果

```text
P17 golden results (2026-09-21 22:14 CST)
PASS 01_ok_outparam_stack
PASS 02_ok_uptr_encode_heap
PASS 03_err_sptr_to_heap (expected error: matched)
PASS 04_ok_auto_stack_struct
PASS 05_err_gptr_mix_cptr (expected error: matched)
PASS 06_ok_gptr_only
PASS 07_err_return_sptr (expected error: matched)
```

一键：`./p17-frontend/build.sh`

诊断样例：

```text
goc-color-escape: error: sptr escape: stack pointer color must not be stored into a
heap/global field; encode with goc_uptr_from_sptr or declare the value as
auto_ptr/uptr before escape (no automatic promote to uptr)
```

## 如何运行

```bash
cd $GOC_ROOT/p17-frontend && ./build.sh
../bin/goc-fe --emit-ir /tmp/out.color.ll tests/02_ok_uptr_encode_heap.c
```

## 缺口 / P18+（诚实）

| 项 | 说明 |
|----|------|
| 色 IR → stackmap / WB | 后端尚未自动消费 `!goc.color`；需接到 P5–P16 maps/WB |
| 更完整 C / interprocedural | 形参色模板联锁、间接调用、跨 TU 摘要仍粗 |
| uptr 真编码 | 前端只认 builtin 名；MSB=`g.stack.hi` 运行时编码仍在后端/runtime |
| QJS 移植 | 明确 OUT OF SCOPE |
| Clang 插件/自定义类型 | 仍靠 annotate 宏；非一等类型系统 |
| `.goc` 预处理 | `goc-fe` 把 `.goc` 当 C + `goc.h`；无独立词法 |
| 与 `bin/goc demo` | P8 CLI 仍走后端 smoke；前端为 `goc-fe` 并行入口 |

## 触及文件

- `p17-frontend/**`（新）
- `bin/goc-fe`（新）
- `p17-archive/P17-REPORT.md`（本文件）
- `roadmap-next.md` / `p5-machinepass-goobj/PLAN.md`（标注前端已开工）

**结论：P17 前端第一刀已交付**（色表面 + 逃逸拒绝 + 注解 IR + 一键测）。
