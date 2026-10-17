# 运行时模型

[← 目标文件与链接](objects-link.md) · [ARCHITECTURE](../../ARCHITECTURE.md) · [Go 版本 →](go-versions.md)

goc **没有自己的调度器或 GC**，用的就是 Go 运行时。生成的代码只需要遵守 Go 运行时的几条约定。本篇说明每条约定由谁保证。

## 1. 栈增长：morestack

| 约定 | 谁保证 |
|---|---|
| 入口比较 SP 和 `g.stackguard0`，不够就调用 `runtime.morestack_noctxt` | `GocStackCheck` + 架构钩子（[codegen.md](codegen.md) §4） |
| morestack 调用时 g 在 G 寄存器（amd64 R14；arm64 X28） | x86 冷块 `movq %fs:-8, %r14`；arm64 X28 全程保留 |
| morestack 返回后重新执行检查 | 冷块最后跳回检查块 |
| 不在函数中间改变 SP（pcsp 必须精确） | `no-realign-stack`、`-no-x86-call-frame-opt`、动态 alloca 改成 `goc_dynalloc` 池 |
| 检查区和冷块不可抢占 | `UNSAFE_POINT` = −2（`GocObjTablesWriter.cpp`） |
| 小叶子函数可以不检查 | `NoSplit`：链接器的 nosplit 预算计账 |

为什么不用 Go 的 `NOSPLIT` 帧 + 切栈：那就是 cgo 的做法，每次调用都要 `entersyscall` 级别的开销。goc 的 C 帧就是 goroutine 栈上的普通帧。

## 2. 搬栈：stackmap 与指针修正

Go 搬栈（`copystack`）时：
- 新栈 = 旧栈原样复制；
- 遍历每个帧，按该 pc 的 **locals/args 位图**，把落在旧栈范围 `[old.lo, old.hi)` 内的字加上 `delta`（`adjustpointer`）；
- 保存的帧指针链也会被调整。

所以 goc 必须保证：**跨调用存活的栈地址，要么在位图标记的槽里，要么能在调用后重新算出来。** 分工：

| 情况 | 处理 | 位置 |
|---|---|---|
| SSA 栈指针跨调用存活 | 放进 `goc.spill.root` 槽，记 stackmap，调用后 volatile 重读 | `goc-stackmap` |
| 指针实参（可能指向调用者帧） | `goc.arganchor` 槽，同上 | `goc-reanchor` |
| 局部槽里存着栈地址（`int *q = &x` 没被优化掉、`&s->token`） | `!goc.frame.words` 标记 → `.goc.funcs.sptr_slots` + gcmaps 位 | `goc-reanchor` / `goc-stackmap` → `GocObjTables` |
| 局部变量地址 `RBP+c` 被放在 CSR / 溢出槽 | 调用后 `LEA` 重新计算或按 `delta` 平移 | `GocX86FrameAddrFix`；arm64 `GocA64FrameAddrFix` |
| 冷块里保存的参数寄存器 | morestack 调用点自己的位图（`stubSlots`） | `GocStackCheck` |
| 栈上存的是整数（小常数） | **不能**标记：Go 遇到 `0 < p < 4096` 的「指针」会报 `bad pointer in frame` | `goc-stackmap` 只报指针类型 |

例如一个在两次调用之间把 `&buf[i&7]` 留在局部槽里的 `walk()`，`goc tool objdump dump` 显示的槽标记：

```
func walk.impl size=255 frame=64 … flags=0x49 … sptr=[16]
  gcmaps args=0 locals=7
    pc=70 args= locals=20
    pc=81 args= locals=20
```

`sptr=[16]` = `BP-16` 处的槽存着 `&buf[i&7]`，两次调用的返回地址处 locals 位图都标了它。

## 3. GC：goc 帧里的 Go 指针

- **Go 堆指针在 C 里叫 `gptr`**。语言合同（[language.md](../language.md) §3.5）写的是「stackmap + 写屏障」，并禁止 `gptr` 与其它颜色隐式混用。
- 当前代码里，`goc-color-escape` 的 `CECheck.cpp` 强制「不混用」：`gptr` 存进 cptr/sptr/uptr/auto 槽、或存进无颜色的堆 / 全局槽，都报错。
- **写屏障**：`llvm/` 和 `internal/` 里没有插入 Go 写屏障（`runtime.gcWriteBarrier` / `runtime.writeBarrier` 检查）的代码。
  `gptr` 的写屏障目前是**合同里写了、代码里没有**（[roadmap.md](../roadmap.md)）。
- 帧里 `gptr` 的栈图：`goc-stackmap` 根据 sptr 颜色和栈派生关系选根，选的是**栈地址**；按代码看，它不会因为一个值是 `gptr` 就把它放进位图。对 QuickJS 这类不在 C 帧里长期持有 Go 堆指针的用法没有影响，但不是完整的 GC 根支持。（这一条是读代码得出的结论，没有专门的测试覆盖。）

