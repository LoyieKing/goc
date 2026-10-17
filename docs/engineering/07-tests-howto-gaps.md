# 07 测试、扩展指南、已知缺口

[← 06 运行时](06-runtime.md) · [索引](../engineering-architecture.md)

## 1. 测试布局

规则（`test/README.md`）：所有测试在 `test/`，每个目录一个 `run.sh`；Go 单元测试按 Go 的要求放在包旁边；构建输出在 `build/test/`；基准在 `bench/`，不在 CI。

| 目录 | 测什么 | 入口 |
|---|---|---|
| `test/clang/` | 前端 Sema：打补丁的 clang 和树外插件的颜色规则 | `test/clang/run.sh`（`goc test`、`goc test --plugin`） |
| `test/color-escape/` | `goc-color-escape`：期望颜色、报错、uptr 往返、颜色报表；用例清单在 `EXPECTATIONS` | `test/color-escape/run.sh` |
| `test/lit/` | FileCheck：`ColorEscape/{implicit-uptr-store,return-sptr}.ll`、`StackMap/{name-blocks,stackmap-remat}.ll`、`CodeGen/{x86-stack-check,arm64-frame}.ll` | `test/lit/run.sh`（需要 `lit`；CI 固定 `lit==23.1.3`） |
| `test/gocobj/` | `.gocobj` 格式金样（`v1.0/valid.gocobj` + `bad-*`），`gen/` 重新生成，`diff.sh` 测 `gocobj-diff` | `go test ./gocobj/`、`test/gocobj/diff.sh` |
| `test/e2e/realbody/` | 一个 C 文件走完整流水线 | `run.sh` |
| `test/e2e/repro/` | 可复现：同一文件构建两次 `.gocobj`/`.goobj` 逐字节相同；同一 `.gocobj` 转换 10 次结果相同 | `run.sh` |
| `test/e2e/goabi/` | Go → C 的 ABIInternal thunk：标量、聚合、栈参数 | `run.sh` |
| `test/e2e/stress/` | C 递归覆盖所有栈检查档位，同时 GC 反复伸缩 goroutine 栈 | `run.sh` |
| `test/e2e/lines/` | Go traceback 里的 C 文件 / 行号（`runtime.CallersFrames`）、内联帧 | `run.sh` |
| `test/e2e/arm64/` | qemu-aarch64 下的 linux/arm64：`run.sh` 语料，`stress.sh` 压力和 C 栈参数 | `run.sh`、`stress.sh`（`ARM64_REQUIRE_QEMU=1` 时缺 qemu 算失败） |
| `test/e2e/qjs/` | QuickJS 跑在 goroutine 栈上：`host/`（goc 编的 C 宿主和 libc shim）、`embed/`、`cli/`，test262 / 官方测试 / CLI 运行器 | 先 `scripts/qjs-cli-build.sh`（`QJS_FLAVOR=bellard` 换引擎），再 `test262-sample.py`、`official-tests.py`、`cli-tests.sh` |

### CI（`scripts/ci-test.sh`，按顺序）

```
file sizes      ./scripts/check-file-size.sh          # 单文件 800 行，无例外（third_party/、test/、testdata/ 不查）
goc check       ./cmd/goc check
clang           ./test/clang/run.sh
color-escape    ./test/color-escape/run.sh
lit             ./test/lit/run.sh
realbody        ./test/e2e/realbody/run.sh
reproducible    ./test/e2e/repro/run.sh
goabi           ./test/e2e/goabi/run.sh
go_extern       ./test/e2e/go_extern/run.sh
stress          ./test/e2e/stress/run.sh
lines           ./test/e2e/lines/run.sh
arm64           ./test/e2e/arm64/run.sh
arm64 stress    ./test/e2e/arm64/stress.sh
go unit tests   go test ./gocobj/ ./goobj/lower/ ./tools/...
gocobj goldens  go run ./tools/goc-objdump verify test/gocobj/v1.0/valid.gocobj
gocobj-diff     ./test/gocobj/diff.sh
```

