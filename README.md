# GOC

一个 C 语言方言，旨在解决 Go 无法与 C 语言编写的库高效通信的问题。

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

goc 编译出来的 C 代码可以直接跑在 goroutine 的栈上，Go 调用它就像调用一个普通 Go 函数，不需要经过 cgo。目前是实验性质，只支持 linux/amd64。

## 背景

C 语言和 Go 无法高效交互，核心原因是 Go 的可增长栈。

### Go 的可增长栈

为了支持大量 goroutine 同时存在，Go 给每个 goroutine 只分配很小的初始栈（几 KB）。但 Go 程序几乎从来不会栈溢出，因为每个 Go 函数入口都会检查栈够不够用；不够时，运行时会分配一块更大的新栈，把旧栈的内容整体搬过去，再继续执行。

栈会搬家，带来的问题是：指向栈的指针在搬家后会失效，变成野指针。Go 分几种情况处理：

- **栈上有指向栈的指针。** 编译器会记录每个函数的栈上哪些位置存的是指针。搬栈时，运行时根据这份记录，把指向旧栈的指针逐个改成指向新栈。这要求每个函数的栈帧大小在编译期固定，所以 Go 不支持在栈上分配变长数组。
- **堆上有指向栈的指针。** 搬栈时运行时只修正栈上的指针，不会去改堆里的。所以 Go 必须保证堆上永远不会有指向栈的指针。它的做法是逃逸分析：一个变量的地址只要可能被存到堆上或全局变量里，编译器就直接把这个变量分配到堆上。例如：

  ```go
  var sink *int

  func f() {
      x := 42
      sink = &x // x 会被编译器放到堆上，而不是栈上
  }
  ```

- **Go 调用非 Go 代码。** C 代码不知道栈会搬家，不会在入口检查栈够不够，编译器也没有它的指针记录。所以 C 代码不能直接跑在 goroutine 的栈上，Go 的做法是每次调用都换到一个足够大、不会搬家的栈上去执行它。这样做很安全，但每次调用都要付出一笔固定开销。

### 已有方案

