# 02 走一遍：从 add.c 到二进制

[← 01 总览](01-overview.md) · [索引](../engineering-architecture.md) · [下一篇：IR pass →](03-ir-passes.md)

本篇所有输出都是在 efe840d 上实际跑出来的（x86-64 Linux，Go 1.24）。复现方法：

```bash
# 产品路径：一条命令编整个包
./cmd/goc go examples/hello -o /tmp/hello && /tmp/hello        # → hello 42

# 单文件，保留中间产物（cmd/goc 一个临时目录，realbody.sh 一个目录）
GOC_KEEP_TMP=/tmp/rb GOC_OPT_LEVEL=3 GOC_DEFAULT_PTR_COLOR=cptr \
  ./cmd/goc build foo.c -o out/foo.goobj
#   stderr 打印 "goc build: kept /tmp/tmp.XXXX"：a.ll a.color.ll a.gate.ll a.opt.ll a.sm.ll a.reanchor.ll
#   /tmp/rb：body.ll body.impl.ll thunks.s thunks.goc.s goabi.json body.s body.goc.s body.thunks.s body.llc.o
#   out/：foo.goobj foo.gocobj foo.symabis foo.ll
```

> 注意：`-o` 的目录必须已存在，否则最后一步 `gocobj2goobj` 报 `open …: no such file or directory`。

## 1. 产品路径：`goc go examples/hello`

`examples/hello/add.c`：

```c
#include "goc.h"

int hello_add(int a, int b) { return a + b; }
```

`examples/hello/main.go` 里只有一个**没有函数体的声明**：

```go
func hello_add(a, b int32) int32

func main() {
	fmt.Printf("hello %d\n", hello_add(20, 22))
}
```

`scripts/goc-go.sh` 做的事（按顺序）：

1. `goc_isolate_product_env` 清掉继承的 `GOC_*`，`goc_product_defaults` 设 `GOC_OPT_LEVEL=3`、`GOC_DEFAULT_PTR_COLOR=cptr`、`GOC_INLINE_DYNALLOC=1`。
2. 检查 `go env GOOS/GOARCH` 是 `linux/amd64`。
3. 包里没有 `.s` 时写一个 `goc_marker_amd64.s`（只有一个 `TEXT ·gocMarker`）。Go 只有在包里有汇编文件时才接受无体声明。
4. 对每个 `*.c` 调 `cmd/goc build src -o build/goc-go/<hash>/<base>.goobj <flags>`。
5. `GOC_BINOBJ=<所有 .goobj>`，然后 `CGO_ENABLED=0 go build -a -toolexec tools/goc/toolexec_recognize_goobj.sh`。

实际日志（节选）：

```
goc-color-escape: summary: 0 error(s), scoped dynamic allocas=0, implicit uptr stores=0, decoded pointer loads=0, guarded returns=0 in module '/tmp/tmp.QDOl4KPpTz/a.ll'
goc build: O3 inlined before stack maps
realbody: override-stack-alignment=8 (no aligned SSE on the C stack)
goabi: 1 rewritten, 0 left SysV-only
realbody: wrote /workspace/goc/build/goc-go/0cfdae83286705bf/add.goobj (encoding=clang-real-isel, TEXT syms=2)
toolexec: appended goc symabis from add.symabis
toolexec: packed add.goobj into $WORK/b001/_pkg_.a
goc go: /workspace/archtrace/hello/hello
hello 42
```

## 2. 逐阶段：`hello_add`（叶子函数）

### 2.1 clang → `a.ll`

`cmd/goc` 的 clang 参数（`cmd_build`）：`-mno-red-zone -fno-stack-protector -fno-asynchronous-unwind-tables -I include -emit-llvm -S -O3`，
`-O>0` 时再加 `-fwrapv -fno-strict-aliasing -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls -Xclang -disable-llvm-passes`，以及 `-gline-tables-only` 和 `-DGOC_USE_INTREE_ATTRS`。

为什么：
- **不要红区**：红区是 SP 下面 128 字节「不用调整 SP 就能用」的区域；Go 的抢占信号和搬栈都不认它。
- **`-disable-llvm-passes`**：clang 只出未优化 IR。颜色分析必须在 SROA/内联之前看到原始的指针存储；O3 由 `cmd/goc` 自己之后跑。
- **保留帧指针**：goc 的栈图用 `BP - off` 描述槽（见 05 篇 `sptr_slots`）。

