# 使用指南

下载即用见 [quickstart.md](quickstart.md)。这篇是在仓库里自己把一个 `.c` 编成 `.goobj`，再用 `go build` 链上。语言规则见 [syntax-guide.md](syntax-guide.md)。从源码编出编译器见 [build-from-source.md](build-from-source.md)。

只支持 linux/amd64。这不是 cgo。

## 1. 把 C 编成 `.goobj`

在仓库根目录：

```bash
./cmd/goc build test/e2e/goabi/goabi.c -o build/test/goabi/goabi.goobj
```

留下旁边的 `goabi.gocobj` 和 `goabi.symabis`，不要改名。这个 `.goobj` 交给 Go 链接器。`gcc` 用不了它。

Go 能直接调用的是整数、指针、浮点，以及很小的结构体。变参不行。符号形如 `main.goabi_add2`。Go 包是 `package main` 时才对得上。

## 2. 用 go build 链接

一条命令完成这件事用 `goc go`，见 [quickstart.md](quickstart.md)。下面是自己链，和 [test/e2e/goabi/run.sh](../test/e2e/goabi/run.sh) 相同。

```bash
export GOC_ROOT="$PWD"   # 仓库根目录
cd test/e2e/goabi
CGO_ENABLED=0 GOC_BINOBJ="$GOC_ROOT/build/test/goabi/goabi.goobj" \
  go build -a \
  -toolexec "$GOC_ROOT/tools/goc/toolexec_recognize_goobj.sh" \
  -o "$GOC_ROOT/build/test/goabi/goabi_test" .
"$GOC_ROOT/build/test/goabi/goabi_test"
```

`-toolexec` 这个脚本的作用，是在 Go 的 asm 和 compile 阶段认出 goc 编出来的 `.goobj`。asm 只看 `.s`，脚本补上这些符号；compile 写完包之后，脚本把 `.goobj` 打进去。不经过它，Go 看不到 C 的函数体。

也可以直接跑 `./test/e2e/goabi/run.sh`。成功时输出里有 `PASS goabi`。

缺一件就会链失败，或者函数是空的：

1. 这个例子是 `package main`。别的包还要设置 `GOC_PACK_PKG` 为它的导入路径，否则 `.goobj` 不会打进去。
2. 每个要从 Go 调用的 C 函数，在 Go 里写同名、没有函数体的声明。包里还要有一个 `.s`。`test/e2e/goabi/stubs_amd64.s` 里的 `GoabiMarker` 就是这个空文件，不是 C 函数的桩。
3. `CGO_ENABLED=0`。C 里不要调用 `printf`、`malloc`、`memcpy`。
4. `go build -a`。不加 `-a`，增量编译可能不把 `.goobj` 打进去。多个 `.goobj` 在 `GOC_BINOBJ` 里用空格分开。

C 的 `int` 写成 Go 的 `int32`，`long` 写成 `int64`，`float` 写成 `float32`，`double` 写成 `float64`。

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

`//go:noescape` 表示这次调用不留下这个指针。不要在 Go 里声明带 `.impl` 的名字。

指针颜色、栈指针能不能交给别的 goroutine，见 [syntax-guide.md](syntax-guide.md)。

## 3. C 调用 Go

在 C 里用 `__go_extern("pkg.Func")` 标记声明（见 `include/goc.h`）。goc 会生成 SysV 可调用的 stub：按签名预留 Go 参数溢出区、装好 `g`（R14）、清零 `xmm15`，并带上 pcsp，再 `call pkg.Func.goabi`。宿主侧写普通 C 调用即可。Go 被调方**不需要** `//go:nosplit`——stub 自己的帧表能支撑搬栈。

```c
#include "goc.h"
__go_extern("main.go_extern_add")
int64_t go_extern_add(int64_t a, int64_t b);

int64_t use(int64_t a, int64_t b) { return go_extern_add(a, b); }
```

多返回值在 C 侧写成小结构体（与 Go 的多返回寄存器约定对应）。金测见 `test/e2e/go_extern/`。

不要在 `.goobj` 里导出名叫 `malloc` 或 `printf` 的符号。外部链接时，这两个名字会在进程启动时被 libc 调到。

## 4. 先不要假设这些能用

