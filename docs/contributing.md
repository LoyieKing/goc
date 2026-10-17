# 贡献指南：测试与扩展

[ARCHITECTURE](../ARCHITECTURE.md) · [roadmap](roadmap.md)

改动的原则见根目录 [CONTRIBUTING.md](../CONTRIBUTING.md)。这篇讲测试怎么跑、改哪里。

## 1. 测试

所有测试在 `test/`（目录说明见 [test/README.md](../test/README.md)）；Go 单元测试按 Go 的要求放在包旁边；构建输出在 `build/test/`。QuickJS 在 `ports/quickjs/`，跑分在 `bench/`，都不在 CI 里。

| 目录 | 测什么 | 入口 |
|---|---|---|
| `test/lit/Sema/` | 打过补丁的 clang 的颜色规则（Sema 报错与通过的用例） | `test/lit/run.sh`（需要 `lit`；CI 固定 `lit==23.1.3`） |
| `test/lit/ColorEscape/` | `goc-color-escape`：C 和 IR 输入的期望颜色、报错、uptr 往返可执行文件（含 5 种错 tag 的 SIGILL）、颜色报表 | 同上 |
| `test/lit/StackMap/`、`test/lit/CodeGen/` | `goc-name-blocks`、`goc-stackmap` 的重算；goc-llc 的 x86 栈检查和 arm64 帧 | 同上 |
| `test/gocobj/` | `.gocobj` 格式金样（`v1.0/valid.gocobj` + `bad-*`），`gen/` 重新生成 | `go test ./internal/gocobj/` |
| `test/e2e/` | C 经 `bin/goc` 进 Go 程序再运行：`realbody` `repro` `goabi` `go_extern` `stress` `retptr` `manyfuncs` `lines`，以及 qemu 下的 `arm64` `arm64-stress` `arm64-retptr` `arm64-stackargs` | `test/e2e/run.sh [用例...]`（`ARM64_REQUIRE_QEMU=1` 时缺 qemu 算失败） |

`scripts/ci-test.sh` 按顺序跑：

```
file sizes      ./scripts/check-file-size.sh          # cmd internal llvm scripts ports 下单文件 800 行，无例外（testdata/ 不查）
goc check       bin/goc check
lit             ./test/lit/run.sh
e2e             ./test/e2e/run.sh
go unit tests   go test ./internal/... ./cmd/...
gocobj goldens  bin/goc tool objdump verify test/gocobj/v1.0/valid.gocobj
```

GitHub Actions（`.github/workflows/ci.yml`）在 10 个 Go 版本（1.18–1.27）上跑 `ARM64_REQUIRE_QEMU=1 GOC_REQUIRE_LIT=1 ./scripts/ci-test.sh`，装 `qemu-user`、arm64 交叉头文件、`llvm-19-tools`；
打补丁的 clang 和 goc 的 pass 分两个缓存键，clang 的键不中时完整重编（约 1.5 小时）。每个 job 还检查 `cmd/goc-gometa` 从该 GOROOT 读出的事实与 `internal/gometa/data` 一致。

### 本地常用

```bash
./scripts/build-passes.sh                       # 已构建就跳过；GOC_REBUILD_PASSES=1 强制
(cd build/llvm && ninja)                        # 改了 llvm/ 下的 C++ 后增量重编
go build -o bin/goc ./cmd/goc                   # 改了 Go 代码（test/lib.sh 也会重编）
./test/lit/run.sh                               # 改 pass 后最快的反馈
./test/e2e/run.sh goabi stress                  # 只跑几个 e2e 用例
bin/goc build x.c -o out/x.goobj -O3 --keep-tmp=/tmp/k   # 看每一步 IR
bin/goc tool objdump dump out/x.gocobj          # 看表
bin/goc tool objdump diff a.goobj b.goobj       # 改转换器前后对比
./scripts/ci-test.sh                            # 提交前
```

## 2. 改了编译路径之后：QuickJS

改 pass、goc-llc、`internal/lower` 或转换器之后，除了 CI，还要在 QuickJS 上核对（引擎源码按 [third_party/README.md](../third_party/README.md) 放好）：

```bash
ports/quickjs/build.sh && ports/quickjs/build.sh --flavor bellard
ports/quickjs/tests/test262.py --test262 DIR --engine goc-ng=build/qjs/qjscli --engine goc-bellard=build/qjs-bellard/qjscli
ports/quickjs/tests/official.py --tests BELLARD/tests --engine goc-ng=build/qjs/qjscli
ports/quickjs/tests/cli_tests.py
```

