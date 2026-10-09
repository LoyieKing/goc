# RegExp 热路径：每个 `T *` 收成了什么色

对象：`libregexp.c` 的执行路径（`lre_exec` / `lre_exec_backtrack`），goc @ a52263b，Bellard 与 quickjs-ng 两个版本。上级文档：[perf-gap.md](../perf-gap.md) 第 4.4 节。

工具：`scripts/goc-color-report.py`。它按 `cmd/goc build` 的步骤重跑前半段编译，保留每一级 IR，再把 `goc-color-escape` 的决定映射回 C 声明和行号。用法见文末“复现”。

标注：【实测】直接从 IR 元数据或 callgrind 数据读出；【推导】根据 IR 形状加 `frontend/color-escape/pass/GocColorEscape.cpp` 的规则重建出来的原因；【推测】按指令模式匹配，没有直接验证。下文行号都是 Bellard 版 `third_party/quickjs-bellard/libregexp.c`（打过 `scripts/qjs-gstack-bellard.patch`）；ng 的着色完全相同，行号整体 +58。

## 1. 结论

- 热路径上的 uptr 编码/解码，**根源是 gstack 补丁里两个恢复宏用了 `goc_uptr_decode`**，不是回溯栈字段没有标色。解码结果在 pass 里是 DynamicUPtr（“可能指向栈”），于是 `pc`、`cptr` 在整个函数里都被当成可能指向栈：每次压栈要编码，每个 `pc+k` 变成 sptr、被 GocStackMap 当成 GC 根。【实测+推导】
- 把恢复宏改成普通整数转换（下称 E1），`lre_exec` 里的编码、解码、`FS:-8` 读取全部消失，`pc` 相关的根槽也没了。**只做了编译，没有运行。**【实测：IR】
- 把 `StackElem.ptr` 标成 `goc_cptr`（下称 E2，原先 todo 里的方案）**没有任何变化**。决定是否编码的是被存的值的来源，不是目的字段的色。【实测：IR】
- `-g` 不影响着色：Bellard 82 个函数、ng 108 个函数，有无 `-g` 的 `!goc.color` 决定 0 处不同；O3 之后的 `FS:-8` 读取（33）和根槽数（217/251）也相同。【实测】

## 2. 颜色是怎么定的（现状）

- Clang 侧只把显式的 `goc_cptr/sptr/uptr/auto_ptr/gptr` 属性变成 `llvm.var.annotation` / `llvm.ptr.annotation`。`libregexp.c` 没有任何注解，这里不起作用。
- 裸 `T *` 的着色全部在 IR 工具 `goc-color-escape` 里完成。它跑在 `-O3 -Xclang -disable-llvm-passes` 产出的 IR 上，此时还没有 SROA 和内联，一个 C 变量对应一个 alloca。QuickJS 构建传 `-default-ptr-color=cptr`，所以**所有没注解的指针形参和局部变量一开始都是 cptr**，之后再按栈来源传播、插入 `goc_uptr_from_ptr`（编码）和解码。
- 结果只以 `!goc.color` / `!goc.prov` / `!goc.uptr_encoded` 挂在指令上（alloca、load、GEP、call、phi/select）。形参没有元数据，只能看它的 `%x.addr` 溢出槽；struct 字段没有按声明的色；全局变量没有元数据。pass 没有转储开关（`-verbose` 只打印显式注解）。【实测】
- 后端还会另外做决定：`GocStackMap`（O3 之后）按 `isSptrColored` 或它自己的 alloca 数据流选根槽；`goc-reanchor` 对**所有**跨安全点使用的指针实参都做 anchor，不看色。所以 `capture`、`bc_buf`、`cbuf` 虽然是 cptr，仍有 `goc.arganchor`。【实测】

## 3. 哪个 `T *` 收成了什么色，为什么

O3 之后 `lre_exec_backtrack`、`lre_poll_timeout`、`get_u16/get_u32` 都内联进 `lre_exec`，只剩 `lre_exec`、`stack_realloc`、`lre_get_flags`。`lre_check_stack_overflow` 只在正则*编译*阶段调用（L1390/L2410），不在执行路径上。两个版本都没有 `push_state`。

