# 03 前端与 IR pass

[← 02 走一遍](02-walkthrough.md) · [索引](../engineering-architecture.md) · [下一篇：goc-llc →](04-goc-llc.md)

## 1. 前端：颜色怎么进 IR

### 1.1 语法表面：`include/goc.h`

```c
#define cptr(T) T *GOC_ANN_CPTR
#define sptr(T) T *GOC_ANN_SPTR
#define uptr(T) T *GOC_ANN_UPTR
#define auto_ptr(T) T *GOC_ANN_AUTO
#define gptr(T) T *GOC_ANN_GPTR
```

`GOC_ANN_*` 有两种展开：
- 定义了 `GOC_USE_INTREE_ATTRS`（打补丁的 clang，驱动默认加）或 `GOC_USE_PLUGIN_ATTRS`（树外插件）→ 原生属性 `__attribute__((goc_cptr))` 等。
- 都没定义 → `__attribute__((annotate("goc.color.cptr")))`，IR pass 也能读。

没写颜色的裸 `T *` 等于 `auto_ptr`，由 IR pass 推断；`--default-ptr-color` 给推断不出来的指针一个默认色（产品默认 `cptr`）。

### 1.2 打补丁的 Clang 19.1.7（`clang/`）

`scripts/build-clang.sh` 下载 LLVM 19.1.7，`scripts/apply-patches.sh` 打补丁，产物在 `third_party/llvm-19.1.7-clang-build`：

| 补丁 / 文件 | 改了什么 |
|---|---|
| `clang/patches/0001-Attr.td-goc-colors.patch` | `Attr.td` 加 `GocCPtr/SPtr/UPtr/AutoPtr/GPtr` 五个属性 |
| `clang/sema/SemaGocColors.cpp` | 复制到 `lib/Sema/`：(1) 把 `Goc*Attr` 镜像成 `AnnotateAttr("goc.color.*")`，CodeGen 因此产出 `llvm.var.annotation` / `llvm.global.annotations`；(2) 翻译单元结束时做 AST 级逃逸检查 |
| `clang/patches/0003-mirror-goc-colors-before-codegen.patch` | `ParseAST.cpp`：在 `HandleTopLevelDecl` 前镜像（CodeGen 是增量的，等到 TU 结束就晚了） |
| `clang/patches/0004-wire-sema-goc-colors.patch` | `CMakeLists.txt`、`Sema.h`、`Sema.cpp` 的接线 |
| `clang/plugin/GocClangPlugin.cpp` | 同一套规则的树外插件（`goc test --plugin`），不是产品路径 |

所以 **颜色进 IR 的载体是 annotate**（02 篇 §5 有真实输出），之后 `goc-color-escape` 把它们读出来变成 metadata。

## 2. `libGocPasses.so`：注册了什么

`llvm/lib/Plugin/GocPasses.cpp` 在 `registerPipelineParsingCallback` 里注册：

| 层级 | 名字 | 实现 |
|---|---|---|
| module | `goc-color-escape` | `GocColorEscapePass` → `goc::runColorEscape(M, nullptr)`，有错误则 `report_fatal_error` |
| module | `print<goc-color>` | `GocColorPrinterPass`：在**克隆的模块**上跑 `runColorEscape(*Clone, &errs())`，只打印、不改 IR |
| function | `goc-stackmap` `goc-reanchor` `goc-pin-i64` `goc-name-blocks` `goc-inline-gate` | `goc::parseStackMapPass`（`llvm/lib/Transforms/StackMap/GocStackMap.cpp`） |

`cmd/goc` 的调用顺序（`-O>0`）：

```
goc-color-escape            （GOC_COLOR_REPORT=1 时：print<goc-color>,goc-color-escape）
goc-inline-gate
default<O3> 的文本管线去掉 ,argpromotion 和 ,globalopt  +  function(goc-name-blocks)
goc-stackmap
goc-reanchor
```

`goc-pin-i64` 已注册但**不在默认管线里**（注释：给每个 i64 半字钉槽在 `JS_CallInternal` 里多出 137 个槽）。

## 3. 颜色与 metadata 约定

| metadata | 挂在哪 | 含义 | 谁写 / 谁读 |
|---|---|---|---|
| `!goc.color !{!"sptr"\|"cptr"\|"uptr"…}` | 指令、alloca | 这个值的颜色 | color-escape 写；stackmap 读（`isSptrColored`） |
| `!goc.prov !{!"stack"\|"cheap"…}` | 同上 | 来源 | color-escape 写 |
| `!goc.uptr_encoded` | uptr 编码调用 | 这是 pass 插入的编码 | color-escape |
| `!goc.root !{!"spill"\|"anchor"\|"arganchor"\|"pin"}` | pass 新建的 alloca | 槽的角色 | stackmap / reanchor / pin-i64 写，goc-llc 读 |
| `!goc.slot !{i64 N}` + 函数上的 `!goc.nextslot` | alloca | 函数内唯一槽号 | 同上 |
| `!goc.frame.words !{!"goc.frame.words", i64 N, i64 off…}` | 写入指令或 alloca | 槽 N 在这些偏移上的 8 字节字是指针 | reanchor / stackmap 写；goc-llc 的 `GocObjTables` 读成 `sptr_slots` |

