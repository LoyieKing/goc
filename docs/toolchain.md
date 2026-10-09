# 工具链和 `goc go`

linux/amd64。一条 `goc go` 把一个 Go 包里的 `.c` 编进去。默认就是 QuickJS 验证过的那组配置，写在命令行参数里。指针色、单字 `uptr`、默认架构都不改。`--fixed-g` 和 `--opt-extra=-inline-threshold=250` 仍然关着。

下载即用见 [quickstart.md](quickstart.md)。从仓库编出发布包见 [build-from-source.md](build-from-source.md)。

## 命令

在仓库根目录：

```bash
./cmd/goc toolchain status
./cmd/goc check
./cmd/goc go examples/hello
./examples/hello/hello
```

`goc check` 复制 `examples/hello` 到临时目录，编完再跑，输出必须是 `hello 42`。它接受和 `goc go` 一样的参数。

`goc go [dir] [-o bin] [参数] [-- clang 参数...]`：

- 只编这个目录下的 `*.c`，不递归。
- `CGO_ENABLED=0`。Go 因此不去编这些 `.c`，也不调用系统 cc。C 里不能调 libc。要 libc 的仍走原来的 QuickJS 宿主（`scripts/qjs-cli-build.sh`）。
- 包里没有 `.s` 时写入 `goc_marker_amd64.s`。Go 拒绝没有任何汇编文件的无函数体声明。这个文件不是 C 函数的桩。
- Go 侧仍要写同名的无函数体声明。`int` 对 `int32`。
- `package main` 产出二进制，默认为 `<目录>/<目录名>`。其它包只做编译并打进构建缓存，不接受 `-o`。
- 对象文件在 `build/goc-go/<目录哈希>/`，经 `backend/tools/toolexec_pack_goobj.sh` 打进包。打进哪一个包由该包的编译器 `-p` 决定。

`goc go` 和 `goc check` 不读环境里已有的 `GOC_*`。要改默认就加参数。

`goc build` 不带 `--goabi` 时不套用这组默认。P28 黄金测试仍是 `-O0`。仓库脚本可以继续用同名环境变量调用 `goc build`；同一项又写了参数时，参数优先。

## 默认

| 参数 | `goc go` |
|---|---|
| `-O` | `3` |
| `--default-ptr-color` | `cptr` |
| `--fast-stack-alloca` | 开。快很多。游标是进程全局的，只能单线程用；多线程同时调用有线程安全问题 |

每个 Go 调 C 的 thunk 帧由编译器按该函数的栈参数计算，再向上取整到 16。amd64 另外加上保存的 `%rbp`。

morestack 前导、可分裂帧、sptr 栈图在 `goc go` 上一定打开，没有参数可以关掉。`goc build --goabi` 同样打开 morestack 和可分裂帧；amd64 还会打开 sptr 栈图。不带 `--goabi` 时这三项保持关闭，除非调用方用环境变量打开。arm64 记不了 sptr 栈图。

`goc go` 会把 clang 所在树的 `lib/` 加到本次进程的库搜索路径前面，这样 `goc-llc` 和 `goc-color-escape` 找得到 `libLLVM.so.19.1`。

全部参数见 `./cmd/goc go --help`。

## Clang 放哪

按这个顺序找打过补丁的 clang，找不到就停，不用系统 `clang-19`：

1. `bin/clang`（发布包）
2. `third_party/llvm-19.1.7-clang-build/bin/clang`（在仓库里编出来的）
3. `third_party/llvm-clang-build/bin/clang`

没有 `--clang`，也没有 `--toolchain`。

`opt`、`llc`、`llvm-mc`、`llvm-objdump` 用 clang 旁边的同名文件。pass 在发布包的 `passes/`，在仓库里则是已编好的 `backend/build/pass-out` 和 `frontend/color-escape/build`。

打包（本机已经编好 clang 和 pass 时）打出的是完整的 goc，不是单独的 clang 目录：

```bash
./cmd/goc toolchain pack dist/goc-linux-amd64.tar.gz
```

和 `scripts/pack-release.sh` 是同一个脚本。二进制不进 git。用户下载的是 GitHub Release 上的那个压缩包。

## 例子

`examples/hello`：`hello_add` 在 C 里是 `int`，在 Go 里是 `int32`，无函数体。

```go
func hello_add(a, b int32) int32
```

```c
#include "goc.h"
int hello_add(int a, int b) { return a + b; }
```