## 4. uptr：可以放进堆里的栈指针

问题：栈地址存进**堆或全局**（Go 搬栈看不到的地方），搬栈后就失效。uptr 是这种地址的编码。合同见 [language.md](../language.md) §3.3，API 声明在 [`include/goc_uptr.h`](../../include/goc_uptr.h)。

goc 没有运行时库。下面每个 helper 的实现都由编译器给出：`goc-uptr-inline`（`llvm/lib/Transforms/StackMap/SMUptrInline.cpp`）把对它们的调用换成模块内的 `internal alwaysinline` 函数体，`goc build` 在 `-O0` 下也会跑这个 pass。所以 goc 生成的对象不引用任何外部 uptr 符号，也不需要额外链接一个 C 文件。amd64 和 arm64 都一样。

编译器何时插入：`goc-color-escape` 发现 sptr 存进非栈 cptr 槽时插 `goc_uptr_from_ptr`，从这种槽读时插解码（[walkthrough.md](walkthrough.md) §5）。

### 4.1 字的布局（amd64 / arm64 用户态地址）

| MSB | 含义 | 解码 |
|-----|------|------|
| 0 | 绝对的 `cptr` 地址 | 直接当指针用 |
| 1 | 相对 **所属 g** 的 `stack.hi` 的 `int64` 补码偏移 | `abs = (uintptr_t)((int64_t)hi + (int64_t)stored)` |

栈向下长，所以通常 `abs < hi`，偏移是负数，补码下 MSB 自然是 1。

### 4.2 怎么读 g

| 架构 | g | `stack.lo` | `stack.hi` |
|------|---|-----------|-----------|
| linux/amd64 | `movq %fs:-8, reg`（和 morestack 序言同一个 TLS 槽） | `g+0` | `g+8` |
| linux/arm64 | `x28`（llc `+reserve-x28`，clang `-ffixed-x28`） | `g+0` | `g+8` |

g 的读取是 `readnone`（g 在 goroutine 生命期内不变），`goc-stackmap` 里的 `hoistTlsG` 让每个函数只读一次。`stack.lo` / `stack.hi` 的读取是 `volatile`：它们只会在调用里改变，但 morestack 序言是在这层 IR 之后才插入的，O3 证明为 readonly 的调用照样可能搬栈。

### 4.3 helper 一览

| 函数 | 行为 |
|------|------|
| `goc_stack_hi()` / `goc_stack_lo()` | 当前 g 的 `stack.hi` / `stack.lo` |
| `goc_uptr_from_cptr(p)` / `_hi(p, hi)` | 原样存；MSB 已经是 1 就 **FATAL** |
| `goc_uptr_from_sptr(p)` / `_hi(p, hi)` | `off = abs - hi`；MSB 不是 1（地址不在 hi 之下）就 **FATAL** |
| `goc_uptr_as_cptr(u)` / `_hi(u, hi)` | 要求 MSB=0，否则 **FATAL** |
| `goc_uptr_as_sptr(u)` / `_hi(u, hi)` | 要求 MSB=1，`abs = hi + off`，否则 **FATAL** |
| `goc_uptr_from_ptr(p)` | 编译器插入。`p` 落在 `[stack.lo, stack.hi)` 就按 sptr 编码，否则原样 |
| `goc_uptr_decode(u)` | 编译器插入。按 MSB 解码，不检查 |
| `goc_uptr_require_cptr(p)` | 编译器插入。`p` 落在当前栈上就 **FATAL**（sptr 不能当 cptr 返回） |
| `goc_dynalloc` / `goc_dynrelease` | 动态 `alloca` 的降级，见第 5 节 |

不带 `_hi` 的版本读当前 g；`_hi` 版本用调用者给的 `stack_hi`。`goc_uptr_is_stack_encoded` / `goc_uptr_is_cptr_encoded` 是头文件里的 `static inline`。

`goc-color-escape` 会插入 `goc_uptr_from_ptr`（栈指针存进非栈槽）、`goc_uptr_decode` 和 `goc_uptr_as_cptr`（从 uptr 槽读出、返回），以及 `goc_uptr_require_cptr`（返回值守卫）。

### 4.4 FATAL

错 tag 不返回。helper 调用模块内的 `goc.uptr.fatal`（`internal`、`noinline`、`cold`），它用裸系统调用 `write(2, …)` 打印一行 `goc FATAL uptr: <原因>`，然后执行 `llvm.trap`（amd64 `ud2`，arm64 `brk`）。不用 libc；Go traceback 认不出这个 trap 时，消息还在 stderr 上。宿主测试里进程以 SIGILL 退出（rc 132）。

