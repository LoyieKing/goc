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

## 开箱即用的 goc 编译器

已落地，见 [quickstart.md](quickstart.md) 和 [toolchain.md](toolchain.md)。`goc go` 用命令行参数给出 QuickJS 那组默认（`-O3`、`--default-ptr-color cptr`），不读环境里的 `GOC_*`。每个 Go 调 C 的 thunk 帧按该函数的栈参数计算。morestack、可分裂帧和 sptr 栈图一定打开，没有开关。`goc check` 跑 `examples/hello`。`goc toolchain pack` 打出可搬的 clang、`libLLVM`、`opt` 和 pass。没有托管的下载地址；解压目录是 `~/.goc/toolchain` 或 `third_party/goc-toolchain`。`goc build` 不带 `--goabi` 时仍不套用这组默认。`goc build --goabi` 会打开 morestack 和可分裂帧；amd64 还会打开 sptr 栈图。

## 量化 goc 与 Go 互相调用的开销

现在仓库里没有测过 Go 调用 goc、goc 调用 Go 各自一次要花多少时间。README 只说了“不切栈、不经过 `entersyscall`”，给不出数字。下一版要补：

- 基准：空函数、少量整数参数、指针参数、浮点参数几种签名，分别测 Go 调 goc、goc 调 Go 的单次开销（ns/op）。
- 对照组：同样签名的 Go 调 Go（不内联）、cgo、以及可能的话 purego / directcgo。
- 同时测调用中触发搬栈的情况，看 morestack 在 goc 帧上的代价。
- 结果写进 benchmark.md，图表带上所有对照组。

## 性能：缩小和原生 QuickJS 的差距

来源是 [perf-gap.md](perf-gap.md) 第 8 节。下面每一项都在默认构建里（linux/amd64，`GOC_OPT_LEVEL=3`，gep 模式用 `goc-llc`）。`GOC_FIXED_G` 仍默认关。perf-gap 第 13 项 `-inline-threshold=250` 不在这份清单里，没有开。收益一栏仍是合入前的数字：“实测”是当时的 A/B（时间比，小于 1 表示变快）；“估计”按指令份额推算，这次没有再计时。

这套二进制上的正确性对照：

- goc-ng：test262 1502/1526，栈增长扫描 300/300。`tests.conf` 套件 116/116。`quickjs.ll` 里没有函数读 `g` 超过一次，`JS_CallInternal` 是 1 次。
- goc-bellard：test262 1501/1526，栈增长扫描 300/300。Bellard `tests/` 按函数 77/77，`make test` 的整文件（含 `test_builtin.js --std`、`test_std.js`、`test_rw_handler.js`）通过。同样每个函数最多一次 `g` 读取。
- 两边 CLI：`Math.sin(1).toString()` 为 `0.8414709848078965`。
- 五引擎对照（[benchmark.md](benchmark.md)，2026-10-09）里，同一套 Bellard `tests/test_*.js` 按函数计是 goc-ng 73/77（与 native ng 相同）、goc-bellard 77/77（与 native Bellard 相同）。上面的 `tests.conf` 116/116 是另一套。
- Bellard 和 ng 的 `libregexp`：隐式 uptr 存储 2，解码指针加载 0。
- `backend/realbody/check_arm64.sh` 通过。arm64 ELF 没有在 qemu 里执行。

### RegExp（V8 里差距最大的子项）

2026-09-27 的指令计数里，同样的编译参数下 goc 的 RegExp 多执行 36% 的指令，耗时是原生的 1.24 到 1.28 倍（见 perf-gap.md 第 4.4 和 4.5 节）。2026-10-09 的 V8 RegExp 分数比是 goc-ng/ng 0.90、goc-bellard/Bellard 0.94。这次没有重测指令数。

- **恢复回溯栈时不再解码（E1）。** 已进两份 `scripts/qjs-gstack*.patch`。`pc` / `cptr` / `capture` 用 `(uint8_t *)(uintptr_t)(elem).val`。`next_sp` 仍走 `goc_regexp_restore_stackptr`（`goc_uptr_decode`）：它保存的是真栈地址。标成 `cptr` 的做法（E2）没有采用。
  - 收益：合入前只编译未运行。`lre_exec` 的 `FS:-8` 读取 29 → 0，TU 隐式 uptr 存储 12 → 2，`lre_exec` spill 根槽 12 → 7、根重载 52 → 12。按指令份额估计 RegExp 指令数少约 15 个点。这次默认构建上 Bellard 和 ng 的 `libregexp` 都是隐式 uptr 存储 2、解码加载 0。