```llvm
define dso_local i32 @hello_add(i32 noundef %a, i32 noundef %b) #0 !dbg !9 {
entry:
  %a.addr = alloca i32, align 4
  %b.addr = alloca i32, align 4
  store i32 %a, ptr %a.addr, align 4
  store i32 %b, ptr %b.addr, align 4
  %0 = load i32, ptr %a.addr, align 4, !dbg !13
  %1 = load i32, ptr %b.addr, align 4, !dbg !14
  %add = add i32 %0, %1, !dbg !15
  ret i32 %add, !dbg !16
}
```

### 2.2 opt 各 pass → `a.reanchor.ll`

没有指针、没有调用，`goc-color-escape`/`goc-stackmap`/`goc-reanchor` 都没东西可做，O3 把它化简成：

```llvm
define dso_local noundef i32 @hello_add(i32 noundef %a, i32 noundef %b) #0 !dbg !10 {
entry:
  %add = add i32 %b, %a, !dbg !14
  ret i32 %add, !dbg !15
}
```

### 2.3 `goc-lower goabi`：改名 + 生成 thunk

`realbody.sh` 调 `goc-lower goabi body.ll body.impl.ll thunks.s goabi.json thunks.goc.s`：C 本体改名为 `hello_add.impl`（SysV 调用约定），
另生成一个叫 `hello_add` 的 **Go ABIInternal 入口**（`goobj/lower/goabi_x86.go`）。Go 的 ABIInternal 第 1、2 个整数参数在 RAX、RBX，SysV 在 RDI、RSI：

```asm
# Go 1.24 ABIInternal -> SysV entry thunks — generated
	.section	.text.hello_add,"ax",@progbits
	.globl	hello_add
hello_add:
	pushq	%rbp
.Lgocw_p0:
	movq	%rsp, %rbp
	movq	%rax, %rdi
	movq	%rbx, %rsi
.Lgocw_c0:
	call	hello_add.impl
.Lgocw_r0:
	pxor	%xmm15, %xmm15        # Go ABIInternal 要求返回时 X15 为 0
	movq	%rbp, %rsp
.Lgocw_m0:
	popq	%rbp
.Lgocw_o0:
	ret
```

### 2.4 goc-llc → `body.s`

叶子函数、帧很小 → `GocStackCheck` 判定不需要栈检查（nosplit），输出就是普通代码：

```asm
hello_add.impl:                         # @hello_add.impl
	pushq	%rbp
	movq	%rsp, %rbp
	leal	(%rdi,%rsi), %eax
	popq	%rbp
	retq
.Lgoc_end0:
```

### 2.5 `.gocobj`：用 `goc-objdump` 看

```
$ go run ./tools/goc-objdump dump out/add.gocobj
gocobj 1.4 arch=1 flags=0x1 profile="go1.24-amd64" producer="goc-llc (LLVM 19.1.7)"
file 0 /workspace/archtrace/add.c
func hello_add.impl size=9 frame=8 args=0 abi=0 kind=0 flags=0xe id=0 file=0 line=3 sysvptr=0x0
  pcvalue SP_DELTA: 0=8
  pcvalue FILE: 0=0
  pcvalue LINE: 0=3
func hello_add size=25 frame=8 args=8 abi=1 kind=1 flags=0xc id=0 file=0 line=0 sysvptr=0x0
  pcvalue SP_DELTA: 0=0 1=8 23=8 24=0
  gcmaps args=1 locals=0
    pc=15 args=00 locals=
  wrapper -> hello_add.impl abi 1->2 spills=[{rax 8 4 false} {rbx 12 4 false}] stackptrs=[]
  reloc +11 w=4 CALL hello_add.impl-4 flags=0x0
```

读法：`flags=0x1` = `stack_checks_in_code`（栈检查已经在机器码里）。`hello_add.impl` 的 `flags=0xe` = bit1 leaf + bit2 nosplit + bit3 has_frame_pointer；
`abi=0` 是 ABI0，`kind=1` 的 `hello_add` 是 ABI wrapper（thunk）。`SP_DELTA: 0=0 1=8 23=8 24=0` 是 pcsp：pc 0 时 SP 偏 0，`pushq` 之后偏 8……

