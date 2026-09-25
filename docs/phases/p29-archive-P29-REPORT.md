# P29 归档报告 — ABIInternal thunks · 多函数 goobj · quickjs-ng 着色与 goc build

**日期：** 2026-09-22（Asia/Shanghai）
**合同：** `goc-syntax-guide.md` v0.2.1（无 dsptr；sptr 逃逸 = 硬错误；`uptr` 承载堆内栈引用）

## 追加实测（2026-09-24；下文 2026-09-23 记录是历史快照）

- **2026-09-25：带调用的函数可以内联。** 先前 `goc-inline-gate` 把含调用的函数
  标成 `noinline`，因为打开后 `Promise.resolve(1).then(x => x + 6)` 在
  `js_free_function_def` 里把空闲链表 atom 当变量名释放。根因不是混合槽：
  `next_token` 把栈上的 `JSParseState *` 留在 `RBX`/`R14`，`free_token` 的
  SysV morestack 桩只调整参数寄存器和 `BP`，不改被调用者保存寄存器（那些槽
  也装着小整数，标成指针会触发 `bad pointer in frame`）。`goc-reanchor` 把
  指向调用者栈的指针参数在 safepoint 后从已调整槽重载。另一处漏标：
  `js_parse_postfix_expr` 把 `&s->token` 存在 `-0x40(%rbp)`，槽地址被调整了，
  内容没有；栈复制后循环读到旧 token（`-128`），真实 token 已是 `)`。
  reanchor 现在给这类 store 打 `goc.frame.words`。只重编 `quickjs.c` 后
  `QJS_EVAL=1 QJS_PROMISE=1`：`qjs-eval=7`、`qjs-promise-eval=7`、
  `qjs-promise-hook=1`、`qjs-freeruntime` 均 PASS。libregexp/libunicode/dtoa
  仍是上一份对象。

- **`-O3` 八套件已完整跑完一次。**
  SROA 把 `JSValue` 的 i64 载荷拆成 32 位半字（`lshr`/`trunc`），这些半字跨
  `string_buffer_free` 等 safepoint 活着。llc `-O0` 把高半字溢出到可复用槽；
  该槽后来持有 `g`（`0xc000002380`）。接合 `(high<<32)|low` 得到
  `0x2380006xxxxx`，`JS_FreeValueRT` 在 `js_regexp_Symbol_replace` 里解引用崩溃。
  `goc-pin-i64`（`backend/pass/GocStackMap.cpp`，跟在 `goc-reanchor` 后）把这些
  整数存进专用、非指针根的 alloca，并在 safepoint 之后的使用点 volatile 重载。
  栈复制按字节搬运该字，不把它当栈指针调整。四 TU 已用该 pass 重编。
  `build/qjs/qjscli --stack-size 16384 /tmp/regexp-only.js` 打出 `regexp-done`，
  退出码 0。那次 CLI 仍对每次 JS 调用 `mmap`/`munmap`，V8-v7 一次运行
  SCORE 70.2，墙钟 333.1 s。`goc_dynalloc` 改走 `goc_malloc`/`goc_free` 后，
  同一套件一次运行：Richards 189、DeltaBlue 178、Crypto 265、RayTrace 450、
  EarleyBoyer 585、RegExp 110、Splay 1215、NavierStokes 584，SCORE 341，
  墙钟 89.5 s，退出码 0。这是一次运行，不是其它行所用的两次中位数。
- `scripts/qjs-cli-build.sh` 构建四 TU 的 Go CLI；`scripts/qjs-cli-tests.sh`
  按上游 `tests.conf` 选出 116 项（另有 9 项上游排除）：**115 PASS、0 FAIL、1 UNSUPPORTED**。
  `test_std.js` 的 FILE/进程/定时器宿主 API 和 `test_worker.js` 的跨 runtime
  SharedArrayBuffer 消息已通过；剩余 2 GiB `bug1468.js` 内存压力未运行。
  运行器在有不支持项时退出非零，不能把未跑的项目计作通过。
- `qjs:std` 的 `gc`/`evalScript`/FILE、`qjs:os` 的时钟/平台/文件系统/进程/
  定时器/单 goroutine 协作调度 worker、`qjs:bjson` 与
  JSON/text/bytes 导入属性均已在上游 JS 用例中运行。`test_builtin.js`
  覆盖 331072 层 Proxy、Float16、日期时区和正则回溯。
- 常规 shim 已从不释放的 bump 改为合并空闲块、内存不足时按需 mmap 新 slab；
  数学/JSON 数值/本地时区交给 Go 标准库。正则保存捕获值的回滚路径必须解码
  `uptr` 后再写回堆槽，计数器不得伪装成指针；这两处源级适配与 QJS 移动栈
  检查一起由 `scripts/qjs-gstack.patch` 可复现地施于忽略的上游源码树。

## 现状更新（2026-09-23；以下旧分期记录保留为历史快照）

- **原版 QuickJS 源码，无私有链表改写。** `JSRuntime.parent_promise`、`current_stack_frame`
  的堆中 `T *` 存储由 `GocColorEscape` 自动升为 `uptr` 编码，读取时解码；
  不让 `sptr` 原始绝对地址进入堆。Sema 源码已有此规则，但先前所用 Clang 二进制
  早于源码，已增量重建并实测全局 `T *` 赋栈地址通过 Sema；不合格逃逸和
  `sptr` 返回仍为硬错误。
