# GOC

[English](README.md) | 简体中文

一种让 Go 直接调用、不经过 cgo 的 C 方言。

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

goc 把 C 编成 Go 的目标文件。C 跑在 goroutine 的栈上，Go 像调用普通 Go 函数一样调用它。goc 还在实验阶段：宿主是 linux/amd64，目标是 amd64 和 arm64（qemu 或 arm64 机器上运行），Go 1.18–1.27。

## 为什么

Go 的 goroutine 栈起步很小，长大时会整块搬走。每个 Go 函数入口都检查栈够不够，编译器还记下哪些栈槽是指针，搬栈后运行时据此修正。C 两样都不做，所以 cgo 每次调用都切到系统的大栈，每次几十纳秒。

goc 让 C 遵守 Go 的栈规则。打过补丁的 Clang/LLVM 生成 Go 需要的入口栈检查、pc 表和栈图；C 指针分颜色，编译器才知道哪些指向栈：

| 颜色 | 指向 |
|---|---|
| `cptr` | C 堆或全局，可以存到任何地方 |
| `sptr` | goroutine 栈；只待在寄存器和栈槽里，搬栈时被修正 |
| `uptr` | 可能是栈；栈地址存成相对栈顶的位置，存进堆里也不失效 |
| `gptr` | Go 堆 |

普通的 `T *` 按用法推断颜色，所以 QuickJS 这样现成的 C 几乎不用改。返回局部变量的地址是编译错误。C 仍然经过 LLVM 的完整优化。

用 goc 编的 QuickJS，V8-v7 分数（2026-10-09，Ryzen 9 7900X）：goc-ng 是原生 QuickJS-ng 的 92%、Goja 的 4.1 倍；goc-bellard 是 gcc 编的 Bellard QuickJS 的 90%、Goja 的 4.7 倍。两者的 test262 抽样和官方测试结果都和各自的原生版一致。

![相对原生 ng 的速度](docs/benchmark/charts/overview-speed.svg)

## 上手

```bash
tar -xzf goc-0.0.2-linux-amd64.tar.gz      # 从 Releases 页下载
export PATH="$PWD/goc-0.0.2-linux-amd64/bin:$PATH"
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
