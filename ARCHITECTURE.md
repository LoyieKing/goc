# goc 架构

这是读代码的入口：goc 要解决什么、一段 C 怎样变成 Go 链接器能接受的目标文件、每个目录负责什么。细节按主题分在 [docs/design/](docs/design/)。

## 要解决的问题

Go 的 goroutine 栈很小，不够时运行时会把整块栈搬到更大的内存（copystack），再把栈上指向栈自己的指针逐个修正。Go 编译器生成的每个函数都配合这件事：

1. 入口栈检查：比较 `SP` 和 `g.stackguard0`，不够就调用 `runtime.morestack_noctxt`。
2. pc 表：每个 pc 上 SP 偏了多少（pcsp）、能不能被抢占（`UNSAFE_POINT`）、源文件行号。
3. 栈图：每个调用点，帧里哪些字是指针。搬栈和 GC 靠它。

普通 C 编译器什么都不给，所以 cgo 要切到系统栈。goc 让 LLVM 编出来的 C 也带上这三样，再写成 Go 的目标文件。为此 C 的指针要分颜色（[docs/language.md](docs/language.md)），编译器才知道哪些值是栈地址：`sptr` 栈指针、`cptr` C 堆或全局指针、`uptr` 编码后可以存进堆的栈指针、`gptr` Go 堆指针。

## 流水线

一个 `.c` 的编译全在 `internal/pipeline`（`goc build` 和 `goc go` 都走它）。`goc build --keep-tmp=DIR` 留下下面每一步的文件。

```
foo.c
  │  clang（Clang 19.1.7 + clang/patches + clang/sema/SemaGocColors.cpp）
  │    Sema 检查并推断指针颜色，颜色以 annotate 进 IR                       a.ll
  ▼
opt，libGocPasses.so（llvm/lib/Plugin）
  ├─ goc-color-escape   逃逸细化，插入 uptr 编码/解码（--color-report 先跑 print<goc-color>）  a.color.ll
  ├─ goc-uptr-inline    uptr helper 和 alloca 池的函数体                     a.gate.ll
  ├─ default<On> 去掉 argpromotion、globalopt，加 goc-name-blocks（-O0 跳过）  a.opt.ll
  ├─ goc-stackmap       根槽 + llvm.experimental.stackmap                     a.sm.ll
  └─ goc-reanchor       指针实参锚定                                          a.reanchor.ll
  ▼
internal/lower
  ├─ FixIR、RenameIR    IR 小修                                               body.ll
  └─ WriteGoABI         C 本体改名 F.impl，生成 Go ABIInternal thunk F         body.impl.ll、thunks.s
  ▼
goc-llc -function-sections -goc-tables（llvm/tools/goc-llc，只是 llc 驱动）
  │  lib/CodeGen：GocStackmapPlacement、GocStackCheck（栈检查 + morestack）
  │  lib/Target/X86：帧地址修复；lib/Target/AArch64：LR 槽和调用后修复
  │  lib/Object：GocObjTables（.goc.* 表）                                   body.s、body.goc.s
  ▼
llvm-mc → foo.gocobj（ELF：机器码 + ELF 重定位 + .goc.* 表）
  ▼
internal/convert → foo.goobj + foo.symabis（按目标 Go 版本，事实来自 internal/gometa）
  ▼
go build -a -toolexec "goc toolexec -pkg P foo.goobj --"
  asm 阶段追加 symabis，compile 阶段把对象打进包归档，交给 Go 链接器
```

所有指令、重定位、pc 表和栈图都由 LLVM 产出。转换器不解码也不生成指令，只把表翻译成当前 Go 版本的目标文件；`-function-sections` 保证每个跨函数引用都有 ELF 重定位。一个 goc 二进制覆盖 go1.18–1.27（arm64 从 go1.21 起）。

用 `examples/hello` 和一个带调用的文件逐步看真实产物：[docs/design/walkthrough.md](docs/design/walkthrough.md)。

## 目录

