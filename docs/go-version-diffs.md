# Go 各版本之间与 goc 相关的目标文件 / ABI 差异

本文汇总 Go 1.23–1.27 之间、会影响 goc 后端（goc-llc → `.gocobj` → gocobj2goobj → goobj → `go build -toolexec`）的差异。所有结论都来自真实的 Go 源码，比对的是官方发布包里的 `go/src`：

| 版本 | 来源 |
|---|---|
| go1.22.12 | go.dev/dl 官方 linux-amd64 包（只作对照） |
| go1.23.12 | 同上 |
| go1.24.4 | Ubuntu `golang-1.24`（/usr/lib/go-1.24），即仓库基线 |
| go1.25.14 | go.dev/dl |
| go1.26.9 | go.dev/dl |
| go1.27.2 | go.dev/dl（2026-10 时的最新稳定版） |

“最近的 5 个有差异的版本”就是 **go1.23、go1.24、go1.25、go1.26、go1.27**：后四个每个都改了重定位类型表（见第 2 节）。go1.23 和 go1.22 的差别在 goobj 的 flag（第 1 节）和 `cmd/internal/notsha256`。抽取脚本是 `/workspace/gover-study/extract.py`，比对的都是下面列出的文件。

## 1. 目标文件格式（cmd/internal/goobj/objfile.go）

| 项 | 1.23 | 1.24 | 1.25 | 1.26 | 1.27 |
|---|---|---|---|---|---|
| `Magic` | `"\x00go120ld"` | 同 | 同 | 同 | 同 |
| 新增的符号 flag | `SymFlagLinkname`、`SymFlagABIWrapper`、`ObjFlagStd`（1.23 新增） | `SymFlagWasmExport`、`AuxWasmType`，`Align` 字段 | 无 | 无（只有 Reader 的整数运算重构） | `SymFlagLinknameStd` |
| `cmd/internal/goobj/funcinfo.go` | 五个版本逐字节一致（md5 98407171） | | | | |

结论：块布局和 magic 没变，变的是 flag 位。goc 用到的是 writer 一侧（`obj.WriteObjFile`），只要 writer 来自同一个 Go 版本，flag 就自然一致。

## 2. 重定位类型编号（cmd/internal/objabi/reloctype.go）

`RelocType` 是 `iota` 枚举，写进 goobj 的是**数字**。中间插入或删除一项，后面的编号就会整体移位：

| 版本 | `R_*` 个数 | 相对上一版的变化 |
|---|---|---|
| 1.23 | 94 | — |
| 1.24 | 98 | 末尾加 `R_LOONG64_ADD64/SUB64`、`R_JMP16LOONG64`、`R_JMP21LOONG64` |
| 1.25 | 102 | **删除 `R_DWARFFILEREF`**（位于 ARM64 一组之前）；加 `R_RISCV_GOT_PCREL_ITYPE`、`R_DWTXTADDR_U1..U4` |
| 1.26 | 106 | 加 `R_RISCV_ADD32/SUB32`、`R_LOONG64_ADDR_PCREL20_S2`、`R_LOONG64_CALL36` |
| 1.27 | 109 | 加 `R_LOONG64_ADDR64_HI/LO`、`R_LOONG64_GOT64_HI/LO`，删 `R_JMPLOONG64` |

goc 用到的类型的实际编号：

| 类型 | 1.23 | 1.24 | 1.25 | 1.26 | 1.27 |
|---|---|---|---|---|---|
| `R_ADDR` / `R_CALL` / `R_PCREL` | 0 / 6 / 13 | 同 | 同 | 同 | 同 |
| `R_CALLARM64` / `R_ADDRARM64` | 8 / 2 | 同 | 同 | 同 | 同 |
| `R_ARM64_GOTPCREL` | 34 | 34 | **33** | 33 | 33 |
| `R_ARM64_PCREL` | 36 | 36 | **35** | 35 | 35 |
| `R_ARM64_PCREL_LDST8..64` | 37–40 | 37–40 | **36–39** | 36–39 | 36–39 |
| `R_ARM64_LDST8..128` | 41–45 | 41–45 | **40–44** | 40–44 | 40–44 |

**这是最关键的差异**：仓库原来那份 go1.24 的 vendored `objabi`，在 Go 1.25+ 上给 arm64 写出的 ADRP/LDST 重定位编号会差 1，链接时会解释成别的重定位类型。amd64 用到的三种类型编号没变，所以之前 amd64 上没暴露出来。

## 3. FUNCDATA / PCDATA（internal/abi/symtab.go）

| 常量 | 1.23–1.27 |
|---|---|
| `FUNCDATA_ArgsPointerMaps` / `LocalsPointerMaps` / `StackObjects` / `InlTree` | 0 / 1 / 2 / 3 |
| `FUNCDATA_OpenCodedDeferInfo` / `ArgInfo` / `ArgLiveInfo` / `WrapInfo` | 4 / 5 / 6 / 7 |
| `PCDATA_UnsafePoint` / `StackMapIndex` / `InlTreeIndex` / `ArgLiveIndex` | 0 / 1 / 2 / 3 |
| `PCDATA_PanicBounds` | 1.26 新增 = 4（goc 不用） |

symtab.go 的其他变化：1.25 把 `FuncID_runfinq` 换成 `FuncID_runFinalizers`、`FuncID_runCleanups`；1.26 把 `PCLnTabMagic` 挪进 internal/abi。goc 只写 `FuncID_normal`（0），不受影响。

## 4. 栈与 g 的布局

