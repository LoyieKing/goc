# Go 版本

目标：goc 后端尽量与 Go 版本无关。目前支持 amd64 上的 go1.18–go1.27、arm64 上的 go1.21–go1.27（arm64 下限的原因见第 9 节）。新出一个 Go 版本时，一般只需运行一次事实解析器、提交生成的 JSON。差异的事实依据见第 11 节，JSON 的字段定义见第 12 节，ABI 常量见第 10 节。

## 1. 哪些与版本有关，哪些无关

| 部件 | 与 Go 版本的关系 | 依据 |
|---|---|---|
| 打过补丁的 clang、IR pass、goc-llc | 无关 | 只依赖 ABI profile（栈常量、g 偏移、寄存器），而这些在 1.18–1.27 中完全相同 |
| `.gocobj` | 无关 | header 里记录的是 profile 名（`goabi1-amd64`），而不是 Go 版本 |
| 转换器（`internal/convert`） | **一个二进制覆盖所有版本**，按目标版本的事实文件写 goobj | 重定位编号会移位（1.25 起 arm64 的部分编号减 1），符号类型编号、内容哈希、DWARF、对齐规则也有差异；文件头逐字节比较版本号 |
| toolexec 钩子 | 无关 | asm/compile/pack 的参数形式没变 |

与版本有关的一切都收敛在 `internal/gometa/data/go1.X.json` 里。这些 JSON 由工具从 Go 源码生成，转换器的源码里没有版本分支表，也不含从 GOROOT 拷贝来的代码。

## 2. goc-gometa：从 GOROOT 读出事实

`cmd/goc-gometa` 用 go/parser 读目标 GOROOT 的 `src`（默认 `go env GOROOT`），输出一个 JSON：

```sh
GOTOOLCHAIN=local go run ./cmd/goc-gometa -o internal/gometa/data/go1.27.json
```

它读出的内容：

- goobj 格式：magic、19 个块的顺序、对象/符号 flag、aux 类型、特殊包下标、哈希长度、builtin 表（`cmd/internal/goobj`）；
- `objabi` 的重定位名 → 编号、符号类型名 → 编号，用一个支持 iota 的常量求值器计算；
- FUNCDATA/PCDATA 下标、FuncID/FuncFlag（1.21 起在 `internal/abi/symtab.go`，之前在 `cmd/internal/objabi/funcdata.go`、`funcid.go`，名字换成 internal/abi 的写法）；
- 栈常量（1.21 前从 `objabi/stack.go` 的 StackGuard−StackSmall 算出 nosplit 预算）、g 字段偏移、g 寄存器、整数/浮点参数寄存器（SSA 规则目录 1.20 前是 `ssa/gen`）、morestack 入口；
- runtime 拷贝栈时是否改写各帧保存的帧指针（`runtime.adjustframe`，按架构）；
- writer 行为：内容哈希算法（1.18 是 sha1）、汇编符号是否带 Linkname 标志、对齐规则、是否裁剪 inline 哈希、Flushplist 是否给转换器写出的（带包名的）符号加 args_stackmap/arginfo、类型/itab 符号名前缀（1.20 前是 `type.`/`go.itab.`）、重定位是否按偏移排序（1.19 起）、FuncInfo 是否有 StartLine（1.20 起）、遍历时是否引用内联调用点的文件符号（1.22 前）；
- DWARF：函数 DIE 的 abbrev 和属性布局（v0 为没有 decl_line 的 v1，1.21 前；v1 为 addr/addr；v2 为 dwarf5 下的 addrx+udata），以及 `FuncCountToDwTxtAddrFlavor` 的阈值表；
- 每组事实读的是哪些源文件（`sources`）。

**审查过的函数指纹**：`internal/goobj` 复刻了 Go writer 中的 39 个函数的行为。`-print FILE:FUNC` 打印某个函数去掉注释后的源码，便于两个版本间 diff。`reviewed.go` 记录了每个函数在每个已核对版本上的指纹，即去掉注释后 gofmt 输出的 sha256 前 16 位。如果遇到没见过的指纹，解析器直接报错，要求先读懂 Go 的改动、必要时修改 `internal/goobj`，再把指纹加进表里。这样，Go 某个版本的 writer 行为一变，就不会悄悄写出错误的目标文件。`-fingerprints` 选项会打印当前 GOROOT 的指纹。

## 3. internal/goobj：自己的 goobj writer

`internal/goobj` 是按 goobj 格式自行实现的 writer 和 reader，没有引用 `cmd/internal/obj`，也没有拷贝 GOROOT 代码。它复刻了汇编器路径（`go tool asm`）写目标文件时的行为，因为 goc 的产物在链接器看来就是汇编对象。复刻的行为包括：

