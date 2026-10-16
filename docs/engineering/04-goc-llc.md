# 04 goc-llc 与机器 pass

[← 03 IR pass](03-ir-passes.md) · [索引](../engineering-architecture.md) · [下一篇：.gocobj 与链接 →](05-gocobj-and-link.md)

## 1. 为什么要自己的 llc

IR pass 可以用插件加载，但 **LLVM 19 的 `llc` 不能从插件往代码生成管线里插 MachineFunction pass**。
goc 需要的几件事只有在机器指令层面才做得对：

- 栈检查必须看到**最终帧大小**（寄存器分配和 PEI 之后才知道），而且不能放在 IR 里——IR 里的检查会被内联复制到调用者，看到错的 SP。
- 帧地址修复必须在寄存器分配之后：只有那时才知道哪个值被放进了被调用者保存寄存器。
- `.goc.*` 表要引用最终指令位置。

所以 `llvm/tools/goc-llc/goc_llc.cpp` 复刻了 `LLVMTargetMachine::addPassesToEmitFile`（同样的 `TargetPassConfig`、`cl::opt`、`TargetOptions`），在末尾多加几个 pass。
它链接的是 `third_party/llvm-19.1.7-clang-build` 里的 libLLVM（`llvm/CMakeLists.txt`，`scripts/build-passes.sh`）。

## 2. pass 顺序（`goc_llc.cpp` 的 `main`）

```
addISelPasses()
[arm64] GocA64CallFrame              createA64CallFramePass()        lib/Target/AArch64/GocAArch64Frame.cpp
addMachinePasses()                   ← LLVM 自己的 RA、PEI、BranchFolding…
[arm64] GocA64FrameAddrFix           createA64FrameAddrFixPass()
[arm64] GocA64SaveLR                 createA64SaveLRPass()
GocStackmapPlacement                 createGocStackmapPlacementPass() lib/CodeGen/GocStackmapPlacement.cpp
[x86_64] GocX86FrameAddrFix          createGocX86FrameAddrFixPass()   lib/Target/X86/GocX86FrameAddrFix.cpp
GocStackCheck(+ X86/AArch64 钩子)    createGocStackCheckPass(SO, createX86StackCheckTarget() | createAArch64StackCheckTarget())
GocObjTables（有 -goc-tables 时）    createGocObjTablesPass(path, profile, symABI)  lib/Object/GocObjTables.cpp
AsmPrinter
```

goc-llc 自己的选项：

| 选项 | 默认 | 含义 |
|---|---|---|
| `-goc-tables FILE` | 空 | 把 `.goc.*` 段以汇编文本写到 FILE（`realbody.sh` 写 `body.goc.s`） |
| `-goc-profile NAME` | `go1.24-amd64`；arm64 目标且未指定时自动 `go1.24-arm64` | ABI profile（`AbiProfiles.def`），名字的架构必须和目标一致 |
| `-goc-sym-abi 0\|1` | 0 | 表里 C 函数的 Go 符号 ABI：0 = ABI0（有 thunk 包着，或纯 C 对象），1 = ABIInternal |
| `-goc-fixed-g` | off | g 常驻 profile 的 G 寄存器（配合 `-reserve-goc-r14`） |
| `-goc-no-stack-check` | off | 不发栈检查（仅测试） |

另外 goc-llc 在用户没给时自动追加 `-enable-tail-merge=false`。

`realbody.sh` 传给 goc-llc 的参数（amd64）：
`-O<n> -relocation-model=pic -march=x86-64 -frame-pointer=all -enable-shrink-wrap=false -disable-tail-calls -no-x86-call-frame-opt -enable-tail-merge=false -tail-dup-pred-size=1000 -tail-dup-succ-size=1000 -function-sections -goc-sym-abi=N -goc-tables body.goc.s -filetype=asm`。
每一项都有理由（注释在 `realbody.sh`）：