- 修复首字段（零偏移）GEP 被 `stripPointerCasts` 抹掉，造成编码的
  `JSStackFrame.prev_frame` 被原样读取、再次编码时触发 fatal；phi 选择
  局部/全局目的地现在分别追踪字段，不把同 struct 的无关字段也标成 `uptr`。
  `frontend/color-escape/build.sh` 的 10 个色合同用例通过。
- `JSValueLink.next` 先写入临时聚合再经 `llvm.memcpy` 复制到栈局部 `link`：
  `GocStackMap` 把准确的指针字段从源栈对象传到目标栈对象，而不是改 QuickJS
  类型或把整个 struct 当指针。`JS_PromiseThen` 的 BP 距离现在同时包括源 +264
  和目标 +224；`GOC_SPTR_MAPS=1` 时运行时复制时可修正后者。
- ABI0 C morestack 慢路径保存 SysV GPR/XMM0–7；慢路径用独立的指针寄存器 bitmap，
  只调整真正是 `ptr` 参数的槽。`test-p29-goabi.sh` 另证明第七个（栈传）
  指针参数和 DI 寄存器指针参数在栈复制后仍指向新的栈局部。
- Go pcsp 不能表达动态 `alloca` 的任意 SP 变化；色分析前把 `alloca(i8,size)`
  降成有函数调用期生命周期的 `goc_dynalloc`/`goc_dynrelease`。freestanding
  路径现在走 shim 的 `goc_malloc`/`goc_free`（仍是 cptr，不在 g 栈上）；
  先前每次调用一对 `mmap`/`munmap`。`stacksave/restore`、EH 或其它未建模的动态分配
  形态会硬失败，绝不发布伪 pcsp；跨动态分配帧的 C `longjmp` 尚不支持。
  常规 QJS libc shim 仍是实验性 bump allocator。
- 端到端：`GOC_SPTR_MAPS=1 GOC_CRESERVE=8192 QJS_EVAL=1 QJS_PROMISE=1
  ./scripts/qjs-build.sh` 产出四 TU goobj、链接 Go 程序，`JS_GetVersion`、
  `JS_NewRuntime`、链表/帧链栈复制、`JS_Eval=7`、Promise parent hook=1、
  `JS_FreeRuntime` 均 PASS。GDB `runtime.copystack` 观察到 `emit_op` 32 KiB 和
  `JS_CallInternal` 64/128 KiB 的真实 C 帧栈复制；不再依赖 256 KiB 预留来躲避它。
- 上述只证明这些路径，不证明所有 QuickJS API、并发或 GC 抢占安全；完整 real-MF
  流水线、更多 ABI byval/identified aggregate、普通 shim malloc/free 仍需工作。

## 本阶段达成（全部有实测证据）

| # | 目标 | 状态 | 证据 |
|---|------|------|------|
| 1 | 修复 `MachineRegisterInfo` use-list 双挂（P5–P16 管线在断言开启下跑通） | ✅ | `backend/build.sh` → `PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])` |
| 2 | 多函数 TU → goobj（每函数 frame 由 llc 序言推导） | ✅ | `goc build --all`：4 函数 TU → 4 TEXT（frame 16/56/16/24） |
| 3 | Go amd64 **ABIInternal** 入口 thunk（int/ptr 子集，≤6 参） | ✅ | `scripts/test-p29-goabi.sh`：Go 调用 C 全部 PASS |
| 4 | QJS 规模真实代码的数据/GOT/局部标签重定位 | ✅ | quickjs.c（1851 函数）→ 3199 TEXT + data/GOT 符号，无 FATAL |
| 5 | quickjs-ng **着色**（默认 cptr + 显式覆盖） | ✅ | color-escape 545 → **17** 错误；Sema 修全局取址误判 |
| 6 | `goc build` 全量 QJS（4 TU）→ goobj | ✅ | quickjs/libregexp/libunicode/dtoa 全部产出（`scripts/qjs-build.sh`） |
| 7 | Go 二进制链接 QJS + freestanding libc shim（无 libc） | ✅ | 2.2MB 二进制链接成功（`-lm` 仅用于链接期符号） |
| 8 | **QJS 函数在 goroutine 栈上执行** | ⚠️ 部分 | `PASS qjs-version: 0.17.0`（`JS_GetVersion` 经 goc thunk 在 Go 栈上运行） |

## 本阶段追加修复（QJS 规模真实代码的重定位正确性）

| 问题 | 根因 | 修复 |
|------|------|------|
| `FATAL data reloc … unresolved ""` | `.rela.data.rel.ro` 里静态函数指针形如 `R_X86_64_64 .text+0x3E220`（**段符号 + 加数**），旧代码用段符号 `Value`(=0) 解析并丢弃加数 | `resolveTarget` 对段符号用**重定位加数**定位；加数落在函数间对齐填充时回退到下一函数（负 delta） |
| `textRanges` 为空导致解析失败 | 由 `pend` 构建，而 `pend` 在 TEXT 发射循环才填充 | 改为直接由 meta + ELF 符号表构建（含 `LookupABI`） |
| 跨 TU C 调用符号不一致 | goabi 只重命名**定义**（`X` → `X.impl`），引用侧仍是原名 | `goc_goabi.py` 对**声明**应用同一规则（引用侧与定义侧一致） |
| 按名全局（`stdout`/`stderr`）无法解析 | 只发射段级数据符号 | 新增按名全局数据符号 + 其内部重定位归属（`nameRanges`） |
| libc 符号劫持 glibc 初始化 | 裸名（`malloc`/`printf`/`memcpy`…）被导出为动态全局符号 | shim 全部改 `goc_*` 前缀 + `-D` 重定向 + `GOC_IR_RENAMES`（编译器生成的 `llvm.memcpy` 调用） |
| C 侧符号 ABI 语义 | C 代码用 SysV 互调；Go 侧 thunk 需 ABIInternal | meta 支持**逐函数 ABI**（thunk=ABIInternal，C 侧=ABI0） |
| 只复制 ELF 元数据段 | `.init_array`/`.dynamic`/`.got*` 被当数据复制 → glibc 执行伪造 init | 仅复制程序数据段（`.rodata*`/`.data*`/`.bss*`） |

