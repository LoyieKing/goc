# goc

Pointer-colored C that runs on the **goroutine stack**: morestack, stackmaps,
and write barriers, so C can be linked into a Go binary without a cgo stack
switch.

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## What it is

`goc` adds pointer colors (`cptr` / `sptr` / `uptr` / `auto_ptr` / `gptr`) to a
C11 subset. The product frontend is patched **Clang 19.1.7**. The product
lower path is Clang IR → llc → `elfpack` → a Go object file.

QuickJS-ng's four translation units build this way and run on a goroutine
stack. See [docs/benchmark.md](docs/benchmark.md) for the comparison.

## Quick start

**Deps:** `clang-19`, `clang++-19`, `llc-19`, `llvm-config-19`, `cmake`,
`ninja`, Go 1.24+, `python3`, `rg`.

```bash
git clone https://github.com/LoyieKing/goc.git
cd goc
export GOC_ROOT="$(pwd)"

# Patched Clang: clang/README.md
export GOC_CLANG=/path/to/llvm-19.1.7-clang-build/bin/clang

./cmd/goc test --p28
./cmd/goc build examples/hello_colors.c -o /tmp/hello_colors.o
```

QuickJS-ng (clone into `third_party/quickjs-ng`, then):

```bash
./scripts/qjs-build.sh          # engine smoke on a goroutine stack
./scripts/qjs-cli-build.sh      # CLI
./scripts/microcall-bench.sh    # call microbenchmark
```

Without a patched clang, `./cmd/goc test --p27` uses the out-of-tree plugin
and system `clang-19`.

## Pointer colors

| Color | Meaning |
|-------|---------|
| `cptr<T>` | Non-stack object (C heap, global, arena). Not the Go heap. |
| `sptr<T>` | Object on the current goroutine stack. The raw word stays in a register or a stack slot. |
| `uptr<T>` | Encoded `cptr\|sptr`. MSB 0 is an absolute address; MSB 1 is an int64 offset from `g.stack.hi`. Decode before use. |
| `auto_ptr<T>` | Inferred. `T *` is this. A stack pointer stored through a non-stack `T *` is encoded as `uptr`. |
| `gptr<T>` | Go heap pointer. Stackmap plus a write barrier. No implicit conversion to the other colors. |

There is no `dsptr`. Contract: [docs/syntax-guide.md](docs/syntax-guide.md).

## Pipeline

```text
.c  →  Clang 19 Sema (colors, escape)
    →  LLVM IR
    →  color-escape
    →  O3, then stack maps
    →  llc ISel
    →  elfpack → goobj
    →  Go link
```

Detail: [docs/architecture.md](docs/architecture.md).

## Layout

```text
cmd/goc           driver
include/goc.h     color attributes
clang/            patches, Sema, plugin
backend/          realbody lower, elfpack, machine passes
frontend/         color-escape and legacy helpers
runtime/          uptr encode/decode
tests/            goldens, QuickJS smoke, CLI, microcall
docs/             contract, architecture, benchmark
scripts/          Clang patches, QuickJS build, benches
```

## Limits

- Go amd64 ABIInternal covers the int/pointer subset (at most six arguments). Variadics and other aggregates stay SysV.
- AVX, x87, and EH are not on the Go-callable path.
- The QuickJS build uses a freestanding libc shim, not glibc. The alloca pool is single-goroutine.
- `uptr` decode uses the owner goroutine's `stack.hi`. Decoding with another `g` is an error.

## Docs

| Doc | |
|-----|--|
| [docs/syntax-guide.md](docs/syntax-guide.md) | Language contract |
| [docs/architecture.md](docs/architecture.md) | Pipeline |
| [docs/glossary.md](docs/glossary.md) | Terms |
| [docs/benchmark.md](docs/benchmark.md) | Latest comparison |
| [clang/README.md](clang/README.md) | Build patched Clang |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Patches |
| [SECURITY.md](SECURITY.md) | Reporting |

## 中文

`goc` 把带指针色的 C 编到 goroutine 用户栈上，和 Go 共用 morestack 与
stackmap，而不是走 cgo。`uptr` 用最高位区分绝对地址和相对 `g.stack.hi` 的
偏移。语法合同见 [docs/syntax-guide.md](docs/syntax-guide.md)。横向跑分见
[docs/benchmark.md](docs/benchmark.md)。

## License

MIT — Copyright (c) 2026 Loyie King. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