### 2.6 goobj

```
$ go tool nm out/add.goobj | grep -E ' T |R gcl'
     7df R gclocals.goc.main.hello_add.args
     7e8 R gclocals.goc.main.hello_add.locals
     7c6 T main.hello_add
     7bd T main.hello_add.impl
$ cat out/add.symabis
def main.hello_add.impl ABI0
def main.hello_add ABIInternal
```

`-p main` 让符号带上包前缀 `main.`。`symabis` 告诉 Go 的 asm/compile：`main.hello_add` 是 ABIInternal 定义，于是 `main.go` 里的无体声明直接链到它，不再要 ABI 包装。

## 3. 逐阶段：带调用和栈指针的 `call.c`

叶子函数看不到 goc 的核心机制。下面这个例子有调用，并且把指向调用者帧的指针传给被调用者：

```c
#include "goc.h"

__attribute__((noinline)) void fill(int *p) { *p = 41; }

/* p points into the caller's frame and is used again after a call */
__attribute__((noinline)) int bump(int *p) {
  fill(p);
  return *p + 1;
}

int sum2(void) {
  int x = 0;
  int *q = &x;          /* a stack slot that holds a stack pointer */
  bump(q);
  return *q;
}
```

**风险在哪里**：`bump` 调用 `fill` 时可能触发搬栈。`p` 指向 `sum2` 的帧（栈上），搬完后旧地址失效。
如果 `p` 只放在寄存器里，Go 的搬栈代码看不见它，`fill` 之后的 `*p` 就读到旧栈。

### 3.1 `goc-color-escape`：给值标颜色

`a.color.ll` 里（`--default-ptr-color cptr`）：

```llvm
define dso_local i32 @sum2() #2 {
entry:
  %x = alloca i32, align 4, !goc.color !22, !goc.prov !23   ; !22 = !{!"sptr"}  !23 = !{!"stack"}
  ...
define dso_local void @fill(ptr noundef %p) #0 {
entry:
  %p.addr = alloca ptr, align 8, !goc.color !14, !goc.prov !15
  store ptr %p, ptr %p.addr, align 8
  %0 = load ptr, ptr %p.addr, align 8, !goc.color !14, !goc.prov !17
```

`%x` 是局部变量的地址 → `sptr`（来源 `stack`）。O3 之后 `q` 被消掉，`sum2` 直接把 `%x` 传给 `bump`：

```llvm
define dso_local i32 @bump(ptr nocapture noundef %p) #1 {
entry:
  call void @fill(ptr noundef %p) #4
  %0 = load i32, ptr %p, align 4
  %add = add i32 %0, 1
  ret i32 %add
}
```

### 3.2 `goc-reanchor`：把指针实参放进栈图里

`goc-stackmap` 在这个例子里没有插入记录（没有 SSA 栈指针跨调用存活；指针实参归 `goc-reanchor` 管）。`goc-reanchor` 的输出：

```llvm
define dso_local i32 @bump(ptr nocapture noundef %p) #1 !goc.nextslot !17 {
entry:
  %goc.arganchor = alloca ptr, align 8, !goc.frame.words !19, !goc.root !20, !goc.slot !21
  store volatile ptr %p, ptr %goc.arganchor, align 8
  %goc.oldfp = alloca i64, align 8
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 5138399369279569921, i32 0, ptr %goc.arganchor)
  call void @fill(ptr noundef %p) #5
  %goc.reanchor = load volatile ptr, ptr %goc.arganchor, align 8
  %0 = load i32, ptr %goc.reanchor, align 4
  %add = add i32 %0, 1
  ret i32 %add
}
```

1. 新建槽 `%goc.arganchor`（`!goc.root !{"arganchor"}`），入口处 `store volatile` 把 `p` 存进去。
2. 调用前插一条 stackmap，**操作数是槽的地址**（不是 `p` 的值）。ID `5138399369279569921` = `0x474F430080000001`，高位是 ASCII `"GOC"`。
3. 调用后从槽里 `load volatile` 重新读出 `p`。搬栈时 Go 会改槽里的值，重读就拿到新地址。
4. `%goc.oldfp` 是给 goc-llc 帧地址修复用的旧帧指针槽（04 篇）。