## 关键实现（代码路径）

| 路径 | 作用 |
|------|------|
| `backend/pass/GocInsertStackCheck.cpp` / `GocExpandStoreGptr.cpp` | 修复：MBB 先插入 MF 再 splice（操作数 use-list 语义） |
| `backend/goobj/elfpack/main.go` | 多函数 meta、ABI 感知重定位、数据段/按名全局符号、GOT 槽、`.text` 局部标签折叠、TU 限定、ABI0 支持 |
| `backend/realbody/goc_p28_realbody.sh` | `--all` / `--goabi` / `--abi0` / `GOC_IR_RENAMES`；frame 由序言推导 |
| `backend/realbody/goc_goabi.py` | 外部函数 → `<name>.impl` + Go-ABI 入口 thunk（Go regs → SysV regs） |
| `frontend/color-escape/pass/GocColorEscape.cpp` | `-default-ptr-color`（大批量着色）+ 声明色权威化 |
| `clang/sema/SemaGocColors.cpp` | 修复：全局/静态取址应为 `cptr`（原误判 `sptr`） |
| `backend/tools/toolexec_pack_goobj.sh` | 多 goobj 打包 + 按 meta ABI 生成 symabis |
| `tests/qjs/*`、`scripts/qjs-build.sh` | 冒烟：Go 调用 QJS；freestanding libc shim（内存/字符串/stdio 空实现/数学 trap） |

## 诚实缺口（下一步）

1. **~~Go 链接器 792B nosplit 栈预算~~（已解决，2026-09-22）**

   **真因（此前误判为"链接器忽略 NoSplit"）**：elfpack 用 `ctxt.IsAsm=true`、`frame=0` 注册 TEXT
   （帧由 LLVM 字节自己分配），真实 CALL 是**原始字节**注入的，因此 vendored `x86.preprocess`
   的叶子启发式（只扫 Prog 链表里的 `ACALL`）看不到任何调用 → 判定 leaf → 对**每个** goc TEXT
   `Set(AttrNoSplit, true)`。对象里实测 `flag=0x10`（`SymFlagNoSplit`，注意是 bit4 而非 bit0）。
   链接器行为完全正确：nosplit 走 pcsp 增长边，`js_atomics_op` 840B、`JS_CallInternal` 8888B
   必然超 792B 预算。诊断工具：手写 goobj 解析器读 `BlkNonpkgdef` 记录的 flag 字节。

   **解法（已实现）**：`GOC_MORESTACK=1` 时 elfpack 为每个非 nosplit TEXT 生成 Go 式分裂前导：

   ```
   entry:  <check>                 ; frame<=128: CMPQ SP,16(R14)
                                   ;  128<frame<=4096: LEAQ -(f-128)(SP),R11; CMPQ R11,16(R14)
                                   ;  frame>4096: MOVQ SP,R11; SUBQ $(f-128),R11; JCS stub; CMPQ ...
                                   ; JBE stub (rel32)
           <LLVM body>             ; PUSH BP + SUB $frame
           0f 0b (UD2)             ; stub 不可由 fallthrough 进入
   stub:   CALL runtime.morestack_noctxt(SB)   ; R_CALL, ABI0（同 Go 自身 stub）
           JMP  entry                   ; R_PCREL 自引用
   ```

   pcsp 变为三段：`[check)=0`（morestack 拷贝 0 字节）、`[body)=frame+8`、`[ud2+stub)=0`；
   并清除启发式误置的 `AttrNoSplit`。依赖不变式 **R14 == g**：Go 经 ABIInternal thunk 进入，
   SysV 代码按 callee-saved 保留 R14，因此检查可直接读 `16(R14)`、stub 可直接调 morestack。
   实测：dtoa 47/53、quickjs 1996 个 TEXT 转为 splittable。

   **解锁后暴露并修掉的真实缺陷**（此前链接在栈检查处即中止，全部被掩盖）：
   - GOT 槽符号只有 size 无数据 → 链接器 `invalid relocation X: 0+8 not in [0,0)`；现写满 8 字节。
   - NOBITS 段符号带重定位时同样缺字节 → 统一补零。
   - 跨 TU C 引用未加包前缀（`lre_realloc.impl`）→ 统一限定为 `main.<name>.impl`。
   - llc 把 `__builtin_trap()` 降为 `llvm.trap` 调用 → 映射到 ABI0 `runtime.abort`。
   - 后端 libcall / IR 改名后命名不一致：int/ptr 子集（memcpy/memset/…）绑定 `.impl`，
     浮点 libcall（log/sin/round/…）保持裸限定名（goabi thunk 仅覆盖 int/ptr）。
   - shim 改为 `--goabi` 构建（与 QJS TU 命名一致），补 `round`/`vsnprintf` 定义。

