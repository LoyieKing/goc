# 上手

C 写函数体，Go 来调用。这不是 cgo：goc 把 C 编成 Go 的对象文件，C 在 goroutine 的栈上跑，由 Go 链接器直接接上。

需要 linux/amd64，以及 Go 1.18–1.27（`go version`）。arm64 目标要 Go 1.21 以上。C 里不能调用 libc（`printf`、`malloc`、`memcpy`），要输出就用 Go 的 `fmt`。

## 1. 下载

发布页：<https://github.com/LoyieKing/goc/releases>

```bash
tar -xzf goc-0.0.2-linux-amd64.tar.gz
cd goc-0.0.2-linux-amd64
export PATH="$PWD/bin:$PATH"
goc version
goc check            # 编出并运行 examples/hello，打印 PASS goc check
```

包里有 `goc` 和配套的 Clang、LLVM 工具。换机器时把整个目录拷走。自己从源码编这个包见 [build-from-source.md](build-from-source.md)。

## 2. 第一个程序

```bash
mkdir -p "$HOME/src/goc-hello" && cd "$HOME/src/goc-hello"
```

`go.mod`：

```go
module goc.local/hello

go 1.18
```

`add.c`：

```c
#include "goc.h"

int hello_add(int a, int b) { return a + b; }
```

`main.go`：

```go
package main

import "fmt"

func hello_add(a, b int32) int32

func main() {
	fmt.Printf("hello %d\n", hello_add(20, 22))
}
```

```bash
goc go .              # 产出 ./goc-hello；-o 换路径
./goc-hello           # hello 42
```

`goc go [dir] [-o bin] [参数] [-- clang 参数]` 做三件事：

1. 用 goc 编译目录下的每个 `*.c`（不递归），得到 Go 可以直接调用的 `hello_add`。对象默认在 goc 树的 `build/goc-go/<目录哈希>/`，`--objdir DIR` 换位置。
2. 以 `CGO_ENABLED=0` 和 `go build -a` 编译 Go 部分。`hello_add(20, 22)` 就是一次普通的函数调用。
3. `goc toolexec` 在 Go 的 asm 和 compile 阶段把对象打进包里，由 Go 链接器接上。

几条规矩：

- Go 侧为每个 C 函数写同名、无函数体的声明。C 的 `int` 写成 `int32`，`long` 写成 `int64`，`float` 写成 `float32`，`double` 写成 `float64`。参数只支持整数、指针、浮点和很小的结构体，不支持变参。
- 包里没有 `.s` 时，goc 写入一个空的 `goc_marker_amd64.s`。Go 拒绝没有任何汇编文件的无函数体声明。它不是 C 函数的桩，把它加入 `.gitignore`。
- `package main` 产出二进制，默认是 `<目录>/<目录名>`。其它包只编译并打进构建缓存，不接受 `-o`。

C 指针按 goroutine 栈 / C 堆 / Go 堆分色（`sptr`、`cptr`、`gptr`、`uptr`），规则见 [language.md](language.md)。例如：

```c
int goabi_store(sptr(int) out, int v) { *out = v; return v * 2; }
```

```go
//go:noescape
func goabi_store(out *int32, v int32) int32
```

`//go:noescape` 表示这次调用不留下这个指针。

## 3. 参数

| 参数 | `goc go`、`goc check` 默认 | `goc build` 默认 |
|---|---|---|
| `-O0`…`-O3` | `-O3` | `-O0` |
| `--default-ptr-color cptr\|sptr\|uptr\|gptr`、`--no-default-ptr-color` | `cptr` | 不设 |
| `--fast-stack-alloca`、`--no-fast-stack-alloca` | 开 | 关 |
| `--arch amd64\|arm64` | `amd64` | `amd64` |

`--fast-stack-alloca` 让 `alloca` 在每个对象的 16 MiB 池里内联分配，快很多；游标是进程全局的，只能一个 goroutine 用。多个 goroutine 同时调用 `alloca` 时要加 `--no-fast-stack-alloca`。

`--color-report` 打印每个指针最后收成的颜色和来源，不改变编译结果。其余参数（`--opt-extra`、`--llc-extra`、`--pkg`、`--ir-renames`、`--libcall-impl`、`--abi0`、`--keep-tmp`）见 `goc go --help`。goc 不读任何 `GOC_*` 环境变量；不认识的 `-` 参数和 `--` 之后的参数交给 Clang。

morestack 前导、可分裂帧和 sptr 栈图总是打开的（amd64 和 arm64）。

## 4. C 调用 Go

在 C 里用 `__go_extern("pkg.Func")` 标记声明（`include/goc.h`）：

```c
#include "goc.h"
__go_extern("main.go_extern_add")
int64_t go_extern_add(int64_t a, int64_t b);

int64_t use(int64_t a, int64_t b) { return go_extern_add(a, b); }
```

goc 生成一个按 SysV 调用的 stub：按签名预留 Go 参数溢出区，装好 `g`，带上帧表，再调用 Go 的 ABIInternal 入口。Go 被调方不需要 `//go:nosplit`，栈可以在调用中增长。多返回值在 C 侧写成小结构体。用例在 `test/e2e/go_extern/`。

不要在对象里导出名叫 `malloc` 或 `printf` 的符号：外部链接时，进程启动时 libc 会调到它们。

## 5. 不用 goc go，自己链

`goc build foo.c -o foo.goobj` 编一个文件，旁边留下 `foo.gocobj`、`foo.symabis` 和 `foo.ll`，不要改名。然后：

```bash
CGO_ENABLED=0 go build -a -toolexec "goc toolexec -pkg main $PWD/foo.goobj --" -o prog .
```

包里要有一个 `.s`（空文件即可，作用同上面的 `goc_marker_amd64.s`）。`-pkg` 是要打进去的包的导入路径（默认 `main`），多个对象用空格分开，路径里不能有空格或引号。必须加 `-a`，否则增量编译可能跳过打包。这个 `.goobj` 只给 Go 链接器用，`gcc` 和 `ld` 读不了它。

`goc cc` 是带 goc 头文件的那个 Clang；`goc env` 打印 goc 用的工具路径（发布包里是 `bin/` 和 `passes/`，仓库里是 `third_party/llvm-19.1.7-clang-build` 和 `build/llvm`），没有参数能换成别的工具。

## 6. 先不要假设这些能用

- 把一份现成的 C 工程丢进目录就链进 Go。C 里调用 libc，要像 `ports/quickjs/host` 那样自带 shim。
- 把栈上的指针交给别的 goroutine（[language.md](language.md) §8.3）。
- 变参、x87、异常、AVX 的 Go 可调用入口。
- Go 和 C 之间互传函数指针（[roadmap.md](roadmap.md)）。