| 事实 | 出处 | 1.23–1.27 |
|---|---|---|
| `StackSmall` / `StackBig` / `StackNosplitBase` | internal/abi/stack.go（五版 md5 都是 f3d244b4） | 128 / 4096 / 800 |
| `g` 的前几个字段 | runtime/runtime2.go `type g struct { stack stack; stackguard0 uintptr; stackguard1 uintptr; …}`，`type stack struct { lo, hi uintptr }` | `stack.lo`=0、`stack.hi`=8、`stackguard0`=16 |
| g 寄存器 | cmd/compile/internal/ssa/_gen/AMD64Ops.go `"g", // a.k.a. R14`；ARM64Ops.go 对应 R28 | amd64 R14，arm64 R28 |
| morestack 入口 | runtime/asm_amd64.s、asm_arm64.s `TEXT runtime·morestack_noctxt` | 五版都有 |
| objabi.StackNosplit 的算法 | cmd/internal/objabi/stack.go | 1.24 起 OpenBSD 也乘 StackGuardMultiplier；linux 下不变 |

## 5. ABIInternal 寄存器（cmd/compile/internal/ssa/_gen/{AMD64,ARM64}Ops.go）

| 架构 | `ParamIntRegNames` | `ParamFloatRegNames` |
|---|---|---|
| amd64 | `AX BX CX DI SI R8 R9 R10 R11` | `X0`–`X14` |
| arm64 | `R0`–`R15` | `F0`–`F15` |

五个版本完全一致（cmd/compile/abi-internal.md 的寄存器表也一致：R14/R28 = 当前 goroutine，X15 = 零寄存器）。

## 6. 链接器与 toolexec

| 项 | 出处 | 结论 |
|---|---|---|
| 目标文件头 | cmd/internal/objabi/util.go `HeaderString()`：`go object GOOS GOARCH <buildcfg.Version>[ GOAMD64=v1 …] X:<experiments>` | 格式五版一致；`Version` 是**完整版本号**（如 `go1.24.13`） |
| 头校验 | cmd/link/internal/ld/lib.go `var wantHdr = objabi.HeaderString()`，`line != wantHdr` 就报 `linked object header mismatch` | 1.23 起一直是**逐字节**比较。所以 goobj 必须由与链接器**完全同版本**（含补丁号）的 writer 写出 |
| `buildcfg.Version` 来源 | internal/buildcfg/zbootstrap.go `const version = "go1.X.Y"` | 拷贝 writer 时 zbootstrap.go 会一起带过来，所以这份拷贝写出的头就是该 GOROOT 的版本 |
| linkname 限制 | cmd/link/internal/loader/loader.go `blockedLinknames` | 1.23 引入、之后逐版扩充；goc 的符号不在名单里 |
| writer 的 API | cmd/internal/obj/data.go `func (s *LSym) AddRel` | **1.24 新增**（会检查 FIPS 重定位）；1.23 直接 `s.R = append(s.R, r)` |
| writer 依赖的包 | `go list -deps cmd/internal/obj/x86` | 1.23 需要 `cmd/internal/notsha256`（带汇编）；1.24 起换成 `cmd/internal/hash`；1.26 起多了 `internal/strconv` |
| toolexec 用到的工具 | `asm -gensymabis`、`compile`、`pack r` | 参数形式五版一致 |
| 发行包里预装的工具 | `pkg/tool/linux_amd64/` | 1.23、1.24 都有 `pack`、`nm`、`objdump`；**1.25 起只剩 asm、cgo、compile、cover、link、preprofile、vet（1.26 起加 fix）**，`pack` 改由 `go tool pack` 按需构建 |
| DWARF5 文本地址索引 | cmd/internal/obj/link.go `DwTextCount`、dwarf.go 第 298 行 `WriteDwTxtAddrx(…, c.DwTextCount*2)`；cmd/compile/internal/gc/compile.go 每编一个函数就 `DwTextCount++` | **1.25 新增**。重定位宽度（`R_DWTXTADDR_U1..U4`，同版本 reloctype.go 新增）按这个计数选；不设置时为 0，函数多的包在链接时报 `R_DWTXTADDR_U1 relocation overflow`（QuickJS 实测，`test/e2e/manyfuncs` 复现） |
| `LSym.Align` | cmd/internal/obj/link.go 的 `type LSym struct` | **1.27 新增**（int16）；1.23–1.26 只有 `FuncInfo.Align`。1.27 的 objfile.go 第 391 行起：内容可寻址、非 TEXT 且 `Align == 0` 的符号会被 `Diag("… is content-addressable but alignment is not set")` 拒绝，pctab 符号必须设 `Align: 1`（同文件 funcpctab 的写法） |

## 7. 对 goc 的影响汇总

1. **重定位编号**（第 2 节）和**文件头版本号**（第 6 节）决定了 gocobj2goobj 必须用目标 Go 版本自己的 `cmd/internal/obj` 构建，任何一份固定的 vendored 拷贝都不行。
2. FUNCDATA/PCDATA、栈常量、g 偏移、g 寄存器、ABIInternal 寄存器在 1.23–1.27 中**完全相同**，所以 goc-llc 生成的机器码和 `.gocobj` 可以与 Go 版本无关（一个 ABI profile 覆盖五个版本）。
3. writer 一侧的 API 差异有三处：`LSym.AddRel`（1.24+）、`Link.DwTextCount`（1.25+，必须设成函数个数），以及 `LSym.Align`（1.27+，内容可寻址的数据必须设置）。
4. toolexec 不能再假设 `$GOTOOLDIR/pack` 存在（1.25+）。