2. **~~运行期 fault~~（已解决，2026-09-22）**：`JS_NewRuntime`/`JS_FreeRuntime` 现在通过。

   **真因**：llc 对**本地（static）符号的同 section 调用/跳转直接写死 rel32 位移、不产生重定位**
   （`go tool objdump` 证据：`0x2253b7: e832020000 CALL 0x2255ee`，无 `R_CALL` 注解）。链接器
   会按自己的顺序摆放符号、前导又让每个函数增长，于是这类写死位移全部失准——故障表现为控制流
   跳进 `js_call_c_function+0x1a`（指令中间），`addr=0xffffffff8b481845`。

   **修法**（elfpack）：
   - 由脚本反汇编导出**每个 call 的函数内偏移**（`meta.calls[].off`），elfpack 对**没有 ELF 重定位**
     的 call 自行发出 `R_CALL`（按名字经同一套 C 符号解析规则定目标），并清零位移字段。
     （不用字节扫描：链接器可任意重排，误判一个 `0xe8` 会静默改坏无关指令。）
   - 归一化**加数**：`resolveTarget` 统一返回「相对符号起点的绝对偏移」，section 折叠型重定位不再
     被多算 +4（此前 `R_CALL ... goAdd=4` 会 FATAL），GOT 槽内容也修正为纯地址。
   - 指向 body 内部的重定位加数按前导插入量重基准化（`shiftAt`）。
   - 顺带修掉两个被掩盖的缺陷：无 TU tag 的 meta 会让 section 符号重名（`goc.data.tu..data`）；
     脚本解析 `llvm-objdump` 的正则不认 `0x` 前缀 → `calls` 恒为空（PCDATA 配对从未生效）。
   - shim 补 `putchar`/`strtod`/`scalbn`/`round`/`vsnprintf` 定义（此前这些调用是写死位移，链接器看不见）。

   **结果**：`MARK start` → `PASS qjs-version` → `PASS qjs-newruntime: 0x46c5cf0` → `PASS qjs-freeruntime`
   → `PASS p29-qjs`（3/3 稳定）。

3. **运行真实 JS（`JS_Eval`）触发的两个新缺口**（`QJS_EVAL=1` 探测，2026-09-22）：

   探测路径：`tests/qjs/tramp/`（Go 汇编 SysV trampoline）——因为 `JS_Eval` 返回 16 字节 `JSValue`
   结构体，不在 goabi 的 int/ptr thunk 子集内（ABI 覆盖缺口的直接体现）。解释器确实跑起来了
   （执行到 `JS_CreateProperty` 等），但**第一次栈增长**就暴露两件事：

   - **`FUNCDATA_LocalsPointerMaps` 必须存在且 `n > 0`**（否则 `runtime.getStackMap` 直接
     `throw("missing stackmap")`）。已实现：按函数检测 IR 里是否存在 `sptr` 着色
     （`!goc.color !N`，`!N = !{!"sptr"}`，脚本写入 `meta.functions[].has_sptr`）——
     **无 sptr 的函数**其帧对 GC 而言不含指针，发 `n=1, nbit=0` 的空图（`gclocals.gocEmpty`），
     增长可安全复制；**含 sptr 的函数**保持索引 -1，运行时若需复制其帧就**大声失败**而不是静默留下
     失效栈指针（诚实优先）。
   - **增长后按保存的 BP 链回溯仍失败**：`runtime: g 1: unexpected return pc for
     main.quickjs_c.JS_CreateProperty called from 0x400000000`。Go 的 `adjustframe` 只在
     `frame.argp-frame.varp == 2*PtrSize` 时调整保存的帧指针，需要我们的帧布局/`FuncInfo`
     与 Go unwinder 的约定完全对齐（下一步）。

   现状：核心冒烟（version/newruntime/freeruntime，不触发增长）稳定 PASS；`QJS_EVAL=1` 为
   上述缺口的可复现探测入口。

4. **栈增长路径的两个约定（本轮实测，2026-09-22）**：

   - **pcsp 必须整函数常量 = `frame+8`**，与 Go 自身一致：unwinder 用
     `frame.fp = frame.sp + funcspdelta(f, pc) + 8` 定位帧边界，再在 `fp-16` 处找"保存的帧指针"。
     我原先的"三段 pcsp（检查区=0 / body=frame+8 / stub=0）"会让 `fp` 偏到帧内垃圾处 →
     `traceback did not unwind completely`。已改回常量（检查区、stub 区也报 frame+8，与 Go 的
     `spfix.Spadj = -framesize` 同义）。
   - **命名规则收敛**：`.impl` 只出现在「IR 改名过的调用/声明」与「int/ptr 后端 libcall」两类上；
     浮点/结构体签名的 C 函数（如 `acos`、`JS_Eval`）没有 thunk，C 侧定义保持 `main.<name>`。
     另：**写死 call 改为按地址解析**（用 meta 里反汇编得到的精确偏移解码位移）——静态函数是
     TU 限定名（`main.libregexp_c.cr_regexp_canonicalize.impl`），按名字猜会错。
   - thunk（纯寄存器搬运）标记为 sptr-free，获得空 locals map；**含 sptr 的 C 帧仍只有索引 -1**，
     所以解释器一旦在 C 内触发增长就大声失败（当前 `add_property`）——这正是"真实指针图"待办。
   - 反例留档：曾尝试"在 Go 侧用大数组预留 C 栈"，实为**堆分配**（`make`），触发 GC 并破坏冒烟，
     与栈预留无关；已移除。

