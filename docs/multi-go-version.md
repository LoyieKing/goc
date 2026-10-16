# 多 Go 版本支持的设计

目标：goc 后端尽量与 Go 版本无关。目前支持 go1.23–go1.27 的 amd64 和 arm64。新出一个 Go 版本时，一般只需运行一次事实解析器、提交生成的 JSON。差异的事实依据见 [go-version-diffs.md](go-version-diffs.md)，JSON 的字段定义见 [go-metadata-schema.md](go-metadata-schema.md)。

## 1. 哪些与版本有关，哪些无关

| 部件 | 与 Go 版本的关系 | 依据 |
|---|---|---|
| 打过补丁的 clang、IR pass、goc-llc | 无关 | 只依赖 ABI profile（栈常量、g 偏移、寄存器），而这些在 1.23–1.27 中完全相同 |
| `.gocobj` | 无关 | header 里记录的是 profile 名（`goabi1-amd64`），而不是 Go 版本 |
| gocobj2goobj | **一个二进制覆盖所有版本**，按目标版本的事实文件写 goobj | 重定位编号会移位（1.25 起 arm64 的部分编号减 1），符号类型编号、内容哈希、DWARF、对齐规则也有差异；文件头逐字节比较版本号 |
| toolexec 钩子 | 无关 | asm/compile/pack 的参数形式没变 |

与版本有关的一切都收敛在 `gometa/data/go1.X.json` 里。这些 JSON 由工具从 Go 源码生成，gocobj2goobj 的源码里没有版本分支表，也不含从 GOROOT 拷贝来的代码。

## 2. golangc-metadata-resolver：从 GOROOT 读出事实

`tools/golangc-metadata-resolver` 用 go/parser 读目标 GOROOT 的 `src`（默认 `go env GOROOT`），输出一个 JSON：

```sh
GOTOOLCHAIN=local go run ./tools/golangc-metadata-resolver -o gometa/data/go1.27.json
```

它读出的内容：

- goobj 格式：magic、19 个块的顺序、对象/符号 flag、aux 类型、特殊包下标、哈希长度、builtin 表（`cmd/internal/goobj`）；
- `objabi` 的重定位名 → 编号、符号类型名 → 编号，用一个支持 iota 的常量求值器计算；
- FUNCDATA/PCDATA 下标、FuncID/FuncFlag（`internal/abi/symtab.go`）；
- 栈常量、g 字段偏移、g 寄存器、整数/浮点参数寄存器、morestack 入口；
- writer 行为：内容哈希算法、汇编符号是否带 Linkname 标志、对齐规则、是否裁剪 inline 哈希、Flushplist 是否加 args_stackmap/arginfo；
- DWARF：函数 DIE 的 abbrev 和属性布局（v1 为 addr/addr，v2 为 dwarf5 下的 addrx+udata），以及 `FuncCountToDwTxtAddrFlavor` 的阈值表；
- 每组事实读的是哪些源文件（`sources`）。

**审查过的函数指纹**：goobjw 复刻了 Go writer 中的 39 个函数的行为。`reviewed.go` 记录了每个函数在每个已核对版本上的指纹，即去掉注释后 gofmt 输出的 sha256 前 16 位。如果遇到没见过的指纹，解析器直接报错，要求先读懂 Go 的改动、必要时修改 goobjw，再把指纹加进表里。这样，Go 某个版本的 writer 行为一变，就不会悄悄写出错误的目标文件。`-fingerprints` 选项会打印当前 GOROOT 的指纹。

## 3. goobjw：自己的 goobj writer

`goobjw` 是按 goobj 格式自行实现的 writer 和 reader，没有引用 `cmd/internal/obj`，也没有拷贝 GOROOT 代码。它复刻了汇编器路径（`go tool asm`）写目标文件时的行为，因为 goc 的产物在链接器看来就是汇编对象。复刻的行为包括：

- 非 static 符号都放进 non-pkg 块；
- ABI0 函数加 `.args_stackmap` 和 `.arginfo0` funcdata 引用；
- pcinline 为空表，UsedFiles 为 {0}；
- 生成大小为 0 的 DWARF info 和 lines 片段；
- 内容可寻址的 pcdata 符号按内容哈希命名；
- builtin 下标查找；
- FuncInfo 编码。

每一项随版本变化的地方都由 JSON 中的字段决定，不按版本号写分支。

gocobj2goobj 选择事实文件的方式：

