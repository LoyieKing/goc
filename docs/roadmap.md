# Roadmap

已经完成的事不在这里：当前行为见 [ARCHITECTURE.md](../ARCHITECTURE.md) 和 [design/](design/)，已知缺口见 [contributing.md](contributing.md) §7。

## linux/arm64：剩下的部分

编译器和 Go 侧已经支持 arm64（`goc build --arch arm64`、`goc go --arch arm64`，qemu-aarch64 下的 e2e 用例），见 [design/codegen.md](design/codegen.md)。还没做的：

- QuickJS 宿主（`ports/quickjs/host` 下的 CLI 宿主和 libc shim）用 x86 syscall 和 SSE 汇编，arm64 上被拒绝，所以 QuickJS 还没在 qemu 里跑。
- 通过栈传参、指向栈的指针，被调用方在栈移动之后才读时不会被修正（栈参数区不在栈图里）。

## `sptr` / `uptr` 带上所属 goroutine

当前 `sptr` 和 `uptr`（MSB=1）是一个机器字，相对所属 goroutine 的 `stack.hi`。表示里没有 goroutine 指针，解码用的是当前 `g`。所以持有它们的 context（例如 QuickJS 的 `JSContext`）只能在同一条 goroutine 上调用，否则 offset 加到错误的 `stack.hi` 上。这是 v0.2.4 的合同，见 [language.md](language.md) §8.3。

下一版不替换默认表示。默认仍是现在的单机器字，§8.3 继续有效。开发者用开关自己决定要不要带上所属 goroutine：

- 开关是一个 `goc build` / `goc go` 参数（名字待定），默认关。goc 不读环境变量。
- 打开后，`sptr` 和 `uptr` 从单机器字改成两个 64 位。一个是 offset，一个是所属 goroutine 指针。
- 打开后，解码用结构里的 goroutine 的 `stack.hi`，不再默默用当前 `g`。
- 调用约定、stackmap、着色和 Go ABI thunk 只在开关打开时改。两个字不再塞进一个寄存器。
- 开和关的对象不能混链。指针字宽度不一样。
- 开关关掉时，行为和现在相同：跨 goroutine 调用仍是合同错误。

## LSP

现在没有语言服务。编辑器只能把 `.c` 当普通 C，看不见指针色，也看不见着色之后裸 `T *` 收成了什么。

下一版加一个 goc LSP。它消费同一套着色结果，不另写一套分析：

- 诊断：`sptr` 逃逸、跨色赋值、合同里已经是错误的用法。和 `goc build` 的报错一致。
- 悬停：函数形参、返回值、变量、字段上的指针色。裸 `T *` 显示收成的 `cptr` / `sptr` / `uptr`，以及来源。这和 `--color-report` 是同一份数据。
- 跳转：`sptr` / `cptr` / `uptr` / `gptr` 宏和 `goc.h` 里的运行时声明。
- 补全：色宏和已有的 `goc_*` 声明。不发明新语法。
- 两字 sptr 表示未落地之前，不要在悬停里假装它已经生效。

## goc 调用 Go

已落地 `__go_extern("pkg.Func")`（`include/goc.h`）：C 侧声明驱动 goc 生成 SysV→ABIInternal stub（按签名预留溢出区、装 `g`、清 `xmm15`、保存 SysV callee-saved、带 pcsp）。宿主写普通 C 调用；Go 被调方不必 nosplit。金测 `test/e2e/go_extern/`；QuickJS 宿主已去掉手写 `.goabi` 汇编。

还没做的：

- 非 `main` 包的打包与符号约定（属性里已能写任意 `pkg.Func`，链入脚本仍偏 `main`）。
- `string` / 切片 / 接口；更大结构体与 sret。
- 指针色跨边合同写进 [language.md](language.md)；panic 穿过 C 帧仍是合同错误。
- 调用中触发 GC 的专项金测；开销基准（见下节）。

## 两边交互

