# 从源码编出完整的 goc 包

这篇是给要自己编发布包的人。只用 `goc` 的话，下载现成的包：[quickstart.md](quickstart.md)。

产物是一个目录（或 `.tar.gz`）：Go 写的 `goc` 一个二进制、打过补丁的 Clang 19.1.7、`opt`、`llc`、`llvm-mc`、`llvm-objdump`、`goc-llc`、`libGocPasses.so`。用户把 `bin` 放到 `PATH` 上即可。Clang 不进 git。

## 1. 宿主工具

linux/amd64。需要：

- `git`、`curl`
- Go 1.23 或更新（仓库的 Go 代码用 go1.23 编；目标 Go 可以是 1.18–1.27，见下）
- `cmake`、`ninja`、`patchelf`
- 用来编 Clang 的 C/C++ 编译器：`clang-19` 和 `clang++-19`。没有时脚本会退到系统 `clang` 或 `gcc`

`patchelf` 用来去掉二进制里指向编译机的绝对 `RUNPATH`，让包只靠旁边的 `lib/libLLVM.so.19.1`；没有时打包脚本用 `python3` 改写。测试脚本还会用到 `python3`、`rg` 和 `lit`。

PATH 里的 `go` 是目标 Go。它比 1.23 老时，用 `GOC_HOST_GO=/path/to/go1.23+/bin/go` 指定编仓库工具的 Go（只有仓库脚本读这个变量，`goc` 本身不读）。

## 2. 克隆

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

后面的命令都在仓库根目录执行。

## 3. 编打过补丁的 Clang

`scripts/build-clang.sh` 下载 LLVM 19.1.7，打上 `clang/patches` 里的补丁、放入 `clang/sema/SemaGocColors.cpp`，再编进 `third_party/llvm-19.1.7-clang-build`。目标是 X86 和 AArch64，共享 `libLLVM`，带上 `clang`、`opt`、`llc`、`llvm-mc`、`llvm-objdump` 和 `llvm-config`。

Release 构建大约要几十分钟到一个多小时。构建目录已被 gitignore。

```bash
./scripts/build-clang.sh
```

已经编过、且 `bin/clang`、`opt`、`llc`、`llvm-mc`、`llvm-objdump` 都在时，脚本直接退出。要重编：`GOC_REBUILD_CLANG=1 ./scripts/build-clang.sh`。

`--slim` 编完后删掉目标文件、静态库和只在构建时用的子目录，只留 goc 和 `build-passes.sh` 用得到的部分（CI 和发布用它，缓存因此小得多）。删过的目录不能增量重编。

## 4. 编 goc 自己的 pass 和 goc

```bash
./scripts/build-passes.sh
go build -o bin/goc ./cmd/goc
```

pass 用宿主的 `clang++-19`、刚才的 `llvm-config` 和 X86 后端头文件编出（打过补丁的 `clang++` 会把 LLVM 头文件里的 `return &局部` 判成栈指针逃逸，所以不用它）：

- `build/llvm/goc-llc`
- `build/llvm/libGocPasses.so`（所有 IR pass，包括 goc-color-escape）

`bin/goc` 按自己的位置找工具：在 checkout 里是 `third_party/llvm-19.1.7-clang-build` 和 `build/llvm`，在发布包里是 `bin/` 和 `passes/`。没有参数或环境变量能换成别的工具；`bin/goc env` 打印它找到的路径。

## 5. 打成一个包

```bash
./scripts/pack-release.sh dist/goc-0.0.2-linux-amd64.tar.gz
```

版本号读仓库根目录的 `VERSION`。解压后的布局：

| 路径 | 内容 |
|---|---|
| `bin/goc` | Go 写的 goc（`go build -trimpath ./cmd/goc`） |
| `bin/clang`、`bin/clang-19` | 打过补丁的 Clang |
| `bin/opt`、`bin/llc`、`bin/llvm-mc`、`bin/llvm-objdump` | 同一个 LLVM 的工具 |
| `lib/libLLVM.so.19.1`、`lib/libclang-cpp.so.19.1`、`lib/clang/` | Clang 的运行库和资源目录 |
| `passes/` | `goc-llc`、`libGocPasses.so` |
| `include/` | `goc.h`、`goc_uptr.h` |
| `examples/hello/` | `goc check` 用的样例 |

## 6. 确认能用

```bash
tar -C /tmp -xzf dist/goc-0.0.2-linux-amd64.tar.gz
export PATH="/tmp/goc-0.0.2-linux-amd64/bin:$PATH"
goc check
```

仓库里不打包也可以：

```bash
bin/goc check
./scripts/ci-test.sh       # 文件大小、goc check、lit、e2e（含 arm64/qemu）、Go 单元测试、gocobj 金样
```

GitHub Actions 在每次推送到 `main` 和每个 pull request 上，用 10 个 Go 版本（1.18–1.27）跑 `scripts/ci-test.sh`，另外编一次 QuickJS-ng 跑冒烟。打 `v*` tag 时，release workflow 用同一个 clang 缓存跑同样的测试，再执行 `pack-release.sh`，解开压缩包跑 `goc check`，然后传到对应的 GitHub Release。tag 必须和 `VERSION` 一致，例如 `VERSION` 是 `0.0.2` 时 tag 是 `v0.0.2`。在 Actions 页手动运行 release workflow 是演练：压缩包留作 artifact，不发布。

完整的 QuickJS 核对和跑分不在 CI 里，它们要另外下载引擎源码：[ports/quickjs/README.md](../ports/quickjs/README.md)、[benchmark/README.md](benchmark/README.md)。
