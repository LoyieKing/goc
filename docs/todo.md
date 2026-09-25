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