期望值：test262 抽样 goc-ng 1502/1526、goc-bellard 1501/1526；官方测试 73/77、77/77；cli_tests 116/116，失败清单与改动前相同。性能用 `bench/all.sh` 的 V8 套件（见 [benchmark/README.md](benchmark/README.md)）。

## 3. 加一个新 Go 版本（例：go1.28）

详见 [design/go-versions.md](design/go-versions.md) §8。通常只需要：

1. 用新 Go 运行 `GOTOOLCHAIN=local go run ./cmd/goc-gometa -o internal/gometa/data/go1.28.json`，在 CI 矩阵里加上新版本，本地跑 `./scripts/ci-test.sh`。
2. 如果转换器报 “has ABI facts no goc profile states”：说明 ABI 事实变了，在 `llvm/include/Goc/AbiProfiles.def` 和 `internal/gocobj/abi.go` 加 `goabi2-*`，并修改用到它的后端代码。
3. 如果 `goc-gometa` 报某个函数的指纹未登记（Go 的 writer 行为可能变了）：读懂改动，必要时改 `internal/goobj` 并在 schema 里加字段，用 `goc tool objdump diff` 和链接测试核对后，把指纹加进 `cmd/goc-gometa/reviewed.go`。
4. 检查 thunk（`internal/lower/goabi_x86.go`、`goabi_a64.go`）和 `goc toolexec` 是否仍然适用。
5. 在 [design/go-versions.md](design/go-versions.md) §11 里补上新版本的差异。

## 4. 加一个新 IR pass

1. 代码：只读分析放 `llvm/lib/Analysis/<家族>/`，改写放 `llvm/lib/Transforms/<家族>/`；家族内共享状态放 `llvm/include/Goc/Internal/`，对外入口放 `llvm/include/Goc/`。单文件不超过 800 行。
2. 构建：把 `.cpp` 加进 `llvm/CMakeLists.txt` 的 `GocCore`（`OBJECT` 库，链进 `GocPasses` 模块）。
3. 注册：函数级 pass 加到 `GocStackMap.cpp` 的 `goc::parseStackMapPass`（或仿照它写一个新的回调）；模块级加到 `llvm/lib/Plugin/GocPasses.cpp` 的 `registerPipelineParsingCallback`。需要在 `-O0` 也跑的 pass 写 `static bool isRequired() { return true; }`。
4. 接进流水线：在 `internal/pipeline/pipeline.go` 的 `frontend` 里加一步 `b.opt("你的pass", …)`（注意相对 O3 / `goc-stackmap` 的位置：会内联或改调用约定的必须在栈图之前，记录栈图的之后不能再内联）。`-O0` 也要跑的，看 `backend` 里补跑 `goc-stackmap` 的分支。
5. 跨 pass 传递槽身份用 metadata（`llvm/include/Goc/SlotMD.h`），不要用值名。新 metadata kind 要在 pass 开头先 `getMDKindID` 注册，保证输出 IR 确定（参见 `runColorEscape`）。
6. 测试：`test/lit/<家族>/x.ll`，`; RUN: %opt-goc -passes=你的pass -S %s -o - | FileCheck %s`（替换在 `test/lit/lit.cfg.py`：`%opt-goc`、`%goc-color-escape`、`%goc-llc`）。端到端行为加到对应 `test/e2e/*`。
7. `(cd build/llvm && ninja)`，`./test/lit/run.sh`，`./scripts/ci-test.sh`；涉及代码生成的改动还应该跑 `test/e2e/run.sh repro` 和 QuickJS（第 2 节）。

## 5. 加一个新机器 pass

1. 架构无关放 `llvm/lib/CodeGen/`，架构相关放 `llvm/lib/Target/<Arch>/`，表相关放 `llvm/lib/Object/`。
2. 在 `llvm/include/Goc/MachinePasses.h` 声明工厂函数（`llvm::MachineFunctionPass *createXxxPass();`）。
3. 把 `.cpp` 加进 `llvm/CMakeLists.txt` 的 `goc-llc` 可执行目标。
4. 在 `llvm/tools/goc-llc/goc_llc.cpp` 的 `main` 里 `PM.add(...)`，位置决定它看到什么：`addMachinePasses()` 之前（PEI 前）、之后（post-RA），`GocObjTables` 必须保持最后。
5. 如果它改变指令位置或 SP，记得同步 `FuncStackInfo` / 表输出，否则 pcsp 会错。
6. lit 测试放 `test/lit/CodeGen/`，用 `%goc-llc`。