GitHub Actions（`.github/workflows/ci.yml`）用 `ARM64_REQUIRE_QEMU=1 GOC_REQUIRE_LIT=1 ./scripts/ci-test.sh`，装 `qemu-user`、arm64 交叉头文件、`llvm-19-tools`；
打补丁的 clang 按补丁 / 脚本 / `llvm/**` 的哈希缓存，缓存不中时完整重编 clang（约 1.5 小时）。

### 本地常用

```bash
./scripts/build-passes.sh                       # 已构建就跳过；GOC_REBUILD_PASSES=1 强制
(cd build/llvm && ninja)                        # 改了 llvm/ 下的 C++ 后增量重编
./test/lit/run.sh                               # 改 pass 后最快的反馈
GOC_KEEP_TMP=1 ./cmd/goc build x.c -o out/x.goobj -O3   # 看每一步 IR
go run ./tools/goc-objdump dump out/x.gocobj    # 看表
go run ./tools/gocobj-diff a.goobj b.goobj      # 改转换器前后对比
./scripts/ci-test.sh                            # 提交前
```

注意：带着一个指向别的工作树的 `GOC_ROOT` 跑 `lit` 会测错树（`env -u GOC_ROOT`）。

## 2. 加一个新 Go 版本（例：go1.28）

详见 [multi-go-version.md](../multi-go-version.md) §8。通常只需要：

1. 用新 Go 运行 `GOTOOLCHAIN=local go run ./tools/golangc-metadata-resolver -o gometa/data/go1.28.json`，在 CI 矩阵里加上新版本，本地跑 `./scripts/ci-test.sh`。
2. 如果 gocobj2goobj 报 “has ABI facts no goc profile states”：说明 ABI 事实变了，在 `llvm/include/Goc/AbiProfiles.def` 和 `gocobj/abi.go` 加 `goabi2-*`，并修改用到它的后端代码。
3. 如果解析器报某个函数的指纹未登记（Go 的 writer 行为可能变了）：读懂改动，必要时改 `goobjw` 并在 schema 里加字段，用 `tools/gocobj-diff` 和链接测试核对后，把指纹加进 `tools/golangc-metadata-resolver/reviewed.go`。
4. 检查 thunk（`goobj/lower/goabi_x86.go`、`goabi_a64.go`）和 toolexec 是否仍然适用。
5. 在 [go-version-diffs.md](../go-version-diffs.md) 里补上新版本的差异。

## 3. 加一个新 IR pass

1. 代码：只读分析放 `llvm/lib/Analysis/<家族>/`，改写放 `llvm/lib/Transforms/<家族>/`；家族内共享状态放 `llvm/include/Goc/Internal/`，对外入口放 `llvm/include/Goc/`。单文件不超过 800 行。
2. 构建：把 `.cpp` 加进 `llvm/CMakeLists.txt` 的 `GocCore`（`OBJECT` 库，链进 `GocPasses` 模块）。
3. 注册：函数级 pass 加到 `GocStackMap.cpp` 的 `goc::parseStackMapPass`（或仿照它写一个新的回调）；模块级加到 `llvm/lib/Plugin/GocPasses.cpp` 的 `registerPipelineParsingCallback`。需要在 `-O0` 也跑的 pass 写 `static bool isRequired() { return true; }`。
4. 接进流水线：在 `cmd/goc` 的 `cmd_build` 里加一步 `"$OPT" -load-pass-plugin="$SMPASS" -passes=…`（注意相对 O3 / `goc-stackmap` 的位置：会内联或改调用约定的必须在栈图之前，记录栈图的之后不能再内联）。
   如果 `realbody.sh` 直接吃 `.c` 的路径（`test/e2e/*` 用到）也要跑，同步改 `tools/goc/realbody.sh`。