`llvm/include/Goc/SlotMD.h` 的原则：**pass 之间靠 metadata 认槽，不靠 IR 值名**，因为 Release 版 clang 会丢弃值名。
（例外：`GocX86FrameAddrFix.cpp` 仍按名字找 `goc.oldfp`，见 07 篇。）

## 4. `goc-color-escape`（module pass）

位置：`llvm/lib/Transforms/ColorEscape/GocColorEscape.cpp`（入口）、`CECheck.cpp`、`CERewrite.cpp`；分析在 `llvm/lib/Analysis/GocColor/ColorAnalysis.cpp`。
入口 `goc::runColorEscape` 的步骤就是代码顺序：

1. 预先注册 metadata kind（`goc.color` `goc.prov` `goc.uptr_encoded`），保证输出 IR 确定。
2. `lowerDynamicAllocas`：动态 `alloca` → `goc_dynalloc` / `goc_dynrelease`（Go 的 pcsp 表不能描述函数中间 SP 变化）。`--fast-stack-alloca` 时之后由 `goc-inline-gate` 内联池分配。
3. 播种：`seedAnnotations`（读 annotate）、`seedProducers`（alloca → sptr 等）、`proveStackOnlyFormals`、`applyDefaultPtrColor`。
4. 每个函数 `propagate`。
5. 循环直到不动点：`encodeEscapingSptrStores`（sptr 存进非栈 cptr 槽 → 插 `goc_uptr_from_ptr`）、`decodeAutoUptrLoads`（从这种槽读 → 插解码），再 `propagate`。
6. `guardDynamicUptrReturns`。
7. `checkEscapesAndMix`：报错，例如 `gptr` 混入 `cptr/sptr/uptr`、raw `sptr` 返回值（`CECheck.cpp`）。
8. `attachMetadata`；有 `Report` 时 `emitColorReport`（`ColorPrinter.cpp`）。

- **输入**：clang 的未优化 IR（必须在 SROA / 内联之前）。
- **输出**：带 `!goc.color` 的 IR，加上 uptr 编解码调用；stderr 一行 `goc-color-escape: summary: N error(s), …`。
- **不变量**：错误数 > 0 时编译失败；`print<goc-color>` 不改变着色结果；输出 IR 字节确定（CI 有 `test/e2e/repro`）。
- **flag**：`-default-ptr-color=cptr|sptr|uptr|gptr`。

## 5. `goc-inline-gate`（function pass，O3 前）

`SMUptrInline.cpp`。goc 没有运行时库：`goc_stack_hi` / `goc_stack_lo`、`goc_uptr_*`（含 `_hi` 变体）和 `goc_dynalloc` / `goc_dynrelease` 的实现都在这里。
这个 pass（`rewriteUptrCall`）把对它们的调用改成调用模块内新建的 `goc.inline.*` 帮助函数（`inlineStackBound`、`inlineUptrDecode`、`inlineUptrFromPtr`、`inlineUptrRequireCptr`、`inlineUptrConv`、`inlineDynAlloc*`），带 `alwaysinline` 函数体，O3 再内联掉；`-O0` 下也跑，留下的是模块内的 internal 函数。错 tag 调 `goc.uptr.fatal`（`write(2)` 一行消息再 trap）。见 `docs/design/uptr.md`。读 `g` 用 `tlsGWord`：amd64 是 `movq %fs:-8`，arm64 是 `mov x28`，标成 `readnone` 以便后面合并；
`stack.lo` / `stack.hi` 的加载保持 `volatile`（它们只在调用里变，但 O3 证明为 readonly 的调用里也可能搬栈）。
`GOC_INLINE_DYNALLOC=1` 时 alloca 走每个对象一个的 16 MiB BSS 池（内联指针碰撞，单 goroutine 限定）；否则每次 `malloc` / `free`。
名字是历史遗留：它不再限制内联（`cmd/goc` 注释写作「goc-inline-gate is a no-op: call-bearing functions may inline」，指的就是这一点），含调用的函数照样可以被内联。

## 6. O3 + `goc-name-blocks`

`cmd/goc` 用 `opt -passes=default<O3> -print-pipeline-passes` 取出文本管线，再删掉 `,argpromotion` 和 `,globalopt`：
这两个会把内部函数改成 fastcc、减少参数，栈图按 SysV 出参区建模，改了就对不上。
`goc-name-blocks` 给所有没名字的、被取地址的基本块命名：LLVM 19 的 `.ll` 读取器会把同一张表里一个编号块和一个命名块合并（QuickJS 的 computed goto 表会触发）。
`GOC_OPT_EXTRA` 追加 opt 参数（文本管线不带 `default<O3>` 的内联阈值 250，见 `docs/perf-gap.md`）。