| 源码指针（声明行） | 结果 | 原因 | 热路径代价 |
|---|---|---|---|
| `const uint8_t *pc`（形参 L2794）及 `pc1`（L3003/3137/3233） | 声明 **cptr**，但带栈来源（DynamicUPtr） | 接收 `goc_uptr_decode()` 的结果：`pc = goc_regexp_restore_ptr(sp[-3])`（L2914/2937/2972）。【推导】 | 每次压栈编码：**L3014** `sp[0].ptr = pc1`、L3027、L3172。`pc+1`、`pc+n` 按“栈来源的 GEP”规则变成 **sptr**，被当成根：`opcode = *pc++`（L2887）的 `goc.spill.root` 有 30 次 volatile 重载，那条存根的 `mov` 执行 **1.175 亿次**；另有 4 个 pc 根槽（L3036/3126/3150/3239）。 |
| `const uint8_t *cptr`（形参 L2794）及 GET_CHAR/PEEK_CHAR 里的 `_p` | 声明 cptr，带栈来源 | `cptr = goc_regexp_restore_ptr(sp[-2])`（L2915/2938/2973） | 编码：**L3015** `sp[1].ptr = cptr`、L3028、L3173；存捕获 **L3091** `SAVE_CAPTURE`、L3185 `SAVE_CAPTURE_CHECK` |
| 恢复进 `capture[]` 的值 | uptr 字 → 解码 → 再编码 | `capture[sp[-2].val] = goc_regexp_restore_capture(sp[-1])`：先解码，再存进 `capture`（cptr 形参，算“非栈目的地”） | L2910、L2969 各一次解码 + 一次编码 |
| `next_sp`（L2930） | cptr，带栈来源 | L2950 解码 | 只有解码（lookahead 路径） |
| `REExecContext *s`（`lre_exec` L3385；各被调函数的形参） | **sptr**（正确） | `&s_s` 在栈上；文件内被调函数的形参由 `proveStackOnlyFormals` 证明只收栈地址 | 无直接代价 |
| `sp`、`bp`、`stack_end`（L2800） | **sptr** | 通过只收栈地址的形参 `s` 读出 `s->stack_buf`，规则把读出的值当栈来源。结果碰巧对：它一开始就是 `s_s.static_stack_buf`。【推导】 | L2917/2940 的 `bp` 根槽（各 3 次重载），是真根 |
| `cbuf_end`（L2799）及宏里的 `_end`/`_start` | **sptr**（错：指向输入字符串） | 同一规则：`s->cbuf_end`、`s->cbuf` 经 `s` 读出 | O3 后没看到代价（不跨调用存活） |
| `s->opaque` 的值（L2763） | **sptr**（错：是 `JSContext *`） | 同上 | `lre_check_timeout` 附近 4 个根槽，各 1 次重载（冷路径） |
| `sp1`、`sp_top`（L2930 等） | **cptr**，但实际存的是栈地址 | `sp = sp ± n` 自环让 pass 证明不了，保留 TU 默认色。【推导】 | 无。`sp[-1].ptr = sp1` 原样存、原样读回（L2950），中间没有安全点。**色不严格，实际无害。【推测】** |
| `capture`、`bc_buf`、`cbuf`、`opaque`、`cptr1*`、`new_stack`、`get_u16/u32` 的 `tab` | **cptr** | TU 默认色，没有栈来源的值流进来 | 前端无代价；后端仍对 `capture` 做 arg-anchor（12 次重载），与色无关 |
| `StackElem.ptr`（union 成员） | 没有字段色；存进去要不要编码看**值**的色 | 目的地 `sp[i]` 证明不了在栈上，算逃逸 | 见上面各编码行 |

**为什么热路径要付钱：** pass 对存进 `sp[i].ptr` 的“可能指向栈”的指针做编码；补丁恢复时用 `goc_uptr_decode` 解码；解码结果是 DynamicUPtr，于是 `pc`、`cptr` 在函数余下部分都“可能指向栈”，每次压栈又要编码，每个 `pc+k` 都成了 sptr 根。实际上 `pc` 指向正则字节码（`re->bytecode->u.str8`，一个 JSString），`cptr` 指向被匹配的字符串（`str->u.str8`）；QuickJS 里两个调用点（`quickjs.c` L47958、L48185）传的都是堆缓冲区，`capture` 来自 `js_malloc`。唯一真正指向栈、又写进回溯栈的是 `sp1`（`sp[-1].ptr = sp1`，L2941，指向 `s_s.static_stack_buf`），而它现在本来就是原样写的。【实测：调用点；推导：来源链】

