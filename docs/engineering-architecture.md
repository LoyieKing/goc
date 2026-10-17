# goc 工程架构（开发者从零读起）

> 基于 `main` 的 efe840d。文中所有路径、pass 名、flag、环境变量都对得上这个提交里的代码；
> 代码片段（IR / 汇编 / `goc-objdump` 输出）是用这个提交实际跑出来的，不是手写的。
> 入口是命令行 `goc`（`cmd/goc`），重点讲 LLVM 这一段。

读者画像：会 C、会一点 Go，但不熟 LLVM 后端术语。所以文档从下往上讲，先讲「为什么要这样」，再讲「代码在哪里」，每一层都配真实的输出。

## 一句话

goc 把带「指针颜色」注解的 C 编成 **Go 目标文件（goobj）**，让 C 函数直接跑在 goroutine 栈上：
有 Go 一样的栈检查（`runtime.morestack_noctxt`），Go 搬栈时能修正 C 帧里的栈指针（stackmap），
Go 链接器把它当普通 Go 函数链接。不走 cgo，不切栈。

## 目录（分篇）

| 篇 | 内容 |
|---|---|
| [01 总览与目录地图](engineering/01-overview.md) | 要解决的问题、术语表、目录职责、全流程 mermaid 图、`goc` 子命令 |
| [02 走一遍：从 add.c 到二进制](engineering/02-walkthrough.md) | 用 `examples/hello` 和一个带调用的 `call.c`，逐阶段给出真实产物 |
| [03 前端与 IR pass](engineering/03-ir-passes.md) | Clang 补丁与 Sema、`libGocPasses.so` 里每个 pass 的输入 / 输出 / 不变量 |
| [04 goc-llc 与机器 pass](engineering/04-goc-llc.md) | 为什么要自己的 llc、栈检查（共享 + X86/AArch64 钩子）、帧地址修复、stackmap 落位、`.goc.*` 表输出、arm64 细节 |
| [05 .gocobj、转换与 Go 链接](engineering/05-gocobj-and-link.md) | `.gocobj` 格式摘要、`gocobj2goobj`（按 Go 版本构建）、`go build -toolexec`、所有 Go 版本耦合点 |
| [06 运行时模型](engineering/06-runtime.md) | morestack、stackmap 怎么被 Go 用、uptr、`gptr` 与写屏障现状、alloca 池 |
| [07 测试、扩展指南、已知缺口](engineering/07-tests-howto-gaps.md) | 测试布局与运行、加新 Go 版本 / 新 pass / 新架构、已知缺口与文档不一致 |

## 最短路径

```bash
./scripts/build-clang.sh        # 打补丁的 Clang 19.1.7 → third_party/llvm-19.1.7-clang-build（30–60 分钟）
./scripts/build-passes.sh       # build/llvm/libGocPasses.so 和 build/llvm/goc-llc
./cmd/goc check                 # 编 examples/hello，要求输出 "hello 42"
./scripts/ci-test.sh            # CI 跑的全套
```

## 和已有文档的关系

- 根目录 `ARCHITECTURE.md` 是一页纸版本；本组文档是它的展开版。
- 格式权威：`docs/gocobj-format.md`（`.gocobj`）、`docs/go-abi-profiles.md`（ABI 常量）、`docs/stackmap-format.md`。
- 语言合同：`docs/syntax-guide.md`（指针颜色语义）。
- 运行时：`docs/runtime-model.md`、`docs/design/uptr.md`。
- 待办：`docs/todo.md`。