**关键约束**：内联必须发生在栈图之前。栈图记录在合并后的帧上；栈图之后再内联，调用者没法重定位被调用者已经发出的帧偏移和 stackmap ID。

## 7. `goc-stackmap`（function pass）

`SMStackMap.cpp`，共享帮助在 `SMRoots.cpp` 和 `llvm/lib/Analysis/StackMap/SafepointAnalysis.cpp`。

做什么：
1. `hoistTlsG`：一个函数里多次读 `g` 合并成入口一次。
2. `stackAddresses`：找出栈派生的值（alloca、alloca 的 GEP、从存着这些地址的槽里 load 出来的值），以及「调用者传进来的实参是 alloca」的形参。
3. 只看**指针类型**的非 alloca 值：带 sptr 颜色、或栈派生。整型不报（槽里放的小整数会让运行时报 `bad pointer in frame`）。
4. 对跨 safepoint 存活的这种值（`CrossCallLiveness`：后向传播，不能只看支配关系），建 `goc.spill.root` 槽，定义处 volatile 存，每个 safepoint 前插 `llvm.experimental.stackmap(ID, 0, 槽地址…)`，之后 volatile 重读。
5. 帧地址（alloca + 常量偏移）不进根槽，而是在 safepoint 之后 `rematFrameAddress` 重新计算成 GEP（最后由 goc-llc 的帧地址修复兜底，见 04 篇）。

- **输入**：O3 之后的 IR。
- **输出**：根槽 + stackmap 调用；stackmap ID 高 32 位是 `GocStackMapTag = 0x474f430000000000`（ASCII "GOC"）。
- **不变量**：stackmap 操作数是**槽地址**（LLVM Direct location），不是 SSA 值——Go 的搬栈只能改内存里的字，改不了寄存器；死值不报（复用的溢出槽此时可能放着整数）；之后不允许再内联。
- safepoint：`isSafepoint`；会被 ISel 展开成指令的 intrinsic（如 `llvm.umul.with.overflow`）也可能被当 safepoint，多余的记录由 goc-llc 的 `GocStackmapPlacement` 删掉。

## 8. `goc-reanchor`（function pass）

`SMReanchor.cpp`。处理 `goc-stackmap` 没覆盖的栈地址：

- **指针实参**：建 `goc.arganchor`，入口存、stackmap 记、safepoint 后重读（02 篇 §3.2 的例子）。即使本 TU 证明不了实参是栈地址也照做——调用者可能传自己帧里的地址（例如 goroutine 栈上的 Go slice）。堆指针放在被标记的槽里没关系：Go 只改落在旧栈范围内的值。
- **存着 `&s->token` 这类地址的局部槽**：打 `!goc.frame.words` 标签（`goc.anchor`），让搬栈改写槽里存的指针。
- **`goc.oldfp`**：给每个函数建一个 i64 槽，goc-llc 的 `GocX86FrameAddrFix` 在调用前把 RBP 存进去，调用后用 `RBP - goc.oldfp` 算搬动距离。它不在任何栈图里，所以搬栈时原样复制。
- `unmergeReportedAllocas`、`addOldFramePointerSlot` 总是运行（`asm` 帧地址模式已删除）。
- 每段没有 safepoint 的区间只重读一次，同段其它使用留在寄存器。
- 根槽的 lifetime 标记被去掉，避免 `StackColoring` 把根槽和标量槽合并。
- `GOC_CSR_ADJUST=1`（仅 x86）：依赖 SysV morestack 冷块改写保存的 RBX/R12–R15，指针实参跨调用时可以留在寄存器，省掉重读。

## 9. `goc-pin-i64`（未启用）

`SMPinI64.cpp`：把携带栈地址的整数钉进 `goc.pin` 槽。不在默认管线。

## 10. `-O0` 路径的差别

`cmd/goc` 在 `-O0` 时只跑 `goc-color-escape`，**不跑** inline-gate / O3 / goc-stackmap / goc-reanchor，`GOC_STACKMAP_PREPARED=0`；
`realbody.sh` 看到没准备好，会自己补跑一次 `goc-stackmap`（但不跑 `goc-reanchor`）。`goc build` 的默认就是 `-O0`。

## 11. 想看一个 pass 的效果

```bash
opt -load-pass-plugin=build/llvm/libGocPasses.so -passes=goc-color-escape -S in.ll -o out.ll
opt -load-pass-plugin=build/llvm/libGocPasses.so -passes='print<goc-color>' -disable-output in.ll   # 需要 -g 的 IR
test/color-escape/color-escape.sh in.ll -o out.ll        # 测试用包装（GOC_COLOR_REPORT=1 先打印报表）
GOC_KEEP_TMP=1 ./cmd/goc build foo.c -o out/foo.goobj -O3  # 保留 a.*.ll 每一步
```
