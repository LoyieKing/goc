# goc 架构

这一页说明每个目录负责什么，以及一段 C 代码怎样变成 Go 链接器能接受的目标文件。细节文档放在 `docs/` 里。

## 流水线

```
C 源码
  │  clang（打过补丁的 Clang 19.1.7：clang/patches + clang/sema/SemaGocColors.cpp）
  │    Sema 检查和推断指针颜色，输出 LLVM IR
  ▼
opt -passes=goc-color-escape     llvm/lib/Transforms/ColorEscape（分析在 lib/Analysis/GocColor）
                                   IR → IR：逃逸细化，插入 uptr 编码/解码；--color-report 时先跑 print<goc-color>
  ▼
tools/goc/realbody.sh   （由 cmd/goc build / scripts/goc-go.sh 调用）
  ├─ goc-lower fix-ir                tools/goc-lower + goobj/lower   Go ABI 包装、IR 修正
  ├─ opt -load-pass-plugin=libGocPasses.so -passes=goc-uptr-inline
  ├─ opt default<O{0..3}>
  ├─ opt -load-pass-plugin=libGocPasses.so -passes=goc-stackmap    llvm/lib/Transforms/StackMap
  ├─ goc-llc -function-sections -goc-tables   llvm/tools/goc-llc（只是 llc 驱动）
  │    lib/CodeGen：GocStackmapPlacement、GocStackCheck（栈检查 + morestack）
  │    lib/Target/X86：GocX86FrameAddrFix；lib/Target/AArch64：arm64 帧和 LR
  │    lib/Object：GocObjTables（.goc.* 表）
  ├─ goc-lower goabi                 生成 Go ABI thunk（thunks.s + thunks.goc.s）
  ├─ llvm-mc → 一个 ELF，即 .gocobj（机器码 + ELF 重定位 + .goc.* 表）
  └─ gocobj2goobj -symabis           tools/gocobj2goobj   纯表 → goobj 转换
  ▼
go build -toolexec tools/goc/toolexec_recognize_goobj.sh
  把 .goobj 打进包归档，交给 Go 链接器
```

所有指令、重定位、pc 表（SP_DELTA、UNSAFE_POINT）和栈图都由 LLVM 产出。`gocobj2goobj` 不解码也不生成任何指令，只把表翻译成当前 Go 版本的目标文件；`-function-sections` 保证每个跨函数引用都有 ELF 重定位，不需要修补已烘焙的位移。

### 栈检查：共享部分与架构钩子

栈检查分两层，目标是让每个架构只写最少的代码：

| 层 | 位置 | 负责 |
|---|---|---|
| 共享（与架构无关） | `llvm/include/Goc/StackCheck.h`、`llvm/lib/CodeGen/GocStackCheck.cpp` | 判定哪些函数需要检查（叶子 / 无栈帧 / 帧 ≤ 128 且无调用 → nosplit）；按 ABI profile 常量选 small（≤128）/ big（≤4096）/ huge 档；记录 body/stub 标签、每段 SP 偏移、morestack 调用点，供 GocObjTables 写表 |
| 架构钩子 | `llvm/lib/Target/X86/GocX86StackCheck.cpp`（`createX86StackCheckTarget`）、`llvm/lib/Target/AArch64/GocAArch64StackCheck.cpp`（`createAArch64StackCheckTarget`） | 只发指令：读 g 和 stackguard、比较、条件跳转、stub 里保存参数寄存器、调用 `runtime.morestack_noctxt`、恢复并跳回入口 |

新增一个架构只需要实现钩子接口（约 150 行），再在 `gocobj2goobj` 里加该架构的重定位映射（arm64 的在 `tools/gocobj2goobj/arm64.go`）。AArch64 钩子按名字在目标自己的表里查 opcode 和寄存器，不需要 AArch64 的私有头文件，所以只要 goc-llc 链接的 libLLVM 带 AArch64 后端即可。

其他按架构区分的地方都很小：GocObjTables 里 x86 的调用记在返回地址前 4 字节（CALL，addend −4），AArch64 的 BL 记在指令本身（ARM64_CALL26，addend 0）；`goc-lower goabi` 为两种架构都输出 thunk 的表。arm64 另有 `llvm/lib/Target/AArch64/GocAArch64Frame.cpp`：每个有调用的帧在 0(SP) 留 LR 副本（Go 的回溯从这里读返回地址），C 的栈参数整体上移 16 字节，调用后对被调用者保存寄存器和溢出槽里的帧地址做和 copystack 相同的修正。arm64 在 qemu-aarch64 里运行测试（`test/e2e/arm64`、arm64 压力测试）。

## 目录

