# 走一遍：从 add.c 到二进制

[ARCHITECTURE](../../ARCHITECTURE.md) · [前端与 IR pass →](frontend-ir.md)

本篇所有输出都是用 `bin/goc`（Go 版 CLI）实际跑出来的（x86-64 Linux，Go 1.24）。复现方法：

```bash
# 产品路径：一条命令编整个包
bin/goc go examples/hello -o /tmp/hello && /tmp/hello        # → hello 42

# 单文件，保留中间产物
bin/goc build foo.c -o out/foo.goobj -O3 --default-ptr-color cptr --keep-tmp=/tmp/k
#   /tmp/k：a.ll a.color.ll a.gate.ll a.opt.ll a.sm.ll a.reanchor.ll
#           body.ll body.impl.ll thunks.s thunks.goc.s goabi.json body.s body.goc.s body.thunks.s body.llc.o
#   out/：foo.goobj foo.gocobj foo.symabis foo.ll
```

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

`goc go`（`cmd/goc/gocmd.go`）做的事（按顺序）：

1. 默认参数 `-O3 --default-ptr-color cptr --fast-stack-alloca`，命令行可以覆盖。goc 不读任何 `GOC_*` 环境变量。
2. `GOOS=linux`，`GOARCH` 取 `--arch`（默认 amd64），`CGO_ENABLED=0`。
3. 包里没有 `.s` 时写一个 `goc_marker_amd64.s`（只有一个 `TEXT ·gocMarker`）。Go 只有在包里有汇编文件时才接受无体声明。
4. 对每个 `*.c` 在进程内跑一遍 `goc build` 的管线，输出到 `build/goc-go/<hash>/<base>.goobj`。
5. `go build -a -toolexec "goc toolexec -pkg main <所有 .goobj> --"`。

实际日志：

```
goc go: -O3 --default-ptr-color cptr --fast-stack-alloca --pkg main
goc go: wrote /tmp/wt/hello/goc_marker_amd64.s
goc go: compile /tmp/wt/hello/add.c
goc build: clang=/workspace/goc/third_party/llvm-19.1.7-clang-build/bin/clang
goc-color-escape: default-ptr-color=cptr
goc-color-escape: summary: 0 error(s), scoped dynamic allocas=0, implicit uptr stores=0, decoded pointer loads=0, guarded returns=0 in module '/tmp/goc-build4011883560/a.ll'
goc build: O3 inlined before stack maps
realbody: override-stack-alignment=8 (no aligned SSE on the C stack)
goabi: 1 rewritten, 0 left SysV-only
goc build: wrote /workspace/goc/build/goc-go/98af4de2ac0aff45/add.goobj
# goc.local/hello
toolexec: appended goc symabis from add.symabis
# goc.local/hello
toolexec: packed add.goobj into $WORK/b001/_pkg_.a
goc go: /tmp/wt/hello.bin
hello 42
```

## 2. 逐阶段：`hello_add`（叶子函数）

### 2.1 clang → `a.ll`

`internal/pipeline` 的 clang 参数（`frontend`）：`-mno-red-zone -fno-stack-protector -fno-asynchronous-unwind-tables -I include -emit-llvm -S -O3 -gline-tables-only`，
`-O>0` 时再加 `-fwrapv -fno-strict-aliasing -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls -Xclang -disable-llvm-passes`。

为什么：
- **不要红区**：红区是 SP 下面 128 字节「不用调整 SP 就能用」的区域；Go 的抢占信号和搬栈都不认它。
- **`-disable-llvm-passes`**：clang 只出未优化 IR。颜色分析必须在 SROA/内联之前看到原始的指针存储；O3 由 `internal/pipeline` 自己之后跑。
- **保留帧指针**：goc 的栈图用 `BP - off` 描述槽（见 [objects-link.md](objects-link.md) 的 `sptr_slots`）。

```llvm
define dso_local i32 @hello_add(i32 noundef %0, i32 noundef %1) #0 !dbg !9 {
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  store i32 %0, ptr %3, align 4
  store i32 %1, ptr %4, align 4
  %5 = load i32, ptr %3, align 4, !dbg !12
  %6 = load i32, ptr %4, align 4, !dbg !13
  %7 = add i32 %5, %6, !dbg !14
  ret i32 %7, !dbg !15
}
```