## 4. 编码/解码在机器码输入里的位置（O3 之后的 `lre_exec`）

每次编码 2 次 volatile `movq %fs:-8`（lo、hi），每次解码 1 次。`lre_exec` 共 29 次 `FS:-8` 读取（10 次编码 + 9 次解码）；整个 TU 33 次，另外 4 次是正则编译器里的两次编码（`re_emit_string_list`、`re_string_list_op`）。每一处都能对到补丁行：【实测】

| 行 | 源码 | helper | 热度（按执行次数配对的 callgrind 块，`data/regexp-lre-exec-bycount.txt`） |
|---|---|---|---|
| 3014, 3015 | `sp[0].ptr = pc1; sp[1].ptr = cptr;`（REOP_split_*） | 2× 编码 | **最热块：2442 万次执行，goc 48 条 / 原生 22 条，tls 类 Ir 5.617 亿**【推测：按 `cmp $0xf`（REOP_split_next_first = 15）、CHECK_STACK_SPACE(3)、`sp[2]=(bp-stack_buf)>>3`、`sp+=0x18` 匹配】 |
| 3091 | `SAVE_CAPTURE(idx, cptr)`（save_start/end） | 编码 | **第二热：2663 万次，34 / 17 条，3.192 亿**【推测】 |
| 2910 | 撤销循环 `capture[..] = restore_capture(sp[-1])` | 解码 + 编码 | 2310 万 + 2540 万次（tls 类约 1.45 亿）；多数恢复的捕获是 NULL，走快路径【推测】 |
| 2914/2915、2937/2938、2972/2973、2950、2969 | pc/cptr/next_sp/capture 恢复 | 解码 | 回溯时 |
| 3027/3028、3172/3173、3185 | lookahead 压栈、循环压栈、SAVE_CAPTURE_CHECK | 编码 | 次要 |

在配对的块上累计，uptr 序列约占 goc-bellard `lre_exec` 比原生多出的 +17.53 亿 Ir 中的 10.26 亿（约 58%）；pc 根槽的存储另加 1.175 亿。【实测：计数；块到行的对应是推测】

## 5. 反事实实验（只编译，未运行）

在 `libregexp.c` 的临时副本上改宏，用工具重建 IR。仓库、前端、third_party 都没动。

| 变体 | TU 隐式 uptr 存储 | FS:-8 读取（TU / lre_exec） | TU spill 根槽 | lre_exec spill 根槽 / volatile 根重载（两者都另有 3 个 arg anchor） |
|---|---:|---:|---:|---:|
| 现状（恢复用 `goc_uptr_decode`） | 12 | 33 / 29 | 217 | 12 / 52 |
| **E1**：恢复改成整数转换 `(uint8_t *)(uintptr_t)(elem).val` | **2**（都不在 lre_exec） | **4 / 0** | 212 | 7 / 12 |
| E2：保留解码，`StackElem.ptr` 标 `goc_cptr` | 12 | 33 / 29 | 217 | 不变 |
| E3：恢复用 `goc_uptr_as_cptr` | 2 | 4 / 0 | 212 | 同 E1，另多 9 个**不内联**的 `goc_uptr_as_cptr` 调用 |

E1 的改动（只动补丁里 `GOC_QJS_GSTACK` 分支的两个宏）：

```c
#define goc_regexp_restore_capture(elem) ((uint8_t *)(uintptr_t)(elem).val)
#define goc_regexp_restore_ptr(elem)     ((uint8_t *)(uintptr_t)(elem).val)
```

说明：
- 杠杆是**值的来源**（那次解码），不是字段色。E2 说明把目的地标成 cptr 不改变任何东西：往逃逸的 cptr 目的地存“可能指向栈”的值，照样要编码。
- E1 下 `pc`、`pc1`、`cptr`、`_p` 和捕获值都成了普通 cptr；`lre_exec` 里所有压栈、恢复都是原样读写，`pc` 根槽全部消失，只剩 `bp` 和 `s->opaque` / `s->stack_buf` 的根。E1 表格见附录。
- E3 效果和 E1 一样，但多出函数调用，不如 E1。

