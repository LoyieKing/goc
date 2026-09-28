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

## goc 调用 Go

现在没有完善的方案。能工作的只有手写调用，见 [guide.md §8](guide.md)：C 侧自己把参数放进 Go ABI 寄存器，恢复 R14 里的 `g`，清零 `xmm15`，再 `call main.函数名.goabi`。QuickJS 的数学桥和 `localtime` 桥就是这么写的。问题有这些：

- 没有导入语法。`goc_goimport` 只是占位名，没有实现。每个 Go 函数都要手写汇编式的调用。
- 参数溢出区靠调用方手留。Go 被调方会把寄存器参数溢出到返回地址上方，C 调用方没有这块区域，现在靠 `call` 前后各留 64 字节顶着。签名大了就可能盖掉 C 帧。
- 只认 `main` 包。符号写死成 `main.函数名.goabi`。
- 签名只覆盖整数、指针和少量浮点。结构体、多返回值、`string`、切片、接口都没有约定。
- 指针色跨边没有规则。C 传给 Go 的 `sptr` / `uptr` 要不要先解码、Go 能不能留住，Go 返回的指针在 C 侧收成什么色（应当是 `gptr`），现在都没写进合同。
- Go 侧可能触发栈增长、GC、调度和 panic。调用点要算 safepoint，C 帧里活着的 `gptr` 要在 stackmap 里，搬栈之后 C 侧的 `sptr` 仍要有效。panic 穿过 C 帧的行为没有定义。
- 没有金测。只有 QuickJS 里的几个桥间接覆盖。

下一版要做的：

- 导入声明。在 C 里声明一个 Go 函数（形式待定，例如 `goc_goimport("pkg.Func")` 属性），由 Clang 着色后发出调用，不再手写寄存器搬运。
- 编译器生成调用序列：按 Go amd64 ABIInternal 放参数、恢复 `g` 和 `xmm15`、预留溢出区、在调用点记 PCDATA 和 stackmap。溢出区大小按签名算，不再用固定 64 字节。
- 支持任意包。符号按导入路径生成，打包脚本把非 `main` 包也链进去。
- 类型映射表：Go 的整数、浮点、指针、`unsafe.Pointer` 先落地。`string`、切片、结构体、多返回值按 ABIInternal 的拆分规则后做。接口和闭包留到上面“两边交互”一起讨论。
- 指针色合同：`sptr` 不能直接传给 Go（Go 会当成可能逃逸），需要先显式转换或报错；Go 返回的指针在 C 侧是 `gptr`。写进 syntax-guide。
- panic 语义：先定为 panic 穿过 C 帧是合同错误，运行时直接 fatal，不做展开。
- 金测：普通调用、调用中触发搬栈、调用中触发 GC、多参数溢出区、非 `main` 包。

## 两边交互

接下来先讨论方案，不在这里定实现。现在 Go 和 goc 没有函数指针互转：Go 走 ABIInternal thunk，C 的函数指针走 SysV `.impl`，C 调 Go 是手写的 `.goabi` 调用。`JS_NewCFunction2` 这类回调、闭包、以及其它跨边的值该怎么过，都留到这次讨论。

## 性能：缩小和原生 QuickJS 的差距

来源是 [perf-gap.md](perf-gap.md) 第 8 节。收益一栏里，“实测”是 A/B 计时的结果（时间比，小于 1 表示变快）；“估计”是按指令份额推算的，没有计时验证。所有项都还没合入默认构建。合入任何一项前，都要重跑正确性对照：goc-ng 为 test262 1502/1526、官方测试 69/77；goc-bellard 为 1501/1526、73/77。

### RegExp（V8 里差距最大的子项）

同样的编译参数下，goc 的 RegExp 多执行 36% 的指令，耗时是原生的 1.24 到 1.28 倍。原因见 perf-gap.md 第 4.4 和 4.5 节。

- **恢复回溯栈时不再解码（E1）。** `lre_exec` 每压一条回溯记录，都要存两个指针：字节码位置 `pc` 和输入字符指针 `cptr`，现在各插一段 uptr 编码检查（每段读两次 `g`、比较两次、分支一次）。最热的块执行 2442 万次，goc 版 48 条指令，原生 22 条。根源是 `scripts/qjs-gstack*.patch` 的两个恢复宏 `goc_regexp_restore_ptr` / `goc_regexp_restore_capture` 用 `goc_uptr_decode` 读回：解码结果在 color-escape 里算“可能指向栈”，于是之后每次压栈都要再编码，`pc+k` 也成了 GC 根。改成普通整数转换 `(uint8_t *)(uintptr_t)(elem).val` 即可。原先设想的“把回溯栈字段标成 `cptr`”已被实验否定（E2：标了之后没有任何变化）。细节见 [perf-gap/regexp-colors.md](perf-gap/regexp-colors.md)。
  - 收益：只编译未运行。`lre_exec` 的 `FS:-8` 读取 29 → 0，TU 隐式 uptr 存储 12 → 2，`lre_exec` spill 根槽 12 → 7、根重载 52 → 12（`pc` 的根槽全部消失）。按指令份额估计 RegExp 指令数少约 15 个点。
  - 风险：中。条件是压进回溯栈的指针跨安全点时都不是 goroutine 栈地址。合入前：
    - 用 E1 构建，跑 test262 和正则测试（bellard、ng 各一遍），对照上面的正确性基线。
    - 确认没有外部 `lre_exec` 调用者把栈上缓冲区作为输入或字节码传进来（它是导出函数；QuickJS 自己的两个调用点都传堆缓冲区）。
    - 查清补丁注释里说的“return guard traps”当初是哪个指针触发的。嫌疑是 `next_sp`（保存的 `sp1`，真栈地址）；如果是它，只对 `next_sp` 保留解码。
