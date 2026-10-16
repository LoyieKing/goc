# 05 .gocobj、转换与 Go 链接

[← 04 goc-llc](04-goc-llc.md) · [索引](../engineering-architecture.md) · [下一篇：运行时 →](06-runtime.md)

## 1. 为什么要有 `.gocobj` 这一层

Go 链接器只吃 goobj。goobj 的编码随 Go 版本变，而 LLVM 只会写 ELF。goc 的分工是：

- **LLVM 一侧**（goc-llc + llvm-mc）产出所有和机器相关的东西：指令、重定位、pc 表（SP_DELTA、UNSAFE_POINT）、栈图。写成一个 ELF 加若干 `.goc.*` 段，即 `.gocobj`。**除了 header 里的 ABI profile 名，`.gocobj` 与 Go 版本无关。**
- **Go 一侧**（`tools/gocobj2goobj`）只做查表翻译：不解码也不生成任何指令，只决定每个名字在 Go 里叫什么、是什么 ABI，以及 ELF 重定位对应哪个 Go 重定位类型。

权威格式：`docs/gocobj-format.md`（当前 1.4）。读写库：`gocobj/`。查看：`go run ./tools/goc-objdump dump|verify FILE`。金样：`test/gocobj/v1.0/`。

## 2. 格式摘要

编码规则：小端；`uleb`/`sleb` 是 LEB128；`strref` 是 `.goc.strtab` 的字节偏移；**符号按名字引用**（MC 写文件时才分配 ELF 符号号）；
除 header 和 strtab 外每个段都是 `uleb length + 内容` 的记录列表，读者可以跳过不认识的尾部字段（minor 版本可以追加字段）。

| 段 | 每条记录 | 要点 |
|---|---|---|
| `.goc.header` | 定长 28 字节 | magic `GOCOBJ\0\0`、major=1、minor、arch（1 amd64 / 2 arm64）、flags bit0 `stack_checks_in_code`、ABI profile、producer |
| `.goc.strtab` | 字符串池 | 不是记录列表 |
| `.goc.files` | 一个源文件 | 记录 0 是本 TU；1.4 起包含行表和内联用到的头文件 |
| `.goc.funcs` | 一个 Go 可见函数 | `text_size`、`frame_size`、`arg_size`、`abi`（0 ABI0 / 1 ABIInternal）、`kind`（1 = ABI wrapper）、`flags`（bit0 needs_stack_check、1 leaf、2 nosplit、3 has_frame_pointer、4 unsafe_entry、5 internal、6 有 sptr、7 sysv_ptr_regs 未建模）、`file`、`start_line`、指向 pcvalue/gcmaps/inltree 记录的偏移、`sysv_ptr_regs`、`sptr_slots`（`BP - off` 的槽偏移） |
| `.goc.relocs` | 一个函数内重定位 | kind：1 CALL、2 CALL_INDIRECT_MARK、3 PCREL、4 ADDR_ABS、5 FUNC_ADDR、6 DATA_ADDR、7 TLS_G、8 GOT_PCREL、9 JMP_TAILCALL、10 ARM64_ADRP_ADD、11 ARM64_CALL26；flags bit0 safepoint_call |
| `.goc.stackmaps` | 一个 LLVM STACKMAP | `.llvm_stackmaps` v3 的位置编码（1 Register、2 Direct、3 Indirect、4 Constant；`reg` 是 DWARF 号）。转换器用 gcmaps，这里留给 verify 和调试 |
| `.goc.gcmaps` | 一个函数 | `nargs_bits`、`nlocals_bits`，每个 safepoint：返回地址 pc、args 位图、locals 位图 |
| `.goc.pcvalue` | 一个函数 | 若干表，kind：1 SP_DELTA、2 FILE、3 LINE、4 UNSAFE_POINT、5 INL_INDEX、6 STACKMAP_INDEX；存原值，Go 的差分编码由转换器做 |
| `.goc.inltree` | 一个函数的内联树 | `parent`、调用点 file/line、被内联函数名、`parent_pc`、`start_line` |
| `.goc.wrappers` | 一个 ABI wrapper | `from_abi`/`to_abi`（0 ABI0、1 ABIInternal、2 C）、morestack 路径上寄存器参数溢出到哪、栈上指针参数偏移 |
| `.goc.data` | 一个数据符号 | 大小、对齐、flags（rodata/noptr/local/dupok）、指针位图 |