### E1 合入前检查清单

E1 成立的条件（**没有运行验证**）：压进回溯栈的指针，在跨越安全点（`stack_realloc`、`lre_check_timeout`）期间都不能是 goroutine 栈地址。

- [ ] 用 E1 构建 CLI，跑 test262 和正则相关测试（bellard、ng 各一遍）。
- [ ] 查外部 `lre_exec` 调用者：它是导出 API，如果有 C 调用者把栈上缓冲区作为输入字符串或字节码传进来，E1 会出错。QuickJS 自己的两个调用点都传堆缓冲区。
- [ ] 找出之前那个版本到底是哪个指针触发了陷阱。补丁注释写着“pointer-typed load is colored cptr and the return guard traps when the address is still on this goroutine”，说明更早的版本出过陷阱。嫌疑最大的是 `next_sp`（保存的 `sp1`，一个真栈地址）【推测，未复现】。如果确认是它，可以只对 `next_sp` 保留 `goc_uptr_decode`，`pc`/`cptr`/capture 用整数转换。

## 6. 缺口与局限

- 原因列是重建的。pass 不记录为什么选某个色；工具重新遍历每个槽的写入，套用 pass 规则得出原因。
- 字段没有按声明的色。`StackElem.ptr` 只有在一次编码存储之后才成为“auto-uptr 字段”（键是 IR 类型 + GEP 下标，`sp[0]` 和 `sp[1]` 是不同的键）。工具按 GEP 报字段访问，不报字段声明色。
- 按容器传色：通过 sptr 的 `s` 读 `s->field`，值带栈来源。所以 `cbuf_end`、`_end`、`opaque` 成了 sptr，即使字段里存的是堆指针。
- `lre_exec` 里 `s->stack_buf` / `s->opaque` 的读取显示 `cptr`+`prov:stack`：这是迭代顺序的残留（`s` 槽先是默认 cptr，后被改成 sptr，`Refined` 不回退），不是解码，这些值也不被存储，没有代价。
- O3 会丢掉被改写指令的 `!goc.color`；GocStackMap 用自己的 alloca 数据流，并且不看色就 anchor 指针实参，所以根槽表是后端决定的。
- 热块到源码行的对应是按指令模式配的，不是行号表。goobj/二进制里没有 DWARF；可以对 `a.reanchor.ll` 跑 `llc -g` 再按地址对，本次没做。
- 第 5 节全部是编译期计数，没有运行时数据，也没有测 E1 的实际加速。

## 7. 复现

在仓库根目录执行（`build/` 已被 gitignore）。前提：QuickJS 源码在 `third_party/` 下且打过 gstack 补丁（`scripts/qjs-build.sh` 会打），打过补丁的 clang、它旁边的 `opt`、`frontend/color-escape/build/goc-color-escape`、`backend/build/pass-out/GocStackMap.so` 都已就绪。

```bash
R=build/regexp-colors
scripts/goc-color-report.py build --flavor bellard --out $R/bellard     # clang -g … → a.reanchor.ll
scripts/goc-color-report.py build --flavor ng      --out $R/ng
scripts/goc-color-report.py report $R/bellard --title "Bellard libregexp.c" --md $R/bellard.md
scripts/goc-color-report.py report $R/ng      --title "quickjs-ng libregexp.c" --md $R/ng.md

# -g 不改变着色
scripts/goc-color-report.py build --flavor bellard --no-g --out $R/bellard-nog
scripts/goc-color-report.py diff $R/bellard $R/bellard-nog

# E1：拷一份 libregexp.c，改两个恢复宏（见第 5 节），再建
mkdir -p $R/e1-src && cp third_party/quickjs-bellard/libregexp.c $R/e1-src/
#   （编辑 $R/e1-src/libregexp.c）
scripts/goc-color-report.py build --flavor bellard --src $R/e1-src/libregexp.c \
    --cflags "-I third_party/quickjs-bellard" --out $R/e1
scripts/goc-color-report.py report $R/e1 --title "E1" --md $R/e1.md
scripts/goc-color-report.py diff $R/bellard $R/e1

# 其它函数 / 翻译单元：--src file.c，report --funcs f1,f2
```

