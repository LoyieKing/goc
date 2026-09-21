# P28 归档报告 — In-tree Clang Sema + real-body goobj

**日期：** 2026-09-22（Asia/Shanghai，构建跨 09-21 23:38 → 09-22 00:13）  
**合同：** `goc-syntax-guide.md` v0.2.1（无 dsptr；sptr 逃逸 = 硬错误）

## 成功标准

| # | 标准 | 状态 |
|---|------|------|
| 1 | Built `_deps/llvm-19.1.7-clang-build/bin/clang` | ✅ `clang-19` 146MB；`bin/clang` → symlink |
| 2 | In-tree Sema rejects sptr→heap without `-fplugin` | ✅ `PASS P28-sema-err` |
| 3 | `goc build` / `goc cc` 默认该 clang | ✅ `GOC_CLANG` / intree 优先；无 plugin |
| 4 | Product goobj 真实函数体（无 P21 seed） | ✅ magic `0x28C0DE42`；meta `encoding=clang-real-isel` |
| 5 | `goc test --p28` ALL PASS + 本报告 | ✅ |

## PASS 行（摘录）

```
PASS P28-sema-ok (in-tree clang, no -fplugin: 01_ok_outparam_stack)
PASS P28-sema-err (in-tree Sema rejects sptr→heap without -fplugin)
PASS P28-realbody-goobj (Clang .c → real ISel body → goobj; not seedMIR)
PASS P28-driver-clang (in-tree clang present for goc build/cc)
PASS p28-clang-intree (in-tree Sema + real-body goobj)
goc test --p28: ALL PASS
```

附：`goc test --p27` 仍 **ALL PASS**（plugin legacy）。

## 重建 clang

构建耗时：约 **35 分钟**（Release，`-j4`，clang-19 作宿主机 C/CXX；日志 `p28-clang-intree/build-clang.log`）。

```bash
export PATH="$GOC_ROOT/third_party/cmake/bin:$GOC_ROOT/third_party:$PATH"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  -S $LLVM_SRC/llvm \
  -B $GOC_ROOT/third_party/llvm-19.1.7-clang-build
ninja -C $GOC_ROOT/third_party/llvm-19.1.7-clang-build -j4 clang
```

**注意：** 勿用 GCC 作 `CMAKE_C_COMPILER`（会把 Clang 专用 `-Wcovered-switch-default` 传给 `cc` 导致失败）。勿复用仅含 `llvm-lit` 的 `_deps/llvm-19.1.7-build`。

## 测试

```bash
./bin/goc test --p28
./bin/goc build p28-clang-intree/tests/03_ok_realbody_goobj.c -o /tmp/x.o
./bin/goc cc -c -emit-llvm -S -o /tmp/x.ll p28-clang-intree/tests/01_ok_outparam_stack.c
```

## 非 seed 证明（诚实）

| 证据 | 内容 |
|------|------|
| meta.json | `"encoding": "clang-real-isel"`；`"not_source": "p21-color-vertical seed templates"`；**无** `attrs→seedMIR` |
| goobj TEXT | little-endian 立即数 `42 de c0 28` = `0x28C0DE42`（源码 `tests/03_ok_realbody_goobj.c`） |
| `go tool objdump` | `ADDL $0x28c0de42, AX` + `CALL p28_external_hook` |
| 产品路径 | `bin/goc build` 调 `goc_p28_realbody.sh`，**不**调 `seedMinimal` / `seedLiveAcrossCall` |

## 诚实表

| 声称 | 真相 |
|------|------|
| In-tree Sema（非 system+plugin 冒充） | `_deps/llvm-19.1.7-clang-build/bin/clang` + `SemaGocColors.cpp` |
| Real body（非 seed 冒充 ISel） | llc 自 Clang IR；magic 在 TEXT |
| 完整 Go ABIInternal / 任意 MF morestack+Spill+WB | **未完成 → P29** |
| 全量 QJS via goc | **未完成 → P29+** |
| AVX / x87 / EH | 仍不支持（P16 合同） |

## P29 缺口

1. 任意 Clang IR 上的 Go amd64 **ABIInternal**（形参/返回寄存器约定）
2. 在**真实 ISel MF** 上跑完整 Spill → Maps → StackCheck/morestack → WB（P15–P16 管道接到 arbitrary MF）
3. 复杂类型 / 多函数 TU / 跨文件 LTO 级 lowering
4. 全量 quickjs-ng 着色 + `goc build`

## 关键路径

| 路径 | 作用 |
|------|------|
| `_deps/.../Sema/SemaGocColors.cpp` | 实装：Goc*→AnnotateAttr + sptr 逃逸 |
| `_deps/.../Sema.cpp` / `Sema.h` / `CMakeLists.txt` | TU 末诊断 + 编译进 clangSema |
| `_deps/.../Attr.td` | GocCPtr/SPtr/UPtr/AutoPtr/GPtr |
| `p28-clang-intree/` | PLAN、tests、`pass/goc_p28_realbody.sh`、`build.sh` |
| `bin/goc`、`bin/goc-cc` | 优先 in-tree；`--p28`；无 plugin 默认 |
| `roadmap-next.md`、`glossary.md`、`p27-clang-frontend/PLAN.md` | 状态更新 |