### 4.5 搬栈不变式

堆上 MSB=1 的字在栈复制下不变：运行时把活的绝对指针加上 `delta`、把 `hi` 改成 `hi' = hi + delta` 之后，同一个编码字仍然满足 `hi' + enc == abs'`。

用不属于自己的 g 的 `stack.hi` 解码 MSB=1 的字是逻辑错误（算出错的地址）。这和 tag 不符的 FATAL 是两回事；所属关系由引擎保证（一个 JS runtime 对应一个 g）。

消费者（`goc-color-uptr-encoded`）要把这个字当作带 MSB 标记的不透明值：不能当绝对指针放进 Go 指针图，不能套 gptr 写屏障，解码只用所属 g 的 `stack.hi`。

### 4.6 限制

表示里没有 goroutine 指针，解码用当前 g。持有 sptr/uptr 的对象（如 QuickJS 的 `JSContext`）只能在所属 goroutine 上用（[language.md](../language.md) §8.3）。带 goroutine 指针的两字表示列在 [roadmap.md](../roadmap.md)，尚未实现。

## 5. 动态 alloca：`goc_dynalloc` 池

Go 的 pcsp 表描述不了函数中间的 SP 变化，所以 VLA / `alloca` 不能真的移动 SP。
`goc-color-escape` 的 `lowerDynamicAllocas` 把动态 `alloca` 换成 `goc_dynalloc(bytes, &scope)` / 返回前的 `goc_dynrelease(&scope)`，结果是 `cptr`，不在 goroutine 栈上（Go 的 pcsp 描述不了函数中间的 SP 变化）。`goc-uptr-inline` 按 `--fast-stack-alloca` 给出两种函数体：

| 模式 | 实现 |
|------|------|
| `--fast-stack-alloca`（`goc go` 和 QuickJS 默认） | 每个对象一个 16 MiB 的 BSS 池（`goc.alloca.pool` / `goc.alloca.off`，`internal`），内联指针碰撞，16 字节对齐，不清零。函数第一次分配记下水位，返回时退回水位。池游标是普通全局变量，只许一个 goroutine 用。各对象的池分别按函数返回 LIFO 嵌套，所以一个对象一个池和整个进程一个池一样正确 |
| `--no-fast-stack-alloca` | 每次 `malloc(16 + bytes)`，16 字节块头 `{previous, size}` 串在 scope 上，返回时逐个 `free`。`malloc` / `free` 可以用 `--ir-renames` 换成别的分配器 |

池满、scope 为空、大小溢出、水位不在池内都会 FATAL。16 MiB 和 goc CLI 的 `--stack-size 16384` 一致，所以池不会比 QuickJS 自己的栈检查先报错。

## 6. Go 与 C 之间的调用

| 方向 | 机制 |
|---|---|
| Go → C | Go 调 `pkg.F`（thunk，ABIInternal）→ 搬寄存器 → `call F.impl`（SysV / AAPCS）→ 返回前清零 X15。thunk 帧 = 栈参数（向上取整到 16）+ 保存的帧指针 |
| C → C | 直接 `call F.impl` |
| C → Go | `__go_extern("pkg.Func")` 声明 → `internal/lower` 生成 stub（溢出区、g、xmm15、SysV callee-saved、pcsp）。见 [quickstart.md](../quickstart.md)「C 调用 Go」、`test/e2e/go_extern/` |

## 7. 不支持

- 完整的 ISO C 或宿主 libc。
- Go 可调用入口上的 AVX、x87 和异常。
- `stacksave` / `stackrestore`，以及不能降成对齐 ≤ 16 的 `alloca i8` 的 VLA。
- 变参函数和不支持的聚合类型的 Go ABIInternal 入口。
- 在所属 goroutine 之外使用 `sptr` / `uptr` 或持有它们的上下文（第 4.6 节）。

## 8. 测试

- `test/lit/ColorEscape/09–11`：三个宿主程序经 color-escape + `goc-uptr-inline` 后直接链接，不带运行时。`test/lit/ColorEscape/fake_g.h` 让可执行文件唯一的 TLS 变量正好落在 `%fs:-8`，指向一个假 g，测试就能移动 `stack.lo/hi`，跑的是编译器自己的 helper 函数体。11 号覆盖全部显式转换和五种错 tag（检查消息和 SIGILL）。
- `test/e2e/run.sh arm64`：arm64 下 helper 读 `x28`、没有外部引用，在 qemu 里的 goroutine 栈上跑通。
- QuickJS：`js_check_stack_overflow` 用 `goc_stack_hi()`，libregexp 用 `goc_uptr_decode`，引擎的 uptr 存取全部走这些内联体。

没有 `dsptr`。不会把 `sptr` 悄悄提升成 `uptr`（前端仍然报错）。
