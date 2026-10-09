# 从零编译出完整的 goc 包

这篇是给要自己编发布包的人。只用 `goc` 的话，下载现成的包：[quickstart.md](quickstart.md)。

产物是一个目录（或 `.tar.gz`）。里面有驱动、打过补丁的 Clang 19.1.7、`opt`、`llc`、`llvm-mc`、`llvm-objdump`、`goc-llc`、`goc-color-escape`、`GocStackMap.so`，以及预编译的 `elfpack` 和 `goc-lower`。用户把 `bin` 放到 `PATH` 上即可。Clang 不进 git。

## 1. 宿主工具

linux/amd64。需要：

- `git`、`curl`
- Go 1.24 或更新
- `cmake`、`ninja`、`patchelf`
- 用来编 Clang 的 C/C++ 编译器：`clang-19` 和 `clang++-19`。没有时脚本会退到系统 `clang` 或 `gcc`

`patchelf` 用来去掉二进制里指向编译机的绝对 `RUNPATH`，让包只靠旁边的 `lib/libLLVM.so.19.1`。打包脚本在没有 `patchelf` 时用 `python3` 改写 `RUNPATH`，并用 `python3` 改写编译机路径。仓库里的测试脚本还会用到 `python3` 和 `rg`。

## 2. 克隆

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

后面的命令都在仓库根目录执行。

## 3. 编打过补丁的 Clang

`scripts/build-clang.sh` 下载 LLVM 19.1.7，打上 `clang/patches` 里的补丁，再编进 `third_party/llvm-19.1.7-clang-build`。目标是 X86 和 AArch64，共享 `libLLVM`，并带上 `clang`、`opt`、`llc`、`llvm-mc`、`llvm-objdump` 和 `llvm-config`。

Release 构建大约要几十分钟到一个多小时。构建目录已被 gitignore。

```bash
./scripts/build-clang.sh
```

已经编过、且 `bin/clang`、`opt`、`llc`、`llvm-mc`、`llvm-objdump` 都在时，脚本直接退出。要重编：`GOC_REBUILD_CLANG=1 ./scripts/build-clang.sh`。

## 4. 编 goc 自己的 pass

```bash
./scripts/build-passes.sh
```

这一步用宿主的 `clang++-19`、刚才的 `llvm-config` 和 X86 后端头文件编出。打过补丁的 `clang++` 会把 LLVM 头文件里的 `return &局部` 判成栈指针逃逸，所以不用它来编 pass：

- `frontend/color-escape/build/goc-color-escape`
- `backend/build/pass-out/goc-llc`
- `backend/build/pass-out/GocStackMap.so`

## 5. 打成一个包

```bash
./scripts/pack-release.sh dist/goc-0.0.1-linux-amd64.tar.gz
```

版本号读仓库根目录的 `VERSION`。`goc toolchain pack DEST` 做的是同一件事。

解压后的布局：

| 路径 | 内容 |
|---|---|
| `bin/goc` | 指向 `../cmd/goc` |
| `bin/clang` | 打过补丁的 Clang |
| `bin/opt`、`bin/llc`、`bin/llvm-mc`、`bin/llvm-objdump` | 同一个 LLVM 的工具 |
| `bin/elfpack` | 预编译的 goobj 打包器 |
| `bin/goc-lower` | 预编译的 realbody 辅助程序（签名、thunk、meta） |
| `lib/libLLVM.so.19.1`、`lib/clang/` | Clang 的运行库和资源目录 |
| `passes/` | `goc-llc`、`goc-color-escape`、`GocStackMap.so` |
| `cmd/`、`scripts/`、`include/`、`backend/realbody/`、`examples/hello/` | 驱动和自检样例 |

驱动只在这三个位置找 Clang，找不到就停，不用系统 `clang-19`：

1. `bin/clang`（发布包）
2. `third_party/llvm-19.1.7-clang-build/bin/clang`（本机编出来的）
3. `third_party/llvm-clang-build/bin/clang`

没有 `--clang`，也没有 `--toolchain`。

## 6. 确认这个包能用

```bash
tar -C /tmp -xzf dist/goc-0.0.1-linux-amd64.tar.gz
export PATH="/tmp/goc-0.0.1-linux-amd64/bin:$PATH"
goc check
```

仓库里也可以不打包，直接：

```bash
./cmd/goc check
./cmd/goc test --p28
./scripts/test-p29-goabi.sh
./backend/realbody/check_arm64.sh
./cmd/goc test --p27
```

`scripts/ci-test.sh` 就是这五项。GitHub Actions 在每次推送到 `main` 和每个 pull request 上跑它。打 `v*` tag 时，release workflow 先跑同样的测试，再执行 `pack-release.sh`，把压缩包传到对应的 GitHub Release。tag 必须和 `VERSION` 一致，例如 `VERSION` 是 `0.0.1` 时 tag 是 `v0.0.1`。