- 非 static 符号都放进 non-pkg 块；
- ABI0 函数加 `.args_stackmap` 和 `.arginfo0` funcdata 引用（1.22 起；之前汇编器只给 `"".f` 形式的符号加，转换器写的是带包名的符号，所以不加）；
- pcinline 为空表，UsedFiles 为 {0}；
- 生成大小为 0 的 DWARF info 和 lines 片段；
- 内容可寻址的 pcdata 符号按内容哈希命名；
- builtin 下标查找；
- FuncInfo 编码。

每一项随版本变化的地方都由 JSON 中的字段决定，不按版本号写分支。

转换器选择事实文件的方式：

| 方式 | 说明 |
|---|---|
| 默认 | `go env GOVERSION`，取其 minor 版本（如 go1.25.14 → go1.25），从内嵌的 `internal/gometa/data` 中选 |
| `-goversion go1.X.Y` | 指定版本 |
| `-go-meta file.json` | 用解析器生成的外部 JSON，内嵌表里没有的新版本可以先这样用 |
| 未知版本 | 报错，并给出运行解析器的命令 |

文件头一行（`go object linux amd64 go1.X.Y … X:…`）不靠推算：转换器让同一个 Go 的 `go tool asm` 汇编一个空的 .s 文件，读出它写的头，按 `go env` 的输出缓存在 `os.UserCacheDir()/goc/objheader`（Linux 上即 `$XDG_CACHE_HOME` 或 `~/.cache`）。`-header` 可以直接指定这一行。

**正确性核对**：以旧实现（用各版本 `cmd/internal/obj` 构建的转换器）的输出为准，在 41 个真实 .gocobj 上逐字节比较：

- 41 个对象（33 个 amd64、8 个 arm64）× 5 个版本，共 205 次全部一致；
- 加上包路径 `example.com/a.b/qjs.v2`、`GOEXPERIMENT=nodwarf5`、`--libcall-impl`（当时的 `GOC_LIBCALL_IMPL`）三种变体的 615 次也全部一致。

其中 4 个对象和它们 5 个版本的输出放在 `internal/convert/testdata/golden`，由 `TestGolden` 持续检查。go1.18–1.22 没有旧实现可比，它们的 golden 是加入这些版本时本转换器的输出（核对方法见第 9 节），arm64 从 go1.21 起才有。

## 4. 转换器在哪里

转换器是 `goc` 二进制的一部分（`internal/convert`）：`goc build` 在进程内调用它，`goc tool convert` 是同一份代码的命令行入口。一个 `goc` 覆盖所有支持的 Go 版本，没有单独的转换器二进制，也没有构建缓存。

## 5. profile 只在一处选定

| 步骤 | 做什么 |
|---|---|
| 转换器 | `goc tool convert -print-profile -goarch <arch>`（`goc build` 在进程内做同样的事） 用该版本的 ABI 事实（`abi`、`arch`）匹配 `gocobj` 中的 profile，优先用 `goabi*` 名 |
| `internal/pipeline` | 用上面的结果给 goc-llc 传 `-goc-profile=<profile>` |
| goc-llc | 把 profile 写进 `.gocobj` header |
| 转换器 | 检查 header 的 profile 与目标版本的 profile 是否**事实相同**（名字可以不同），不同就拒绝 |

profile 命名：

- 规范名是 `goabi1-amd64`、`goabi1-arm64`，覆盖 1.18–1.27（arm64 实际从 1.21 起支持）；
- `go1.24-*` 是同一组事实的旧名，保留下来，旧的 `.gocobj` 仍可读取。

## 6. 模块与工具链

