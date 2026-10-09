# 使用指南

实验性编译器。只支持 linux/amd64。不是 cgo，也不能把任意 C 丢进去就链进 Go。

语言合同是 [syntax-guide.md](syntax-guide.md)。这里只写第一次怎么编、怎么链、两边怎么对上。能跑通的完整例子是 [scripts/test-p29-goabi.sh](../scripts/test-p29-goabi.sh) 和 [tests/goabi/](../tests/goabi/)。

下一版要做的 arm64 后端见 [todo.md](todo.md)。现在换 `-march` 没有用。

从零开始、并且用命令行参数配置，见 [quickstart.md](quickstart.md)。下面是手动把单个 `.c` 编成 goobj 再链进 Go 的路径。

---

## 1. 要装什么

宿主编译器：`clang-19`、`clang++-19`、`llc-19`、`llvm-config-19`、`cmake`、`ninja`、`python3`、`rg`。Go 1.24 或更新。系统是 linux/amd64。

`goc` 自己没有单独的安装步骤。克隆仓库后，用仓库里的 `./cmd/goc`。

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

## 2. 编打过补丁的 Clang

产品前端是 Clang 19.1.7，加上指针色的 Sema。系统自带的 `clang-19` 不够。没有这个编译器，`goc build` 不能做链接用的对象文件。插件路径 `goc test --p27` 只做颜色检查，不产出可链接的 goobj。

```bash
curl -L -o /tmp/llvmorg-19.1.7.tar.gz \
  https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.tar.gz
tar -C "$HOME/src" -xf /tmp/llvmorg-19.1.7.tar.gz
export LLVM_SRC="$HOME/src/llvm-project-llvmorg-19.1.7"

./scripts/apply-patches.sh "$LLVM_SRC"

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  -S "$LLVM_SRC/llvm" \
  -B third_party/llvm-19.1.7-clang-build

ninja -C third_party/llvm-19.1.7-clang-build -j"$(nproc)" clang
```

Release 构建大约要几十分钟。构建目录在 `third_party/` 下，已被 gitignore，不要提交二进制。细节见 [clang/README.md](../clang/README.md)。

驱动会找到 `third_party/llvm-19.1.7-clang-build/bin/clang`。Clang 在别的路径时给命令加 `--clang`。

## 3. 确认驱动能用

```bash
./cmd/goc version
./cmd/goc test --p28
```

`--p28` 使用仓库里编出来的 Clang，或 `--clang` 指到的那一个。它编译一小批黄金样例，不链接 Go 程序。

`goc build` 第一次会自己编 `frontend/color-escape`。那一步用 `clang++-19` 和 `llvm-config-19` 链 `libLLVM-19`。运行 pass 时，驱动把 clang 那棵树的 `lib/` 加进本次进程。

## 4. 把 C 编成 Go 对象

```bash
./cmd/goc build tests/goabi/goabi.c -o build/p29-goabi/goabi.o --all --goabi \
  -O0 --no-default-ptr-color
```

这组和 [scripts/test-p29-goabi.sh](../scripts/test-p29-goabi.sh) 一致。`--goabi` 一定打开 morestack 前导和可分裂帧，没有开关。没有前导时，帧按 nosplit 核算，超过约 792 字节链接器拒绝。amd64 还会记录 sptr 栈图，搬栈时改写栈上的 `sptr`。arm64 记不了栈图。每个 Go 入口的 thunk 帧是该签名的栈参数，向上取整到 16，amd64 再加 8 字节的 `%rbp`。

| 参数 | 作用 |
|---|---|
| `--no-default-ptr-color` | 不给没注解的 `T*` 盖默认色，每个指针各自推断。 |

产物不是系统链接器能吃的 ELF `.o`。`gcc` 链它会失败。旁边的 `goabi.meta.json` 必须留下，打包时要读它。

`--all` 把这个翻译单元里每个有定义的函数都发出去。`--goabi` 隐含 `--all`。支持的签名会多一个 Go 入口 thunk；C 调用 C 仍走 SysV 体。

支持的签名：整数、指针、浮点，以及每个字段独占一个 eightbyte 的字面聚合。聚合参数要么整块进寄存器，要么整块上栈。结果不超过 16 字节。除此之外保持 SysV，Go 不能直接调用。变参、x87、异常、AVX 不在 Go 可调用路径上。

看符号：

```bash
go tool nm build/p29-goabi/goabi.o | rg 'goabi_add2'
```

应能看到 `main.goabi_add2` 和 `main.goabi_add2.impl`。前者是 Go 调用的 thunk，后者是 SysV 函数体。文件内 `static` 函数会再加翻译单元名，避免跨文件重名。