| 目录 | 职责 |
|---|---|
| `cmd/goc` | 命令行：`goc go`、`build`、`check`、`cc`、`env`、`toolexec`、`tool convert`、`tool objdump`、`version`。参数在 `flags.go`，goc 不读 `GOC_*` 环境变量 |
| `cmd/goc-gometa` | 读一个 GOROOT 的源码，生成该 Go 版本的事实 JSON；升级 Go 时运行 |
| `internal/pipeline` | 一个 C 文件到 Go 对象的全流程（上图） |
| `internal/toolchain` | 找本树的 clang、opt、llvm-mc、goc-llc、libGocPasses.so：发布包的 `bin/`、`passes/`，或 checkout 的 `third_party/llvm-19.1.7-clang-build`、`build/llvm` |
| `internal/lower` | Go ABI thunk（amd64、arm64）、`__go_extern` stub、IR 小修、汇编对齐 |
| `internal/convert` | `.gocobj` → goobj：校验、检查 ABI profile、生成符号、重定位、pc 表和 funcdata |
| `internal/gocobj` | `.gocobj` 的读写和校验，与 Go 版本无关（`goc tool objdump`） |
| `internal/gometa` | 各 Go 版本的事实，内嵌 `data/go1.X.json` |
| `internal/goobj` | goc 自己的 goobj writer/reader，按 gometa 写出和 `go tool asm` 一样的对象 |
| `clang/` | Clang 补丁和树内 Sema `SemaGocColors.cpp`（[clang/README.md](clang/README.md)） |
| `llvm/` | goc 的 LLVM 代码，照 LLVM 布局：`lib/Analysis`、`lib/Transforms`、`lib/Plugin`（IR pass）、`lib/CodeGen`、`lib/Target/<arch>`、`lib/Object`、`tools/goc-llc`（[llvm/README.md](llvm/README.md)） |
| `include/` | 用户头文件 `goc.h`、`goc_uptr.h`。没有运行时库，uptr helper 的函数体由 `goc-uptr-inline` 给出 |
| `scripts/` | 构建 Clang 和 pass、CI 入口、打发布包 |
| `test/` | `lit`（Sema、IR pass、goc-llc 的 FileCheck）、`e2e`（Go 程序端到端，含 arm64/qemu）、`gocobj`（格式金样），见 [test/README.md](test/README.md) |
| `ports/quickjs` | QuickJS（ng 和 Bellard）的 goc 移植：补丁、自带 libc shim 的宿主、CLI、测试脚本（[README](ports/quickjs/README.md)） |
| `bench/` | 基准、内存、perf-gap 对照，不在 CI 里（[docs/benchmark/README.md](docs/benchmark/README.md)） |
| `examples/` | `hello`（`goc check` 用）、`colors` |
| `third_party/` | 只进 git 一份说明；本地放 LLVM 构建目录 |

Go 模块：仓库根是 `github.com/LoyieKing/goc`；`test/e2e`、`ports/quickjs`、`bench`、`examples/hello` 各自是独立模块。

## 按主题读

| 文档 | 内容 |
|---|---|
| [design/walkthrough.md](docs/design/walkthrough.md) | 从 `add.c` 到二进制，每一步的真实输出 |
| [design/frontend-ir.md](docs/design/frontend-ir.md) | Clang 补丁与 Sema、每个 IR pass 的输入、输出、不变量，颜色在 IR 里的表示 |
| [design/codegen.md](docs/design/codegen.md) | 为什么要自己的 llc、栈检查（共享策略 + 架构钩子）、帧地址修复、arm64、`.goc.*` 表 |
| [design/objects-link.md](docs/design/objects-link.md) | `.gocobj`、转换器、`goc toolexec`、符号命名 |
| [design/gocobj-format.md](docs/design/gocobj-format.md) | `.gocobj` 格式规范 |
| [design/go-versions.md](docs/design/go-versions.md) | 和 Go 版本相关的事实、ABI profile、升级 Go 的流程 |
| [design/runtime.md](docs/design/runtime.md) | morestack、搬栈、GC、uptr、alloca 池、Go 与 C 互调 |
| [contributing.md](docs/contributing.md) | 测试、加 Go 版本 / pass / 架构、已知缺口 |
| [roadmap.md](docs/roadmap.md) | 还没做的事 |