QuickJS 和跑分不在 CI 里。它们要另外下载引擎源码。

---

# Build a complete goc package from source

This page is for producing the release archive. To use `goc`, download one: [quickstart.md](quickstart.md).

The result is a directory (or a `.tar.gz`). It contains the driver, patched Clang 19.1.7, `opt`, `llc`, `llvm-mc`, `llvm-objdump`, `goc-llc`, `goc-color-escape`, `GocStackMap.so`, and prebuilt `elfpack` and `goc-lower`. The user puts `bin` on `PATH`. Clang is not committed to git.

## 1. Host tools

linux/amd64. You need:

- `git`, `curl`
- Go 1.24 or newer
- `cmake`, `ninja`, `patchelf`
- A C/C++ compiler to build Clang: `clang-19` and `clang++-19`. Without them the script falls back to system `clang` or `gcc`

`patchelf` strips the absolute `RUNPATH` that points at the build machine, so the package loads `lib/libLLVM.so.19.1` beside itself. Without `patchelf`, the pack script uses `python3` to shorten that `RUNPATH`, and it uses `python3` to rewrite the checkout path. Repository tests also use `python3` and `rg`.

## 2. Clone

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
```

Run the later commands from the repository root.

## 3. Build the patched Clang

`scripts/build-clang.sh` downloads LLVM 19.1.7, applies the patches in `clang/patches`, and builds into `third_party/llvm-19.1.7-clang-build`. The targets are X86 and AArch64, with a shared `libLLVM`, plus `clang`, `opt`, `llc`, `llvm-mc`, `llvm-objdump`, and `llvm-config`.

A Release build takes tens of minutes to a bit over an hour. The build directory is gitignored.

```bash
./scripts/build-clang.sh
```

When `bin/clang`, `opt`, `llc`, `llvm-mc`, and `llvm-objdump` are already there, the script exits. Rebuild with `GOC_REBUILD_CLANG=1 ./scripts/build-clang.sh`.

## 4. Build goc's own passes

```bash
./scripts/build-passes.sh
```

This uses the host `clang++-19`, that `llvm-config`, and the X86 backend headers to produce. The patched `clang++` reports `return &local` inside LLVM headers as a stack-pointer escape, so it is not the compiler for these passes:

- `frontend/color-escape/build/goc-color-escape`
- `backend/build/pass-out/goc-llc`
- `backend/build/pass-out/GocStackMap.so`

## 5. Pack one archive

```bash
./scripts/pack-release.sh dist/goc-0.0.1-linux-amd64.tar.gz
```

The version is the `VERSION` file at the repository root. `goc toolchain pack DEST` does the same thing.

Layout after unpacking:

| Path | Contents |
|---|---|
| `bin/goc` | Symlink to `../cmd/goc` |
| `bin/clang` | Patched Clang |
| `bin/opt`, `bin/llc`, `bin/llvm-mc`, `bin/llvm-objdump` | Tools from that same LLVM |
| `bin/elfpack` | Prebuilt goobj packer |
| `bin/goc-lower` | Prebuilt real-body helper (signatures, thunks, meta) |
| `lib/libLLVM.so.19.1`, `lib/clang/` | Clang's runtime library and resource directory |
| `passes/` | `goc-llc`, `goc-color-escape`, `GocStackMap.so` |
| `cmd/`, `scripts/`, `include/`, `backend/realbody/`, `examples/hello/` | The driver and the self-check sample |

The driver looks for Clang in these three places only, then stops. It does not use system `clang-19`:

1. `bin/clang` (a release)
2. `third_party/llvm-19.1.7-clang-build/bin/clang` (built in this checkout)
3. `third_party/llvm-clang-build/bin/clang`

There is no `--clang` and no `--toolchain`.

## 6. Check the archive

```bash
tar -C /tmp -xzf dist/goc-0.0.1-linux-amd64.tar.gz
export PATH="/tmp/goc-0.0.1-linux-amd64/bin:$PATH"
goc check
```

From the checkout, without packing:

```bash
./cmd/goc check
./cmd/goc test --p28
./scripts/test-p29-goabi.sh
./backend/realbody/check_arm64.sh
./cmd/goc test --p27
```

`scripts/ci-test.sh` is those five. GitHub Actions runs it on every push to `main` and every pull request. On a `v*` tag, the release workflow runs the same tests, then `pack-release.sh`, and uploads the archive to that GitHub Release. The tag has to match `VERSION`: `0.0.1` in the file means the tag `v0.0.1`.

QuickJS and the benchmarks are not in CI. They need an engine tree downloaded separately.
