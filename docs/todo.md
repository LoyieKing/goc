# 下一版

当前产品后端是 linux/amd64。下一版做 linux/arm64。不是换 `-march`。

颜色分析、当前的单字 `uptr` MSB 协议、`alloca` 降成 `cptr` 留着。`g.stackguard0` 仍在偏移 16。arm64 这一项不改指针色的表示。

要重写的是出指令之后：

- `llc` 目标改成 `aarch64`。现在固定 `-march=x86-64`，triple 是 `x86_64-unknown-linux-gnu`。
- 机器 pass 不再发 X86 指令。`g` 在 R28，不在 R14，也不从 `FS:-8` 取。
- `elfpack` 的 morestack 前导改成 arm64 的比较和条件跳转，不再手写 `CMPQ` / `JBE` / `0xe8`。重定位认 `R_AARCH64_*`，不认 `R_X86_64_*`。
- goobj 用 arm64 的 `LinkArch`。现在是 `Linknew(&x86.Linkamd64)`，vendored 汇编器只有 `obj/x86`。
- ABI thunk 按 Go arm64 ABIInternal：整数 R0–R15，浮点 F0–F15。现在是 SysV `rdi/rsi/...` 洗到 Go amd64 `rax/rbx/...`。
- `runtime/uptr` 不再用 `movq %fs:-8, %rax`。arm64 上 Go 不走这个 TLS 槽。
- QuickJS 宿主里写死 linux/amd64 的 syscall 和 `#error` 要分开，不能跟着后端一起假移植。

## `sptr` / `uptr` 带上所属 goroutine

当前 `sptr` 和 `uptr`（MSB=1）是一个机器字，相对所属 goroutine 的 `stack.hi`。表示里没有 goroutine 指针，解码用的是当前 `g`。所以持有它们的 context（例如 QuickJS 的 `JSContext`）只能在同一条 goroutine 上调用，否则 offset 加到错误的 `stack.hi` 上。这是 v0.2.4 的合同，见 syntax-guide §8.3。

下一版不替换默认表示。默认仍是现在的单机器字，§8.3 继续有效。开发者用开关自己决定要不要带上所属 goroutine：

- 开关先定名为 `GOC_SPTR_WITH_G=1`。默认关。
- 打开后，`sptr` 和 `uptr` 从单机器字改成两个 64 位。一个是 offset，一个是所属 goroutine 指针。
- 打开后，解码用结构里的 goroutine 的 `stack.hi`，不再默默用当前 `g`。
- 调用约定、stackmap、着色和 Go ABI thunk 只在开关打开时改。两个字不再塞进一个寄存器。
- 开和关的对象不能混链。指针字宽度不一样。
- 开关关掉时，行为和现在相同：跨 goroutine 调用仍是合同错误。

## 列出每个 `T *` 收成了什么色

`T *` 就是 `auto_ptr`。着色之后它会收成 `cptr`、`sptr` 或 `uptr`，有时是 `-default-ptr-color` 盖上去的。现在这个结果只留在 pass 的内存表里，指令上有时有 `!goc.color`。全局变量直接跳过。stderr 只有错误计数，没有按符号列出的表。

下一版要能快速读出每个函数、变量、字段上的 `T *` 最终是什么色：

- 函数：每个 `T *` 形参和返回值。
- 变量：局部和全局。
- 字段：struct 字段。指针色不允许出现在 union 里，不用列。
- 每行给出声明位置、符号名、收成的色。能标来源就标：显式注解、推断，或 `-default-ptr-color`。
- 同一份报告里可以带上显式 `sptr(T)` / `cptr(T)` / `uptr(T)` / `gptr(T)`，用来和裸 `T *` 对照。裸 `T *` 是必须有的。
- 大翻译单元（例如 `quickjs.c`）要能直接扫，不要只在 verbose 日志里散落。
- 开关先定名为 `GOC_COLOR_REPORT=1`。默认关，不改变着色结果。

## LSP

现在没有语言服务。编辑器只能把 `.c` 当普通 C，看不见指针色，也看不见着色之后裸 `T *` 收成了什么。

下一版加一个 goc LSP。它消费同一套着色结果，不另写一套分析：

- 诊断：`sptr` 逃逸、跨色赋值、合同里已经是错误的用法。和 `goc build` 的报错一致。
- 悬停：函数形参、返回值、变量、字段上的指针色。裸 `T *` 显示收成的 `cptr` / `sptr` / `uptr`，以及来源。这和上面的颜色报告是同一份数据。
- 跳转：`sptr` / `cptr` / `uptr` / `gptr` 宏和 `goc.h` 里的运行时声明。
- 补全：色宏和已有的 `goc_*` 声明。不发明新语法。
- 只服务 linux/amd64 这套方言。arm64 和 `GOC_SPTR_WITH_G` 未落地之前，不要在悬停里假装它们已经生效。

## 两边交互

接下来先讨论方案，不在这里定实现。现在 Go 和 goc 没有函数指针互转：Go 走 ABIInternal thunk，C 的函数指针走 SysV `.impl`，C 调 Go 是手写的 `.goabi` 调用。`JS_NewCFunction2` 这类回调、闭包、以及其它跨边的值该怎么过，都留到这次讨论。