默认符号前缀是 `main.`。只有 Go 包也是 `package main` 时这才对得上。现在的打包脚本也只处理包路径正好是 `main` 的编译（见下一节）。

## 5. 链进 Go 程序

一条命令完成这件事用 `goc go`，见 [quickstart.md](quickstart.md)。它会写标记汇编、按包路径打包，并带上 `-a`。

下面是不经过 `goc go` 的黄金测试。在 `tests/goabi` 里：

```bash
cd tests/goabi
CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$PWD/../../build/p29-goabi/goabi.o" \
  go build -a \
  -toolexec "$PWD/../../backend/tools/toolexec_pack_goobj.sh" \
  -o "$PWD/../../build/p29-goabi/goabi_test" .
../../build/p29-goabi/goabi_test
```

或者直接：

```bash
./scripts/test-p29-goabi.sh
```

成功时输出里有 `PASS p29-goabi`。

自己写程序时，对照这四件事，缺一件就会链失败或链完是空符号：

1. Go 包必须是 `package main`。`backend/tools/toolexec_pack_goobj.sh` 只在 `-p main` 时把 goobj 打进包。别的包路径现在会被静默跳过。
2. 为每个要从 Go 调用的 C 函数写一个无函数体声明，名字和 C 一致。包里还得有至少一个 `.s` 文件，否则 Go 拒绝无函数体声明。`tests/goabi/stubs_amd64.s` 里的 `GoabiMarker` 就是为了满足这条，它不是 C 函数的桩。
3. `CGO_ENABLED=0`。这条路径是内部链接，C 里不能调用 libc。需要 libc 的是另一条、更窄的路，见第 8 节。
4. `go build -a`。不加 `-a`，增量编译可能不重跑 `asm`/`compile`，toolexec 就没有机会打包。`GOC_BINOBJ` 指向第 4 节的对象文件，可以是空格分隔的多个文件。

Go 侧声明和 C 侧对应关系：

```c
#include "goc.h"

int goabi_add2(int a, int b) { return a + b; }

int goabi_store(sptr(int) out, int v) {
  *out = v;
  return v * 2;
}
```

```go
package main

func goabi_add2(a, b int32) int32

//go:noescape
func goabi_store(out *int32, v int32) int32
```

C 的 `int` 是 32 位。Go 的 `int` 在 amd64 上是 64 位。按 C 类型写 Go 类型：`int` → `int32`，`long` → `int64`，`float` → `float32`，`double` → `float64`。

`//go:noescape` 只表示这次调用不保留指针。C 把指针存进堆或全局，是另一件事：把 `sptr` 存进普通 `T *` 或 `cptr` 的全局变量、堆字段时，编译器自动改用 `uptr` 编码，读出时自动解码；返回指向栈的指针、用栈地址初始化全局变量，是编译错误。

不要在 Go 里声明 `.impl`。那是 C 内部的 SysV 入口。

## 6. 两边怎么对上

```text
Go 调用 main.goabi_add2
        │  Go ABIInternal（整数 rax,rbx,rcx,rdi,rsi,r8…）
        ▼
   thunk main.goabi_add2
        │  洗寄存器，jmp
        ▼
   SysV 体 main.goabi_add2.impl
        │  内部 C 调用仍是 SysV，不再次洗寄存器
        ▼
   其它 .impl
```

`g` 在 R14。morestack 前导用它和 `g+16` 的 `stackguard0` 比较。栈不够时调用 `runtime.morestack_noctxt`，返回后从函数入口重跑，不是从调用点继续。

指针色决定这个字能不能进堆、搬栈时要不要改：

| 写法 | 含义 |
|---|---|
| `cptr(T)` | 非栈对象。C 堆、全局、arena。不是 Go 堆。 |
| `sptr(T)` | 当前 goroutine 栈上的对象。原始指针字只留在寄存器或栈槽。 |
| `uptr(T)` | 可入库的编码。最高位 0 是绝对地址，1 是相对所属 goroutine `g.stack.hi` 的偏移。使用前解码。当前字不带 goroutine 指针。 |
| `T *` | `auto_ptr`。能证明只在栈上就收成 `sptr`；必须入库就收成 `uptr`。 |
| `gptr(T)` | Go 堆指针。有 stackmap 和写屏障。不能和其它色隐式互转。 |

没有 `dsptr`。`syntax-guide.md` 第 8.2 节的 `goc_goimport` / `goc_goexport` 是示意，编译器不认这两个属性。不要写它们。