5. **ABI 对齐修复 + pcsp 语义定论（本轮，2026-09-22）**：

   - **thunk 必须对齐栈**：Go 的 ABIInternal **不保证 16 字节栈对齐**，而 SysV C 代码要求它。
     证据：`main.quickjs_c.unsafe_unconst` 在 `movaps %xmm0,-0x10(%rbp)` 上 #GP（`SIGSEGV addr=0x0
     code=0x80`），gdb 实测该函数入口 `rsp & 15 == 0`（应为 8）。
     修法（`goc_goabi.py`）：thunk 以**对齐后的 SP** 为锚建帧——
     `A = rsp & -16 ; [A-8] = 调用者 SP ; [A-16] = 调用者 BP (= rbp)`，调用点因此 16 字节对齐，
     而 **unwinder 几何保持常量**（pcsp = 8：`fp = sp+pcsp+8` 恒等于 A，`fp-16` 正是保存的 BP）。
     实测：`movaps` 崩溃消失，核心冒烟与全部回归通过。
   - **pcsp 语义定论**（据 runtime 源码与实测）：unwinder 用 `frame.fp = frame.sp + funcspdelta + 8`
     求帧边界、在 `fp-16` 取保存的帧指针；`morestack` 保存 `sched.sp = SP+8`（即**不含** CALL 压入的
     返回地址）。因此：body 区 pcsp = `frame`（meta 的 8 + LLVM locals），检查区/stub 区 = 0。
   - **两个被证伪的方案（留档）**：
     1. "Go 侧大数组垫栈"——实为 `make` 堆分配，触发 GC 且不产生栈帧，与栈预留无关；
     2. "thunk 内预留 C 栈"——预留使 thunk 的 pcsp 与实际几何错位，`copystack` 走栈时
        `frame.fp` 越出栈顶 → `runtime.(*unwinder).resolveInternal` SIGSEGV。已移除，只保留对齐修复。
   - 探测现状：`QJS_EVAL=1` 已跑进 QJS 的**数字格式化**（`clz_uintptr` 等），随后在**含 sptr 的帧**
     上触发增长 → 运行时拒绝（`missing stackmap`，响亮失败）。这正是"真实指针图"待办。

6. **含 sptr 帧的真实指针图：本轮调研结论与实施路线（2026-09-22）**

   现状：sptr-free 函数与 thunk 已发**空图**（`n=1,nbit=0`，增长可安全复制）；**含 sptr 的帧仍保持
   索引 -1**，需要复制时运行时**响亮失败**（`missing stackmap`）。探测已跑到 QJS 的数字格式化
   （`clz_uintptr` 等）后停在这里。

   本轮把可行性与数据来源钉死：

   - **着色 IR 的语义**（实测）：`!goc.color` 挂在**值**上——`getelementptr` 23776、`alloca` 10908、
     `load` 476、`phi` 13（quickjs TU）。注意 `alloca` 被标 sptr 表示「该局部变量的**地址**是 sptr」
     （例如 `%d.addr = alloca double` 也是 sptr），**不代表该槽位存的是指针**——所以不能用
     「所有 sptr alloca」当帧图（会把 `double`/`i32` 槽位标成指针 → GC 会跟随垃圾，**不安全**）。
   - 正确的槽位来源：**遍历 sptr 值的 `store` 用途**得到「存放 sptr 的 alloca」，
     其**帧偏移**取自 `llc -stop-after=prologepilog` 的 MIR `stack:` 对象（含 IR alloca 名与 offset）。
   - **必须同时覆盖寄存器溢出**：`-O0` 下跨调用的 sptr 临时量会被 regalloc 溢出到帧槽
     （实测 asm 里 `movq %rdi, -48(%rbp) # 8-byte Spill`），只标 alloca 槽位**不完备**。
     唯一精确来源是 LLVM 的 stackmap：`llvm.experimental.stackmap` 已在本地验证可用
     （`llc` 产出 `.llvm_stackmaps`，`llvm-readobj --stackmap` 可读，记录含 callsite 指令偏移与
     每个值的**位置**：寄存器或 Direct 栈槽）。
   - 因此实施路线（尚未落地）：① IR 侧在每个 call 前为 sptr 值发 stackmap 内建（超集安全：死值无害）；
     ② elfpack 解析 `.llvm_stackmaps` + MIR 的 alloca 偏移 → 生成 Go 的
     `FUNCDATA_LocalsPointerMaps`（`nbit` = 帧字数，位序按 `varp - i*8`）与按 callsite 的
     `PCDATA_StackMapIndex` 转换；③ 以 `QJS_EVAL=1` 的 JS 结果（`1+2*3` 应得 7）做端到端正确性验证。
   - 另记：曾试图「在 thunk 内预留 C 栈以避免 C 内增长」，因**对齐余量使 thunk 帧长可变**
     （pcsp 无法精确表达）而放弃；「C 代码跑在自有非移动栈」是另一条**架构级**备选（需改 sptr 契约）。