- linux/arm64。
- 把 goc 的 `.goobj` 交给 `gcc` 或 `ld`。
- 变参、x87、异常、AVX 的 Go 可调用入口。
- `goc_goexport`。编译器不认。（C→Go 用 `__go_extern`。）
- 多线程同时调用 `alloca`。
- 把栈上的指针交给别的 goroutine。

---

# Guide

Download and run is [quickstart.md](quickstart.md). This page compiles one `.c` in the repository to a `.goobj` and links it with `go build`. The language rules are [syntax-guide.md](syntax-guide.md). Building the compiler from source is [build-from-source.md](build-from-source.md).

linux/amd64 only. This is not cgo.

## 1. Compile C to a `.goobj`

From the repository root:

```bash
./cmd/goc build test/e2e/goabi/goabi.c -o build/test/goabi/goabi.goobj
```

Leave the sibling `goabi.gocobj` and `goabi.symabis` in place. Do not rename it. Give this `.goobj` to the Go linker. `gcc` cannot use it.

Go can call integers, pointers, floats, and very small structs. Variadic functions are not included. Symbols look like `main.goabi_add2`. That matches a Go `package main`.

## 2. Link with go build

`goc go` does this in one command. See [quickstart.md](quickstart.md). The commands below are the same steps as [test/e2e/goabi/run.sh](../test/e2e/goabi/run.sh).

```bash
export GOC_ROOT="$PWD"   # repository root
cd test/e2e/goabi
CGO_ENABLED=0 GOC_BINOBJ="$GOC_ROOT/build/test/goabi/goabi.goobj" \
  go build -a \
  -toolexec "$GOC_ROOT/tools/goc/toolexec_recognize_goobj.sh" \
  -o "$GOC_ROOT/build/test/goabi/goabi_test" .
"$GOC_ROOT/build/test/goabi/goabi_test"
```

That `-toolexec` script makes Go's asm and compile stages recognize the `.goobj` goc built. asm only looks at `.s` files, so the script adds these symbols; after compile writes the package, the script packs the `.goobj` in. Without it, Go does not see the C function bodies.

Or run `./test/e2e/goabi/run.sh`. Success includes the line `PASS goabi`.

Miss one of these and the link fails, or the function is empty:

1. This example is `package main`. For any other package, set `GOC_PACK_PKG` to its import path, or the `.goobj` is left out.
2. Declare each C function you call as a Go func with the same name and no body. The package also needs one `.s` file. `GoabiMarker` in `test/e2e/goabi/stubs_amd64.s` is that empty file. It is not a stub for the C functions.
3. `CGO_ENABLED=0`. Do not call `printf`, `malloc`, or `memcpy` from C.
4. `go build -a`. Without `-a`, an incremental build may not include the `.goobj`. Separate several `.goobj` files in `GOC_BINOBJ` with spaces.

Write C `int` as Go `int32`, `long` as `int64`, `float` as `float32`, and `double` as `float64`.

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

`//go:noescape` means this call does not keep the pointer. Do not declare a name ending in `.impl` from Go.

Pointer colors, and whether a stack pointer may be handed to another goroutine, are in [syntax-guide.md](syntax-guide.md).

## 3. C calling Go

Mark a C declaration with `__go_extern("pkg.Func")` (`include/goc.h`). goc emits a SysV-callable stub: spill area sized for the signature, `g` in R14, clear `xmm15`, preserve SysV callee-saved registers, and pcsp so the Go callee may grow the stack. The host uses an ordinary C call. The C name must differ from the Go base name (e.g. `goc_x_Foo` → `main.Foo`). The Go callee does not need `//go:nosplit`.

```c
#include "goc.h"
__go_extern("main.go_extern_add")
int64_t goc_x_add(int64_t a, int64_t b);

int64_t use(int64_t a, int64_t b) { return goc_x_add(a, b); }
```

Multiple Go results map to a small C struct. See `test/e2e/go_extern/`.

Do not export a symbol named `malloc` or `printf`. With external linking, libc calls those names during process startup.

## 4. Do not assume these work yet

- linux/arm64.
- Giving a goc `.goobj` to `gcc` or `ld`.
- A Go-callable entry for variadics, x87, exceptions, or AVX.
- `goc_goexport`. The compiler does not recognize it. (`__go_extern` covers C→Go imports.)
- Calling `alloca` from several threads at once.
- Handing a pointer to stack data to another goroutine.
