# Go 版本事实文件（gometa JSON）格式，schema 1

`gometa/data/go1.X.json` 每个 Go minor 版本一份，由 `tools/golangc-metadata-resolver` 从该版本的 GOROOT/src 生成，内嵌进 gocobj2goobj（`gometa` 包）。gocobj2goobj 也可以用 `-go-meta file.json` 读外部文件。Go 侧的类型定义在 `gometa/meta.go`，`gometa.Parse` 会检查必需字段是否齐全。

所有“名字 → 数值”的表都用 Go 源码里的常量名，去掉公共前缀。例如 `SymFlagDupok` 记作 `Dupok`，`FUNCDATA_InlTree` 记作 `InlTree`。重定位和符号类型保留原名，如 `R_ADDR`、`SRODATA`。

## 顶层

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

## object（cmd/internal/goobj/objfile.go 等）

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

## writer

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

## abi（internal/abi/symtab.go、stack.go，runtime/runtime2.go）

| 字段 | 含义 |
|---|---|
| `funcdata` / `pcdata` | `FUNCDATA_*` / `PCDATA_*` 下标 |
| `stack_small` / `stack_big` / `stack_nosplit_base` | 128 / 4096 / 800（1.18–1.27；1.21 前由 `objabi` 的 StackGuard 928 − StackSmall 得出） |
| `g_offsets` | `runtime.g` 字段的字节偏移（64 位）：`stack_lo`、`stack_hi`、`stackguard0` |

## arch.<goarch>

| 字段 | 含义 |
|---|---|
| `ptr_size` / `min_lc` | 指针大小、最小指令长度（PC 量子），出自 cmd/internal/sys/arch.go |
| `g_reg` | g 寄存器（amd64 R14，arm64 R28），出自 ssa/_gen/*Ops.go |
| `int_arg_regs` / `float_arg_regs` | ABIInternal 的参数寄存器（`ParamIntRegNames` / `ParamFloatRegNames`） |
| `morestack` | morestack 入口符号 |
| `stack_copy_adjusts_fp` | runtime 拷贝栈时是否改写帧里保存的帧指针（`runtime.adjustframe`）。goc 的代码用帧指针寻址局部变量，为 false 的架构（1.21 前的 arm64）转换器拒绝 |

gocobj2goobj 用 `abi` 和 `arch` 的事实匹配 goc 的 ABI profile（`gocobj/abi.go`，`llvm/include/Goc/AbiProfiles.def`）。

## dwarf

| 字段 | 含义 |
|---|---|
| `abbrev_function` | `DW_ABRV_FUNCTION` |
| `func_die` | `v0`（1.21 前）：同 v1，但没有 decl_line；`v1`：low/high pc 都是 `DW_FORM_addr`（R_ADDR）；`v2`：`emitHiLoPc`，开启 dwarf5 实验时为 `DW_FORM_addrx` + udata 长度，否则同 v1 |
| `lines` | `generateDebugLinesSymbol` 的片段布局，当前为 `v1` |
| `text_addrx` | `FuncCountToDwTxtAddrFlavor`：`{max_funcs, reloc, size}` 从小到大排列。选择依据是 `DwTextCount*2`，与 Go 一致。只有 v2 版本有这一项 |

## toolexec

| 字段 | 含义 |
|---|---|
| `pack_prebuilt` | 发行包是否预装 `pkg/tool/<os_arch>/pack`（1.25 起没有） |

## 兼容规则

- 新增事实时加字段，旧字段的含义不变。语义不兼容的改动要把 `schema` 加 1，并同步修改 `gometa.Schema`。
- 字符串枚举（`content_hash`、`align`、`func_die`、`lines`）出现 goobjw 不认识的值时，转换器报错，不会猜测。
- 解析器只有在相关 Go 函数的指纹已登记在 `tools/golangc-metadata-resolver/reviewed.go` 时才会输出 JSON（见 [multi-go-version.md](multi-go-version.md) 第 2 节）。
