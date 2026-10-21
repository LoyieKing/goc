# GOC

[English](README.md) | 简体中文

一种让 Go 直接调用、不经过 cgo 的 C 方言。

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

goc 把 C 编成 Go 的目标文件。C 跑在 goroutine 的栈上，Go 像调用普通 Go 函数一样调用它。goc 还在实验阶段：宿主是 linux/amd64，目标是 amd64 和 arm64（qemu 或 arm64 机器上运行），Go 1.18–1.27。

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

完整规则见 [docs/language.md](docs/language.md)。

#### 编译

我们基于 Clang/LLVM 修改出了 goc 的编译器：前端负责检查和推断指针颜色，后端生成符合 Go 栈规则的机器码，最后直接输出 Go 工具链可以链接的目标文件。这样 goc 能和 Go 无缝交互，同时 C 代码仍然享有 LLVM 的全套优化，保持生产级的 C 性能：V8 基准上，goc-ng 达到原生 QuickJS-ng 的约 92%，是纯 Go 实现的 JS 引擎 Goja 的约 4.1 倍；goc-bellard 达到 gcc 版 Bellard 的约 90%，是 Goja 的约 4.7 倍（见 [docs/benchmark](docs/benchmark/README.md)）。

用 goc 编的 QuickJS，V8-v7 分数（2026-10-09，Ryzen 9 7900X）：goc-ng 是原生 QuickJS-ng 的 92%、Goja 的 4.1 倍；goc-bellard 是 gcc 编的 Bellard QuickJS 的 90%、Goja 的 4.7 倍。两者的 test262 抽样和官方测试结果都和各自的原生版一致。

![相对原生 ng 的速度](docs/benchmark/charts/overview-speed.svg)

## 上手

```bash
tar -xzf goc-0.0.3-linux-amd64.tar.gz      # 从 Releases 页下载
export PATH="$PWD/goc-0.0.3-linux-amd64/bin:$PATH"
goc check                                  # PASS goc check
goc go ./mypkg                             # 把 mypkg/*.c 编进这个 Go 包
```

```c
// add.c
#include "goc.h"
int hello_add(int a, int b) { return a + b; }
```

```go
// main.go
func hello_add(a, b int32) int32           // 函数体在 add.c
```

## 文档

| | |
|---|---|
| [docs/quickstart.md](docs/quickstart.md) | 安装、第一个程序、参数、C 调用 Go |
| [docs/language.md](docs/language.md) | 指针颜色和语言合同 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 流水线、目录、设计文档 |
| [docs/build-from-source.md](docs/build-from-source.md) | 编 Clang、pass 和发布包 |
| [docs/benchmark/README.md](docs/benchmark/README.md) | 五引擎对照和复现方法 |
| [ports/quickjs/README.md](ports/quickjs/README.md) | 用 goc 编译和测试 QuickJS |
| [docs/contributing.md](docs/contributing.md) · [CONTRIBUTING.md](CONTRIBUTING.md) | 测试、扩展、已知缺口 |
| [docs/roadmap.md](docs/roadmap.md) | 还没做的事 |

## 许可

MIT，见 [LICENSE](LICENSE)。打过补丁的 LLVM/Clang 是 Apache-2.0 WITH LLVM-exception，见 [NOTICE](NOTICE)。
