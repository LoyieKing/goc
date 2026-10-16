# 多 Go 版本支持的设计

目标：goc 后端尽量与 Go 版本无关；目前支持 go1.23–go1.27 的 amd64 和 arm64；以后新出一个 Go 版本时，只要它的 ABI 事实没变，就不用改任何代码。差异的事实依据见 [go-version-diffs.md](go-version-diffs.md)。

## 1. 哪些与版本有关，哪些无关

| 部件 | 与 Go 版本的关系 | 依据 |
|---|---|---|
| 打过补丁的 clang、IR pass、goc-llc | 无关 | 只依赖 ABI profile（栈常量、g 偏移、寄存器），而这些在 1.23–1.27 中完全相同 |
| `.gocobj` | 无关 | header 里记录的是 profile 名（`goabi1-amd64`），而不是 Go 版本 |
| gocobj2goobj | **必须与链接用的 Go 完全同版本** | 重定位编号会移位（1.25 起 arm64 的编号整体减 1），文件头逐字节比较版本号 |
| toolexec 钩子 | 无关 | asm/compile/pack 的参数形式没变 |

所以，唯一与版本相关的部分被收敛到 gocobj2goobj 的**构建过程**里，源码本身不含任何版本表。

## 2. goc-goenc：从 GOROOT 生成 writer

`tools/goc-goenc` 的工作：

- 用目标 GOROOT 自己的 `go list -deps` 计算 `cmd/internal/obj/x86` 在 `cmd/internal/*`、`internal/*` 范围内的闭包。
- 把这些包拷到一个独立模块 `github.com/LoyieKing/goc/goobj/enc`，路径映射为 `cmd/objlib/*`、`stdinternal/*`。
- 用 go/ast 只改写 import，字符串常量原样保留。

它额外加入三个文件：

| 文件 | 作用 |
|---|---|
| `stdinternal/abi/funcpc_stub.go` | `FuncPCABI0` 在 GOROOT 外没有编译器内建实现，用桩函数代替 |
| `cmd/objlib/obj/goc_inl.go` | 提供 `InlTree.SetParentPC` |
| `cmd/objlib/obj/goc_compat.go` | 提供 `LSym.GocAddRel`：源码里有 `AddRel`（1.24+）就调用它，否则 `s.R = append(...)`（1.23）；还有 `LSym.GocSetAlign`：只有 `LSym` 有 `Align` 字段时（1.27+）才设置，否则什么都不做 |

生成过程中它还会从该 GOROOT 的源码**探测 ABI 事实**，并选出与之匹配的 profile，写进 `cmd/objlib/objabi/goc_profile.go`。探测的事实包括：

- `internal/abi/stack.go` 中的 `StackSmall`、`StackBig`；
- `runtime/runtime2.go` 中 `g` 和 `stack` 的字段顺序；
- `ssa/_gen/*Ops.go` 中的 g 寄存器、`ParamIntRegNames`、`ParamFloatRegNames`；
- `runtime/asm_*.s` 中的 `morestack_noctxt`。

如果没有任何 profile 匹配，生成直接失败，并提示需要新增一个 profile。这就是“新 Go 版本是否需要改代码”的自动判定。

由此带来的结果：

- 重定位、FUNCDATA、PCDATA 都按**名字**引用（`objabi.R_ARM64_PCREL`、`abi.FUNCDATA_LocalsPointerMaps`），数字取自该版本的源码；
- 文件头里的版本号来自拷贝过来的 `buildcfg/zbootstrap.go`；
- 原来的 `gover_go1_24.go` 删除，`gover.go` 改为与版本无关的 `relocmap.go`。

仓库里保留一份由 go1.24.4 生成的 `goobj/enc`。它让 `go vet`、`go test ./tools/...` 不需要生成步骤就能运行；CI 在 1.24.4 上会重新生成一遍，检查与仓库里的拷贝一致。

## 3. 每个 Go 版本对应一个转换器

`tools/goc/goenc.sh` 提供 `goc_converter`，供 realbody.sh、repro 测试和发布包使用：

1. 用 `go env GOVERSION GOROOT` 确定当前 Go。缓存键是完整版本号，加上 goc-goenc、gocobj2goobj 源码的哈希。
2. 缓存目录 `${GOC_CACHE:-$ROOT/build/goenc}/<key>/` 下放：
   - 生成的 writer `enc/`；
   - 一份 `go.mod`：根模块的 go.mod，把 replace 指向 `enc/`，go 指令改为该版本；
   - 构建出的 `gocobj2goobj`。
3. 构建命令：`GOTOOLCHAIN=local go build -modfile=<缓存>/go.mod ./tools/gocobj2goobj`，因此用的就是链接时的那个 Go。
4. 发布包里预编译的 `bin/gocobj2goobj` 只有在 `-version` 输出与当前 Go 完全一致时才会使用，否则按上面的步骤现场构建。构建只需要 Go，而用户本来就有 Go。

## 4. profile 只在一处选定

| 步骤 | 做什么 |
|---|---|
| 转换器 | `gocobj2goobj -print-profile -goarch <arch>` 输出这个 Go 对应的 profile（来自 goc_profile.go） |
| realbody.sh | 用上面的输出给 goc-llc 传 `-goc-profile=<profile>`，amd64 和 arm64 都传；不再写死 `go1.24-arm64` |
| goc-llc | 把 profile 写进 `.gocobj` header |
| 转换器 | 检查 header 的 profile 与自己的 profile 是否**事实相同**（名字可以不同），不同就拒绝 |
| realbody.sh | 环境变量 `GOC_PROFILE` 可以覆盖 profile（只用于测试）；cmd/goc 原样继承环境 |

profile 命名：

- 新的规范名是 `goabi1-amd64`、`goabi1-arm64`，覆盖 1.23–1.27；
- 原来的 `go1.24-amd64`、`go1.24-arm64` 作为同事实的别名保留，旧的 `.gocobj` 和 golden 文件仍然可以读；
- goc-llc 的默认值改为 `goabi1-*`。

## 5. 模块与工具链

- 根 go.mod 的 go 指令降到 `go 1.23`（支持范围的下限），`goobj/enc` 是 replace 进来的子模块；`test/e2e/*/go.mod` 也降到 1.23。
- 所有构建都用 `GOTOOLCHAIN=local`，避免 go 命令自己去下载工具链。
- toolexec 打包时，`$GOTOOLDIR/pack` 存在就用它，否则用 `go tool pack`（1.25+ 的发行包不再预装 pack）。
- 发行包在 `src/` 下附带 gocobj2goobj、goc-goenc、gocobj 的源码和 go.mod，供 `goenc.sh` 现场构建。

## 6. CI

- 矩阵：go 1.23.x、1.24.4、1.25.x、1.26.x、1.27.x，全部是必过项（去掉 continue-on-error）。
- toolchain job 与 Go 版本无关，缓存保持不变。
- 每个矩阵 job 都运行 `scripts/ci-test.sh`，其中包括 amd64 e2e 测试、arm64 qemu 测试和 retptr 测试。
- 只有 1.24.4 的 job 额外检查 `goobj/enc` 能重新生成出一致的内容。

## 7. 以后升级 Go 的流程

1. CI 矩阵加上新版本。
2. 如果 goc-goenc 的 ABI 探测失败，说明 ABI 事实变了：在 `AbiProfiles.def` 和 `gocobj/abi.go` 中新增 `goabi2-*`，在 goc-llc 中实现对应差异。
3. 如果 writer 的 API 变了（编译失败），在 `goc_compat.go` 的生成逻辑里增加一个分支。
4. 其余什么都不用做。