### 3.3 goc-llc → `body.s`（x86-64）

```asm
bump.impl:
.LBB1_0:
	movq	%fs:-8, %r11            # g（Go linux/amd64 把 g 放在 TLS 的 FS:-8）
	cmpq	16(%r11), %rsp          # SP vs g.stackguard0（偏移 16，来自 AbiProfiles.def）
	jbe	.LBB1_2                     # 不够 → 冷块
.Lgoc_body0:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$16, %rsp
	movq	%rdi, -8(%rbp)              # p → goc.arganchor，在 BP-8
.Lgoc_sm0:                          # stackmap 记录位置
.Lgoc_call0:
	callq	fill.impl
.Lgoc_ret0:                         # 栈图挂在返回地址上
	movq	-8(%rbp), %rax              # 调用后从槽里重读 p
	movl	(%rax), %eax
	incl	%eax
	addq	$16, %rsp
	popq	%rbp
	retq
.LBB1_2:                            # 冷块：GocStackCheck + GocX86StackCheck
.Lgoc_stub0:
	pushq	%rbp
	pushq	%rax
	pushq	%rdi
	...                             # rsi rdx rcx r8 r9 rbx r12 r13 r15 r14
	subq	$128, %rsp
	movdqu	%xmm0, (%rsp)
	...                             # xmm1..xmm7
	movq	%fs:-8, %r14                # morestack 要求 g 在 R14
.Lgoc_call1:
	callq	runtime.morestack_noctxt
.Lgoc_ret1:
	...                             # 恢复全部寄存器，跳回 .LBB1_0 重新检查
```

`.Lgoc_*` 标签都是 goc-llc 的机器 pass 插的，`body.goc.s` 里的表就用这些标签相减得到偏移。

### 3.4 `.gocobj` 里的 `bump.impl`

```
func bump.impl size=211 frame=24 args=0 abi=0 kind=0 flags=0x9 id=0 file=0 line=6 sysvptr=0x1
  pcvalue SP_DELTA: 0=0 15=24 46=0 47=8 48=16 ... 72=232 187=104 ... 206=0
  pcvalue UNSAFE_POINT: 0=-2 15=-1 46=-2
  pcvalue FILE: 0=0 23=0 32=0
  pcvalue LINE: 0=6 23=7 32=8
  gcmaps args=0 locals=7
    pc=32 args= locals=40
    pc=133 args= locals=20
  stackmap +27 id=0x474f430080000001 locs=[{2 8 6 -8}]
  reloc +28 w=4 CALL fill.impl-4 flags=0x1
  reloc +129 w=4 CALL runtime.morestack_noctxt-4 flags=0x1
```

- `flags=0x9` = bit0 needs_stack_check + bit3 has_frame_pointer。
- `sysvptr=0x1`：入口时 RDI 装的是指针（第一个 SysV 整数参数寄存器）。
- `UNSAFE_POINT: 0=-2 15=-1 46=-2`：栈检查区（pc 0–14）和冷块（≥46）是不可抢占点（Go 的 `abi.UnsafePointUnsafe` = -2），函数体可抢占。
- `stackmap … locs=[{2 8 6 -8}]`：类型 2（Direct）、8 字节、DWARF 寄存器 6（RBP）、偏移 -8，就是 `goc.arganchor`。
- `gcmaps … pc=32 locals=40`：返回地址 pc=32（调用 `fill.impl` 之后）处，locals 位图是 `0x40`，只有一位为 1，对应那个槽。`pc=133` 是 morestack 调用点自己的位图。
- 两个 reloc 的 `flags=0x1` = safepoint_call。`bump` 调 `fill.impl` 和 morestack 都是 safepoint。
- `sum2.impl` 的两个 gcmaps 位图都是 `00`：`%x` 存的是 `int`，不是指针，帧里没有需要修正的字。

`goc-objdump verify out/call.gocobj` 输出 `ok`。

### 3.5 Go 侧