| 参数 | 为什么 |
|---|---|
| `-frame-pointer=all` | 每次调用时 RBP 有效，栈图用 RBP 偏移 |
| `-enable-shrink-wrap=false` | 序言必须在入口，栈检查在序言之前 |
| `-disable-tail-calls` | 尾调用会让帧在 Go 看不到的地方消失 |
| `-no-x86-call-frame-opt` | 否则保留的出参区会变成调用前后的 PUSH/POP，SP 不再是 pcsp 声称的常量，回溯器会把 g 当返回地址读 |
| `-enable-tail-merge=false` | 尾合并会把两个块共享的 CALL 提到公共尾部，stackmap 记录留在 JMP 前，挂到另一条路径的 CALL 上 |
| `-tail-dup-*-size=1000` | 打开尾复制（QuickJS 分派块，性能） |
| `-function-sections` | 每个函数一个段，函数间引用全部带 ELF 重定位，转换器不用修补机器码 |

arm64 换成：`-relocation-model=static -mtriple=aarch64-unknown-linux-gnu -mattr=+reserve-x28 -aarch64-enable-compress-jump-tables=false … -goc-profile=go1.24-arm64`（没有 `-no-x86-call-frame-opt`）。

## 3. `GocStackmapPlacement`（lib/CodeGen）

- **输入**：机器码里的 `STACKMAP` 伪指令。
- **规则**：每个 `STACKMAP` 后面、同一个机器块里必须有 CALL；没有就删掉。
- **为什么**：`GocObjTables` 把记录挂到布局顺序上的下一个 CALL。被当成 safepoint 但被 ISel 展开成指令的 intrinsic（如 `llvm.umul.with.overflow`）后面没有 CALL，记录会挂到别的路径上，那里根槽可能还没初始化。
- 调试：`GOC_STACKMAP_PLACEMENT_TRACE`。

## 4. 栈检查：共享策略 + 架构钩子

接口在 `llvm/include/Goc/StackCheck.h`，两层：

| 层 | 文件 | 负责 |
|---|---|---|
| 共享（决策） | `llvm/lib/CodeGen/GocStackCheck.cpp` | 哪些函数要检查、选档位、建检查块和冷块、记录表要用的信息（`FuncStackInfo`：`BodyStart`、`StubStart`、`SPChanges`、`MorestackCall`、`StubSlots`、`NoSplit`） |
| 钩子（发指令） | `GocX86StackCheck.cpp`、`GocAArch64StackCheck.cpp`，实现 `StackCheckTarget` | `emitCheck`、`emitStub`、`stubSlots`、`emitTrap` |

决策（`GocStackCheck.cpp`）：
- 帧大小 ≤ 0，或「C 入口 + 帧 ≤ StackSmall + 不发出调用」→ **nosplit**，不检查（由调用者的 guard 余量覆盖，链接器的 nosplit 检查计账）。
- 否则按帧大小选档：`Small`（≤ StackSmall=128）、`Big`（≤ StackBig=4096）、`Huge`。常量来自 `AbiProfiles.def`。
- 「调用」包括内联汇编里写的 `call`（goc 的 shim 用这种方式调 Go 和 libc 帮助函数）。

x86-64 钩子发的指令（文件头注释原文的整理）：

```
check（SysV C 入口，R10/R11 可用）：
    movq %fs:-8, %r11              # g；--fixed-g 时 g 已在 R14
  small: cmpq 16(%r11), %rsp
  big:   leaq -(frame-128)(%rsp), %r10 ; cmpq 16(%r11), %r10
  huge:  movq %rsp, %r10 ; subq $(frame-128), %r10 ; jb stub ; cmpq 16(%r11), %r10
    jbe stub
冷块：先存帧指针（运行时在 varp 看到保存的 BP），再存 RAX 和 6 个 C 参数寄存器，
      再存 RBX R12 R13 R15 R14 和 XMM0–7；g 装进 R14；call runtime.morestack_noctxt；
      全部恢复；跳回 check。
```