| 目录 | 职责 |
|---|---|
| `cmd/goc` | 命令行入口（bash）：`goc go`、`goc build`、`goc test`、`goc check`、`goc toolchain` |
| `scripts/` | 驱动用的 shell 库（`goc-flags.sh`、`goc-product-lib.sh`、`goc-go.sh`）、构建（`build-clang.sh`、`build-passes.sh`、`qjs-build.sh`、`qjs-cli-build.sh`）、发布（`pack-release.sh`）、CI 入口（`ci-test.sh`、`check-file-size.sh`：单文件 800 行上限）。测试脚本本身都在 `test/` 下 |
| `clang/` | Clang 补丁、`SemaGocColors.cpp`（树内 Sema）、树外插件 `plugin/`（与树内同规则，`goc test --plugin` 用） |
| `llvm/` | goc 自己的 LLVM 代码，目录照 LLVM 的布局：`lib/Analysis`（只读分析：指针颜色、safepoint、栈地址）、`lib/Transforms`（IR 改写：ColorEscape、StackMap）、`lib/Plugin`（`libGocPasses.so`，所有 IR pass 都在这里，用 `opt -load-pass-plugin`）、`lib/CodeGen` / `lib/Target/<arch>` / `lib/Object`（goc-llc 的机器 pass 和表输出）、`tools/goc-llc`（llc 驱动）。构建：`scripts/build-passes.sh`，产物在 `build/llvm/`。见 `llvm/README.md` |
| `gometa` | 各 Go 版本的事实（`data/go1.X.json`，由 `tools/golangc-metadata-resolver` 从 GOROOT 生成并内嵌；docs/go-metadata-schema.md） |
| `goobjw` | goc 自己的 Go 目标文件 writer/reader，按 `gometa` 的事实写出与 `go tool asm` 同样的对象（docs/multi-go-version.md） |
| `goobj/lower` | Go ABI 降级：thunk、签名、参数布局 |
| `tools/goc` | `realbody.sh`（单个 C 文件 → .goobj 的流水线）、toolexec 钩子 |
| `tools/gocobj2goobj` | `.gocobj` → goobj：先 verify、检查 ABI profile，再按表生成符号、重定位、pc 表和 funcdata。ELF→`R_*` 按名字映射（`relocmap.go`），编号、profile 等随 Go 版本变的事实来自 `gometa`；一个二进制覆盖 go1.18–1.27（arm64 从 go1.21 起） |
| `tools/golangc-metadata-resolver` | 读一个 GOROOT 的源码，输出该版本的事实 JSON；升级 Go 时运行它 |
| `tools/gocobj-diff` | 开发工具：两个 goobj 的语义比对（符号、reloc、解码后的 pc 表、funcdata、FuncInfo），改转换器时用；CI 里由 `test/gocobj/diff.sh` 检查 |
| `tools/goc-lower` | `goobj/lower` 的命令行外壳，realbody.sh 调用，发布包里是 `bin/goc-lower` |
| `tools/goc-objdump` | 查看和校验 `.gocobj`（`dump` / `verify`），格式见 `docs/gocobj-format.md` |
| `gocobj` | `.gocobj` 的 Go 读写和校验库（与 Go 版本无关，ABI 常量见 `docs/go-abi-profiles.md`） |
| `include/` | 用户头文件 `goc.h`、`goc_uptr.h`（uptr helper 的函数体由 `goc-uptr-inline` 给出，没有运行时库，见 `docs/design/uptr.md`） |
| `test/` | 所有测试，入口是各目录的 `run.sh`：`clang`（前端 golden）、`color-escape`、`lit`（FileCheck）、`gocobj`（格式 golden）、`e2e/`（`realbody`、`goabi`、`stress`、`lines`、`arm64`、`qjs` QuickJS 宿主与测试脚本）。见 `test/README.md` |
| `bench/` | 基准和 perf-gap 脚本，不在 CI 里 |
| `third_party/` | QuickJS 源码说明（源码本身不进 git）、本地 LLVM 构建目录 |

## Go 模块

仓库根目录是 Go 模块 `github.com/LoyieKing/goc`，包含 `goobj/...` 和 `tools/...`。`test/*`、`bench/gojamem`、`bench/gojacli`、`examples/hello` 各自是独立的模块。

## 测试

- `scripts/ci-test.sh`：依次跑文件大小检查、`goc check`、`test/clang`、`test/color-escape`、`test/lit`、`test/e2e/{realbody,goabi,stress,lines,arm64}`、Go 单元测试、gocobj golden 和 gocobj-diff。CI 跑的就是这一组。
- QuickJS：`scripts/qjs-cli-build.sh`（ng / `QJS_FLAVOR=bellard`），然后跑 `test/e2e/qjs/test262-sample.py` 和 `test/e2e/qjs/official-tests.py`。
- Go 单元测试：`go test ./goobj/lower ./tools/...`。