## 6. 加一个新架构

现有两个架构的差别都集中在下面这些点上。新架构（例：riscv64）要逐项补：

| # | 位置 | 要做什么 |
|---|---|---|
| 1 | `AbiProfiles.def` + `internal/gocobj/abi.go` | 新 profile（g 寄存器、偏移、morestack、参数寄存器） |
| 2 | `llvm/lib/Target/<Arch>/Goc<Arch>StackCheck.cpp` | 实现 `StackCheckTarget`（`emitCheck`、`emitStub`、`stubSlots`、`emitTrap`），在 `StackCheck.h` 加工厂；AArch64 版约 200 行，按名字查 opcode，不依赖私有头文件 |
| 3 | `goc_llc.cpp` | 按 `Triple` 选钩子和 profile；帧地址修复（x86 和 arm64 各有一份） |
| 4 | Go 帧布局 | 返回地址在哪、帧指针链怎么存（arm64 需要 `GocA64CallFrame`/`GocA64SaveLR`） |
| 5 | `GocObjTables` | 调用重定位记在哪、addend 是多少 |
| 6 | `.gocobj` header | arch 编号（现在 1 amd64、2 arm64），`internal/gocobj/format.go` 和 [design/gocobj-format.md](design/gocobj-format.md) |
| 7 | `internal/convert` | 加该架构的重定位映射（参照 `arm64.go`） |
| 8 | `internal/lower/goabi_<arch>.go` | Go ABIInternal → C 调用约定的 thunk |
| 9 | `SMUptrInline.cpp` `tlsGWord`、`uptrFatal` | 怎么读 g、FATAL 的系统调用 |
| 10 | `cmd/goc/flags.go`、`internal/pipeline` | `--arch` 的取值、clang 目标参数、llc 参数（`llcArgs`）、`llvm-mc` triple |
| 11 | `test/e2e/<arch>*` + CI | qemu 运行 |

注意 `goc-llc` 编 x86 部分时用到 LLVM X86 的私有头文件（`llvm/CMakeLists.txt` 的 `GOC_X86_SRC`/`GOC_X86_BUILD`），新架构尽量走 AArch64 钩子的「按名字查表」方式。

## 7. 已知缺口

来自代码和 [roadmap.md](roadmap.md)：

1. **QuickJS 宿主只有 linux/amd64**：它用 x86 syscall 和 SSE 汇编，arm64 上被拒绝。编译器和 Go 侧在 arm64（qemu）上可用。
2. **`gptr` 写屏障没有实现**（[design/runtime.md](design/runtime.md) §3），`gptr` 也不会进栈图；合同（[language.md](language.md)）里写了。
3. **跨 goroutine 使用 sptr/uptr 是合同错误**（单字表示里没有 goroutine 指针）。两字表示只在 roadmap 里。
4. **C 调 Go**：`__go_extern` stub 已落地；非 main 包链入、string/切片/接口、色合同与 panic 语义见 roadmap。
5. **栈上传参的指向栈的指针**：被调用者在搬栈之后才读时不会被修正（栈参数区不在栈图里）。
6. **帧地址修复的 tainted 值**（`p - x`、`p * k`）不能修复，只打印 note（`-goc-frameaddr-taint`）。
7. 不支持：完整 ISO C 和宿主 libc、Go 可调用路径上的 AVX / x87 / EH、`stacksave`/`stackrestore`、变参和部分聚合的 ABIInternal（[design/runtime.md](design/runtime.md) §7）。
8. **`--fast-stack-alloca` 只能单线程**（池游标进程全局）。
9. **`goc build -o` 的输出目录必须已存在**，否则在转换那一步才报错。
10. **未确认是否是缺陷**：`int *p = pick(&slot)` 这种「通过调用返回的栈指针」，在 `--default-ptr-color cptr` 下返回值被染成 `cptr`，`goc-stackmap` 不为它建根（`stackAddresses` 只认 alloca / GEP / load，不跨过程）。`GocX86FrameAddrFix` 也不把调用返回值当帧地址。若它在 CSR 里跨过下一次调用，搬栈后可能是旧地址。需要专门的测试确认。
11. `GocX86FrameAddrFix.cpp` 按名字 `goc.oldfp` 找槽，和 `llvm/include/Goc/SlotMD.h`「不按 IR 值名认槽」的原则不一致（pass 自己建的 alloca 名字不会被丢弃，所以目前不出错）。
