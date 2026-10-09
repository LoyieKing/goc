# 下载即用

在 linux/amd64 上下载一个包。包里已经有 `goc` 和打过补丁的 Clang。不用再装 LLVM，也不用把编译器路径传给 `goc`。

实验性编译器。这条路径是内部链接：C 里不能调用 libc。语言规则见 [syntax-guide.md](syntax-guide.md)。自己把仓库编成这个包见 [build-from-source.md](build-from-source.md)。

## 1. 安装

系统是 linux/amd64。需要：

- Go 1.24 或更新（`go version`）

## 2. 下载

发布页：<https://github.com/LoyieKing/goc/releases>

```bash
tar -xzf goc-0.0.1-linux-amd64.tar.gz
cd goc-0.0.1-linux-amd64
export PATH="$PWD/bin:$PATH"
goc version
```

`bin/goc` 会用同一个目录里的 `bin/clang`。换一台机器就把整个目录拷走。

## 3. 自检

```bash
goc check
```

通过时打印 `PASS goc check`。它编译包里 `examples/hello` 的一份副本并运行，输出必须是：

```text
hello 42
```

## 4. 跑包里的例子

```bash
goc go examples/hello -o /tmp/hello
/tmp/hello
```

`/tmp/hello` 打印 `hello 42`。C 函数是 `int hello_add(int, int)`，Go 侧是没有函数体的 `func hello_add(a, b int32) int32`。

## 5. 写自己的程序

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

`package main` 的默认产物是 `<目录>/<目录名>`。指定路径：

```bash
goc go "$HOME/src/goc-hello" -o "$HOME/src/goc-hello/hello"
```

驱动会做这几件事：

- 只编译该目录下的 `*.c`，不进入子目录。
- `CGO_ENABLED=0`，所以 Go 不会把这些 `.c` 交给系统 cc。
- 包里没有 `.s` 时写入 `goc_marker_amd64.s`。这是占位，函数体在打进去的 goobj 里。把这个文件加入 `.gitignore`。
- Go 里仍要写同名、无函数体的声明。C 的 `int` 对应 Go 的 `int32`。
- 其它包（不是 `package main`）只做类型检查并打进构建缓存，不能加 `-o`。

给这个包额外传 Clang 参数时，写在 `--` 后面：

```bash
goc go "$HOME/src/goc-hello" -- -DNAME=value
```

## 6. 要改默认时加参数

`goc go` 和 `goc check` 不读环境里已经导出的 `GOC_*`。默认是 QuickJS 验证过的那一组：

```text
-O3 --default-ptr-color cptr --fast-stack-alloca
```

和 Go 一起跑时，morestack 前导、可分裂帧、sptr 栈图一定打开，没有参数可以关掉。没有前导时，帧按 nosplit 核算，超过约 792 字节链接器会拒绝。栈图用来在搬栈时改写栈上的 `sptr`。每个 Go 调 C 的 thunk 帧由编译器按这个函数的栈参数计算，再向上取整到 16，amd64 另外加上保存的 `%rbp`。

举例：

```bash
goc go "$HOME/src/goc-hello" -O2
goc build add.c -o add.o --all --goabi -O3 --default-ptr-color cptr
```

| 参数 | `goc go` 的默认 | 作用 |
|---|---|---|
| `-O0` … `-O3` | `-O3` | LLVM IR 优化级别 |
| `--default-ptr-color cptr` | `cptr` | 没写颜色的 `T*` 用这个颜色 |
| `--no-default-ptr-color` | 关 | 不盖默认色，每个指针各自推断 |
| `--fast-stack-alloca` | 开 | 把 `alloca` 的池推进内联到调用点。快很多。游标是进程全局的，只能单线程用；多线程同时调用有线程安全问题 |
| `--color-report` | 关 | 打印每个指针的颜色，不改变编译结果 |
| `--arch amd64` | `amd64` | `goc go` 只接受 amd64 |

全部参数：`goc go --help`。

`goc build` 不带 `--goabi` 时仍是 `-O0`，那三项也关着，除非环境变量打开了它们。P28 黄金测试依赖这一点。`goc build --goabi` 会打开 morestack 和可分裂帧；amd64 还会打开 sptr 栈图，arm64 记不了。要和 `goc go` 一样，再加上 `-O3 --default-ptr-color cptr --fast-stack-alloca`。仓库里的 QuickJS 脚本还在用同名环境变量调用 `goc build`；同一项两边都有时，命令行优先。

`--fixed-g` 和 `--opt-extra=-inline-threshold=250` 默认关着。

## 7. 现在不要假设能用

- 在这条路径里调用 libc，包括 `printf`、`malloc`、`memcpy`。输出用 Go 的 `fmt`。需要 libc 的是 QuickJS 那条构建（`scripts/qjs-cli-build.sh`，在仓库里）。
- 把任意第三方 C 工程丢进目录就链进 Go。签名限于整数、指针、浮点和很小的结构体。
- 把栈指针交给别的 goroutine。
- linux/arm64。`--arch arm64` 可以编 goobj，`goc go` 不链接它。
- 变参、x87、异常、AVX 的 Go 可调用入口。