- **同一函数里只读一次 `g`。** 已进默认构建，没有开 `GOC_FIXED_G`。`tlsGWord` 的读 `g` 是 `readnone` + `nounwind` + `willreturn`（amd64 `movq %fs:-8, $0`，arm64 `mov $0, x28`）。O3 之后 `hoistTlsG` 把一个函数里的多次读取收成入口处一次。`stack.lo` / `stack.hi` 的加载仍是 volatile。
  - 收益：估计读 `g` 这一类的 9.4 个点指令能省一半左右。E1 之后 `lre_exec` 里已经没有 `g` 读取，剩下的是 JS 栈溢出检查等位置。Bellard 的 `quickjs.ll` 从 481 次读、54 个函数重复读，变成 122 次读、0 个函数重复读。
- **GC 根在两次调用之间留在寄存器。** 定义处仍 volatile 写回根槽（stackmap 认的是槽地址，不能被 DSE 删掉）。同一段没有 safepoint 的区间只 volatile 重载一次，后面的使用留在寄存器里；下一次调用后再重载。另外，根槽（`goc.arganchor` / `goc.anchor` / `goc.spill.root`）的精确拷贝若活在被调用者保存的寄存器里，调用后从槽里重读，不再对这份拷贝做 LEA。Go 已经改过槽里的指针。arm64 的帧地址重物化仍然直接失败，没有移植。
  - 收益：估计最多省 3.9 个点指令。这次没有重测指令数。Bellard 的 300 次栈增长扫描通过；原先字面量 `8` 在搬栈后会被读成 `8*(1+2^-32)`，就是这条路径。

### 其他项（按收益和风险排序）

- **llc 打开分派块的尾部复制。** amd64 和 arm64 都带 `-tail-dup-pred-size=1000 -tail-dup-succ-size=1000`。尾部合并仍关。
  - 收益：合入前实测 goc-bellard 的 V8 时间比 0.927、SunSpider 0.848；goc-ng 的 SunSpider 0.929。
- **精确规定的 libm 函数改用 C 实现。** `sqrt` 是 `sqrtsd`。`floor`、`ceil`、`trunc`、`round` 按 musl 写，`goc_toint` 是 2^52。
  - 收益：合入前实测 `Math.sqrt` 快 2.4 倍，`Math.floor` 快 1.9 倍。
- **`clock_gettime` 走 vDSO。** `CLOCK_MONOTONIC` 用 `runtime.nanotime`，`CLOCK_REALTIME` 用 `time.Now`，其它时钟仍是 syscall 228。`//go:noinline` 在 `gocGoLocaltime` 上，不在这个辅助函数上。
  - 收益：合入前实测 `Date.now()` 从 166 ns 降到 103 ns。
- **`memcmp` 每步比较 8 字节。** `goc_ld8` 加 `bswap64`，没有改成 SSE2。
  - 收益：合入前实测 1 KiB 比较从 366 ns 降到 83 ns。
- **`goc_malloc_usable_size` 返回 `h->capacity`，`goc_realloc` 按这个容量拷贝。**
  - 收益：机制已实测；估计 Bellard microbench 的几何平均少约 2%。
- **超越函数直接调 glibc。** `tests/qjs/asm/goc_libm_glibc.S` 做栈对齐后调用，不经 Go 的 cgo。`frexp` / `modf` / `ldexp` / `scalbn` / `lrint` / `strtod` 仍走原来的 cgo。CLI 要求 `Math.sin(1)` 的十进制和 glibc 一致。
  - 收益：估计 `3d-morph` 少 35% 到 40%。这次没有重测，只对了 `sin` / `log` / `atan2` 的值。
- **非根槽恢复栈槽共享。** 默认 llc 不再传 `-no-stack-slot-sharing`。根 alloca 仍独占槽位。没有开 `goc-pin-i64`。
  - 收益：估计 `JS_CallInternal` 的帧从 1768 字节降到约 1200 字节。关掉槽共享的实验构建在第 16 次扫描 SIGSEGV，默认构建保持打开。
- **小叶子函数免栈检查。** `GOC_NO_NOSPLIT=1` 仍在，大函数不会整段标成 nosplit。没有调用、帧不超过 `StackSmall` 的 ABI0 叶子由 `gocLeafNosplit` 标 `AttrNoSplit`，链接器检查预算。
  - 收益：估计能省下栈检查那 1.8 个点指令里的一部分。
- **`memcpy` / `memset` 在 64 字节到 4096 字节用 SSE2 `movups`。** 不用 AVX，不用 `movaps`。`memset` 的尾巴走 `always_inline` 的 `goc_memset_small`，不递归调用 `goc_memset`。这段汇编只在 amd64 宿主里，arm64 构建会拒绝这份 shim。
  - 收益：估计。这次没有重测。