每个输出目录里有 `a.ll → a.color.ll → a.gate.ll → a.opt.ll → a.sm.ll → a.reanchor.ll`、`color-escape.log` 和 `BUILD.txt`。报告分 A–F 六节：A 按声明的色，B 经 struct/union 字段的指针读写，C 编码点，D 解码点，E O3 后内联的 `FS:-8` 读取，F GC 根槽。

## 附录：生成的表（Bellard，只保留热函数和关键行）

pass 摘要：`0 error(s), scoped dynamic allocas=0, implicit uptr stores=12, decoded pointer loads=0, guarded returns=0`。

### A. 按声明的色（节选；重复的宏临时变量 `_p`/`_end`/`_start`、`sp1` 只留一行）

| function | kind | name | line | alloca !goc.color | loads (value colors) | resolved |
|---|---|---|---|---|---|---|
| lre_exec | param | capture | 3381 | cptr | cptr×2 | cptr |
| lre_exec | param | bc_buf | 3382 | cptr | cptr×3 | cptr |
| lre_exec | param | cbuf | 3382 | cptr | cptr×3 | cptr |
| lre_exec | local | s | 3385 | sptr | sptr×20 | sptr（存 `&s_s`） |
| lre_exec_backtrack | param | s | 2793 | sptr | sptr×137 | sptr（stack-only formal） |
| lre_exec_backtrack | param | capture | 2793 | cptr | cptr×14 | cptr |
| lre_exec_backtrack | param | **pc** | 2794 | cptr | cptr+stackprov×56 | cptr（may-be-stack：decode @L2914/2937/2972） |
| lre_exec_backtrack | param | **cptr** | 2794 | cptr | cptr+stackprov×50 | cptr（may-be-stack：decode @L2915/2938/2973） |
| lre_exec_backtrack | local | cbuf_end | 2799 | sptr | sptr×20 | sptr（load `s->cbuf_end`） |
| lre_exec_backtrack | local | sp | 2800 | sptr | sptr×111 | sptr（load `s->stack_buf`） |
| lre_exec_backtrack | local | bp | 2800 | sptr | sptr×23 | sptr |
| lre_exec_backtrack | local | stack_end | 2800 | sptr | sptr×11 | sptr |
| lre_exec_backtrack | local | sp1 | 2930 | cptr | cptr×6 | cptr（存的是栈地址，pass 证明不了） |
| lre_exec_backtrack | local | sp_top | 2930 | cptr | cptr×1 | cptr |
| lre_exec_backtrack | local | next_sp | 2930 | cptr | cptr+stackprov×1 | cptr（may-be-stack：decode @L2950） |
| lre_exec_backtrack | local | _p (GET_CHAR) | 2993 | cptr | cptr+stackprov×5 | cptr（经 `cptr` 带栈来源） |
| lre_exec_backtrack | local | _end (GET_CHAR) | 2993 | sptr | sptr×1 | sptr（经 `cbuf_end`） |
| lre_exec_backtrack | local | pc1 | 3003 | cptr | cptr+stackprov×1 | cptr（经 `pc` 带栈来源） |
| lre_exec_backtrack | local | cptr1 | 3232 | cptr | cptr×7 | cptr |
| lre_poll_timeout | param | s | 2759 | sptr | sptr×3 | sptr |
| stack_realloc | param | s | 2769 | sptr | sptr×10 | sptr |
| stack_realloc | local | new_stack | 2771 | cptr | cptr×4 | cptr |

### B. 经字段的指针读写（节选）

| function | field | loaded value colors | stored value colors | # encoded stores | lines |
|---|---|---|---|---|---|
| lre_exec_backtrack | %union.StackElem[] .ptr | - | cptr×5, uptr(encoded)×6 | 6 | 2941,3014,3015,3027,3028,3091,3103,3105,3172,3173,3185 |
| lre_exec_backtrack | %struct.REExecContext.stack_buf | sptr×77 | - | 0 | 2813 … 3185 |
| lre_exec_backtrack | %struct.REExecContext.cbuf_end | sptr×1 | - | 0 | 2811 |
| lre_exec_backtrack | %struct.REExecContext.cbuf | sptr×8 | - | 0 | 3042 … 3367 |
| lre_poll_timeout | %struct.REExecContext.opaque | sptr×1 | - | 0 | 2763 |

