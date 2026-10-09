# 从零开始

在一台新的 linux/amd64 上，从克隆仓库到 Go 调通一个 C 函数。配置都写在命令行上，不用导出 `GOC_*`。

实验性编译器。这条路径是内部链接：C 里不能调用 libc。语言规则见 [syntax-guide.md](syntax-guide.md)。手动把单个 `.c` 编成 goobj 的步骤仍在 [guide.md](guide.md)。

## 1. 安装工具

系统是 linux/amd64。需要：

- `git`、`curl`、`python3`、`rg`
- Go 1.24 或更新（`go version`）
- 用来编 Clang 的宿主：`clang-19`、`clang++-19`、`cmake`、`ninja`、`llvm-config-19`、`opt-19`

`opt-19` 要在 `PATH` 上。goc 编完 IR 之后用它做优化。

## 2. 克隆

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

后面的命令都在仓库根目录执行。驱动自己定位仓库，不用设置 `GOC_ROOT`。

## 3. 准备打过补丁的 Clang

goc 的前端是 Clang 19.1.7，加了指针色的 Sema。系统里的 `clang-19` 不能代替它。两条路选一条。

### 已经有人打好的工具链

把目录解压到下面任一位置。驱动按这个顺序找 `bin/clang`：

1. 命令行 `--clang PATH`
2. `--toolchain DIR` 下面的 `bin/clang`
3. `third_party/llvm-19.1.7-clang-build/bin/clang`（在这份仓库里编出来的）
4. `third_party/goc-toolchain/bin/clang`
5. `~/.goc/toolchain/bin/clang`

目录里要有 `bin/clang`、`bin/opt`、`lib/libLLVM.so.19.1`、`lib/clang/`、`passes/`。自己打包：

```bash
./cmd/goc toolchain pack "$HOME/goc-toolchain"
```

没有托管的下载地址。仓库里还没有 Clang 时，用下一节从源码编。

### 从源码编

Release 构建大约要几十分钟。构建目录在 `third_party/` 下，已被 gitignore。

```bash
mkdir -p "$HOME/src"
curl -L -o /tmp/llvmorg-19.1.7.tar.gz \
  https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.tar.gz
tar -C "$HOME/src" -xf /tmp/llvmorg-19.1.7.tar.gz
LLVM_SRC="$HOME/src/llvm-project-llvmorg-19.1.7"

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

编完不用把路径写进 shell 配置。驱动会找到 `third_party/llvm-19.1.7-clang-build/bin/clang`，并把该树的 `lib/` 加进本次进程，用来加载 `libLLVM.so.19.1`。

Clang 放在别的目录时：

```bash
./cmd/goc check --clang /path/to/clang
```

## 4. 自检

```bash
./cmd/goc toolchain status
./cmd/goc check
```

第一次会编 `goc-color-escape` 和 `goc-llc`，要用 `clang++-19` 和 `llvm-config-19`。`goc check` 通过时打印 `PASS goc check`。它编译 `examples/hello` 的一份副本并运行，输出必须是：

```text
hello 42
```

## 5. 跑仓库里的例子

```bash
./cmd/goc go examples/hello
./examples/hello/hello
```

`examples/hello/hello` 打印 `hello 42`。C 函数是 `int hello_add(int, int)`，Go 侧是没有函数体的 `func hello_add(a, b int32) int32`。

## 6. 写自己的程序

在仓库外建一个目录也行。下面假设仓库在当前目录的上一级。

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

回到 goc 仓库：

```bash
cd /path/to/goc
./cmd/goc go "$HOME/src/goc-hello"
"$HOME/src/goc-hello/goc-hello"
```

`package main` 的默认产物是 `<目录>/<目录名>`。指定路径：

```bash
./cmd/goc go "$HOME/src/goc-hello" -o "$HOME/src/goc-hello/hello"
```

驱动会做这几件事：

- 只编译该目录下的 `*.c`，不进入子目录。
- `CGO_ENABLED=0`，所以 Go 不会把这些 `.c` 交给系统 cc。
- 包里没有 `.s` 时写入 `goc_marker_amd64.s`。这是占位，函数体在打进去的 goobj 里。把这个文件加入 `.gitignore`。
- Go 里仍要写同名、无函数体的声明。C 的 `int` 对应 Go 的 `int32`。
- 其它包（不是 `package main`）只做类型检查并打进构建缓存，不能加 `-o`。

给这个包额外传 Clang 参数时，写在 `--` 后面：

```bash
./cmd/goc go "$HOME/src/goc-hello" -- -DNAME=value
```

## 7. 要改默认时加参数

`goc go` 和 `goc check` 不读环境里已经导出的 `GOC_*`。默认是 QuickJS 验证过的那一组：

```text
-O3 --default-ptr-color cptr --fast-stack-alloca
```

和 Go 一起跑时，morestack 前导、可分裂帧、sptr 栈图一定打开，没有参数可以关掉。没有前导时，帧按 nosplit 核算，超过约 792 字节链接器会拒绝。栈图用来在搬栈时改写栈上的 `sptr`。每个 Go 调 C 的 thunk 帧由编译器按这个函数的栈参数计算，再向上取整到 16，amd64 另外加上保存的 `%rbp`。

举例：

```bash
./cmd/goc go "$HOME/src/goc-hello" -O2
./cmd/goc go --clang /path/to/clang "$HOME/src/goc-hello"
./cmd/goc build add.c -o add.o --all --goabi \
  -O3 --default-ptr-color cptr