5. 跨 pass 传递槽身份用 metadata（`llvm/include/Goc/SlotMD.h`），不要用值名。新 metadata kind 要在 pass 开头先 `getMDKindID` 注册，保证输出 IR 确定（参见 `runColorEscape`）。
6. 测试：`test/lit/<家族>/x.ll`，`; RUN: %opt-goc -passes=你的pass -S %s -o - | FileCheck %s`（替换在 `test/lit/lit.cfg.py`：`%opt-goc`、`%goc-color-escape`、`%goc-llc`）。端到端行为加到对应 `test/e2e/*`。
7. `(cd build/llvm && ninja)`，`./test/lit/run.sh`，`./scripts/ci-test.sh`；涉及代码生成的改动还应该跑 `test/e2e/repro`。

## 4. 加一个新机器 pass

1. 架构无关放 `llvm/lib/CodeGen/`，架构相关放 `llvm/lib/Target/<Arch>/`，表相关放 `llvm/lib/Object/`。
2. 在 `llvm/include/Goc/MachinePasses.h` 声明工厂函数（`llvm::MachineFunctionPass *createXxxPass();`）。
3. 把 `.cpp` 加进 `llvm/CMakeLists.txt` 的 `goc-llc` 可执行目标。
4. 在 `llvm/tools/goc-llc/goc_llc.cpp` 的 `main` 里 `PM.add(...)`，位置决定它看到什么：`addMachinePasses()` 之前（PEI 前）、之后（post-RA），`GocObjTables` 必须保持最后。
5. 如果它改变指令位置或 SP，记得同步 `FuncStackInfo` / 表输出，否则 pcsp 会错。
6. lit 测试放 `test/lit/CodeGen/`，用 `%goc-llc`。

## 5. 加一个新架构

现有两个架构的差别都集中在下面这些点上。新架构（例：riscv64）要逐项补：

| # | 位置 | 要做什么 |
|---|---|---|
| 1 | `AbiProfiles.def` + `gocobj/abi.go` | 新 profile（g 寄存器、偏移、morestack、参数寄存器） |
| 2 | `llvm/lib/Target/<Arch>/Goc<Arch>StackCheck.cpp` | 实现 `StackCheckTarget`（`emitCheck`、`emitStub`、`stubSlots`、`emitTrap`），在 `StackCheck.h` 加工厂；AArch64 版约 200 行，按名字查 opcode，不依赖私有头文件 |
| 3 | `goc_llc.cpp` | 按 `Triple` 选钩子和 profile；帧地址修复（x86 和 arm64 各有一份） |
| 4 | Go 帧布局 | 返回地址在哪、帧指针链怎么存（arm64 需要 `GocA64CallFrame`/`GocA64SaveLR`） |
| 5 | `GocObjTables` | 调用重定位记在哪、addend 是多少 |
| 6 | `.gocobj` header | arch 编号（现在 1 amd64、2 arm64），`gocobj/format.go` 和 `docs/gocobj-format.md` |
| 7 | `tools/gocobj2goobj` | `goVersion` 加该架构的重定位映射（参照 `arm64.go`） |
| 8 | `goobj/lower/goabi_<arch>.go` | Go ABIInternal → C 调用约定的 thunk |
| 9 | `SMUptrInline.cpp` `tlsGWord`、`uptrFatal` | 怎么读 g、FATAL 的系统调用 |
| 10 | `cmd/goc`、`tools/goc/realbody.sh` | `GOC_ARCH` 白名单、clang 目标参数、llc 参数、`llvm-mc` triple |
| 11 | `test/e2e/<arch>/` + CI | qemu 运行 |

注意 `goc-llc` 编 x86 部分时用到 LLVM X86 的私有头文件（`llvm/CMakeLists.txt` 的 `GOC_X86_SRC`/`GOC_X86_BUILD`），新架构尽量走 AArch64 钩子的「按名字查表」方式。

## 6. 已知缺口

来自代码、`docs/todo.md`、`docs/runtime-model.md`：

