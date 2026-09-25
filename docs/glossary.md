# 术语

合同全文：[syntax-guide.md](syntax-guide.md)。冲突时以合同为准。无 `dsptr`。

## 指针色

### cptr
指向非栈对象（C 堆、全局、arena）。不是 Go 堆。直接寻址。不走 Go 写屏障。

### sptr
指向当前 goroutine 栈上的对象。原始指针字只许在寄存器或栈槽里。写入非栈的 `T *` 时，那个存储改用 `uptr` 编码，不把绝对栈地址放进堆。

### uptr
`cptr|sptr` 的编码字，可以放进堆。MSB 为 0 是绝对地址；MSB 为 1 是相对 owner `g.stack.hi` 的 int64 偏移，`abs = stack.hi + stored`。不能直接解引用。只用所属 goroutine 的 `stack.hi` 解码。

### auto_ptr
推断色。`T *` 就是它。能证明只指向栈且指针字不入库时收成 `sptr`；必须入库时收成 `uptr`。

### gptr
Go 堆指针。与其它色没有隐式转换。活值进 stackmap；写入 Go 堆槽走写屏障。禁止算术。

## 栈

### morestack
栈不够或需要抢占时调用的运行时入口。返回后从函数入口重跑 prologue，不是从调用点继续。

### stack.hi / stack.lo
当前 `g` 的栈界。amd64 上 `g` 在 `FS:-8`，`stack.lo` 在 `g+0`，`stack.hi` 在 `g+8`。

### safepoint
可能停顿的点（调用、morestack）。此处仍活的栈指针必须能被 stackmap 或帧地址重算找到，否则搬栈会漏改。

### NOSPLIT
静态栈预算内不触发 morestack。超过预算由链接器拒绝。

## ABI

### ABIInternal
Go 编译器生成代码的寄存器约定。goc 对 int/指针、不超过 6 个参数的入口发 thunk，函数体仍是 SysV。

### ABI0
参数和结果在栈上。C 侧 SysV 入口用这个。

### SysV
Linux amd64 的 C 调用约定。goc 函数体按这个生成；Go 调用经 thunk 进入。

## 产物

### goobj
Go 链接器接受的目标文件，带 pclntab、FUNCDATA、PCDATA。`elfpack` 从 llc 的 ELF 生成它。

### FUNCDATA / PCDATA
函数级指针 bitmap，以及随 PC 变化的 stackmap 索引。搬栈时运行时用它们找活指针。