### C. 编码点（goc_uptr_from_ptr）

| line | source | stored value | value color | 来源 |
|---|---|---|---|---|
| 2910 | `capture[sp[-2].val] = goc_regexp_restore_capture(sp[-1]);` | decode 结果 | sptr | decode @L2910 |
| 2969 | 同上 | decode 结果 | sptr | decode @L2969 |
| 3014 | `sp[0].ptr = (uint8_t *)pc1;` | pc1 | cptr+stackprov | 经 pc：decode @L2914/2937/2972 |
| 3015 | `sp[1].ptr = (uint8_t *)cptr;` | cptr | cptr+stackprov | decode @L2915/2938/2973 |
| 3027 | `sp[0].ptr = (uint8_t *)(pc + (int)val);` | pc + val | sptr | 经 pc |
| 3028 | `sp[1].ptr = (uint8_t *)cptr;` | cptr | cptr+stackprov | 经 cptr |
| 3091 | `SAVE_CAPTURE(idx, (uint8_t *)cptr);` | cptr | cptr+stackprov | 经 cptr |
| 3172 | `sp[0].ptr = (uint8_t *)pc1;` | pc1 | cptr+stackprov | 经 pc |
| 3173 | `sp[1].ptr = (uint8_t *)cptr;` | cptr | cptr+stackprov | 经 cptr |
| 3185 | `SAVE_CAPTURE_CHECK(idx, (uint8_t *)cptr);` | cptr | cptr+stackprov | 经 cptr |

解码点（D 节）就是补丁写的 9 处 `goc_uptr_decode`：L2910、2914、2915、2937、2938、2950、2969、2972、2973，都不是 pass 插入的。

### E. O3 后 `lre_exec` 里的 `FS:-8` 读取

| helper | g loads 每处 | lines |
|---|---:|---|
| decode (hi) | 1 | 2910, 2914, 2915, 2937, 2938, 2950, 2969, 2972, 2973 |
| encode (from_ptr: lo+hi) | 2 | 2910, 2969, 3014, 3015, 3027, 3028, 3091, 3172, 3173, 3185 |

合计 9×1 + 10×2 = 29。E1 下这张表为空。

### F. O3 后 `lre_exec` 的 GC 根槽

| slot | source variable | value !goc.color | value expression | def line | # volatile reloads |
|---|---|---|---|---|---:|
| %goc.arganchor | capture | - | param | | 12 |
| %goc.arganchor12 | bc_buf | - | param | | 2 |
| %goc.arganchor15 | cbuf | - | param | | 1 |
| %goc.spill.root | **pc** | sptr | `phi(…) + 1` | 2887 | **30** |
| %goc.spill.root40 | **pc** | sptr | `root + (*root + 4)` | 3036 | 2 |
| %goc.spill.root45 | **pc** | sptr | `(phi + 6) + *(phi + 2)` | 3126 | 2 |
| %goc.spill.root50 | **pc** | sptr | `(phi + 10) + *(phi + 6)` | 3150 | 2 |
| %goc.spill.root55 | **pc** | sptr | `(phi + 2) + *root` | 3239 | 4 |
| %goc.spill.root30 | bp | sptr | `s_s.stack_buf + (sp[-1] & mask)` | 2917 | 3 |
| %goc.spill.root36 | bp | sptr | 同上 | 2940 | 3 |
| %goc.spill.root34/43/48/53 | (`s->opaque`) | sptr | `s_s.opaque` | 2763 | 各 1 |
| %goc.spill.root60 | (`s->stack_buf`) | - | `s_s.stack_buf` | 3416 | 2 |

E1 下 `lre_exec` 只剩 3 个 arg anchor、2 个 `bp` 根（各 3 次重载）、4 个 `s->opaque` 根（各 1 次）和 1 个 `s->stack_buf` 根（2 次）：spill 根 12→7，重载 52→12。
