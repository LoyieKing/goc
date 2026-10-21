# GOC

English | [简体中文](README.zh-CN.md)

A C dialect that Go calls directly, without cgo.

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

goc compiles C into Go object files. The C runs on the goroutine stack, and Go calls it like an ordinary Go function. goc is experimental; the host is linux/amd64, targets are amd64 and (under qemu or on arm64) arm64, with Go 1.18–1.27.

## Background

The core reason Go and C cannot interoperate efficiently is Go's growable stack.

### Go's growable stacks

To support huge numbers of goroutines, Go gives each goroutine a very small initial stack (a few KB). Yet Go programs almost never overflow the stack, because every Go function checks on entry whether there is enough stack left. If not, the runtime allocates a larger stack, copies the old stack over, and carries on.

Because the stack can move, any pointer into the stack becomes a dangling pointer after the move. Go handles this in a few cases:

- **Pointers on the stack that point into the stack.** The compiler records which stack slots of each function hold pointers. When the stack moves, the runtime uses that record to rewrite every pointer into the old stack so it points into the new one. This requires every stack frame to have a size fixed at compile time, which is why Go has no variable-length arrays on the stack.
- **Pointers on the heap that point into the stack.** When the stack moves, the runtime only fixes pointers on the stack, not on the heap. So Go must guarantee that the heap never points into a stack. It does this with escape analysis: if the address of a variable might be stored on the heap or in a global, the compiler simply allocates that variable on the heap. For example:

  ```go
  var sink *int

  func f() {
      x := 42
      sink = &x // the compiler puts x on the heap, not on the stack
  }
  ```

- **Go calling non-Go code.** C code does not know the stack can move, does not check for stack space on entry, and the compiler has no pointer records for it. So C cannot run directly on a goroutine stack. Go instead switches to a large stack that never moves for every call. This is safe, but every call pays a fixed cost.

### Existing approaches

- **cgo** (official): every C call switches to the system thread's large stack and notifies the scheduler. Safe and general, but each call costs tens of nanoseconds, which becomes a bottleneck for C functions that are called often and return quickly.
- **[directcgo](https://github.com/maxpoletaev/directcgo)** (experimental community project): first grows the goroutine stack to a large size (64 KB), then calls C directly on the goroutine stack without switching. Calls drop to a few nanoseconds, but every goroutine holds a large chunk of stack memory, and a C function that uses more stack than reserved can corrupt memory. It only suits short functions whose stack usage is predictable.
- **syscall and friends** (e.g. purego): call prebuilt shared libraries without a C compiler. Under the hood they also switch to the system stack, so performance is in the same range as cgo. Code that instead runs foreign functions directly on the goroutine stack can hit stack overflows.

All of these work well for using C libraries that are already compiled. But if we can recompile the C library, we can go a step further: teach the C code itself about Go's stack.

### Our approach

A new C dialect, goc, which brings Go's growable-stack rules to C, so Go can call goc code directly without any stack risk.

Every function goc compiles behaves like a Go function: it checks for stack space on entry, the compiler records the pointers it keeps on the stack, and those pointers are fixed up correctly when the stack moves. So it can run directly on the goroutine stack, and Go does not need to switch stacks to call it.

#### Stack pointers

This is the heart of the design. C code is full of pointers, and when Go moves a stack it only fixes the pointers it knows about. For C to run safely on a movable stack, the compiler must know where every pointer can point and where it can be stored.

goc therefore gives C pointers a "color":

| Color | Meaning |
|---|---|
| `cptr` | Points to memory outside the stack (C heap, globals). Can be stored anywhere. |
| `sptr` | Points to an object on the stack. Lives only on the stack; fixed up by the runtime when the stack moves. |
| `uptr` | May or may not point into the stack. Can be stored anywhere; stack addresses are stored as relative positions, so they stay valid after a move. |
| `gptr` | Points to a Go heap object and follows Go's garbage-collection rules. |

Colors cannot be converted into each other freely. A plain `T *` is inferred to the right color from how the pointer is used, so existing C code such as QuickJS needs almost no rewriting. For example, storing a stack pointer into a global is allowed; the compiler turns that store into a `uptr` encoding automatically. Uses that cannot be fixed, such as returning a pointer to a local variable, are compile errors:

```c
#include "goc.h"

static int *g_slot;

void keep(sptr(int) p) {
  g_slot = p;       /* OK: stored as uptr, restored on load */
}

int *bad_return(void) {
  int local = 1;
  return &local;    /* compile error: cannot return a stack pointer */
}
```

The full rules are in [docs/language.md](docs/language.md) (Chinese).

#### Compilation

The goc compiler is a modified Clang/LLVM: the frontend checks and infers pointer colors, the backend emits machine code that follows Go's stack rules, and the output is an object file the Go toolchain links directly. goc therefore interoperates seamlessly with Go while the C code still gets LLVM's full optimizer, keeping production-grade C performance: on the V8 benchmark, goc-ng reaches about 92% of native QuickJS-ng and about 4.1× Goja, and goc-bellard reaches about 90% of the gcc Bellard build and about 4.7× Goja (see [docs/benchmark](docs/benchmark/README.md)).

QuickJS compiled with goc, V8-v7 score (2026-10-09, Ryzen 9 7900X): goc-ng is 92% of native QuickJS-ng and 4.1× Goja; goc-bellard is 90% of the gcc build of Bellard's QuickJS and 4.7× Goja. Both pass the same test262 sample and official tests as their native builds.

![Speed relative to native ng](docs/benchmark/charts/overview-speed.svg)

## Quick start

```bash
tar -xzf goc-0.0.3-linux-amd64.tar.gz      # from the Releases page
export PATH="$PWD/goc-0.0.3-linux-amd64/bin:$PATH"
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