下一步：指针色见 [syntax-guide.md](syntax-guide.md)。

---

# Download and run

On linux/amd64, download one archive. It already contains `goc` and the patched Clang. There is no LLVM package to install and no compiler path to pass.

This compiler is experimental. The path below is internal linking: the C cannot call libc. The language rules are in [syntax-guide.md](syntax-guide.md). Building this archive from a git checkout is [build-from-source.md](build-from-source.md).

## 1. Install

linux/amd64. You need:

- Go 1.24 or newer (`go version`)

## 2. Download

Releases: <https://github.com/LoyieKing/goc/releases>

```bash
tar -xzf goc-0.0.1-linux-amd64.tar.gz
cd goc-0.0.1-linux-amd64
export PATH="$PWD/bin:$PATH"
goc version
```

`bin/goc` uses the `bin/clang` in that same directory. Copy the whole directory to move it to another machine.

## 3. Self-check

```bash
goc check
```

Success prints `PASS goc check`. It compiles a copy of `examples/hello` from the archive and runs it. The output has to be:

```text
hello 42
```

## 4. Run the bundled example

```bash
goc go examples/hello -o /tmp/hello
/tmp/hello
```

`/tmp/hello` prints `hello 42`. The C function is `int hello_add(int, int)`. The Go side is a body-less `func hello_add(a, b int32) int32`.

## 5. Write a program

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

For `package main` the default output is `<dir>/<dirname>`. Pick a path:

```bash
goc go "$HOME/src/goc-hello" -o "$HOME/src/goc-hello/hello"
```

What the driver does:

- Compiles every `*.c` in that directory, and does not recurse.
- Sets `CGO_ENABLED=0`, so Go does not hand those `.c` files to the system cc.
- Writes `goc_marker_amd64.s` when the package has no `.s` file. It is a placeholder. The function body is in the packed goobj. Add the file to `.gitignore`.
- The Go file still needs a body-less declaration of the same name. C `int` is Go `int32`.
- Any package other than `package main` is type-checked and packed into the build cache. `-o` is refused.

Extra Clang arguments go after `--`:

```bash
goc go "$HOME/src/goc-hello" -- -DNAME=value
```

## 6. Change a default with a flag

`goc go` and `goc check` ignore `GOC_*` variables already in the environment. The defaults are the set QuickJS was verified with:

```text
-O3 --default-ptr-color cptr --fast-stack-alloca
```

On a Go stack, the morestack prologue, splittable frames, and sptr maps are always on. There is no flag to turn them off. Without the prologue the linker accounts the frame as nosplit and rejects anything over about 792 bytes. The maps rewrite `sptr` slots when the stack is copied. Each Go-to-C thunk frame is that function's stack arguments rounded up to 16, plus the saved `%rbp` on amd64.

Examples:

```bash
goc go "$HOME/src/goc-hello" -O2
goc build add.c -o add.o --all --goabi -O3 --default-ptr-color cptr
```

| Flag | `goc go` default | Effect |
|---|---|---|
| `-O0` … `-O3` | `-O3` | LLVM IR optimization level |
| `--default-ptr-color cptr` | `cptr` | Color of an unannotated `T*` |
| `--no-default-ptr-color` | off | Do not paint one default; each pointer is inferred |
| `--fast-stack-alloca` | on | Inline the alloca pool bump. Much faster. The cursor is process-global, so this is single-threaded only. Concurrent calls are not thread-safe |
| `--color-report` | off | Print each pointer's color. Does not change the compile |
| `--arch amd64` | `amd64` | `goc go` accepts amd64 only |

Every flag: `goc go --help`.

`goc build` without `--goabi` stays at `-O0`, and those three stay off unless an environment variable turns them on. The P28 goldens depend on that. `goc build --goabi` turns on morestack and splittable frames; amd64 also records sptr maps, and arm64 cannot. To match `goc go`, add `-O3 --default-ptr-color cptr --fast-stack-alloca`. Repository QuickJS scripts still call `goc build` with the same environment variables; a flag wins when both are present.

`--fixed-g` and `--opt-extra=-inline-threshold=250` stay off.

## 7. Do not assume these work yet

- Calling libc on this path, including `printf`, `malloc`, and `memcpy`. Print with Go's `fmt`. The QuickJS build (`scripts/qjs-cli-build.sh`, in the repository) is the one that needs libc.
- Dropping an arbitrary third-party C project into the directory and linking it into Go. Signatures are integers, pointers, floats, and very small structs.
- Handing a stack pointer to another goroutine.
- linux/arm64. `--arch arm64` can emit a goobj. `goc go` does not link it.
- A Go-callable entry for varargs, x87, exceptions, or AVX.

Next: pointer colors are in [syntax-guide.md](syntax-guide.md).