```

| 参数 | `goc go` 的默认 | 作用 |
|---|---|---|
| `-O0` … `-O3` | `-O3` | LLVM IR 优化级别 |
| `--default-ptr-color cptr` | `cptr` | 没写颜色的 `T*` 用这个颜色 |
| `--no-default-ptr-color` | 关 | 不盖默认色，每个指针各自推断 |
| `--fast-stack-alloca` | 开 | 把 `alloca` 的池推进内联到调用点。快很多。游标是进程全局的，只能单线程用；多线程同时调用有线程安全问题 |
| `--clang PATH` | 自动查找 | 打过补丁的 clang |
| `--toolchain DIR` | 自动查找 | 含 `bin/clang` 的工具链目录 |
| `--color-report` | 关 | 打印每个指针的颜色，不改变编译结果 |
| `--arch amd64` | `amd64` | `goc go` 只接受 amd64 |

全部参数：`./cmd/goc go --help`。

`goc build` 不带 `--goabi` 时仍是 `-O0`，那三项也关着，除非环境变量打开了它们。P28 黄金测试依赖这一点。`goc build --goabi` 会打开 morestack 和可分裂帧；amd64 还会打开 sptr 栈图，arm64 记不了。要和 `goc go` 一样，再加上 `-O3 --default-ptr-color cptr --fast-stack-alloca`。仓库里的 QuickJS 脚本还在用同名环境变量调用 `goc build`；同一项两边都有时，命令行优先。

`--fixed-g` 和 `--opt-extra=-inline-threshold=250` 默认关着。

## 8. 现在不要假设能用

- 在这条路径里调用 libc，包括 `printf`、`malloc`、`memcpy`。输出用 Go 的 `fmt`。需要 libc 的是 QuickJS 那条构建（`scripts/qjs-cli-build.sh`）。
- 把任意第三方 C 工程丢进目录就链进 Go。签名限于整数、指针、浮点和很小的结构体。
- 把栈指针交给别的 goroutine。
- linux/arm64。`--arch arm64` 可以编 goobj，`goc go` 不链接它。
- 变参、x87、异常、AVX 的 Go 可调用入口。

下一步：指针色见 [syntax-guide.md](syntax-guide.md)，工具链目录见 [toolchain.md](toolchain.md)。

---

# From zero

A new linux/amd64 machine, from clone to a Go program calling one C function. Settings are command-line flags. Nothing named `GOC_*` has to be exported.

This path is internal linking. The C cannot call libc. The language rules are in [syntax-guide.md](syntax-guide.md). Compiling one `.c` to a goobj by hand is still [guide.md](guide.md).

## 1. Install tools

linux/amd64. You need:

- `git`, `curl`, `python3`, `rg`
- Go 1.24 or newer (`go version`)
- A host toolchain that can build Clang: `clang-19`, `clang++-19`, `cmake`, `ninja`, `llvm-config-19`, `opt-19`

`opt-19` must be on `PATH`. goc runs it after emitting IR.

## 2. Clone

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

Run the later commands from the repository root. The driver finds the root itself.

## 3. Patched Clang

The frontend is Clang 19.1.7 plus pointer-color Sema. A stock `clang-19` is not a substitute.

### A toolchain you already have

Unpack it in one of these places. The driver looks for `bin/clang` in this order:

1. `--clang PATH`
2. `--toolchain DIR` → `bin/clang`
3. `third_party/llvm-19.1.7-clang-build/bin/clang` (built in this checkout)
4. `third_party/goc-toolchain/bin/clang`
5. `~/.goc/toolchain/bin/clang`

The directory contains `bin/clang`, `bin/opt`, `lib/libLLVM.so.19.1`, `lib/clang/`, and `passes/`. Pack one from a machine that already builds:

```bash
./cmd/goc toolchain pack "$HOME/goc-toolchain"
```

There is no hosted download. Without a local Clang, build it from source.

### From source

A Release build takes tens of minutes. The build directory is under `third_party/` and gitignored.

```bash
mkdir -p "$HOME/src"
curl -L -o /tmp/llvmorg-19.1.7.tar.gz \
  https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.tar.gz
