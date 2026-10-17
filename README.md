# GOC

English | [简体中文](README.zh-CN.md)

A C dialect that Go calls directly, without cgo.

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

goc compiles C into Go object files. The C runs on the goroutine stack, and Go calls it like an ordinary Go function. goc is experimental; the host is linux/amd64, targets are amd64 and (under qemu or on arm64) arm64, with Go 1.18–1.27.

## Why

Go's goroutine stacks start small and move when they grow. Every Go function checks for stack space on entry, and the compiler records which stack slots hold pointers so the runtime can fix them after a move. C code does neither, so cgo switches to a large system stack on every call, which costs tens of nanoseconds.

goc makes C play by Go's stack rules. A patched Clang/LLVM adds the entry stack check, the pc tables and the stack maps Go needs, and C pointers get a color so the compiler knows which ones point into the stack:

| Color | Points to |
|---|---|
| `cptr` | C heap or globals; can be stored anywhere |
| `sptr` | the goroutine stack; lives in registers and stack slots, fixed up when the stack moves |
| `uptr` | maybe the stack; stack addresses are stored relative to the stack top, so heap copies stay valid |
| `gptr` | the Go heap |

A plain `T *` is inferred from how it is used, so existing C such as QuickJS needs almost no changes. Returning the address of a local is a compile error. The C still gets LLVM's full optimizer.

QuickJS compiled with goc, V8-v7 score (2026-10-09, Ryzen 9 7900X): goc-ng is 92% of native QuickJS-ng and 4.1× Goja; goc-bellard is 90% of the gcc build of Bellard's QuickJS and 4.7× Goja. Both pass the same test262 sample and official tests as their native builds.

![Speed relative to native ng](docs/benchmark/charts/overview-speed.svg)

## Quick start

```bash
tar -xzf goc-0.0.2-linux-amd64.tar.gz      # from the Releases page
export PATH="$PWD/goc-0.0.2-linux-amd64/bin:$PATH"
goc check                                  # PASS goc check
goc go ./mypkg                             # compiles mypkg/*.c into the Go package
```

```c
// add.c
#include "goc.h"
int hello_add(int a, int b) { return a + b; }
```

```go
// main.go
func hello_add(a, b int32) int32           // body is in add.c
```

## Documentation

Most documents are in Chinese.

| | |
|---|---|
| [docs/quickstart.md](docs/quickstart.md) | Install, first program, flags, C calling Go |
| [docs/language.md](docs/language.md) | Pointer colors and the language contract |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Pipeline, directories, design documents |
| [docs/build-from-source.md](docs/build-from-source.md) | Building Clang, the passes and the release archive |
| [docs/benchmark/README.md](docs/benchmark/README.md) | The five-engine benchmark and how to rerun it |
| [ports/quickjs/README.md](ports/quickjs/README.md) | Building and testing QuickJS with goc |
| [docs/contributing.md](docs/contributing.md) · [CONTRIBUTING.md](CONTRIBUTING.md) | Tests, extending goc, known gaps |
| [docs/roadmap.md](docs/roadmap.md) | What is not done yet |

## License

MIT, see [LICENSE](LICENSE). The patched LLVM/Clang is Apache-2.0 WITH LLVM-exception, see [NOTICE](NOTICE).