Release 版 clang 不保留 IR 值名，所以参数和局部变量都是编号（`%0`、`%3`）。pass 之间靠 metadata 认槽，不靠名字。

### 2.2 opt 各 pass → `a.reanchor.ll`

没有指针、没有调用，`goc-color-escape`/`goc-stackmap`/`goc-reanchor` 都没东西可做，O3 把它化简成：

```llvm
define dso_local noundef i32 @hello_add(i32 noundef %0, i32 noundef %1) #0 !dbg !10 {
  %3 = add i32 %1, %0, !dbg !13
  ret i32 %3, !dbg !14
}
```

### 2.3 Go ABI 改写：改名 + 生成 thunk

`internal/pipeline` 调 `lower.WriteGoABI(body.ll → body.impl.ll, thunks.s, goabi.json, thunks.goc.s)`：C 本体改名为 `hello_add.impl`（SysV 调用约定），
另生成一个叫 `hello_add` 的 **Go ABIInternal 入口**（`internal/lower/goabi_x86.go`）。Go 的 ABIInternal 第 1、2 个整数参数在 RAX、RBX，SysV 在 RDI、RSI：

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

### 2.5 `.gocobj`：用 `goc tool objdump` 看

```
$ goc tool objdump dump out/add.gocobj
gocobj 1.4 arch=1 flags=0x1 profile="goabi1-amd64" producer="goc-llc (LLVM 19.1.7)"
file 0 add.c
file 1 /tmp/wt/add.c
func hello_add.impl size=9 frame=8 args=0 abi=0 kind=0 flags=0xe id=0 file=1 line=3 sysvptr=0x0
  pcvalue SP_DELTA: 0=8
  pcvalue FILE: 0=1
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
     7f4 R gclocals.goc.main.hello_add.args
     7fd R gclocals.goc.main.hello_add.locals
     7db T main.hello_add
     7d2 T main.hello_add.impl
$ cat out/add.symabis
def main.hello_add.impl ABI0
def main.hello_add ABIInternal
```

`--pkg main`（转换器的 `-p`）让符号带上包前缀 `main.`。`symabis` 告诉 Go 的 asm/compile：`main.hello_add` 是 ABIInternal 定义，于是 `main.go` 里的无体声明直接链到它，不再要 ABI 包装。

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
define dso_local i32 @sum2() #1 !dbg !26 {
  %1 = alloca i32, align 4, !goc.color !27, !goc.prov !14   ; !27 = !{!"sptr"}  !14 = !{!"stack"}
  %2 = alloca ptr, align 8, !goc.color !27, !goc.prov !14
  ...
define dso_local void @fill(ptr noundef %0) #0 !dbg !10 {
  %2 = alloca ptr, align 8, !goc.color !13, !goc.prov !14   ; !13 = !{!"cptr"}
  store ptr %0, ptr %2, align 8
  %3 = load ptr, ptr %2, align 8, !dbg !15, !goc.color !13, !goc.prov !16   ; !16 = !{!"cheap"}