ELF 自带的重定位仍然有效，只作交叉检查。02 篇 §2.5 / §3.4 有 `goc-objdump dump` 的真实输出。

## 3. `gocobj2goobj`

```
gocobj2goobj -in foo.gocobj -o foo.goobj [-p main] [-symabis foo.symabis] [-goarch arm64]
gocobj2goobj -print-profile -goarch arm64     # 这个 Go 对应的 ABI profile
gocobj2goobj -version                          # 写出的对象头里的 Go 版本
```

转换器**按 Go 版本构建**：`tools/goc/goenc.sh` 的 `goc_converter` 先用 `tools/goc-goenc` 从当前 GOROOT 拷出 writer（`cmd/internal/obj…`），再针对它构建 gocobj2goobj，按完整版本号缓存在 `build/goenc/`。设计见 [multi-go-version.md](../multi-go-version.md)，版本差异见 [go-version-diffs.md](../go-version-diffs.md)。

`main.go` 的流程：
1. `gocobj.Open` + `gocobj.Verify`，有错就停。
2. header 里的 profile 必须与这个 Go 的 profile **事实相同**（`gocobj.SameFacts`；`go1.24-amd64` 是 `goabi1-amd64` 的旧名）。
3. header 的 `stack_checks_in_code` 必须为 1（后端必须已经发出栈检查）。
4. `newConverter` → `write`；`-symabis` 时写出 `def <pkg>.<name> ABI0|ABIInternal` 行。

分文件：

| 文件 | 内容 |
|---|---|
| `convert.go` | 名字解析：`tuTag`（内部 C 名加 TU 标签）、`resolveCName`（`.impl` / `.goabi` / `@plt` 约定）、`resolveTarget`（ELF 重定位 → Go 符号 + 偏移；PC 相对形式去掉 −4，因为 Go 的 `R_PCREL`/`R_CALL` 自己减字段宽度）、`gotSlot`、`GOC_LIBCALL_IMPL`、`textSecs`/`sortedKeys`（按段号排序遍历，保证输出确定） |
| `data.go` | 数据段 → 数据符号 |
| `pctab.go` | pcvalue → Go 的 pctab 编码；gcmaps → `gclocals.*` 符号和 `PCDATA_StackMapIndex` |
| `write.go` | 用 writer 写 goobj；为每个 `.goc.files` 建 PosBase |
| `arm64.go` | arm64 重定位：ADRP+低 12 位配对、GOT 配对等 |
| `relocmap.go` | ELF → Go 重定位映射（下节），与版本无关 |

## 4. `relocmap.go`：按名字引用的映射

| 字段 | 值（都是 `objabi`/`abi` 的常量名，数字来自所用 Go 的 writer） |
|---|---|
| `X86Text` | `R_X86_64_PC32`/`PLT32` → `R_PCREL`，`R_X86_64_32`/`32S` → `R_ADDR` |
| `X86Call` | `R_CALL`（`.goc.relocs` 标为调用的 PC 相对字段） |
| `X86Data` | `R_X86_64_64` → `{R_ADDR, 8}` 等 |
| `A64Call` / `A64Page` / `A64PagePair` / `A64Lo12` / `A64GOTPair` / `A64Data` | `R_CALLARM64`、`R_ARM64_PCREL`、`R_ADDRARM64`、`R_ARM64_PCREL_LDST{8,16,32,64}`、`R_ARM64_LDST*`、`R_ARM64_GOTPCREL`… |
| `FuncdataArgs` / `FuncdataLocals` / `PcdataStackMapIndex` | `abi.FUNCDATA_ArgsPointerMaps`、`abi.FUNCDATA_LocalsPointerMaps`、`abi.PCDATA_StackMapIndex` |

profile 来自生成的 `objabi.GocProfiles`（goc-goenc 探测 GOROOT 后写入）。各版本 writer API 的差异由生成的 `goc_compat.go` 吸收（`GocAddRel`、`GocSetAlign`、`GocSetTextCount`）。

## 5. 接进 `go build`：`tools/goc/toolexec_recognize_goobj.sh`

`scripts/goc-go.sh` 用 `go build -a -toolexec <这个脚本>`。Go 每调用一个工具都先经过它：