7. **sptr 帧图 Stage A 已落地（gated `GOC_SPTR_MAPS=1`，2026-09-22）**

   实现（脚本 + elfpack）：
   - 脚本从着色 IR 取出 **sptr 值**（`!goc.color` 挂在*值*上），沿其 `store` 用途找到「存放 sptr 的
     alloca」，再用 `llc -stop-after=prologepilog` 的 MIR `stack:` 对象取**帧偏移**（实测 MIR 的
     offset 就是 rbp 相对：`retval` offset -32 ↔ asm `MOVQ DI, -0x20(BP)`），写入 meta
     `functions[].sptr_slots`（正值 = varp 距离；varp == rbp）。
   - elfpack 为这些函数发**真实** `FUNCDATA_LocalsPointerMaps`：`n=1`、`nbit` = 帧字数
     （`(frame-8)/8`）、位 `i` 覆盖 `varp-(i+1)*8`（即 `bit = slot/8 - 1`），符号名
     `gclocals.gocSptr.<goSym>`；超出帧范围的槽位只打 note 并跳过（LLVM 会列出随后丢弃的对象）。
   - 实测：quickjs TU **299/1996** 函数拿到真实槽位图；默认关闭时行为与之前一致。

   **仍未完成（Stage B，Stage A 单独不完备）**：`-O0` 下跨调用的 sptr *临时量*会被 regalloc 溢出到
   帧槽（asm 证据：`movq %rdi, -48(%rbp) # 8-byte Spill`），这些槽位 Stage A 看不到。
   典型：`clz_uintptr` 的 sptr 只是「alloca 地址」（`%a.addr = alloca i64, !goc.color !6`，
   槽里其实是不带指针的 i64），因此它 `sptr_slots=[]` 而仍保留索引 -1 → 增长时响亮失败。
   计划：IR 侧按**支配关系**在每个 call 前为 sptr 值发 `llvm.experimental.stackmap`（需要一个小 IR
   pass：仓库已有 llvm-config 构建的 pass 先例），elfpack 解析 `.llvm_stackmaps` 的
   Direct 位置 → 与 Stage A 的 alloca 槽位合并成每 callsite 的图与 PCDATA 索引。

8. **sptr 帧图 Stage B 已落地（gated `GOC_SPTR_MAPS=1`，2026-09-22）**

   - 新增 IR pass `backend/pass/GocStackMap.cpp`（构建为 `GocStackMap.so`，由脚本经
     `opt -load-pass-plugin=… -passes=goc-stackmap` 运行）：在每个 call 前，对**支配该调用**的
     sptr 着色值发 `llvm.experimental.stackmap`（超集安全：死值只多一个位置）。**插件不能链接
     `-lLLVM`**，否则其静态初始化与宿主 `opt` 冲突、`dlopen` 直接崩溃。
   - elfpack 解析 `.llvm_stackmaps`（布局按 LLVM `StackMaps.cpp` 的 emit 代码核对：位置 =
     u8 type、u8 rsvd、u16 size、u16 reg、u16 rsvd、i32 offset；记录尾部按 8 字节对齐 +
     u16 pad + u16 nLiveOuts + 4·nLiveOuts + 再对齐），并经 `.rela.llvm_stackmaps` 把函数记录
     归到具体函数。
   - 每函数地图 = 着色 pass 的 alloca 槽位 ∪ stackmap 的 **Direct (RBP)** 槽位 → 生成
     `FUNCDATA_LocalsPointerMaps`（`n` 张图）与按 callsite 的 `PCDATA_StackMapIndex` 转换。
   - 实测：quickjs TU **1826/1996** 函数拿到真实地图（Stage A 时是 299）。
   - 顺带修掉一个真实链接器缺陷：funcdata 符号**必须以 `gclocals.` 开头**（obj writer 按
     `contentHashSection` 把它们放进 `go:func.*` 载体段），我一度写成 `goclocals.` →
     `panic: bad carrier sym for symbol …`。

   **当前唯一阻塞（结构性）**：**C 栈 16 字节对齐**。LLVM x86 的帧对齐硬编码 16，对 16 字节
   （`JSValue`）拷贝/溢出会发 `movaps`；而 Go 的调用方只保证 8 → `movaps` #GP
   （实测 `unsafe_unconst+0x1e`，入口 `rsp & 15 == 0`）。已尝试并**证伪**两条：
   - thunk 内 `andq $-16` 对齐：帧长随调用方对齐变化，Go 的 pcsp 是「每 PC 常量」，无法表达 →
     unwinder 取错返回地址槽 → `unexpected return pc`；
   - IR 注入 `!"stack-alignment", 8` 模块标志：LLVM 19 x86 对**帧**仍按 16 处理，`movaps` 仍在。
   结构性解法（下一步）：像 cgo 的 `cgocall` 那样让 C 代码跑在**对齐由运行时保证的专用栈**上，
   或在 Go→C 边界保证静态可证的 16 字节对齐。

9. **本轮（2026-09-22）Stage B 精化与对齐修复**

   - **`override-stack-alignment`**：Go 只保证 8 字节栈对齐，SysV 代码假定 16 并发对齐 SSE
     （`movaps` → #GP）。clang 的开关名是 **`override-stack-alignment`**（我先前误写
     `stack-alignment`，被忽略）；注入 IR 模块标志 `!{i32 1, !"override-stack-alignment", i32 8}`
     后探测越过了原先的 `unsafe_unconst` 故障点（帧几何不变，pcsp 仍精确）。
   - pass 精化：① 只记录**指针类型**的 sptr 值；② **活跃性过滤**——仅当该调用**支配**该值的某个
     使用点时才记录（支配 ⇒ 该值在调用处必活）；③ **零初始化指针 alloca**——函数级地图会标出尚未
     赋值的槽位，而运行时会校验每个被标槽位（`adjustpointers`：`0 < p < minLegalPointer` →
     `bad pointer in frame`），0 被跳过，故零初始化消除该类假阳性。
   - **运行时校验是安全网**：地图假阳性会**大声**中止（`invalid pointer found on stack`），
     不会静默损坏 —— 本轮实测到 `main.JS_NewCFunction3` 的 `rbp-96` 槽位在该调用点持有 `0x8`。
   - **剩余不精确点（已定位）**：地图的 **alloca 部分是函数级**（非按调用点），因此某些调用点上
     持有非指针的槽位仍被标出。修法：把 alloca 槽位也做成**按调用点**——要求「该调用点前最近的
     支配性 store 是指针类型」；同一 pass 内即可实现（已有 DominatorTree）。