`sptr` 和 `uptr` 绑定创建它们的那一条 goroutine。`JSContext` 这类把栈引用编进对象的 context，只能在那一条 goroutine 上调用。换一条，offset 会加上错误的 `stack.hi`。编译器现在不检查。合同是 syntax-guide §8.3。下一版不改这个默认；带 goroutine 指针的双字形式是开关，默认关，见 [todo.md](todo.md)。

## 7. 颜色检查，不链接

只想看 Sema 拒不拒绝，不必走 Go 链接：

```bash
./cmd/goc cc -emit-llvm -S -o /tmp/hello.ll examples/hello_colors.c
./cmd/goc cc -c examples/sptr_escape_bad.c   # 预期失败：返回指向栈的指针
```

`examples/` 只覆盖颜色，不是集成例子。

## 8. C 调用 Go

没有稳定的导入语法。现在能工作的做法是手写调用，QuickJS 数学桥就是这样。Go 函数是普通函数，带 `//go:noinline`。C 在调用前做三件事：

1. 参数按 Go ABI 放进寄存器。整数从 `rax` 起，浮点从 `xmm0` 起。
2. `movq %fs:-8, %r14`，把当前 `g` 放回 R14。`pxor %xmm15, %xmm15`，Go ABI 要求 `xmm15` 为 0。
3. `call main.函数名.goabi`。不是 C 名字，也不是不带 `.goabi` 的 Go 符号。

Go 会在返回地址上方溢出寄存器参数，供 traceback 使用。从 C 裸调用时调用方没有这块区域，溢出会盖掉 C 帧。QuickJS 的 `localtime` 桥在 `call` 前后各留了 64 字节。签名更大就得更大。这不是语言保证，是现在的调用约定缝隙。

这条路还要求 Go 包是 `main`，因为符号写成了 `main.函数名.goabi`。

需要 glibc 时不能走第 5 节的 `CGO_ENABLED=0`。QuickJS CLI 用 `CGO_ENABLED=1` 做外部链接，但 C 侧 libc 面是自备 shim，符号带 `goc_` 前缀。裸的 `malloc` / `printf` 会被 Go 链接器当成动态全局符号，劫持进程初始化。不要把 glibc 符号从 goc 对象里导出。

## 9. 现在不要假设能用的

- arm64，以及任何非 linux/amd64 的目标。
- 非 `main` 的 Go 包。打包脚本不会把对象打进去。
- `goc_goimport` / `goc_goexport`。
- 把 goc 对象交给 `gcc` / `ld`。
- 在 `CGO_ENABLED=0` 的程序里调用 libc。
- 变参、x87、异常、AVX 的 Go 可调用入口。
- 两个 goroutine 同时用 `GOC_DYNALLOC_POOL`。那个池是进程全局游标。
- 把 `JSContext` 或其它持有 `sptr` / `uptr` 的对象交给另一条 goroutine。

---

# Guide

Experimental. linux/amd64 only. This is not cgo, and it will not link arbitrary C into a Go binary.

The language contract is [syntax-guide.md](syntax-guide.md). This page is how to build, link, and match the two sides the first time. The path that actually runs is [scripts/test-p29-goabi.sh](../scripts/test-p29-goabi.sh) and [tests/goabi/](../tests/goabi/).

The arm64 backend is the next version, [todo.md](todo.md). Changing `-march` does nothing today.

From zero, with command-line flags, see [quickstart.md](quickstart.md). Below is compiling one `.c` to a goobj and linking it by hand.

## 1. Tools

Host compiler: `clang-19`, `clang++-19`, `llc-19`, `llvm-config-19`, `cmake`, `ninja`, `python3`, `rg`. Go 1.24 or newer. The machine is linux/amd64.

There is no separate install step for `goc`. After cloning, use `./cmd/goc` from the repo.

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

## 2. Build patched Clang

The product frontend is Clang 19.1.7 plus the pointer-color Sema. A distro `clang-19` is not enough. Without this compiler, `goc build` cannot emit an object the Go linker accepts. `goc test --p27` is the plugin path: color checks only, no linkable goobj.

```bash
curl -L -o /tmp/llvmorg-19.1.7.tar.gz \
  https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.tar.gz
tar -C "$HOME/src" -xf /tmp/llvmorg-19.1.7.tar.gz
export LLVM_SRC="$HOME/src/llvm-project-llvmorg-19.1.7"

./scripts/apply-patches.sh "$LLVM_SRC"

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  -S "$LLVM_SRC/llvm" \
  -B third_party/llvm-19.1.7-clang-build

ninja -C third_party/llvm-19.1.7-clang-build -j"$(nproc)" clang
```