| 工具 | 做什么 |
|---|---|
| `asm`（带 `-gensymabis`，且 `-p` 是我们的包） | 先跑原工具，再往输出的 symabis 文件追加：几行写死的 `def <pkg>.GocCheckedAdd ABIInternal` 等，加上每个对象旁边的 `foo.symabis`。asm 只扫描 `.s` 文件，不知道 goobj 里有哪些 TEXT |
| `compile`（`-p` 是我们的包） | 先跑原工具，再用 `go tool pack r` 把每个 `.goobj` 追加进包归档 `_pkg_.a`。成员名改成 `.o`：Go 链接器只加载名字以 `.o` 或 `.syso` 结尾的成员 |
| 其它 | 原样 `exec` |

「我们的包」由 `GOC_PACK_PKG`（`goc-go.sh` 设为 `main` 或导入路径）判断，依赖包不碰。对象列表在 `GOC_BINOBJ`（空格分隔）。

`goc-go.sh` 还写 `goc_marker_amd64.s`：Go 只有在包里有汇编文件时才接受无函数体的 Go 声明；声明本身由 symabis 里的 ABIInternal 定义满足，所以不生成 ABI 包装。

## 6. Go 版本耦合点（升级 Go 时要看的地方）

| # | 位置 | 内容 |
|---|---|---|
| 1 | `llvm/include/Goc/AbiProfiles.def` | g 寄存器、`stackguard0` 偏移、StackSmall/StackBig、morestack 名、ABIInternal 整数 / 浮点参数寄存器 |
| 2 | `gocobj/abi.go` | 上表的 Go 副本，`TestProfilesMatchDef` 保证两者一致 |
| 3 | `tools/goc-goenc` | 从 GOROOT 拷 writer、探测 ABI 事实选 profile、生成 `goc_compat.go`（各版本 API 差异） |
| 4 | `goobj/enc/` | goc-goenc 由 go1.24.4 生成的树内副本（供 `go vet`/单测；CI 检查能重新生成一致）；实际转换用的是按版本生成的副本 |
| 5 | `goobj/lower/goabi_x86.go`、`goabi_a64.go` | thunk 按 Go 1.24 ABIInternal 生成（生成文件的首行就写着 `Go 1.24 ABIInternal -> SysV entry thunks`）：参数寄存器分配、X15 清零、溢出区 |
| 6 | `llvm/lib/Target/X86/GocX86StackCheck.cpp`、`GocAArch64StackCheck.cpp` | g 的取法（`FS:-8` / X28）、morestack 的寄存器约定（R14 装 g、X3 装返回地址） |
| 7 | `SMUptrInline.cpp`（`tlsGWord`）、`runtime/uptr/goc_uptr_tls_amd64.S` | `g.stack.lo` 在 `g+0`、`g.stack.hi` 在 `g+8` |
| 8 | `llvm/lib/Target/AArch64/GocAArch64Frame.cpp` | Go arm64 帧布局：LR 在 `0(RSP)` |
| 9 | `UNSAFE_POINT` 的取值 | −1 safe / −2 unsafe（Go 的 `abi.UnsafePointSafe`/`Unsafe`），在 GocObjTables 里产生 |
| 10 | `tools/goc/toolexec_recognize_goobj.sh` | 依赖 `go build -toolexec` 调 `asm -gensymabis` / `compile` 的参数形状；`pack` 不在 GOTOOLDIR 时（1.25+）用 `go tool pack` |
| 11 | `.github/workflows/ci.yml` | 矩阵：go 1.23.x、1.24.4、1.25.x、1.26.x、1.27.x，全部必过 |

## 7. 符号命名约定（从产物里观察到的）

以 `call.c`、`-p main` 为例（`go tool nm`）：

| 符号 | 是什么 |
|---|---|
| `main.bump` | Go ABIInternal 入口（thunk，`kind=1`），symabis `def main.bump ABIInternal` |
| `main.bump.impl` | C 本体（SysV），symabis `def main.bump.impl ABI0` |
| `gclocals.goc.main.bump.impl.args` / `.locals` | 栈图位图（R 只读数据） |
| `main.bump.impl.arginfo0` 等（U） | 引用了但这个对象没定义的辅助符号 |

C 对 C 的调用直接 `call fill.impl`（SysV 到 SysV），不经过 thunk。