- 根 go.mod 的 go 指令是 `go 1.23`：仓库里的 Go 代码（`goc`、`goc-gometa`）要用 go1.23+ 构建。目标 Go 更老时，用 `GOC_HOST_GO=/path/to/go1.23+/bin/go` 指定构建这些工具的 Go，PATH 里的 go 仍是目标版本（发布包自带编译好的转换器，不需要）。不再有子模块和 replace。
- 示例和测试程序（examples/hello、test/e2e/*）的 go.mod 是 `go 1.18`，代码只用 go1.18 就有的 API。
- 所有构建都用 `GOTOOLCHAIN=local`。
- toolexec 打包时，`$GOTOOLDIR/pack` 存在就用它，否则用 `go tool pack`（1.25+ 的发行包不再预装 pack；事实文件的 `toolexec.pack_prebuilt` 记录了这一点）。

## 7. CI

- 矩阵：go 1.18.x–1.22.x、1.23.x、1.24.4、1.25.x、1.26.x、1.27.x，全部必过。1.23 之前的 job 另装一个 go1.27.x 作为 `GOC_HOST_GO`。
- 每个矩阵 job 都运行 `scripts/ci-test.sh`，包括 amd64 e2e、arm64 qemu 和 retptr 测试（arm64 测试在 1.21 前跳过）。
- 每个矩阵 job 还会检查解析器从该 GOROOT 读出的 JSON 与 `internal/gometa/data` 中的一致。比较时忽略 `source_version`，因为同一 minor 的新补丁版本只应改变这一项。

## 8. 以后升级 Go 的流程

1. 用新版本运行解析器：`GOTOOLCHAIN=local go run ./cmd/goc-gometa -o internal/gometa/data/go1.N.json`。
2. 如果解析器报告指纹未知：阅读对应函数的 Go 改动，必要时修改 `internal/goobj`，并在 schema 中加入新的事实字段。然后用 `goc tool objdump diff` 和链接测试核对，再把指纹加入 `reviewed.go`。
3. 如果 profile 匹配失败，说明 ABI 事实变了：在 `AbiProfiles.def` 和 `internal/gocobj/abi.go` 中新增 `goabi2-*`，并在 goc-llc 中实现对应差异。
4. CI 矩阵加上新版本。

## 9. go1.18–go1.22

下限的选择（2026-10 调研，各版本 GOROOT 源码）：

- amd64 从 go1.18 开始。go1.17 是 amd64 第一个有寄存器 ABI 的版本，但目标文件格式是 `go117ld`：pc 表放在单独的 Pcdata 块里，FuncInfo 记录的是块内偏移，需要另写一套 writer/reader，因此不支持。
- arm64 从 go1.21 开始。arm64 的寄存器 ABI 虽然 1.18 就有，但 go1.20 及更早的 `runtime.adjustframe` 拷贝栈时只改写 amd64 帧里保存的帧指针（go1.20.14 `runtime/stack.go` 里写着 “TODO what about arm64 frame pointer adjustment?”），go1.21 才加上 arm64。goc 的 arm64 代码用 x29 寻址局部变量（例如 `MOVD R0, 24(R29)`），栈被拷贝后旧的 x29 指向旧栈，arm64 stress 测试在 1.19/1.20 上结果就不对。事实文件的 `arch.arm64.stack_copy_adjusts_fp` 记录了这一点，转换器对 1.21 前的 arm64 直接报错。

与 go1.23 写法不同的地方（都由 JSON 字段控制，见第 12 节）：

| 版本 | 差异 |
|---|---|
| ≤1.22 | 汇编符号不带 Linkname 标志 |
| ≤1.21 | 不加 args_stackmap/arginfo 引用；遍历时引用内联调用点的文件符号 |
| ≤1.20 | 函数 DIE 没有 decl_line（v0） |
| ≤1.19 | FuncInfo 没有 StartLine；类型/itab 前缀是 `type.`/`go.itab.`；文件头没有 GOARCH 变体部分 |
| 1.18 | 内容哈希是 sha1（20 字节）；重定位不排序 |

核对方法（这些版本没有旧实现可做逐字节比较）：

- 41 个真实 .gocobj 用各版本转换，再用 `goc tool objdump diff` 与 go1.23 的输出比较（重定位、符号类型和 flag 按名字比较）。每个差异都归到上表的某一项，没有其他差异；
- 用每个版本的链接器实际链接、运行：`scripts/ci-test.sh` 和完整套件（test262、QuickJS 测试、V8）每个版本都跑，结果与 verify-p9 相同；
- go1.23–1.27 的输出仍与旧实现逐字节一致（205/205，`TestGolden`）。

测试里的一处版本差异：go1.23 前的 `runtime.CallersFrames` 不会给手工拼出的 pc 列表补上内联外层帧，所以 `test/e2e/lines` 在这些版本上只检查内联的那一帧。

## 10. ABI profile

goc 生成的代码要和 Go 运行时在几个常量上一致。它们很少变，但仍是 Go 版本的事实，所以集中在一张表里：`llvm/include/Goc/AbiProfiles.def`，Go 侧的副本是 `internal/gocobj/abi.go`（有测试保证两者相同）。`.gocobj` 在 `.goc.header` 里写 profile 名，转换器不认识的 profile 一律拒绝。

| | goabi1-amd64 | goabi1-arm64 | 出处 |
|---|---|---|---|
| 覆盖的 Go 版本 | go1.18–go1.27 | go1.21–go1.27 | 第 9 节、第 11 节 |
| g 寄存器 | R14 | R28 | `cmd/compile/internal/ssa/_gen/{AMD64,ARM64}Ops.go` |
| `g.stackguard0` 偏移 | 16 | 16 | `runtime/runtime2.go`（在 `stack{lo,hi}` 之后） |
| StackSmall / StackBig | 128 / 4096 | 128 / 4096 | `internal/abi/stack.go`（1.21 前 `cmd/internal/objabi/stack.go`） |
| morestack | `runtime.morestack_noctxt` | 同 | `cmd/internal/obj/x86/obj6.go`、`obj/arm64/obj7.go` |
| ABIInternal 整数参数 | RAX RBX RCX RDI RSI R8–R11 | R0–R15 | `cmd/compile/abi-internal.md` |
| ABIInternal 浮点参数 | X0–X14 | F0–F15 | 同上 |

`go1.24-amd64` / `go1.24-arm64` 是 goabi1 的旧名，事实相同，留着是为了旧的 `.gocobj` 仍能读取。升级 Go 时逐行对照新版本源码：都没变就沿用 goabi1；有变化就加 `goabi2-*`，并让 goc-llc 和转换器处理这个差异（第 8 节）。

## 11. 版本差异的依据

本文汇总 Go 1.23–1.27 之间（go1.18–1.22 见文末一节）、会影响 goc 后端（goc-llc → `.gocobj` → 转换器 → goobj → `go build -toolexec`）的差异。所有结论都来自真实的 Go 源码，比对的是官方发布包里的 `go/src`：

| 版本 | 来源 |
|---|---|
| go1.22.12 | go.dev/dl 官方 linux-amd64 包（只作对照） |
| go1.23.12 | 同上 |
| go1.24.4 | Ubuntu `golang-1.24`（/usr/lib/go-1.24），即仓库基线 |
| go1.25.14 | go.dev/dl |
| go1.26.9 | go.dev/dl |
| go1.27.2 | go.dev/dl（2026-10 时的最新稳定版） |

“最近的 5 个有差异的版本”就是 **go1.23、go1.24、go1.25、go1.26、go1.27**：后四个每个都改了重定位类型表（见第 2 节）。go1.23 和 go1.22 的差别在 goobj 的 flag（第 1 节）和 `cmd/internal/notsha256`。抽取脚本是 `/workspace/gover-study/extract.py`，比对的都是下面列出的文件。

### 11.1 目标文件格式（cmd/internal/goobj/objfile.go）

| 项 | 1.23 | 1.24 | 1.25 | 1.26 | 1.27 |
|---|---|---|---|---|---|
| `Magic` | `"\x00go120ld"` | 同 | 同 | 同 | 同 |
| 新增的符号 flag | `SymFlagLinkname`、`SymFlagABIWrapper`、`ObjFlagStd`（1.23 新增） | `SymFlagWasmExport`、`AuxWasmType`，`Align` 字段 | 无 | 无（只有 Reader 的整数运算重构） | `SymFlagLinknameStd` |
| `cmd/internal/goobj/funcinfo.go` | 五个版本逐字节一致（md5 98407171） | | | | |

结论：块布局和 magic 没变，变的是 flag 位。这些数值由 `cmd/goc-gometa` 从各版本源码读进 `internal/gometa/data/go1.X.json`（`object.*`），goc 自己的 writer（`internal/goobj`）按它写。

### 11.2 重定位类型编号（cmd/internal/objabi/reloctype.go）

`RelocType` 是 `iota` 枚举（`R_ADDR RelocType = 1 + iota`，所以从 1 开始），写进 goobj 的是**数字**。中间插入或删除一项，后面的编号就会整体移位：

| 版本 | `R_*` 个数 | 相对上一版的变化 |
|---|---|---|
| 1.23 | 94 | — |
| 1.24 | 98 | 末尾加 `R_LOONG64_ADD64/SUB64`、`R_JMP16LOONG64`、`R_JMP21LOONG64` |
| 1.25 | 102 | **删除 `R_DWARFFILEREF`**（位于 ARM64 一组之前）；加 `R_RISCV_GOT_PCREL_ITYPE`、`R_DWTXTADDR_U1..U4` |
| 1.26 | 106 | 加 `R_RISCV_ADD32/SUB32`、`R_LOONG64_ADDR_PCREL20_S2`、`R_LOONG64_CALL36` |
| 1.27 | 109 | 加 `R_LOONG64_ADDR64_HI/LO`、`R_LOONG64_GOT64_HI/LO`，删 `R_JMPLOONG64` |

goc 用到的类型的实际编号（即 `internal/gometa/data/go1.X.json` 的 `relocs`；本表早先的版本误写成了从 0 开始的 iota 下标，已更正）：

| 类型 | 1.23 | 1.24 | 1.25 | 1.26 | 1.27 |
|---|---|---|---|---|---|
| `R_ADDR` / `R_CALL` / `R_PCREL` | 1 / 7 / 14 | 同 | 同 | 同 | 同 |
| `R_CALLARM64` / `R_ADDRARM64` | 9 / 3 | 同 | 同 | 同 | 同 |
| `R_ARM64_GOTPCREL` | 35 | 35 | **34** | 34 | 34 |
| `R_ARM64_PCREL` | 37 | 37 | **36** | 36 | 36 |
| `R_ARM64_PCREL_LDST8..64` | 38–41 | 38–41 | **37–40** | 37–40 | 37–40 |
| `R_ARM64_LDST8..128` | 42–46 | 42–46 | **41–45** | 41–45 | 41–45 |

**这是最关键的差异**：仓库原来那份 go1.24 的 vendored `objabi`，在 Go 1.25+ 上给 arm64 写出的 ADRP/LDST 重定位编号会差 1，链接时会解释成别的重定位类型。amd64 用到的三种类型编号没变，所以之前 amd64 上没暴露出来。

### 11.3 FUNCDATA / PCDATA（internal/abi/symtab.go）

| 常量 | 1.23–1.27 |
|---|---|
| `FUNCDATA_ArgsPointerMaps` / `LocalsPointerMaps` / `StackObjects` / `InlTree` | 0 / 1 / 2 / 3 |
| `FUNCDATA_OpenCodedDeferInfo` / `ArgInfo` / `ArgLiveInfo` / `WrapInfo` | 4 / 5 / 6 / 7 |
| `PCDATA_UnsafePoint` / `StackMapIndex` / `InlTreeIndex` / `ArgLiveIndex` | 0 / 1 / 2 / 3 |
| `PCDATA_PanicBounds` | 1.26 新增 = 4（goc 不用） |

symtab.go 的其他变化：1.25 把 `FuncID_runfinq` 换成 `FuncID_runFinalizers`、`FuncID_runCleanups`；1.26 把 `PCLnTabMagic` 挪进 internal/abi。goc 只写 `FuncID_normal`（0），不受影响。

### 11.4 栈与 g 的布局

| 事实 | 出处 | 1.23–1.27 |
|---|---|---|
| `StackSmall` / `StackBig` / `StackNosplitBase` | internal/abi/stack.go（五版 md5 都是 f3d244b4） | 128 / 4096 / 800 |
| `g` 的前几个字段 | runtime/runtime2.go `type g struct { stack stack; stackguard0 uintptr; stackguard1 uintptr; …}`，`type stack struct { lo, hi uintptr }` | `stack.lo`=0、`stack.hi`=8、`stackguard0`=16 |
| g 寄存器 | cmd/compile/internal/ssa/_gen/AMD64Ops.go `"g", // a.k.a. R14`；ARM64Ops.go 对应 R28 | amd64 R14，arm64 R28 |
| morestack 入口 | runtime/asm_amd64.s、asm_arm64.s `TEXT runtime·morestack_noctxt` | 五版都有 |
| objabi.StackNosplit 的算法 | cmd/internal/objabi/stack.go | 1.24 起 OpenBSD 也乘 StackGuardMultiplier；linux 下不变 |

### 11.5 ABIInternal 寄存器（cmd/compile/internal/ssa/_gen/{AMD64,ARM64}Ops.go）

| 架构 | `ParamIntRegNames` | `ParamFloatRegNames` |
|---|---|---|
| amd64 | `AX BX CX DI SI R8 R9 R10 R11` | `X0`–`X14` |
| arm64 | `R0`–`R15` | `F0`–`F15` |

五个版本完全一致（cmd/compile/abi-internal.md 的寄存器表也一致：R14/R28 = 当前 goroutine，X15 = 零寄存器）。

### 11.6 链接器与 toolexec

| 项 | 出处 | 结论 |
|---|---|---|
| 目标文件头 | cmd/internal/objabi/util.go `HeaderString()`：`go object GOOS GOARCH <buildcfg.Version>[ GOAMD64=v1 …] X:<experiments>` | 格式五版一致；`Version` 是**完整版本号**（如 `go1.24.13`） |
| 头校验 | cmd/link/internal/ld/lib.go `var wantHdr = objabi.HeaderString()`，`line != wantHdr` 就报 `linked object header mismatch` | 1.23 起一直是**逐字节**比较。所以 goobj 的头必须与链接器**完全同版本**（含补丁号）；转换器让同一个 Go 的 `go tool asm` 汇编一个空文件取得这一行（按版本缓存） |
| `buildcfg.Version` 来源 | internal/buildcfg/zbootstrap.go `const version = "go1.X.Y"` | goc 不读它，头部直接向该 Go 的汇编器探测 |
| linkname 限制 | cmd/link/internal/loader/loader.go `blockedLinknames` | 1.23 引入、之后逐版扩充；goc 的符号不在名单里 |
| writer 的 API | cmd/internal/obj/data.go `func (s *LSym) AddRel` | **1.24 新增**（会检查 FIPS 重定位）；1.23 直接 `s.R = append(s.R, r)`。goc 自己的 writer 不涉及 |
| writer 依赖的包 | `go list -deps cmd/internal/obj/x86` | 内容哈希：1.23 是 `cmd/internal/notsha256`（sha256 各字取反）；1.24 起是 `cmd/internal/hash`（sha256(0x01‖data) 取前 16 字节）。对应 `writer.content_hash` |
| toolexec 用到的工具 | `asm -gensymabis`、`compile`、`pack r` | 参数形式五版一致 |
| 发行包里预装的工具 | `pkg/tool/linux_amd64/` | 1.23、1.24 都有 `pack`、`nm`、`objdump`；**1.25 起只剩 asm、cgo、compile、cover、link、preprofile、vet（1.26 起加 fix）**，`pack` 改由 `go tool pack` 按需构建 |
| DWARF5 文本地址索引 | cmd/internal/obj/link.go `DwTextCount`、dwarf.go 第 298 行 `WriteDwTxtAddrx(…, c.DwTextCount*2)`；cmd/compile/internal/gc/compile.go 每编一个函数就 `DwTextCount++` | **1.25 新增**。重定位宽度（`R_DWTXTADDR_U1..U4`，同版本 reloctype.go 新增）按这个计数选；不设置时为 0，函数多的包在链接时报 `R_DWTXTADDR_U1 relocation overflow`（QuickJS 实测，`test/e2e/run.sh manyfuncs` 复现） |
| `LSym.Align` | cmd/internal/obj/link.go 的 `type LSym struct` | **1.27 新增**（int16）；1.23–1.26 只有 `FuncInfo.Align`。1.27 的 objfile.go 第 391 行起：内容可寻址、非 TEXT 且 `Align == 0` 的符号会被 `Diag("… is content-addressable but alignment is not set")` 拒绝，pctab 符号必须设 `Align: 1`（同文件 funcpctab 的写法） |

### 11.7 对 goc 的影响汇总

1. **重定位编号**（第 2 节）等数值每版不同，**文件头版本号**（第 6 节）要逐字节相同：转换器按 `go env GOVERSION` 选用该版本的事实文件（`internal/gometa/data`），头部向该 Go 的汇编器探测。
2. FUNCDATA/PCDATA、栈常量、g 偏移、g 寄存器、ABIInternal 寄存器在 1.23–1.27 中**完全相同**，所以 goc-llc 生成的机器码和 `.gocobj` 可以与 Go 版本无关（一个 ABI profile 覆盖五个版本）。
3. writer 行为的差异记在事实文件的 `writer`/`dwarf` 里：内容哈希（1.24 变）、DWARF5 `addrx` 与 `DwTextCount`（1.25+）、汇编符号的 Linkname 标志（1.27 去掉）、对齐规则与 `TrimInlineHash`（1.27）。见第 12 节。
4. toolexec 不能再假设 `$GOTOOLDIR/pack` 存在（1.25+）。

### 11.8 go1.18–go1.22（与 go1.23 的差异）

2026-10 加入这些版本时，从 go.dev/dl 官方包 go1.18.10、go1.19.13、go1.20.14、go1.21.13、go1.22.12 的 `go/src` 读出：

| 项 | 1.18 | 1.19 | 1.20 | 1.21 | 1.22 | 出处 |
|---|---|---|---|---|---|---|
| 目标文件 magic | `go118ld` | `go118ld` | `go120ld` | 同 | 同 | `cmd/internal/goobj/objfile.go` |
| FuncInfo 的 StartLine | 无 | 无 | 有 | 有 | 有 | `goobj/funcinfo.go` `(*FuncInfo).Write` |
| 内容哈希 | sha1，20 字节 | notsha256，16 字节 | 同 | 同 | 同 | `obj/objfile.go` `contentHash`、`goobj.HashSize` |
| 重定位按偏移排序 | 否 | 是 | 是 | 是 | 是 | `obj/objfile.go` `WriteObjFile` |
| 类型/itab 前缀 | `type.` / `go.itab.` | 同 | `type:` / `go:itab.` | 同 | 同 | `obj/objfile.go` `(*writer).Sym` |
| 文件头 GOARCH 变体部分 | 无 | 无 | 有 | 有 | 有 | `objabi/util.go` `HeaderString` |
| 函数 DIE 的 decl_line | 无 | 无 | 无 | 有 | 有 | `dwarf/dwarf.go` `PutDefaultFunc` |
| 汇编函数的 args_stackmap | 只给 `"".f` | 同 | 同 | 同 | 带包名的本包符号 | `obj/plist.go` `Flushplist` |
| 内联调用点的文件符号引用 | 有 | 有 | 有 | 有 | 无 | `obj/sym.go` `traverseFuncAux` |
| 汇编符号的 Linkname 标志 | 无 | 无 | 无 | 无 | 无（1.23 起有） | `obj/objfile.go` `(*writer).Sym` |
| FUNCDATA/FuncID 定义 | `objabi/funcdata.go`、`funcid.go` | 同 | 同 | `internal/abi/symtab.go` | 同 | |
| ARM64 ADRP+LDR 配对重定位 | 无 | 无 | `R_ARM64_PCREL_LDST*` | 同 | 同 | `objabi/reloctype.go` |
| 拷贝栈时改写 arm64 保存的帧指针 | 否 | 否 | 否 | 是 | 是 | `runtime/stack.go` `adjustframe` |

栈常量（StackSmall 128、StackBig 4096、nosplit 预算 800）、g 的字段偏移、g 寄存器和参数寄存器在 1.18–1.27 相同，所以 ABI profile 不变。arm64 因最后一行从 go1.21 起支持，见第 9 节。

## 12. 事实文件格式（gometa JSON，schema 1）

`internal/gometa/data/go1.X.json` 每个 Go minor 版本一份，由 `cmd/goc-gometa` 从该版本的 GOROOT/src 生成，内嵌进转换器（`internal/gometa` 包）。`goc tool convert` 也可以用 `-go-meta file.json` 读外部文件。Go 侧的类型定义在 `gometa/meta.go`，`gometa.Parse` 会检查必需字段是否齐全。

所有“名字 → 数值”的表都用 Go 源码里的常量名，去掉公共前缀。例如 `SymFlagDupok` 记作 `Dupok`，`FUNCDATA_InlTree` 记作 `InlTree`。重定位和符号类型保留原名，如 `R_ADDR`、`SRODATA`。

### 顶层

| 字段 | 类型 | 含义 |
|---|---|---|
| `schema` | int | 本格式的版本，当前为 1，`gometa.Schema` 不同就拒绝 |
| `go` | string | 覆盖的 minor 版本，如 `"go1.25"` |
| `source_version` | string | 生成时读的 GOROOT/VERSION，如 `"go1.25.14"`（只作记录） |
| `resolver` | string | 生成工具 |
| `object` | object | goobj 容器格式，见下 |
| `writer` | object | Go writer 中随版本变化的行为 |
| `sym_kinds` | map | `objabi.SymKind` 名 → 编号（cmd/internal/objabi/symkind.go；1.23 的 `SRODATA`=2，1.24 起为 3） |
| `relocs` | map | `objabi.RelocType` 名 → 编号（cmd/internal/objabi/reloctype.go，从 1 开始） |
| `abi` | object | 与架构无关的运行时事实 |
| `arch` | map | GOARCH（`amd64`、`arm64`）→ 架构事实 |
| `dwarf` | object | 汇编路径为每个函数生成的 DWARF |
| `toolexec` | object | 发行包工具的事实 |
| `sources` | map | 事实组 → 读过的 GOROOT/src 文件（空格分隔） |

### object（cmd/internal/goobj/objfile.go 等）

| 字段 | 含义 |
|---|---|
| `magic` | `"\u0000go120ld"`（8 字节） |
| `header_format` | `objabi.HeaderString` 的格式说明，只作文档。实际的头由转换器向 `go tool asm` 探测 |
| `blocks` | 19 个块名，按文件中的顺序，`Autolib` … `End` |
| `obj_flags` / `sym_flags` / `sym_flags2` | `ObjFlag*` / `SymFlag*`（Flag 与 Flag2 两组）位值 |
| `aux_types` | `Aux*` 类型值（`Gotype`、`FuncInfo`、`Funcdata`、`DwarfInfo`、`DwarfLines`、`Pcsp`…） |
| `pkg_idx` | 特殊包下标 `None`、`Hashed64`、`Hashed`、`Builtin`、`Self` |
| `sym_abi_static` | `SymABIstatic` |
| `hash_size` / `hash64_size` | 内容哈希的长度（16 / 8；1.18 的 hash_size 是 sha1 的 20） |
| `builtins` | `goobj.builtins`，按下标排列的 `{name, abi}` |

### writer

| 字段 | 取值 | 含义（出处 cmd/internal/obj/objfile.go、plist.go） |
|---|---|---|
| `content_hash` | `sha1` \| `notsha256` \| `sha256-prefix01` | 内容可寻址符号的哈希：1.18 为 sha1，1.19–1.23 为取反的 sha256，1.24 起为 sha256(0x01‖data)，截取 `hash_size` 字节 |
| `asm_named_syms_linkname` | bool | 汇编对象的有名符号是否带 `SymFlagLinkname`（1.23–1.26 为 true） |
| `align` | `heuristic` \| `explicit` | heuristic：内容可寻址的数据按大小推出对齐（8/4/2/1）；explicit（1.27）：写 `LSym.Align`，pctab 为 1 |
| `trim_inline_hash` | bool | 符号名是否去掉 `#…#` inline 后缀（1.27） |
| `asm_args_stackmap` | bool | Flushplist 是否给转换器写出的本包 ABI0 函数（名字带包名）加 `.args_stackmap` 和 `.arginfo<ABI>` funcdata。1.22 起为 true；之前只加给 `"".f` 形式的名字，所以为 false |
| `type_prefix` / `itab_prefix` | string | 标记 Go 类型和 itab 符号的名字前缀：1.20 起 `type:` / `go:itab.`，之前 `type.` / `go.itab.` |
| `sort_relocs` | bool | WriteObjFile 是否按偏移排序重定位（1.19 起） |
| `funcinfo_start_line` | bool | `goobj.FuncInfo` 在 FuncFlag 后是否有 StartLine（1.20 起） |
| `inl_file_sym_refs` | bool | traverseFuncAux 是否还访问每个内联调用点的文件符号（1.22 前） |
| `func_flags` / `func_ids` | map | `internal/abi` 的 `FuncFlag*` / `FuncID*`（1.21 前读 `objabi` 的 `FuncFlag_*`/`FuncID_*`，换成 internal/abi 的名字） |
| `runtime_func_ids` | map | `objabi.funcIDs`：runtime 函数名（不带 `runtime.`）→ FuncID |

### abi（internal/abi/symtab.go、stack.go，runtime/runtime2.go）

| 字段 | 含义 |
|---|---|
| `funcdata` / `pcdata` | `FUNCDATA_*` / `PCDATA_*` 下标 |
| `stack_small` / `stack_big` / `stack_nosplit_base` | 128 / 4096 / 800（1.18–1.27；1.21 前由 `objabi` 的 StackGuard 928 − StackSmall 得出） |
| `g_offsets` | `runtime.g` 字段的字节偏移（64 位）：`stack_lo`、`stack_hi`、`stackguard0` |

### arch.<goarch>

| 字段 | 含义 |
|---|---|
| `ptr_size` / `min_lc` | 指针大小、最小指令长度（PC 量子），出自 cmd/internal/sys/arch.go |
| `g_reg` | g 寄存器（amd64 R14，arm64 R28），出自 ssa/_gen/*Ops.go |
| `int_arg_regs` / `float_arg_regs` | ABIInternal 的参数寄存器（`ParamIntRegNames` / `ParamFloatRegNames`） |
| `morestack` | morestack 入口符号 |
| `stack_copy_adjusts_fp` | runtime 拷贝栈时是否改写帧里保存的帧指针（`runtime.adjustframe`）。goc 的代码用帧指针寻址局部变量，为 false 的架构（1.21 前的 arm64）转换器拒绝 |

转换器用 `abi` 和 `arch` 的事实匹配 goc 的 ABI profile（`internal/gocobj/abi.go`，`llvm/include/Goc/AbiProfiles.def`）。

### dwarf

| 字段 | 含义 |
|---|---|
| `abbrev_function` | `DW_ABRV_FUNCTION` |
| `func_die` | `v0`（1.21 前）：同 v1，但没有 decl_line；`v1`：low/high pc 都是 `DW_FORM_addr`（R_ADDR）；`v2`：`emitHiLoPc`，开启 dwarf5 实验时为 `DW_FORM_addrx` + udata 长度，否则同 v1 |
| `lines` | `generateDebugLinesSymbol` 的片段布局，当前为 `v1` |
| `text_addrx` | `FuncCountToDwTxtAddrFlavor`：`{max_funcs, reloc, size}` 从小到大排列。选择依据是 `DwTextCount*2`，与 Go 一致。只有 v2 版本有这一项 |

### toolexec

| 字段 | 含义 |
|---|---|
| `pack_prebuilt` | 发行包是否预装 `pkg/tool/<os_arch>/pack`（1.25 起没有） |

### 兼容规则

- 新增事实时加字段，旧字段的含义不变。语义不兼容的改动要把 `schema` 加 1，并同步修改 `gometa.Schema`。
- 字符串枚举（`content_hash`、`align`、`func_die`、`lines`）出现 `internal/goobj` 不认识的值时，转换器报错，不会猜测。
- 解析器只有在相关 Go 函数的指纹已登记在 `cmd/goc-gometa/reviewed.go` 时才会输出 JSON（见第 2 节）。