A Release build takes tens of minutes. The build directory is under `third_party/` and gitignored. Do not commit the binaries. Details: [clang/README.md](../clang/README.md).

The driver finds `third_party/llvm-19.1.7-clang-build/bin/clang`. Pass `--clang` when the binary lives elsewhere.

## 3. Check the driver

```bash
./cmd/goc version
./cmd/goc test --p28
```

`--p28` uses the Clang built in the repo, or the one named by `--clang`. It compiles a small golden set. It does not link a Go program.

The first `goc build` compiles `frontend/color-escape` itself, with `clang++-19` and `llvm-config-19`, against `libLLVM-19`. When a pass starts, the driver adds that Clang tree's `lib/` to the process.

## 4. Compile C to a Go object

```bash
./cmd/goc build tests/goabi/goabi.c -o build/p29-goabi/goabi.o --all --goabi \
  -O0 --no-default-ptr-color
```

That matches [scripts/test-p29-goabi.sh](../scripts/test-p29-goabi.sh). `--goabi` always inserts the morestack prologue and marks the frame splittable. There is no switch. Without the prologue the linker accounts the frame as nosplit and rejects anything over about 792 bytes. amd64 also records sptr maps, so a stack copy can rewrite `sptr` slots. arm64 cannot record those maps. Each Go entry's thunk frame is that signature's stack arguments rounded up to 16, plus 8 bytes for `%rbp` on amd64.

| Flag | Effect |
|---|---|
| `--no-default-ptr-color` | Do not paint unannotated `T*` with one color. Each pointer is inferred. |

The output is not an ELF `.o` the system linker accepts. `gcc` will not link it. Keep the sibling `goabi.meta.json`. The pack step reads it.

`--all` emits every defined function in the translation unit. `--goabi` implies `--all`. Supported signatures gain a Go entry thunk. C-to-C calls stay on the SysV body.

Supported signatures: integers, pointers, floating-point, and literal aggregates whose fields each occupy one eightbyte. An aggregate argument goes entirely in registers or entirely on the stack. Results are at most 16 bytes. Anything else stays SysV and cannot be called from Go. Variadics, x87, exceptions, and AVX are not on the Go-callable path.

Inspect the symbols:

```bash
go tool nm build/p29-goabi/goabi.o | rg 'goabi_add2'
```

Expect `main.goabi_add2` and `main.goabi_add2.impl`. The first is the thunk Go calls. The second is the SysV body. A `static` function is further qualified with the translation-unit name so two files can reuse the name.

The default symbol prefix is `main.`. That matches only a Go `package main`. The packer also only fires for the package path `main` (next section).

## 5. Link into a Go program

`goc go` does this in one command. See [quickstart.md](quickstart.md). It writes the marker assembly, packs the package path, and passes `-a`.

Below is the golden test without `goc go`. From `tests/goabi`:

```bash
cd tests/goabi
CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$PWD/../../build/p29-goabi/goabi.o" \
  go build -a \
  -toolexec "$PWD/../../backend/tools/toolexec_pack_goobj.sh" \
  -o "$PWD/../../build/p29-goabi/goabi_test" .
../../build/p29-goabi/goabi_test
```

Or just:

```bash
./scripts/test-p29-goabi.sh
```

Success includes the line `PASS p29-goabi`.

A program you write has to match four constraints. Miss one and the link fails, or the symbols stay empty:

1. The Go package must be `package main`. `backend/tools/toolexec_pack_goobj.sh` packs the goobj only when `-p` is `main`. Any other package path is skipped with no error.
2. Declare each C function you want to call as a bodyless Go func with the same name. The package also needs at least one `.s` file, or Go rejects the bodyless declarations. `GoabiMarker` in `tests/goabi/stubs_amd64.s` exists only to satisfy that rule. It is not a stub for the C functions.
3. `CGO_ENABLED=0`. This path is internal linking. The C file must not call libc. libc is a narrower path, section 8.
4. `go build -a`. Without `-a`, an incremental build may not rerun `asm` / `compile`, and the toolexec never packs. `GOC_BINOBJ` is the object from section 4. A space-separated list is accepted.

Declarations:

```c
#include "goc.h"

int goabi_add2(int a, int b) { return a + b; }

int goabi_store(sptr(int) out, int v) {
  *out = v;
  return v * 2;
}
```

```go
package main

func goabi_add2(a, b int32) int32

//go:noescape
func goabi_store(out *int32, v int32) int32
```