| 方式 | 说明 |
|---|---|
| 默认 | `go env GOVERSION`，取其 minor 版本（如 go1.25.14 → go1.25），从内嵌的 `gometa/data` 中选 |
| `-goversion go1.X.Y` | 指定版本 |
| `-go-meta file.json` | 用解析器生成的外部 JSON，内嵌表里没有的新版本可以先这样用 |
| 未知版本 | 报错，并给出运行解析器的命令 |

文件头一行（`go object linux amd64 go1.X.Y … X:…`）不靠推算：转换器让同一个 Go 的 `go tool asm` 汇编一个空的 .s 文件，读出它写的头，按 `go env` 的输出缓存在 `os.UserCacheDir()/goc/objheader`（Linux 上即 `$XDG_CACHE_HOME` 或 `~/.cache`）。`-header` 可以直接指定这一行。

**正确性核对**：以旧实现（用各版本 `cmd/internal/obj` 构建的转换器）的输出为准，在 41 个真实 .gocobj 上逐字节比较：

- 41 个对象（33 个 amd64、8 个 arm64）× 5 个版本，共 205 次全部一致；
- 加上包路径 `example.com/a.b/qjs.v2`、`GOEXPERIMENT=nodwarf5`、`GOC_LIBCALL_IMPL` 三种变体的 615 次也全部一致。

其中 4 个对象和它们 5 个版本的输出放在 `tools/gocobj2goobj/testdata/golden`，由 `TestGolden` 持续检查。

## 4. 转换器的获取

`tools/goc/converter.sh` 提供 `goc_converter`，供 realbody.sh 和 repro 测试使用：

- 发布包：直接用 `bin/gocobj2goobj`，一个二进制服务所有支持的 Go 版本；
- 源码 checkout：按转换器源码（gocobj、gometa 及其数据、goobjw、tools/gocobj2goobj）的哈希构建一次，缓存在 `${GOC_CACHE:-build/conv}/<hash>/`。

## 5. profile 只在一处选定

| 步骤 | 做什么 |
|---|---|
| 转换器 | `gocobj2goobj -print-profile -goarch <arch>` 用该版本的 ABI 事实（`abi`、`arch`）匹配 `gocobj` 中的 profile，优先用 `goabi*` 名 |
| realbody.sh | 用上面的输出给 goc-llc 传 `-goc-profile=<profile>` |
| goc-llc | 把 profile 写进 `.gocobj` header |
| 转换器 | 检查 header 的 profile 与目标版本的 profile 是否**事实相同**（名字可以不同），不同就拒绝 |
| realbody.sh | 环境变量 `GOC_PROFILE` 可以覆盖 profile（只用于测试） |

profile 命名：

- 规范名是 `goabi1-amd64`、`goabi1-arm64`，覆盖 1.23–1.27；
- `go1.24-*` 是同一组事实的旧名，保留下来，旧的 `.gocobj` 仍可读取。

## 6. 模块与工具链

- 根 go.mod 的 go 指令是 `go 1.23`（支持范围的下限），不再有子模块和 replace。
- 所有构建都用 `GOTOOLCHAIN=local`。
- toolexec 打包时，`$GOTOOLDIR/pack` 存在就用它，否则用 `go tool pack`（1.25+ 的发行包不再预装 pack；事实文件的 `toolexec.pack_prebuilt` 记录了这一点）。

## 7. CI

- 矩阵：go 1.23.x、1.24.4、1.25.x、1.26.x、1.27.x，全部必过。
- 每个矩阵 job 都运行 `scripts/ci-test.sh`，包括 amd64 e2e、arm64 qemu 和 retptr 测试。
- 每个矩阵 job 还会检查解析器从该 GOROOT 读出的 JSON 与 `gometa/data` 中的一致。比较时忽略 `source_version`，因为同一 minor 的新补丁版本只应改变这一项。

## 8. 以后升级 Go 的流程

1. 用新版本运行解析器：`GOTOOLCHAIN=local go run ./tools/golangc-metadata-resolver -o gometa/data/go1.N.json`。
2. 如果解析器报告指纹未知：阅读对应函数的 Go 改动，必要时修改 goobjw，并在 schema 中加入新的事实字段。然后用 gocobj-diff 和链接测试核对，再把指纹加入 `reviewed.go`。
3. 如果 profile 匹配失败，说明 ABI 事实变了：在 `AbiProfiles.def` 和 `gocobj/abi.go` 中新增 `goabi2-*`，并在 goc-llc 中实现对应差异。
4. CI 矩阵加上新版本。