10. **C 栈预留（thunk 内）与解释器突破（2026-09-22）**

   在「帧几何恒定」的前提下重做 thunk 内预留（`GOC_CRESERVE`，qjs 构建默认 256 KiB）：
   `push rbp; mov rsp,rbp; sub $RESERVE,rsp; <shuffle>; call X.impl; mov rbp,rsp; pop rbp; ret`
   —— 帧是**常量** `8+RESERVE`，因此 elfpack 能照常发分裂检查（实测 thunk 序言含
   `SUBQ $0x3ff88,R11; JB stub; CMPQ R11,16(R14); JBE stub`）与精确 pcsp。

   效果：**Go→C 边界的增长只发生在 Go 帧在场时**，放大后的栈覆盖整棵 C 调用树 → C 帧不再触发
   morestack、**不被复制** —— 这对含 union（`JSValue` 的 `ptr` 视图可能读到整数）的 C 帧是必需的，
   因为这类帧的精确指针图本质上不可判定（这也是上一轮 `bad pointer in frame … 0x8` 的来源）。

   实测（`QJS_EVAL=1` 探测）：
   - 核心冒烟仍稳定 PASS（2/2），全部回归绿；
   - 解释器现在跑进 **QJS 对象系统深处**：
     `JS_InstantiateFunctionListItem → JS_DefinePropertyValue → JS_DefinePropertyValueConst →
      JS_DefineProperty → JS_CreateProperty → js_string_define_own_property`，
     停在后者 `+0x8` 的一次 **nil 解引用**（`panicmem`/`sigpanic`）。
   - 该 panic 触发的栈回溯仍报 `traceback did not unwind completely`（回溯路径的几何问题，次要）。

   下一步：定位 `js_string_define_own_property+0x8` 的 nil 来源（可能是 shim 返回 NULL 的路径、
   数据重定位或某个尚未覆盖的 ABI 形态）。

11. **R14 不变式被证伪 → 检查改为从 TLS 取 g（2026-09-22）**

   实测（gdb）：`js_string_define_own_property` 入口处 **R14 = 0**，返回地址指向
   `JS_CreateProperty+944` —— 即 **`JS_CreateProperty`（LLVM 编译的 C 体）把 R14 当普通寄存器用了**
   （SysV 里 R14 就是 callee-saved GPR，LLVM 完全有自由这么做；对象里该函数有 5 处 R14 引用）。
   于是「R14 == g」的不变式**在 C 体内部不成立**，任何被调函数的栈检查读 `0x10(%r14)` 都会崩。

   修法：`gocSplitCheck` 不再依赖 R14，改为
   `MOVQ FS:-8, R11`（Go TLS → g）+ `CMPQ SP/R10, 16(R11)` + `JBE stub`
   —— 依据是 `runtime.morestack_noctxt.abi0` **自己**从 TLS 取 g
   （反汇编：`mov %fs:0xfffffffffffffff8,%rdi`），所以存根不需要 R14。
   顺带：R11/R10 在我们的 int/ptr 子集（≤6 参）里不是参数寄存器，检查可安全使用。

   另修：**写死的 tail `jmp` 未被重定位**（meta 驱动最初只处理 `call`）——脚本的反汇编收集现在同时
   记录 `jmp` 目标，elfpack 对 0xe9 发 `R_PCREL`（对 0xe8 仍是 `R_CALL`），并跳过函数内目标。
   （注意 `go tool objdump` 会把未重定位的位移显示成"下一个符号"，读数时别被误导。）

   实测：检查不再崩；探测继续推进，当前停在
   `js_typed_array_reverse` 内部一次**指令中间的控制流转移**（gdb：栈上返回地址 `0x562e23`
   落在一条 3 字节指令内部 → 上游某处已跳错），即级联效应；下一步是回溯定位第一个错误转移。

12. **历史记录：当时的运行期阻塞（已解决）**：控制流进入了
   `main.quickjs_c.js_call_c_function` 的**指令中间**（gdb：`rip=0x49651c`，而该函数入口是
   `0x496500`，`0x496516` 处是 7 字节的 `sub $0x230,%rsp`，0x49651c 是它的最后一字节；
   运行时报告 `addr=0xffffffff8b481845`、`code=0x1`(SEGV_MAPERR)；`bt` 的返回地址为 0）。
   即某个**间接跳转/跳转表**给出了 `entry+0x1c`。已排除：该函数没有直接 `call` 站点，
   二进制数据段中也没有 `0x49651c` 字面量（只有正确的 `0x496500`，位于 0x221948）。
   下一步：给 QJS TU 带 `-g` 行号信息重编，定位发出该跳转的 C 语句，并核对对应
   `.rodata` 跳转表条目（PIC 下应为相对表基址的偏移/PC 相对重定位，而不是绝对地址）。

   前导分支位移已修正（`gocSplitCheck` 原先假设 stub 紧跟检查之后，导致 JBE 位移算成 0、
   检查形同虚设；现在返回待回填偏移，由 `patchSplitCheck` 在知道 stub 位置后写入）。
   反汇编核对：`jbe 496d52` → stub `call runtime.morestack_noctxt.abi0; jmp 496500`。