C `int` is 32 bits. Go `int` is 64 bits on amd64. Match the C type: `int` → `int32`, `long` → `int64`, `float` → `float32`, `double` → `float64`.

`//go:noescape` means this call does not retain the pointer. Storing it into the heap or a global is separate: storing an `sptr` into a plain `T *` or `cptr` global or heap field is encoded as `uptr` automatically and decoded on load; returning a stack pointer or initializing a global with a stack address is a compile error.

Do not declare `.impl` from Go. That symbol is the SysV entry used by other C.

## 6. How the two sides meet

```text
Go calls main.goabi_add2
        │  Go ABIInternal (integers rax,rbx,rcx,rdi,rsi,r8…)
        ▼
   thunk main.goabi_add2
        │  shuffle, jmp
        ▼
   SysV body main.goabi_add2.impl
        │  further C calls stay SysV; no second shuffle
        ▼
   other .impl bodies
```

`g` is R14. The morestack preamble compares it with `stackguard0` at `g+16`. If the stack is short, it calls `runtime.morestack_noctxt` and restarts at the function entry, not at the call site.

Pointer color decides whether the word may enter the heap, and whether a stack copy must adjust it:

| Spelling | Meaning |
|---|---|
| `cptr(T)` | Non-stack object. C heap, global, arena. Not the Go heap. |
| `sptr(T)` | Object on the current goroutine stack. The raw word stays in a register or a stack slot. |
| `uptr(T)` | Encoded word that may be stored in the heap. MSB 0 is an absolute address. MSB 1 is an offset from the owner goroutine's `g.stack.hi`. Decode before use. The current word does not carry a goroutine pointer. |
| `T *` | `auto_ptr`. Proven stack-only becomes `sptr`. A store that must enter the heap becomes `uptr`. |
| `gptr(T)` | Go heap pointer. Stackmap plus a write barrier. No implicit conversion to the other colors. |

There is no `dsptr`. `goc_goimport` / `goc_goexport` in syntax-guide §8.2 are illustrations. The compiler does not recognize those attributes. Do not write them.

`sptr` and `uptr` are bound to the goroutine that created them. A context that stores stack references, such as `JSContext`, may be called only on that goroutine. On another one, the offset is added to the wrong `stack.hi`. The compiler does not check this. Contract: syntax-guide §8.3. The next version does not change this default. The two-word form that carries a goroutine pointer is a switch, off by default; see [todo.md](todo.md).

## 7. Color checks, no link

To see Sema reject an escape without linking Go:

```bash
./cmd/goc cc -emit-llvm -S -o /tmp/hello.ll examples/hello_colors.c
./cmd/goc cc -c examples/sptr_escape_bad.c   # expected failure: returns a stack pointer
```

`examples/` covers colors only. It is not the integration example.

## 8. C calling Go

There is no stable import syntax. The working mechanism is a raw call, as in the QuickJS math bridge. The Go function is an ordinary function marked `//go:noinline`. Before the call, C does three things:

1. Put arguments in Go ABI registers. Integers start at `rax`, floats at `xmm0`.
2. `movq %fs:-8, %r14` puts the current `g` back in R14. `pxor %xmm15, %xmm15` because the Go ABI requires `xmm15` to be zero.
3. `call main.Func.goabi`. Not the C name, and not the Go symbol without `.goabi`.

Go spills register arguments above the return address so traceback can recover them. A raw call from C has no such area, so the spill overwrites the C frame. The QuickJS `localtime` bridge reserves 64 bytes around the `call`. A wider signature needs more. That is a calling-convention gap, not a language guarantee.

This path also requires the Go package to be `main`, because the symbol is spelled `main.Func.goabi`.

glibc cannot use the `CGO_ENABLED=0` recipe in section 5. The QuickJS CLI sets `CGO_ENABLED=1` for external linking, and the C side supplies its own libc shim with a `goc_` prefix. A bare `malloc` or `printf` exported from a goc object becomes a dynamic global and hijacks process startup. Do not export glibc symbols from a goc object.

## 9. Do not assume these work

- arm64, or any target other than linux/amd64.
- A Go package other than `main`. The packer will not insert the object.
- `goc_goimport` / `goc_goexport`.
- Feeding a goc object to `gcc` or `ld`.
- Calling libc from a `CGO_ENABLED=0` program.
- A Go-callable entry for variadics, x87, exceptions, or AVX.
- Two goroutines using `GOC_DYNALLOC_POOL` at once. The pool cursor is process-global.
- Handing `JSContext`, or any object that holds `sptr` / `uptr`, to another goroutine.
