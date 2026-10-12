# 下载即用

下载一个包就能用。包里有 `goc`，也有配套的 Clang。系统是 linux/amd64，还需要 Go 1.24 或更新（`go version`）。

C 写函数体，Go 来调用。C 里不要调用 `printf`、`malloc`。要输出，用 Go 的 `fmt`。

## 1. 下载

发布页：<https://github.com/LoyieKing/goc/releases>

```bash
tar -xzf goc-0.0.1-linux-amd64.tar.gz
cd goc-0.0.1-linux-amd64
export PATH="$PWD/bin:$PATH"
goc version
```

换一台机器时，把整个目录拷走。

## 2. 确认能用

```bash
goc check
```

通过时打印 `PASS goc check`，程序输出是：

```text
hello 42
```

## 3. 跑包里的例子

```bash
goc go examples/hello -o /tmp/hello
/tmp/hello
```

`/tmp/hello` 打印 `hello 42`。

## 4. 写自己的程序

```bash
mkdir -p "$HOME/src/goc-hello"
cd "$HOME/src/goc-hello"
```

`go.mod`：

```go
module goc.local/hello

go 1.24
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
goc go "$HOME/src/goc-hello"
"$HOME/src/goc-hello/goc-hello"
```

程序打印 `hello 42`。默认文件名是目录名。要换路径：

```bash
goc go "$HOME/src/goc-hello" -o "$HOME/src/goc-hello/hello"
```

`main` 是 Go。`hello_add` 的函数体是 C，C 的 `int` 写成 Go 的 `int32`。`goc go` 做三件事：

1. 用 goc 编译 `add.c`，得到 Go 可以直接调用的 `hello_add`。
2. 用 Go 编译器编译 `main.go`。`hello_add(20, 22)` 就是一次普通的函数调用。
3. 两份结果放进同一个包，由 Go 链接器接上。运行得到的程序即可。

第一次编译时，目录里会多出 `goc_marker_amd64.s`。`.c` 由 goc 编译，Go 看不见，包里只剩 `.go` 时，没有函数体的声明会被拒绝。这个空汇编文件就是为了让 `func hello_add(a, b int32) int32` 能通过编译，函数体仍在 `add.c` 里。把它加入 `.gitignore`。目录里已经有别的 `.s` 时，goc 不会再写它。

只编译这个目录下的 `*.c`。给 Clang 的额外参数写在 `--` 后面，例如 `goc go . -- -DNAME=value`。

## 5. 先不要假设这些能用

- C 里调用 `printf`、`malloc`、`memcpy`。
- 把一份现成的 C 工程丢进目录就链进 Go。参数目前只支持整数、指针、浮点和很小的结构体。
- 把栈上的指针交给别的 goroutine。
- 多线程同时调用 `alloca`。默认实现更快，只适合单线程。要关掉，加 `--no-fast-stack-alloca`。
- linux/arm64。

指针颜色见 [syntax-guide.md](syntax-guide.md)。从源码编出这个包见 [build-from-source.md](build-from-source.md)。想自己用 `go build` 编译和链接，见 [guide.md](guide.md)。全部参数：`goc go --help`。

---

# Download and run

Download one archive and use it. The archive contains `goc` and the Clang that goes with it. The system is linux/amd64. You also need Go 1.24 or newer (`go version`).

Write the function body in C and call it from Go. Do not call `printf` or `malloc` from C. Print with Go's `fmt`.

## 1. Download

Releases: <https://github.com/LoyieKing/goc/releases>

```bash
tar -xzf goc-0.0.1-linux-amd64.tar.gz
cd goc-0.0.1-linux-amd64
export PATH="$PWD/bin:$PATH"
goc version
```

To move it to another machine, copy the whole directory.

## 2. Check that it works

```bash
goc check
```

Success prints `PASS goc check`. The program prints:

```text
hello 42
```

## 3. Run the bundled example

```bash
goc go examples/hello -o /tmp/hello
/tmp/hello
```

`/tmp/hello` prints `hello 42`.

## 4. Write a program

```bash
mkdir -p "$HOME/src/goc-hello"
cd "$HOME/src/goc-hello"
```

`go.mod`:

```go
module goc.local/hello

go 1.24
```

`add.c`:

```c
#include "goc.h"

int hello_add(int a, int b) { return a + b; }
```

`main.go`:

```go
package main

import "fmt"

func hello_add(a, b int32) int32

func main() {
	fmt.Printf("hello %d\n", hello_add(20, 22))
}
```

```bash
goc go "$HOME/src/goc-hello"
"$HOME/src/goc-hello/goc-hello"
```

The program prints `hello 42`. The default file name is the directory name. To pick a path:

```bash
goc go "$HOME/src/goc-hello" -o "$HOME/src/goc-hello/hello"
```

`main` is Go. The body of `hello_add` is C. Write C `int` as Go `int32`. `goc go` does three things:

1. goc compiles `add.c` and turns `hello_add` into a function Go can call directly.
2. The Go compiler compiles `main.go`. `hello_add(20, 22)` is an ordinary function call.
3. Both results go into the same package, and the Go linker connects the call. Run the program it produces.

The first build writes `goc_marker_amd64.s` into the directory. goc compiles the `.c` files, so Go does not see them. A package that contains only `.go` files cannot declare a function with no body. This empty assembly file lets `func hello_add(a, b int32) int32` compile. The body stays in `add.c`. Add the file to `.gitignore`. If the directory already has another `.s` file, goc does not write it.

Only the `*.c` files in this directory are compiled. Extra arguments for Clang go after `--`, for example `goc go . -- -DNAME=value`.

## 5. Do not assume these work yet

- Calling `printf`, `malloc`, or `memcpy` from C.
- Dropping an existing C project into the directory and linking it into Go. Arguments are integers, pointers, floats, and very small structs.
- Handing a pointer to stack data to another goroutine.
- Calling `alloca` from several threads at once. The default is faster and is for one thread. Turn it off with `--no-fast-stack-alloca`.
- linux/arm64.

Pointer colors are in [syntax-guide.md](syntax-guide.md). Building this archive from source is [build-from-source.md](build-from-source.md). To compile and link with `go build` yourself, see [guide.md](guide.md). Every flag: `goc go --help`.