- **cgo**（Go 官方）：每次调用 C 函数都切换到系统线程的大栈上执行，并通知调度器。安全、通用，但每次调用有几十纳秒的固定开销，对调用频繁、每次很短的 C 函数来说是明显的瓶颈。
- **[directcgo](https://github.com/maxpoletaev/directcgo)**（社区实验项目）：先把 goroutine 的栈撑到足够大（64 KB），然后直接在 goroutine 栈上调用 C 函数，不切换栈。调用开销降到几纳秒，但每个 goroutine 都要占用一大块栈内存；C 函数用的栈一旦超过预留，就可能写坏内存。只适合短小、栈用量可预估的函数。
- **syscall 及其衍生**（如 purego）：不需要 C 编译器就能调用已编译好的动态库，本质上同样是切换到系统栈执行，性能和 cgo 在同一量级。直接在 goroutine 栈上执行外部代码的写法，则可能出现爆栈问题。

这几种方案在“使用已经编译好的 C 库”上都做得很好。但如果我们能重新编译 C 库，就能更进一步：让 C 代码自己懂 Go 的栈。

### 我们的方案

发明一种 C 的方言：goc。为 C 引入 Go 可增长栈的特性，让 Go 能直接调用 goc 代码，而不用考虑任何栈风险。

goc 编译出来的每个函数都和 Go 函数一样：入口会检查栈够不够，编译器会记录栈上的指针，栈搬家时指针会被正确修正。所以它可以直接跑在 goroutine 的栈上，Go 调用它时不需要切换栈。

#### 栈指针

这是整个方案的核心。C 代码里到处是指针，而 Go 搬栈时只会修正它知道的那些指针。要让 C 安全地跑在会搬家的栈上，编译器必须知道每一个指针可能指向哪里、会被存到哪里。

为此，goc 给 C 的指针加上了“颜色”：

| 指针色 | 含义 |
|---|---|
| `cptr` | 指向栈以外的内存（C 堆、全局变量），可以存到任何地方 |
| `sptr` | 指向栈上的对象，只能放在栈上，不能存到堆或全局变量里；栈搬家时由运行时修正 |
| `uptr` | 可能指向栈也可能不指向栈，可以存到任何地方；指向栈时存的是相对位置，搬家后依然有效 |
| `gptr` | 指向 Go 的堆对象，遵守 Go 垃圾回收的规则 |

不同颜色之间不能随意互相转换。普通的 `T *` 由编译器根据指针的用法自动推断成合适的颜色，所以像 QuickJS 这样的现有 C 代码几乎不用改写。例如把指向栈的指针存进全局变量是允许的，编译器会自动把这处存储改成 `uptr` 编码；而真正无法补救的用法（比如返回指向局部变量的指针）会在编译时报错：

```c
#include "goc.h"

static int *g_slot;

void keep(sptr(int) p) {
  g_slot = p;       /* 允许：自动按 uptr 存储，读出时自动还原 */
}

int *bad_return(void) {
  int local = 1;
  return &local;    /* 编译错误：不能返回指向栈的指针 */
}
```

完整规则见 [docs/syntax-guide.md](docs/syntax-guide.md)。

#### 编译

我们基于 Clang/LLVM 修改出了 goc 的编译器：前端负责检查和推断指针颜色，后端生成符合 Go 栈规则的机器码，最后直接输出 Go 工具链可以链接的目标文件。这样 goc 能和 Go 无缝交互，同时 C 代码仍然享有 LLVM 的全套优化，保持生产级的 C 性能：同一份 QuickJS，goc 编译的版本在 V8 基准上达到原生构建的约 93%，是纯 Go 实现的 JS 引擎 Goja 的 4.5 倍（见下文“跑分”）。

## 跑分

以 QuickJS 作为示例：goc 能完整编译 QuickJS-ng 和 Fabrice Bellard 的原版 QuickJS。下面五个 JS 引擎在同一台机器、同一时段测量（2026-09-27）：

| 引擎 | 说明 |
|---|---|
| goc-ng | goc 编译的 QuickJS-ng |
| goc-bellard | goc 编译的 Bellard 原版 QuickJS |
| native ng | 原生编译的 QuickJS-ng（clang） |
| native Bellard | 原生编译的 Bellard QuickJS（gcc） |
| Goja | 纯 Go 实现的 JS 引擎 |

![相对 native ng 的速度](docs/benchmark/charts/overview-speed.png)

| 套件 | goc-ng | goc-bellard | native ng | native Bellard | Goja | 怎么读 |
|---|---:|---:|---:|---:|---:|---|
| V8-v7 总分 | 1119 | 1247 | 1203 | 1558 | 247 | 分数，越高越快 |
| SunSpider 几何平均（ms） | 16.68 | 15.50 | 15.81 | 10.64 | 97.27 | 时间，越低越快 |
| microbench 几何平均（ns） | 55.5 | 45.4 | 52.3 | 32.4 | 195.6 | 时间，越低越快 |
| 函数调用微基准（calls/ms） | 21596 | 20781 | 23337 | 30784 | 5106 | 分数，越高越快 |
| test262 抽样通过 | 1502/1526 | 1501/1526 | 1502/1526 | 1501/1526 | 1453/1526 | |
| QuickJS 官方测试 | 69/77 | 73/77 | 69/77 | 73/77 | 58/77 | |

**正确性**：两个 goc 构建和各自的原生构建逐项结果完全一致。

![正确性](docs/benchmark/charts/overview-correct.png)

**V8-v7 各子项**：

![V8-v7](docs/benchmark/charts/v8.png)

| 子项 | goc-ng | goc-bellard | native ng | native Bellard | Goja |
|---|---:|---:|---:|---:|---:|
| Richards | 768 | 815 | 796 | 1029 | 265 |
| DeltaBlue | 734 | 830 | 789 | 947 | 276 |
| Crypto | 880 | 931 | 850 | 1377 | 118 |
| RayTrace | 1478 | 1654 | 1787 | 1943 | 245 |
| EarleyBoyer | 2077 | 2311 | 2270 | 2525 | 427 |
| RegExp | 360 | 442 | 400 | 547 | 196 |
| Splay | 2825 | 3240 | 3162 | 3484 | 438 |
| NavierStokes | 1622 | 1759 | 1593 | 2817 | 185 |
| **总分** | **1119** | **1247** | **1203** | **1558** | **247** |

**SunSpider 与 microbench**：

![SunSpider](docs/benchmark/charts/sunspider.png)

![microbench](docs/benchmark/charts/micro-groups.png)

**函数调用微基准**：

![microcall](docs/benchmark/charts/microcall.png)

**内存**（峰值 RSS，MiB；最后一行是每多一个 JS 运行时的增量，KiB）：

![峰值 RSS](docs/benchmark/charts/mem-peak.png)

| 负载 | goc-ng | goc-bellard | native ng | native Bellard | Goja |
|---|---:|---:|---:|---:|---:|
| 空脚本 | 8.5 | 8.5 | 3.0 | 2.8 | 5.9 |
| V8-v7 整套 | 164.6 | 155.9 | 155.5 | 147.0 | 1218.9 |
| SunSpider（最大值） | 14.8 | 15.8 | 8.1 | 7.5 | 21.8 |
| microbench | 11.1 | 10.9 | 4.5 | 4.4 | 381.7 |
| 每多一个运行时（KiB） | 225 | 199 | 211 | 190 | 102 |

![多实例](docs/benchmark/charts/mem-scaling.png)

goc 构建比原生多出约 6 到 9 MiB 的固定开销（主要是 Go 运行时本身），负载越大这部分占比越小。

goc 比原生慢的部分，主要来自为了遵守 Go 栈规则而多做的工作：栈检查、指针编解码、栈上的指针记录等。goc-bellard 和 native Bellard 差距更大，主要是因为后者用 gcc 编译；换成同样用 clang 编译，差距就和 ng 一样，只有 6% 到 7%。详细分析见 [docs/perf-gap.md](docs/perf-gap.md)，全部数据和测量方法见 [docs/benchmark.md](docs/benchmark.md)。

## 架构

```mermaid
flowchart LR
    A["C 源码"] --> B["改造的 Clang 前端<br/>检查、推断指针颜色"]
    B --> C["LLVM 优化"]
    C --> D["goc 后端<br/>栈检查、指针记录"]
    D --> E["Go 目标文件"]
    E --> F["Go 链接器<br/>和 Go 代码链接成一个程序"]
```

| 目录 | 作用 |
|---|---|
| `cmd/goc` | 命令行入口 |
| `include/goc.h` | 指针颜色的写法 |
| `clang/` | Clang 前端的修改 |
| `frontend/` | 指针颜色推断 |
| `backend/` | 生成符合 Go 栈规则的代码和 Go 目标文件 |
| `runtime/` | goc 的运行时支持 |
| `tests/` | 测试，包括 QuickJS |
| `scripts/` | 构建和跑分脚本 |
| `docs/` | 文档 |

更多细节见 [docs/architecture.md](docs/architecture.md)。

## 使用方法

**依赖**：linux/amd64；LLVM 19 工具链（`clang-19`、`llc-19` 等）、`cmake`、`ninja`、Go 1.24+、`python3`。

**1. 编译 goc 使用的 Clang**（约几十分钟，细节见 [clang/README.md](clang/README.md)）：

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
export GOC_ROOT="$(pwd)"

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
  -S "$LLVM_SRC/llvm" -B "$GOC_ROOT/third_party/llvm-19.1.7-clang-build"
ninja -C "$GOC_ROOT/third_party/llvm-19.1.7-clang-build" -j"$(nproc)" clang
export GOC_CLANG="$GOC_ROOT/third_party/llvm-19.1.7-clang-build/bin/clang"
```

**2. 确认能用**：

```bash
./cmd/goc version
./cmd/goc test --p28
```

**3. 从 Go 调用 goc**：完整例子在 `tests/goabi`，一条命令跑通：

```bash
./scripts/test-p29-goabi.sh
```

C 这边写普通函数，Go 这边写一个没有函数体的声明，就能直接调用：

```c
// goabi.c
int goabi_add2(int a, int b) { return a + b; }
```

```go
// main.go
func goabi_add2(a, b int32) int32
```

**4. 编译 QuickJS**：

```bash
git clone https://github.com/quickjs-ng/quickjs.git third_party/quickjs-ng
./scripts/qjs-cli-build.sh       # 输出 build/qjs/qjscli
```

Bellard 版 QuickJS 放到 `third_party/quickjs-bellard` 后，用 `QJS_FLAVOR=bellard ./scripts/qjs-cli-build.sh` 编译。跑分脚本和方法见 [docs/benchmark.md](docs/benchmark.md)，每一步的细节和常见问题见 [docs/guide.md](docs/guide.md)。

**目前的限制**：

- 只支持 linux/amd64。
- Go 调用 goc 的函数签名只支持基本类型、指针和小结构体。
- 指向栈的指针只能在创建它的 goroutine 里使用。
- goc 调用 Go 目前还需要手写调用代码。

## 未来规划

详见 [docs/todo.md](docs/todo.md)：

- **支持 linux/arm64。**
- **goc 调用 Go**：像 Go 调用 goc 一样，只写声明就能调用。
- **跨 goroutine 使用栈指针。**
- **指针颜色的可视化和编辑器支持**：在编辑器里查看每个指针被推断成了什么颜色。
- **继续缩小和原生 C 的性能差距。**

## 文档

| 文档 | |
|---|---|
| [docs/guide.md](docs/guide.md) | 使用指南 |
| [docs/syntax-guide.md](docs/syntax-guide.md) | 语言规则 |
| [docs/architecture.md](docs/architecture.md) | 编译流程 |
| [docs/benchmark.md](docs/benchmark.md) | 跑分 |
| [docs/perf-gap.md](docs/perf-gap.md) | 性能差距分析 |
| [docs/todo.md](docs/todo.md) | 后续计划 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献 |

## License

MIT — Copyright (c) 2026 Loyie King. 见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。