接下来先讨论方案，不在这里定实现。现在 Go 和 goc 没有函数指针互转：Go 走 ABIInternal thunk，C 的函数指针走 SysV `.impl`，C 调 Go 走 `__go_extern` 生成的 stub。`JS_NewCFunction2` 这类回调、闭包、以及其它跨边的值该怎么过，都留到这次讨论。

## 量化 goc 与 Go 互相调用的开销

现在仓库里没有测过 Go 调用 goc、goc 调用 Go 各自一次要花多少时间。README 只说了“不切栈、不经过 `entersyscall`”，给不出数字。下一版要补：

- 基准：空函数、少量整数参数、指针参数、浮点参数几种签名，分别测 Go 调 goc、goc 调 Go 的单次开销（ns/op）。
- 对照组：同样签名的 Go 调 Go（不内联）、cgo、以及可能的话 purego / directcgo。
- 同时测调用中触发搬栈的情况，看 morestack 在 goc 帧上的代价。
- 结果写进 [benchmark/README.md](benchmark/README.md)，图表带上所有对照组。

## QuickJS 直接使用 Go 的 GC

现在 QuickJS 的对象由它自己的分配器分配，靠引用计数加环检测回收，Go 的 GC 不管它们。goc 只保证 C 帧里持有的 Go 对象（`gptr`）不会被 GC 漏扫。设想是让 JS 对象直接分配在 Go 堆上，交给 Go 的 GC 回收。先讨论清楚这几件事，再定实现：

- 引用计数和环回收怎么处理：全部去掉，完全交给 Go GC；还是保留引用计数做及时释放，只把环回收交给 Go GC。
- 对象布局怎么告诉 Go GC：`JSObject`、`JSString`、`JSShape` 等结构里哪些字段是指针，要生成对应的指针位图（Go 的类型元数据），`JSValue` 这种标签加联合体的值怎么扫。
- 写屏障覆盖范围：改成 Go 堆对象后，所有往对象里写指针的地方都要带写屏障，数量和性能开销要先估出来。
- 析构的对应关系：`JS_FreeValue`、finalizer（`JSClassFinalizer`）、`FinalizationRegistry` / `WeakRef` 和 Go 的 `runtime.AddCleanup` / 弱指针怎么对应，释放时机的语义会不会变。
- 和指针色的关系：堆上 JS 对象里的指针应当都是 `gptr`，要确认颜色分析能自动推断，不需要大规模手改 QuickJS 源码。
- 收益要量化：内存占用、GC 停顿、V8-v7 分数，和现在的方案对比。

## `goc_dynalloc`：恢复全进程共用的池

删掉 `runtime/` 之后，`goc_dynalloc` 的池改成每个目标文件（翻译单元）各自在 BSS 里放一个 16 MiB。语义是对的，但每多一个翻译单元就多占 16 MiB 虚拟内存，各个池之间也不共享。要改的：

- 恢复成整个进程只有一个池：比如只定义一个 weak / COMDAT 的池符号，或者在链接时把各翻译单元的定义合并成一份。
- 不重新引入 `runtime/` 目录。
- 加一个测试：多个翻译单元链接进同一个程序时，确认它们用的是同一个池。

## 性能：缩小和原生 QuickJS 的差距

2026-10-09 的五引擎对照里，goc-ng 的 V8 分数是 native ng 的 0.92，goc-bellard 是 gcc Bellard 的 0.90（[benchmark/README.md](benchmark/README.md)）。[research/perf-gap.md](research/perf-gap.md) 第 8 节的改进项（回溯栈不解码、每个函数只读一次 `g`、根在调用之间留在寄存器、尾部复制、libm、vDSO 时钟、`memcmp`/`memcpy`、栈槽共享、小叶子免栈检查）都已进默认构建。还开着的：

- `-inline-threshold=250`（perf-gap 第 13 项）：文本管线不带 `default<O3>` 的 250 阈值。打开要重新计时并重跑正确性对照。
- RegExp 仍是 V8 里差距最大的子项（2026-10-09：goc-ng/ng 0.90，goc-bellard/Bellard 0.94），合入改进之后没有重测指令数。
- 剩下的开销主要是栈检查、uptr 编解码和栈图根槽的读写（perf-gap 第 4 节）。
