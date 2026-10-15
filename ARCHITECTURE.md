# goc 架构

这一页说明每个目录负责什么，以及一段 C 代码怎样变成 Go 链接器能接受的目标文件。细节文档放在 `docs/` 里。

## 流水线

```
C 源码
  │  clang（打过补丁的 Clang 19.1.7：clang/patches + clang/sema/SemaGocColors.cpp）
  │    Sema 检查和推断指针颜色，输出 LLVM IR
  ▼
goc-color-escape        llvm/lib/Transforms/ColorEscape   IR → IR：逃逸细化，插入 uptr 编码/解码
  ▼
tools/goc/realbody.sh   （由 cmd/goc build / scripts/goc-go.sh 调用）
  ├─ goc-lower fix-ir                tools/goc-lower + goobj/lower   Go ABI 包装、IR 修正
  ├─ opt -load-pass-plugin=libGocPasses.so -passes=goc-inline-gate
  ├─ opt default<O{0..3}>
  ├─ opt -load-pass-plugin=libGocPasses.so -passes=goc-stackmap    llvm/lib/Transforms/StackMap
  ├─ goc-llc -function-sections -goc-tables   llvm/tools/goc-llc
  │    GocFrameAddrFix、GocStackmapPlacement、GocStackCheck（栈检查 + morestack）、GocObjTables（.goc.s 表）
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
| 架构钩子 | `llvm/lib/Target/X86/GocX86StackCheck.cpp`（`createX86StackCheckTarget`） | 只发指令：读 g 和 stackguard、比较、条件跳转、stub 里保存参数寄存器、调用 `runtime.morestack_noctxt`、恢复并跳回入口 |

新增一个架构只需要实现钩子接口（约 150 行），再在 `gocobj2goobj` 里加该架构的重定位映射。AArch64 钩子尚未实现，见 `progress.md`。

## 目录

| 目录 | 职责 |
|---|---|
| `cmd/goc` | 命令行入口（bash）：`goc go`、`goc build`、`goc test`、`goc check`、`goc toolchain` |
| `scripts/` | 驱动用的 shell 库（`goc-flags.sh`、`goc-product-lib.sh`、`goc-go.sh`）、构建（`build-clang.sh`、`build-passes.sh`）、发布（`pack-release.sh`）、测试入口（`ci-test.sh`、`test-p27/p28/p29*.sh`、`qjs-*.sh`、`test262-sample.py`、`qjs-official-tests.py`） |
| `clang/` | Clang 补丁、`SemaGocColors.cpp`、树外插件（p27 测试用） |
| `llvm/` | goc 自己的 LLVM 代码。`lib/Transforms/ColorEscape` 编成独立工具 `goc-color-escape`；`lib/Transforms/StackMap` 是 opt 插件 `libGocPasses.so`；`tools/goc-llc` 是加了 goc 机器 pass 和表输出的 llc；`lib/CodeGen` 是共享栈检查，`lib/Target/<arch>` 是架构钩子。构建：`scripts/build-passes.sh`，产物在 `build/llvm/` |
| `goobj/enc` | 从 Go 1.24 源码 vendored 的目标文件写入器（`goobj/enc/vendorize.sh` 负责同步） |
| `goobj/lower` | Go ABI 降级：thunk、签名、参数布局 |
| `tools/goc` | `realbody.sh`（单个 C 文件 → .goobj 的流水线）、toolexec 钩子 |
| `tools/gocobj2goobj` | `.gocobj` → goobj：先 verify、检查 ABI profile，再按表生成符号、重定位、pc 表和 funcdata |
| `tools/gocobj-diff` | 两个 goobj 的语义比对（符号、reloc、解码后的 pc 表、funcdata、FuncInfo） |
| `tools/goc-lower` | `goobj/lower` 的命令行外壳 |
| `tools/goc-objdump` | 查看和校验 `.gocobj`（`dump` / `verify`），格式见 `docs/gocobj-format.md` |
| `gocobj` | `.gocobj` 的 Go 读写和校验库（与 Go 版本无关，ABI 常量见 `docs/go-abi-profiles.md`） |
| `runtime/` | uptr 运行时（`runtime/uptr`）和头文件 |
| `include/` | 用户头文件 `goc.h`、`goc_uptr.h` |
| `test/` | 测试：`sema`、`realbody`（p27/p28 golden）、`goabi`（p29）、`arm64`、`color-escape`、`color`、`qjs`/`qjscli`/`qjsmem`（QuickJS 宿主） |
| `bench/` | 基准和 perf-gap 脚本，不在 CI 里 |
| `third_party/` | QuickJS 源码说明（源码本身不进 git）、本地 LLVM 构建目录 |

## Go 模块

仓库根目录是 Go 模块 `github.com/LoyieKing/goc`，包含 `goobj/...` 和 `tools/...`。`test/*`、`bench/gojamem`、`bench/gojacli`、`examples/hello` 各自是独立的模块。

## 测试

- `scripts/ci-test.sh`：`goc check`、`goc test --p28`、`test-p29-goabi.sh`、gocobj 单元测试、`test-stress.sh`（栈增长 + GC 压力）、`test/arm64/check_arm64.sh`、`goc test --p27`。CI 跑的就是这一组。
- QuickJS：`scripts/qjs-cli-build.sh`（ng / `QJS_FLAVOR=bellard`），然后跑 `scripts/test262-sample.py` 和 `scripts/qjs-official-tests.py`。
- Go 单元测试：`go test ./goobj/lower ./tools/...`。