为什么冷块要存这么多：morestack 会破坏 Go 不当成被调用者保存的所有寄存器，而 C 代码期望 RBX/R12–R15 跨调用不变。
冷块里保存的参数寄存器有自己的指针位图（`stubSlots`），所以搬栈时实参里的栈指针也会被修正（02 篇 `pc=133 locals=20`）。

AArch64 钩子：

```
check（X16/X17 是过程内临时寄存器；g 在 X28，全 TU 保留）：
    ldr x16, [x28, #16]            # g.stackguard0
  small: cmp sp, x16
  big:   sub x17, sp, #(frame-128) ; cmp x17, x16
  huge:  movz/movk x17 ; subs x17, sp, x17 ; b.lo stub ; cmp x17, x16
    b.ls stub
冷块：STP 成对存 X0–X9，留 128 字节存 Q0–Q7，返回地址放 X3 交给 morestack（和 Go 自己的 arm64 序言一样），
      BL runtime.morestack_noctxt，恢复，B 回 check。
```

AArch64 钩子按名字在目标自己的表里查 opcode 和寄存器，不需要 AArch64 私有头文件或 `.inc`，只要 goc-llc 链接的 libLLVM 含 AArch64 后端。

## 5. x86 帧地址修复：`GocX86FrameAddrFix`

文件：`GocX86FrameAddrFix.cpp`（修复）、`GocX86FrameAddrLattice.cpp`（数据流格）、`GocX86FrameAddrFix.h`。post-RA，AsmPrinter 之前。

**问题**：一个局部变量的地址 `RBP+c`，寄存器分配器可能把它放在被调用者保存寄存器（RBX、R12–R15）或溢出槽里跨过一次调用。
调用里如果搬了栈，morestack 冷块和被调用者尾声会**原样**恢复这些寄存器；Go 的搬栈只改栈图标记过的字。于是调用后寄存器里还是旧栈地址。

**它维护的不变量**（源码原文）：

> After every call, no callee-saved register and no spill slot that is live holds a frame address computed before the call.

做法（源码注释的整理）：
1. 前向数据流：每个寄存器单元和溢出槽的状态是
   `FA(base,c)`（确定是 RBP/RSP + 常量）、`D1`（随栈移动但偏移未知，如 `&a[i]`）、`MS`（可能是栈地址）。
2. 每次调用后：活着的 `FA` 寄存器 / 槽用 `LEA` 重新算（槽借死掉的 R11 再存回）。
3. `D1` / `MS` 用 `delta = RBP_after - RBP_before` 平移。`RBP_before` 在调用前存进 `goc.oldfp`（`goc-reanchor` 建的槽，不在栈图里，搬栈时原样复制）。平移放在冷块里，只在 `delta != 0` 时进入，热路径每个调用一次存储一次比较。
4. 从根槽（`goc.arganchor`、`goc.anchor`、`goc.spill.root`）复制到 CSR 的值：调用后从槽重读（Go 已经改过槽）。
5. 系数未知的派生值（`p - x`、`p * k`）是 tainted，无法修复，计数后打印 note；`GOC_FRAMEADDR_TAINT=report` 列出，`=fatal` 让编译失败。
6. 非调用指令写 RSP（动态 alloca）或 RBP，会让基于它的 FA 状态变成 tainted。

调试环境变量：`GOC_FRAMEADDR_FIX=0`（不加这个 pass）、`GOC_FRAMEADDR_TRACE=<函数>`、`GOC_FRAMEADDR_STATS`、`GOC_FRAMEADDR_DEBUG`。
`realbody.sh` 会 `grep -a goc-frameaddr-fix` 检查 LLC 二进制，没有就拒绝——普通 `llc-19` 会把 GEP CSE 到寄存器里并跨调用保留。