1. **`goc go` 只支持 linux/amd64**；arm64 只有 `goc build` 和 `test/e2e/arm64`（qemu）。QuickJS 宿主用 x86 syscall 和 SSE 汇编，arm64 上被拒绝。
2. （已解决）原来只有 go1.24 的映射；现在支持 go1.23–1.27（multi-go-version.md）。
3. **`gptr` 写屏障没有实现**（06 篇 §3），`gptr` 也不会进栈图；合同（syntax-guide）里写了。
4. **跨 goroutine 使用 sptr/uptr 是合同错误**（单字表示里没有 goroutine 指针）。`GOC_SPTR_WITH_G` 只在 todo 里。
5. **C 调 Go**：`__go_extern` stub 已落地；非 main 包链入、string/切片/接口、色合同与 panic 语义仍见 `docs/todo.md`。
6. **栈上传参的指向栈的指针**：被调用者在搬栈之后才读时不会被修正（栈参数区不在栈图里）（todo.md）。
7. **帧地址修复的 tainted 值**（`p - x`、`p * k`）不能修复，只打印 note（`-goc-frameaddr-taint`）。
8. 不支持：完整 ISO C 和宿主 libc、Go 可调用路径上的 AVX / x87 / EH、`stacksave`/`stackrestore`、变参和部分聚合的 ABIInternal（`docs/runtime-model.md`）。
9. **`--fast-stack-alloca` 只能单线程**（池游标进程全局）。
10. **`goc build -o` 的输出目录必须已存在**，否则在最后一步才报错。
11. 写本文时观察到、**未确认是否是缺陷**：`int *p = pick(&slot)` 这种「通过调用返回的栈指针」，在 `--default-ptr-color cptr` 下返回值被染成 `cptr`，`goc-stackmap` 不为它建根（`stackAddresses` 只认 alloca / GEP / load，不跨过程）。`GocX86FrameAddrFix` 也不把调用返回值当帧地址。若它在 CSR 里跨过下一次调用，搬栈后可能是旧地址。需要专门的测试确认。

## 7. 文档 / 注释与代码不一致（建议清理）

| 位置 | 写的是 | 实际（efe840d） |
|---|---|---|
| `tools/goc/realbody.sh` 头部注释、`cmd/goc` 第 175–176 行注释 | arm64 的帧地址修复和栈图根「x86-only」，arm64 函数没有 locals 指针图 | `cmd/goc` 对两个架构都跑 `goc-stackmap`/`goc-reanchor`；`goc-llc` 对 arm64 加 `GocA64FrameAddrFix`；实测 arm64 `body.ll` 里有 stackmap 调用 |
| `scripts/goc-flags.sh` 的 `goc_flags_help` | 「arm64 cannot record sptr maps」 | 同上 |
| `test/e2e/arm64/run.sh` 头注释 | 「stack maps stay off on arm64」 | 同上 |
| `llvm/lib/Object/GocObjTables.cpp` 头注释 | 「Transitional content … empty bitmaps … meta.json」 | 位图已由表本身给出（`goc-objdump` 可见非空 locals 位图），没有 meta.json |
| `llvm/include/Goc/SlotMD.h` | pass 和 goc-llc 从不按 IR 值名认槽 | `GocX86FrameAddrFix.cpp` 按名字 `goc.oldfp` 找槽（`AI->getName() == "goc.oldfp"`） |
| `docs/runtime-model.md` 第一段 | 生成的代码「participates in … write barriers for `gptr`」 | 没有写屏障代码 |
| `docs/gocobj-format.md` header 表 | `stack_checks_in_code` 为 0 时转换器补栈检查 | `gocobj2goobj` 现在在该位为 0 时直接报错 |
| `ARCHITECTURE.md` 流水线图 | `goc-lower fix-ir` 在 `opt` 之前，`goc-stackmap` 在 realbody 里 | `goc build -O>0` 时 uptr-inline/O3/stackmap/reanchor 都在 `cmd/goc` 里跑，realbody 只在 `GOC_STACKMAP_PREPARED≠1` 时补跑 stackmap |
| `toolexec_recognize_goobj.sh` | 写死追加 `GocCheckedAdd`、`StoreGptrWB` 等 8 个 `def` | 对应的符号不由当前流水线生成，看起来是旧原型的残留 |