- **同一函数里只读一次 `g`。** `g` 在 goroutine 的生命周期内不变，`stack.lo` 和 `stack.hi` 只会在调用里变。现在 `FS:-8` 的读取是 volatile 的，不能合并，也不能提到循环外。可以改成允许合并和外提，或者默认打开 `GOC_FIXED_G`，让 `r14` 固定存 `g`。
  - 收益：估计读 `g` 这一类的 9.4 个点指令能省一半左右，RegExp 最多快 10% 以上，其他子项也受益。E1 合入后 `lre_exec` 里已没有 `g` 读取，这一项在 RegExp 上的收益会变小，主要剩 JS 栈溢出检查等其他位置。
  - 风险：中。直接关系到搬栈是否正确；`GOC_FIXED_G` 的 Bellard 构建这次还没编通。
- **GC 根槽不再 volatile。** 现在一个可能指向栈、又要跨调用存活的指针，每次修改都要写回栈槽，每次使用都要从栈槽读。可以改成两次调用之间留在寄存器，只在调用前写回、调用后重读，类似 LLVM statepoint 的重定位。
  - 收益：估计最多省 3.9 个点指令，其中 `lre_exec` 2.5 个点，`JS_CallInternal` 0.9 个点。E1 已经去掉 `lre_exec` 里的 `pc` 根槽（最热的一个有 30 次重载），`lre_exec` 这部分要在 E1 之后重估。
  - 风险：高。这是后端改动，漏一处就会在搬栈后留下悬空指针。

### 其他项（按收益和风险排序）

- **llc 打开分派块的尾部复制**（`-tail-dup-pred-size=1000 -tail-dup-succ-size=1000`，也可以只对含 `indirectbr` 的函数放开）。
  - 收益：实测 goc-bellard 的 V8 时间比 0.927、SunSpider 0.848；goc-ng 的 SunSpider 0.929。
  - 风险：低。要重跑正确性对照和栈增长扫描。
- **精确规定的 libm 函数改用 C 实现**（`sqrt`、`floor`、`ceil`、`trunc`、`round`，补丁已有）。
  - 收益：实测 `Math.sqrt` 快 2.4 倍，`Math.floor` 快 1.9 倍。
  - 风险：低。
- **`clock_gettime` 走 vDSO**（补丁已有）。
  - 收益：实测 `Date.now()` 从 166 ns 降到 103 ns。
  - 风险：低。合入前要把 `//go:noinline` 改回 `gocGoLocaltime` 上。
- **`memcmp` 每步比较 8 字节**（补丁已有），或者用 SSE2。
  - 收益：实测 1 KiB 比较从 366 ns 降到 83 ns。
  - 风险：低。
- **shim 的 `goc_malloc_usable_size` 返回实际容量**，同时 `goc_realloc` 按容量拷贝。现在它返回的是申请时的尺寸，Bellard 的字符串追加因此每次都整串重建，`string_build*` 慢 1.8 倍。
  - 收益：机制已实测；估计 Bellard microbench 的几何平均少约 2%。
  - 风险：低。
- **超越函数不走 Go 再转 cgo**，改用 C 实现（如 CORE-MATH），或者用更轻的系统栈直调。
  - 收益：估计 `3d-morph` 少 35% 到 40%。
  - 风险：中。结果的最后一位可能和 glibc 不同，需要逐位对照。
- **只对非根槽恢复栈槽共享。**
  - 收益：估计 `JS_CallInternal` 的帧从 1768 字节降到约 1200 字节。
  - 风险：中高。
- **小叶子函数免栈检查**，由链接器检查 NOSPLIT 预算。
  - 收益：估计能省下栈检查那 1.8 个点指令里的一部分。
  - 风险：低中。
- **shim 的 `memcpy` / `memset` 在 64 字节到几 KiB 的区间用 SSE2 循环**（goc 路径上不能用 AVX）。
  - 收益：估计。
  - 风险：低。