```

`%1`（`x`）是局部变量的地址 → `sptr`（来源 `stack`）。O3 之后 `q` 被消掉，`sum2` 直接把 `x` 的地址传给 `bump`：

```llvm
define dso_local i32 @bump(ptr nocapture noundef %0) #1 !dbg !15 {
  call void @fill(ptr noundef %0) #4, !dbg !16
  %2 = load i32, ptr %0, align 4, !dbg !17
  %3 = add i32 %2, 1, !dbg !18
  ret i32 %3, !dbg !19
}
```

### 3.2 `goc-reanchor`：把指针实参放进栈图里

`goc-stackmap` 在这个例子里没有插入记录（没有 SSA 栈指针跨调用存活；指针实参归 `goc-reanchor` 管）。`goc-reanchor` 的输出：

```llvm
define dso_local i32 @bump(ptr nocapture noundef %0) #1 !dbg !15 !goc.nextslot !16 {
  %goc.arganchor = alloca ptr, align 8, !dbg !17, !goc.frame.words !18, !goc.root !19, !goc.slot !20
  store volatile ptr %0, ptr %goc.arganchor, align 8, !dbg !17
  %goc.oldfp = alloca i64, align 8, !dbg !17
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 5138399369279569921, i32 0, ptr %goc.arganchor), !dbg !17
  call void @fill(ptr noundef %0) #5, !dbg !17
  %goc.reanchor = load volatile ptr, ptr %goc.arganchor, align 8, !dbg !21
  %2 = load i32, ptr %goc.reanchor, align 4, !dbg !21
  %3 = add i32 %2, 1, !dbg !22
  ret i32 %3, !dbg !23
}
```

1. 新建槽 `%goc.arganchor`（`!goc.root !{"arganchor"}`），入口处 `store volatile` 把 `p`（`%0`）存进去。
2. 调用前插一条 stackmap，**操作数是槽的地址**（不是 `p` 的值）。ID `5138399369279569921` = `0x474F430080000001`，高位是 ASCII `"GOC"`。
3. 调用后从槽里 `load volatile` 重新读出 `p`。搬栈时 Go 会改槽里的值，重读就拿到新地址。
4. `%goc.oldfp` 是给 goc-llc 帧地址修复用的旧帧指针槽（[codegen.md](codegen.md)）。

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
func bump.impl size=211 frame=24 args=0 abi=0 kind=0 flags=0x9 id=0 file=1 line=6 sysvptr=0x1
  pcvalue SP_DELTA: 0=0 15=24 46=0 47=8 48=16 ... 72=232 187=104 ... 206=0
  pcvalue UNSAFE_POINT: 0=-2 15=-1 46=-2
  pcvalue FILE: 0=1 23=1 32=1
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
- `sum2.impl` 的两个 gcmaps 位图都是 `00`：`x` 存的是 `int`，不是指针，帧里没有需要修正的字。

`goc tool objdump verify out/call.gocobj` 输出 `out/call.gocobj: ok`。

### 3.5 Go 侧

```
$ go tool nm out/call.goobj | grep -E ' T '
    18ed T main.bump
    172f T main.bump.impl
    18d7 T main.fill
    1723 T main.fill.impl
    1903 T main.sum2
    1802 T main.sum2.impl
$ go tool objdump -s 'main.bump$' out/call.goobj
TEXT main.bump(SB) /tmp/wt/call.c
  call.c:1  0x18ed  55          PUSHQ BP
  call.c:1  0x18ee  4889e5      MOVQ SP, BP
  call.c:1  0x18f1  4889c7      MOVQ AX, DI
  call.c:1  0x18f4  e800000000  CALL 0x18f9  [1:5]R_CALL:main.bump.impl
  call.c:1  0x18f9  66450fefff  PXOR X15, X15
  call.c:1  0x18fe  4889ec      MOVQ BP, SP
  call.c:1  0x1901  5d          POPQ BP
  call.c:1  0x1902  c3          RET
```

ELF 的 `R_X86_64_PLT32` 已经按 `internal/convert/relocmap.go` 的表换成了 Go 的 `R_CALL`。

## 4. 同一个文件编 arm64（`--arch arm64`）

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

arm64 的 `body.ll` 里同样有 2 条 `llvm.experimental.stackmap`：arm64 也记录栈图。

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
@.str.2 = private unnamed_addr constant [15 x i8] c"goc.color.cptr\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @g_slot, ptr @.str.2, ptr @.str.1, i32 3, ptr null }], section "llvm.metadata"
```

`goc-color-escape` 之后：

```llvm
define dso_local void @keep(ptr noundef %0) #0 !dbg !10 {
  %2 = alloca ptr, align 8, !goc.color !13, !goc.prov !14              ; sptr, stack
  store ptr %0, ptr %2, align 8
  call void @llvm.var.annotation.p0.p0(ptr %2, ptr @.str, ptr @.str.1, i32 4, ptr null)
  %3 = load ptr, ptr %2, align 8, !dbg !15, !goc.color !13, !goc.prov !14
  %goc.uptr.store = call ptr @goc_uptr_from_ptr(ptr %3), !dbg !16, !goc.color !17, !goc.uptr_encoded !18   ; uptr
  store ptr %goc.uptr.store, ptr @g_slot, align 8, !dbg !16
  ret void, !dbg !19
}
```

`goc_uptr_from_ptr` 把栈地址编码成「MSB=1 + 相对 `g.stack.hi` 的偏移」（[runtime.md](runtime.md) §4），搬栈后再解码仍然正确。
`goc-uptr-inline` 会给这类 helper 一个 `alwaysinline` 的函数体（`SMUptrInline.cpp`），O3 把它内联掉。