3. **真实 MF 指针 maps**：`stackmap_index=-1`（无 Locals/Args 指针图）→ 触发 GC 的路径尚不安全。
4. **ABI 覆盖**：浮点/结构体参数、>6 参、多返回值仍为 SysV-only（903/1851 函数）。
5. **着色剩余 17 处**：QJS 把栈对象地址存入堆/逃逸位置（JSStackFrame 链、promise/generator 父链、
   临时缓冲）——正是合同要求改用 `uptr` 编码（`goc_uptr_from_sptr`/`as_sptr`）的站点，构成精确移植清单。

## 复现

```bash
export GOC_CLANG=$GOC_ROOT/third_party/llvm-19.1.7-clang-build/bin/clang
./scripts/test-p29-goabi.sh     # Go ABIInternal thunks（Go 调用 C）
./scripts/qjs-build.sh          # QJS 着色 → goobj → Go 二进制 → goroutine 栈冒烟
```

## 2026-09-24：O3 栈复制与 QJS 冒烟

`GOC_OPT_LEVEL=3 ./scripts/test-p29-goabi.sh` 全过，包括三次真实栈复制：
`goabi-sptr-after-stack-copy`、`goabi-stack-passed-pointer-after-copy`、
`goabi-register-pointer-after-copy` 都是 3。`-O0` 同样全过。

修的是三件分开的事，不是一张函数级位图：

1. **跨 safepoint 的帧地址不能留在被调用者保存寄存器里。** `-O3` 把调用前的
   `leaq -12(%rbp), %rbx` 活过 `morestack`。复制只改 BP 和槽，不改 `rbx`，
   比较看到的是旧地址（`got 2`）。`goc-reanchor` 对这种使用一律 volatile
   重载。例外：纯帧地址作为 inline asm 的内存操作数时必须留成 frame index，
   否则 `strtod` 那种 clobber 全部 GPR 的 asm 会 `requires more registers`。
2. **专用槽的函数级位图下标是 `words - off/8`，不是 `off/8`。** 后者标到帧底，
   把不该动的字加上 delta。位图只在 `-no-stack-slot-sharing` 下安全。
3. **pcsp 假设函数体内 SP 恒定。** x86 call-frame opt 在调用前 `push` 参数，
   展开时把 `g`（`0xc000002380`）读成 `JS_EvalInternal` 的返回 PC
   （`unexpected return pc`）。llc 加 `-no-x86-call-frame-opt` 后这个崩溃消失。

另外两处不能再当致命错误：llc 删掉的、IR 里没有 load 的 tagged alloca
（`js_free_value_rt` 的 `p41`）没有 PEI 槽；stackmap 算出的 `off < 8`
是 saved BP 或帧外的动态栈传参数，不是局部槽。有 load 却没有槽、或
`off` 越过帧尾，仍然致命。

`./scripts/qjs-build.sh`（默认 `-O3`）现在能链接并跑过：

```
PASS qjs-version: 0.17.0
PASS qjs-newruntime
PASS qjs-link-copy-after-growth: 3
PASS qjs-frame-chain-after-growth (heap=0): 3
PASS qjs-frame-chain-after-growth (heap=1): 3
```

`1+2*3` 得到 7（`PASS qjs-eval: 7`）。这之前有一次空指针：`js_create_function`
读 `0xac(%rsi)`，`rsi=0`。`r13` 在 `js_new_function_def` 返回后是函数定义，
经 `js_parse_program` 回来变成 0。原因是 SysV morestack 存根在函数序言保存
被调用者保存寄存器之前就调用 `runtime.newstack`，而 Go ABI 不保留
`rbx/r12/r13/r15`。存根现在保存并恢复这四个寄存器，但不把它们标成指针
（标了会把小整数 `0x40` 当成坏指针）。

那次 `js_parse_expr_binary+0x38f` 的野地址是跳转表下标。`gogo` 恢复时把
`R14` 写成 `g`，而 SysV 函数把 `R14` 当普通被调用者保存寄存器（这里是
运算符下标）。存根在调用 `newstack` 前从 TLS 把 `g` 装进 `R14`，返回后
弹出调用方原来的 `R14`。

修完后 `./scripts/qjs-build.sh` 全过：

```
PASS qjs-eval: 7
PASS qjs-promise-eval: 7
PASS qjs-promise-hook: 1 parent promises
PASS qjs-freeruntime
PASS p29-qjs
```

## 2026-09-24：V8 bench 超过 Goja

`goc-pin-i64` 不再进默认管线。llc 已经带 `-no-stack-slot-sharing`，整数溢出
槽不会再被复用成 `g`；那 137 个 pin alloca 只是让 `JS_CallInternal` 在每次
进入时把 JSValue 半字打进内存。reanchor 在同一个无 safepoint 区间里只
volatile 重载一次。ABI0、无直接调用、帧不超过 `StackSmall` 的叶子不再插
TLS 栈探针，链接器把它们的帧算进 nosplit 链。

`build/qjs/qjscli --stack-size 16384` 跑 V8 bench-v8 三次：SCORE 414、403、413，
都高于 Goja 公布的 402。413 是四份 QJS TU 都按叶子规则重编后的结果
（80.3 s）：Richards 237、DeltaBlue 223、Crypto 304、RayTrace 558、
EarleyBoyer 707、RegExp 136、Splay 1482、NavierStokes 664。离 native
QuickJS-ng 的 1389 还差约 3.4 倍。该数字已被 2026-09-25 的两轮结果取代
（SCORE 785、797，中位 791，墙钟 48 s，约 1.8 倍）。解释器里每个 opcode 仍在 volatile 重载
栈指针；把重载收成 safepoint 之后的 PHI 时，llc 丢掉了 `JS_DumpValue` 的
一个已标记槽，构建失败，这版没有留下。