tar -C "$HOME/src" -xf /tmp/llvmorg-19.1.7.tar.gz
LLVM_SRC="$HOME/src/llvm-project-llvmorg-19.1.7"

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

Do not put the path in the shell config. The driver finds `third_party/llvm-19.1.7-clang-build/bin/clang` and adds that tree's `lib/` for `libLLVM.so.19.1`.

Clang lives somewhere else:

```bash
./cmd/goc check --clang /path/to/clang
```

## 4. Self-check

```bash
./cmd/goc toolchain status
./cmd/goc check
```

The first run builds `goc-color-escape` and `goc-llc`, using `clang++-19` and `llvm-config-19`. Success prints `PASS goc check`. The check compiles a copy of `examples/hello` and requires this output:

```text
hello 42
```

## 5. The example in the repo

```bash
./cmd/goc go examples/hello
./examples/hello/hello
```

`examples/hello/hello` prints `hello 42`. The C function is `int hello_add(int, int)`. The Go side is a bodyless `func hello_add(a, b int32) int32`.

## 6. Your own program

The package directory can sit outside the repo.

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

From the goc checkout:

```bash
cd /path/to/goc
./cmd/goc go "$HOME/src/goc-hello"
"$HOME/src/goc-hello/goc-hello"
```

For `package main` the default binary is `<dir>/<dirname>`. Pick a path:

```bash
./cmd/goc go "$HOME/src/goc-hello" -o "$HOME/src/goc-hello/hello"
```

What the driver does:

- Compiles `*.c` in that directory only, not in subdirectories.
- Sets `CGO_ENABLED=0`, so Go does not hand those `.c` files to the system cc.
- Writes `goc_marker_amd64.s` when the package has no `.s` file. The file is a placeholder. The function body is the packed goobj. Ignore it in git.
- You still declare each C function in Go, with no body. C `int` is Go `int32`.
- A package other than `package main` is type-checked and packed into the build cache. `-o` is refused.

Extra Clang arguments go after `--`:

```bash
./cmd/goc go "$HOME/src/goc-hello" -- -DNAME=value
```

## 7. Flags

`goc go` and `goc check` ignore `GOC_*` values already exported in the environment. The defaults are the QuickJS-verified set:

```text
-O3 --default-ptr-color cptr --fast-stack-alloca
```

A Go link always inserts the morestack prologue, marks the frame splittable, and records sptr maps. There is no flag to turn those off. Without the prologue the linker accounts the frame as nosplit and rejects anything over about 792 bytes. The maps let a stack copy rewrite `sptr` slots. Each Go→C thunk frame is that signature's stack arguments, rounded up to 16, plus the saved frame pointer on amd64.

Examples:

```bash
./cmd/goc go "$HOME/src/goc-hello" -O2
./cmd/goc go --clang /path/to/clang "$HOME/src/goc-hello"
./cmd/goc build add.c -o add.o --all --goabi \
  -O3 --default-ptr-color cptr
```

| Flag | `goc go` default | Effect |
|---|---|---|
| `-O0` … `-O3` | `-O3` | LLVM IR optimization level |
| `--default-ptr-color cptr` | `cptr` | Color of an unannotated `T*` |
| `--no-default-ptr-color` | off | Leave each pointer to be inferred |
| `--fast-stack-alloca` | on | Inline the alloca pool bump. Much faster. The cursor is process-global, so this is single-threaded only; concurrent calls are not thread-safe |
| `--clang PATH` | auto | Patched clang |
| `--toolchain DIR` | auto | Toolchain directory that contains `bin/clang` |
| `--color-report` | off | Print each pointer color. Does not change the build |
| `--arch amd64` | `amd64` | `goc go` accepts amd64 only |

Every flag: `./cmd/goc go --help`.

`goc build` without `--goabi` stays at `-O0`, and those three stay off unless an inherited variable turns them on. The P28 goldens depend on that. `goc build --goabi` turns on morestack and splittable frames. amd64 also records sptr maps. arm64 cannot. To match `goc go`, also pass `-O3 --default-ptr-color cptr --fast-stack-alloca`. Repository QuickJS scripts still call `goc build` through the old variables. When both a flag and a variable are present, the flag wins.

`--fixed-g` and `--opt-extra=-inline-threshold=250` stay off.

## 8. Do not assume these work

- Calling libc on this path, including `printf`, `malloc`, and `memcpy`. Print from Go with `fmt`. libc is the QuickJS build (`scripts/qjs-cli-build.sh`).
- Dropping an arbitrary third-party C tree into the directory. Signatures are integers, pointers, floating-point, and small structs.
- Handing a stack pointer to another goroutine.
- linux/arm64. `--arch arm64` can emit a goobj. `goc go` does not link it.
- Variadic, x87, exception, or AVX entry points that Go can call.

Next: pointer colors in [syntax-guide.md](syntax-guide.md), the toolchain directory in [toolchain.md](toolchain.md).