```
$ go tool nm out/call.goobj | grep -E ' T '
    18d7 T main.bump
    1719 T main.bump.impl
    18c1 T main.fill
    170d T main.fill.impl
    18ed T main.sum2
    17ec T main.sum2.impl
$ go tool objdump -s 'main.bump$' out/call.goobj
TEXT main.bump(SB)
  :1  0x18d7  55          PUSHQ BP
  :1  0x18d8  4889e5      MOVQ SP, BP
  :1  0x18db  4889c7      MOVQ AX, DI
  :1  0x18de  e800000000  CALL 0x18e3  [1:5]R_CALL:main.bump.impl
  :1  0x18e3  66450fefff  PXOR X15, X15
  :1  0x18e8  4889ec      MOVQ BP, SP
  :1  0x18eb  5d          POPQ BP
  :1  0x18ec  c3          RET
```

ELF 的 `R_X86_64_PLT32` 已经按 `gover_go1_24.go` 的表换成了 Go 的 `R_CALL`。

## 4. 同一个文件编 arm64（`GOC_ARCH=arm64`）

```asm
bump.impl:
.LBB1_0:
	ldr	x16, [x28, #16]             # g 在 x28（-ffixed-x28 / +reserve-x28），g.stackguard0
	cmp	sp, x16
	b.ls	.LBB1_2
.Lgoc_body0:
	sub	sp, sp, #48
	stp	x29, x30, [sp, #32]
	add	x29, sp, #32
	str	x30, [sp]                   # GocA64SaveLR：LR 副本放在 0(SP)，Go 的 arm64 回溯从这里读
	stur	x0, [x29, #-8]              # p → goc.arganchor
.Lgoc_sm0:
	mov	x16, sp
	str	x16, [sp, #8]               # 调用前存旧 SP 到 8(SP)
.Lgoc_call0:
	bl	fill.impl
.Lgoc_ret0:
	ldr	x9, [sp, #8]                # GocA64FrameAddrFix：算出搬栈距离，
	mov	x16, sp                     #   把落在旧栈 [旧SP, 旧 g.stack.hi) 里的
	sub	x16, x16, x9                #   保存帧指针（[sp,#32]）加上这个距离
	ldr	x17, [x28, #8]
	sub	x17, x17, x16
	ldr	x11, [sp, #32]
	add	x10, x11, x16
	cmp	x11, x9
	ccmp	x11, x17, #2, hs
	csel	x11, x10, x11, lo
	str	x11, [sp, #32]
	ldur	x8, [x29, #-8]              # 从槽里重读 p
	...
```

这一版 arm64 的 `body.ll` 里有 2 条 `llvm.experimental.stackmap`，即 arm64 也记录栈图（与部分旧注释不一致，见 07 篇）。

## 5. 一个 uptr 例子

`sptr` 存进全局 `cptr` 槽时，颜色 pass 不报错，而是自动编码成 `uptr`：

```c
#include "goc.h"
int g_val;
cptr(int) g_slot;            /* a non-stack cptr slot */
void keep(sptr(int) p) { g_slot = p; }   /* sptr stored into cptr -> uptr encode */
```

`a.ll` 里颜色以 annotate 形式进 IR：

```llvm
@.str = private unnamed_addr constant [15 x i8] c"goc.color.cptr\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ... } { ptr @g_slot, ptr @.str, ptr @.str.1, i32 3, ptr null }], section "llvm.metadata"
```

`goc-color-escape` 之后：

```llvm
define dso_local void @keep(ptr noundef %p) #0 {
entry:
  %p.addr = alloca ptr, align 8, !goc.color !14, !goc.prov !15              ; sptr, stack
  store ptr %p, ptr %p.addr, align 8
  %0 = load ptr, ptr %p.addr, align 8, !goc.color !14, !goc.prov !15
  %goc.uptr.store = call ptr @goc_uptr_from_ptr(ptr %0), !goc.color !18, !goc.uptr_encoded !19   ; uptr
  store ptr %goc.uptr.store, ptr @g_slot, align 8
  ret void
}
```

`goc_uptr_from_ptr` 把栈地址编码成「MSB=1 + 相对 `g.stack.hi` 的偏移」（06 篇），搬栈后再解码仍然正确。
`goc-inline-gate` 会给这类 helper 一个 `alwaysinline` 的函数体（`SMUptrInline.cpp`），O3 把它内联掉。