## 6. arm64 专属：`GocAArch64Frame.cpp`

Go 的 arm64 回溯从每个帧的 `0(RSP)` 读返回地址（每个帧把自己的 LR 存在底部）；LLVM 把 LR 存在帧顶的 frame record 里。三个 pass（`llvm/include/Goc/AArch64Frame.h`）：

| pass | 时机 | 做什么 |
|---|---|---|
| `GocA64CallFrame` | PEI 之前 | 每个调用帧多 16 字节，出栈参数整体上移 16，使 `[sp, sp+16)` 在每次调用时空着；读入栈参数时也偏 16 |
| `GocA64SaveLR` | PEI 之后 | 序言建好帧后 `str x30, [sp]` |
| `GocA64FrameAddrFix` | PEI 之后 | 调用前把旧 SP 存到 `8(RSP)`（LR 区的填充字）；调用后对每个可能存着帧派生值的活 CSR / 溢出槽做和 copystack `adjustpointer` 相同的修正：`old.lo <= v < old.hi ? v + delta : v`，`delta = SP - oldSP`，`old.hi = g.stack.hi - delta` |

02 篇 §4 有实际生成的 arm64 汇编。arm64 的其它差别：g 在 X28（clang `-ffixed-x28`、llc `+reserve-x28`），没有 TLS 读取；
`-relocation-model=static` + clang `-fno-pic`，外部数据用 ADRP+ADD/LDR，不经过 GOT；跳转表不压缩（goc-llc 在压缩之后插代码，会把表项撑大）；不跑 `goc-lower align-asm`（没有 SSE，SP 本来就 16 字节对齐）。

## 7. `GocObjTables`（lib/Object）

最后一个机器 pass。`GocObjTables.cpp` 逐函数收集：在每个调用后、每个函数尾放标签（`.Lgoc_call*`、`.Lgoc_ret*`、`.Lgoc_end*`、`.Lgoc_sm*`、`.Lgoc_ln*`…），
收集 STACKMAP、重定位、行号、内联树、sptr 槽（`!goc.frame.words` 指到的 (alloca, offset)，以及直接存入 sptr 值的 alloca）。
模块结束时 `GocObjTablesWriter.cpp` 把这些段**以汇编指令写出**（`.uleb128 .Lgoc_ret0-bump.impl` 这种标签差），
`realbody.sh` 把它接在函数汇编后面，由 `llvm-mc` 算出所有偏移——于是一个 ELF 就是 `.gocobj`。

写出的段：`.goc.header` `.goc.strtab` `.goc.files` `.goc.funcs` `.goc.relocs` `.goc.stackmaps` `.goc.gcmaps` `.goc.pcvalue` `.goc.inltree` `.goc.data`。
thunk 的表（`.goc.funcs` `.goc.wrappers` 等）由 `goobj/lower/goabi.go` 写进 `thunks.goc.s`。格式见 05 篇。

架构差别只在这里一点点：x86 的调用重定位记在返回地址前 4 字节（CALL，addend −4）；AArch64 的 BL 记在指令本身（ARM64_CALL26，addend 0）。

## 8. 之后：汇编 → `.gocobj`

`realbody.sh`：
1. amd64：`goc-lower align-asm body.s` 把 LLVM 仍选出的对齐 SSE 访存（`movaps` 等）改成非对齐（没有栈重对齐，帧只有 8 字节对齐）。
   相关的 IR 侧处理在 `goc-lower fix-ir`：给每个定义的函数加 `no-realign-stack`，x86 再加 `override-stack-alignment=8`（日志里的 `realbody: override-stack-alignment=8`）。
2. `cat body.s thunks.s body.goc.s thunks.goc.s > body.thunks.s`。
3. `llvm-mc -filetype=obj -triple=x86_64-unknown-linux-gnu`（或 aarch64）→ `body.llc.o`，复制为 `foo.gocobj`。不调用 `ld`。
