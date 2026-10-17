# 06 运行时模型

[← 05 .gocobj 与链接](05-gocobj-and-link.md) · [索引](../engineering-architecture.md) · [下一篇：测试与扩展 →](07-tests-howto-gaps.md)

goc **没有自己的调度器或 GC**，用的就是 Go 运行时。生成的代码只需要遵守 Go 运行时的几条约定。本篇说明每条约定由谁保证。

## 1. 栈增长：morestack

| 约定 | 谁保证 |
|---|---|
| 入口比较 SP 和 `g.stackguard0`，不够就调用 `runtime.morestack_noctxt` | `GocStackCheck` + 架构钩子（04 篇 §4） |
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

02 篇的 `walk()` 例子（`sm.c`）显示了槽标记：

```
func walk.impl size=255 frame=64 … flags=0x49 … sptr=[16]
  gcmaps args=0 locals=7
    pc=70 args= locals=20
    pc=81 args= locals=20
```

`sptr=[16]` = `BP-16` 处的槽存着 `&buf[i&7]`，两次调用的返回地址处 locals 位图都标了它。

## 3. GC：goc 帧里的 Go 指针

- **Go 堆指针在 C 里叫 `gptr`**。语言合同（`docs/syntax-guide.md` §3.5）写的是「stackmap + 写屏障」，并禁止 `gptr` 与其它颜色隐式混用。
- 当前代码里，`goc-color-escape` 的 `CECheck.cpp` 强制「不混用」：`gptr` 存进 cptr/sptr/uptr/auto 槽、或存进无颜色的堆 / 全局槽，都报错。
- **写屏障**：在 efe840d 的 `llvm/`、`runtime/`、`goobj/lower/`、`tools/` 里**找不到**编译器插入 Go 写屏障（`runtime.gcWriteBarrier` / `runtime.writeBarrier` 检查）的代码。
  `toolexec_recognize_goobj.sh` 里追加的 `def <pkg>.StoreGptrWB ABIInternal` 是写死的旧 symabis 行，不对应 goc 生成的代码。
  也就是说 `gptr` 的写屏障目前是**合同里写了、代码里没有**（见 07 篇已知缺口）。
- 帧里 `gptr` 的栈图：`goc-stackmap` 根据 sptr 颜色和栈派生关系选根，选的是**栈地址**；按代码看，它不会因为一个值是 `gptr` 就把它放进位图。对 QuickJS 这类不在 C 帧里长期持有 Go 堆指针的用法没有影响，但不是完整的 GC 根支持。（这一条是读代码得出的结论，没有专门的测试覆盖。）

## 4. uptr：可以放进堆里的栈指针

问题：栈地址存进**堆或全局**（Go 搬栈看不到的地方），搬栈后就失效。

uptr 的编码（`docs/design/uptr.md`）：一个机器字。

| MSB | 含义 | 解码 |
|---|---|---|
| 0 | 绝对地址（cptr） | 原样 |
| 1 | 相对**所属 goroutine** 的 `g.stack.hi` 的 int64 偏移 | `abs = stack.hi + stored` |

栈向下长，`abs < hi`，偏移是负数，MSB 天然为 1。搬栈后 `hi' = hi + delta`，同一个编码字解码出 `abs + delta`，自动正确。

- 编译器何时插入：`goc-color-escape` 发现 sptr 存进非栈 cptr 槽 → `goc_uptr_from_ptr`；从这种槽读 → 解码（02 篇 §5）。
- 实现：没有运行时库。`goc-uptr-inline` 给 `goc_uptr_from_cptr/_sptr`、`goc_uptr_as_cptr/_sptr` 及 `_hi` 变体、`goc_uptr_from_ptr`、`goc_uptr_decode`、`goc_uptr_require_cptr`、`goc_stack_hi`/`goc_stack_lo` 生成内联函数体；
  读 `g.stack.hi`：amd64 `movq %fs:-8, %rax; movq 8(%rax), %rax`，arm64 从 `x28` 读 g。错 tag 时 `write(2)` 一行 `goc FATAL uptr: …` 再 trap。`hoistTlsG` 让每个函数只读一次 g。
- **限制**：表示里没有 goroutine 指针，解码用当前 g。持有 sptr/uptr 的对象（如 QuickJS 的 `JSContext`）只能在所属 goroutine 上用（syntax-guide §8.3）。`docs/todo.md` 计划了可选的 `GOC_SPTR_WITH_G=1`（两字表示），尚未实现。

## 5. 动态 alloca：`goc_dynalloc` 池

Go 的 pcsp 表描述不了函数中间 SP 变化，所以 VLA / `alloca` 不能真的移动 SP。
`goc-color-escape` 的 `lowerDynamicAllocas` 把它们改成 `goc_dynalloc` / `goc_dynrelease`（函数返回时释放），结果是 `cptr`。
`--fast-stack-alloca`（`GOC_INLINE_DYNALLOC=1`，`goc go` 默认开）让 `goc-uptr-inline` 用每个对象一个的 16 MiB BSS 池并内联指针碰撞，快很多，但池游标是普通全局变量，只能单 goroutine 使用；关掉时每次 `malloc` / `free`。细节见 `docs/design/uptr.md` §6。

## 6. Go 与 C 之间的调用

| 方向 | 机制 |
|---|---|
| Go → C | Go 调 `pkg.F`（thunk，ABIInternal）→ 搬寄存器 → `call F.impl`（SysV / AAPCS）→ 返回前清零 X15。thunk 帧 = 栈参数（向上取整到 16）+ 保存的帧指针 |
| C → C | 直接 `call F.impl` |
| C → Go | `__go_extern("pkg.Func")` 声明 → goc-lower 生成 stub（溢出区、g、xmm15、SysV callee-saved、pcsp）。见 `docs/guide.md`「C 调用 Go」、`test/e2e/go_extern/` |
