# uptr：编码协议与编译器提供的 helper

合同：[`syntax-guide.md`](../syntax-guide.md) v0.2.1 §3.3。API 声明在 [`include/goc_uptr.h`](../../include/goc_uptr.h)。

goc 没有运行时库。下面每个 helper 的实现都由编译器给出：`goc-uptr-inline`（`llvm/lib/Transforms/StackMap/SMUptrInline.cpp`）把对它们的调用换成模块内的 `internal alwaysinline` 函数体，`goc build` 在 `-O0` 下也会跑这个 pass。所以 goc 生成的对象不引用任何外部 uptr 符号，也不需要额外链接一个 C 文件。amd64 和 arm64 都一样。

## 1. 字的布局（amd64 / arm64 用户态地址）

| MSB | 含义 | 解码 |
|-----|------|------|
| 0 | 绝对的 `cptr` 地址 | 直接当指针用 |
| 1 | 相对 **所属 g** 的 `stack.hi` 的 `int64` 补码偏移 | `abs = (uintptr_t)((int64_t)hi + (int64_t)stored)` |

栈向下长，所以通常 `abs < hi`，偏移是负数，补码下 MSB 自然是 1。

## 2. 怎么读 g

| 架构 | g | `stack.lo` | `stack.hi` |
|------|---|-----------|-----------|
| linux/amd64 | `movq %fs:-8, reg`（和 morestack 序言同一个 TLS 槽） | `g+0` | `g+8` |
| linux/arm64 | `x28`（llc `+reserve-x28`，clang `-ffixed-x28`） | `g+0` | `g+8` |

g 的读取是 `readnone`（g 在 goroutine 生命期内不变），`goc-stackmap` 里的 `hoistTlsG` 让每个函数只读一次。`stack.lo` / `stack.hi` 的读取是 `volatile`：它们只会在调用里改变，但 morestack 序言是在这层 IR 之后才插入的，O3 证明为 readonly 的调用照样可能搬栈。

## 3. helper 一览

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
| `goc_dynalloc` / `goc_dynrelease` | 动态 `alloca` 的降级，见第 6 节 |

不带 `_hi` 的版本读当前 g；`_hi` 版本用调用者给的 `stack_hi`。`goc_uptr_is_stack_encoded` / `goc_uptr_is_cptr_encoded` 是头文件里的 `static inline`。

`goc-color-escape` 会插入 `goc_uptr_from_ptr`（栈指针存进非栈槽）、`goc_uptr_decode` 和 `goc_uptr_as_cptr`（从 uptr 槽读出、返回），以及 `goc_uptr_require_cptr`（返回值守卫）。

## 4. FATAL

错 tag 不返回。helper 调用模块内的 `goc.uptr.fatal`（`internal`、`noinline`、`cold`），它用裸系统调用 `write(2, …)` 打印一行 `goc FATAL uptr: <原因>`，然后执行 `llvm.trap`（amd64 `ud2`，arm64 `brk`）。不用 libc；Go traceback 认不出这个 trap 时，消息还在 stderr 上。宿主测试里进程以 SIGILL 退出（rc 132）。

## 5. 搬栈不变式

堆上 MSB=1 的字在栈复制下不变：运行时把活的绝对指针加上 `delta`、把 `hi` 改成 `hi' = hi + delta` 之后，同一个编码字仍然满足 `hi' + enc == abs'`。

用不属于自己的 g 的 `stack.hi` 解码 MSB=1 的字是逻辑错误（算出错的地址）。这和 tag 不符的 FATAL 是两回事；所属关系由引擎保证（一个 JS runtime 对应一个 g）。

消费者（`goc-color-uptr-encoded`）要把这个字当作带 MSB 标记的不透明值：不能当绝对指针放进 Go 指针图，不能套 gptr 写屏障，解码只用所属 g 的 `stack.hi`。

## 6. 动态 alloca

`goc-color-escape` 的 `lowerDynamicAllocas` 把动态 `alloca` 换成 `goc_dynalloc(bytes, &scope)` / 返回前的 `goc_dynrelease(&scope)`，结果是 `cptr`，不在 goroutine 栈上（Go 的 pcsp 描述不了函数中间的 SP 变化）。`goc-uptr-inline` 按 `GOC_INLINE_DYNALLOC` 给出两种函数体：

| 模式 | 实现 |
|------|------|
| `--fast-stack-alloca`（`GOC_INLINE_DYNALLOC=1`，`goc go` 和 QuickJS 默认） | 每个对象一个 16 MiB 的 BSS 池（`goc.alloca.pool` / `goc.alloca.off`，`internal`），内联指针碰撞，16 字节对齐，不清零。函数第一次分配记下水位，返回时退回水位。池游标是普通全局变量，只许一个 goroutine 用。各对象的池分别按函数返回 LIFO 嵌套，所以一个对象一个池和整个进程一个池一样正确 |
| `--no-fast-stack-alloca` | 每次 `malloc(16 + bytes)`，16 字节块头 `{previous, size}` 串在 scope 上，返回时逐个 `free`。`malloc` / `free` 可以用 `GOC_IR_RENAMES` 换成别的分配器 |

池满、scope 为空、大小溢出、水位不在池内都会 FATAL。16 MiB 和 goc CLI 的 `--stack-size 16384` 一致，所以池不会比 QuickJS 自己的栈检查先报错。

## 7. 测试

- `test/color-escape/run.sh`：09/10/11 三个宿主程序经 color-escape + `goc-uptr-inline` 后直接链接，不带运行时。`test/color-escape/fake_g.h` 让可执行文件唯一的 TLS 变量正好落在 `%fs:-8`，指向一个假 g，测试就能移动 `stack.lo/hi`，跑的是编译器自己的 helper 函数体。11 号测试覆盖全部显式转换和五种错 tag（检查消息和 rc 132）。
- `test/e2e/arm64/run.sh`：arm64 下 helper 读 `x28`、没有外部引用，在 qemu 里的 goroutine 栈上跑通。
- QuickJS：`js_check_stack_overflow` 用 `goc_stack_hi()`，libregexp 用 `goc_uptr_decode`，引擎的 uptr 存取全部走这些内联体。

## 非目标

没有 `dsptr`。不会把 `sptr` 悄悄提升成 `uptr`（前端仍然报错）。
