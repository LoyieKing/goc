> **Public export note:** paths in this guide refer to the `goc` repository
> (`include/goc.h`, `cmd/goc`, `docs/`). The pointer-store rule is v0.2.2.
> The `alloca` contract is v0.2.3. The same-goroutine rule for `sptr` / `uptr` is v0.2.4.

# goc 语言语法指导（v0.2.4）

**定位：** goc = 面向 Go 运行时的 C 方言：在 goroutine 用户栈上跑，遵守 Go 的 stackmap / morestack /（对 `gptr` 的）写屏障，同时保留接近 C 的代码形态。  
**非目标：** 完整 ISO C；任意 libc 语义；通用「加速所有 cgo」。

本文只描述**语法与类型合同**。  
相对 v0.1：**删除 `dsptr` 语言表面**；引入显式 `sptr` / `uptr` / `auto_ptr`；`JSValue` 改为显式 struct（不再 NaN-box 整盒 scalar）。v0.2.2 窄化修订：`sptr` 赋给已染色为 `cptr` 的非栈 `T *` 存储时，编译器隐式用 `uptr` 表示该存储，不再报 sptr-escape 错误；源代码仍可保留 `T *`。v0.2.3：`alloca(n)` 降为函数作用域的 `cptr` 分配（§7.2），不再列为禁止，也不恢复成 `sptr` 变长栈帧。v0.2.4：`sptr` 与 `uptr`（MSB=1）严格绑定所属 goroutine（§3.2、§8.3）。持有它们的 context 只能在那一条 goroutine 上调用；换 goroutine 会把 offset 加到错误的 `stack.hi` 上。

---

## 1. 语言轮廓

| 项 | 约定 |
|---|---|
| 基线 | C11 子集（控制流、表达式、struct、固定数组、`_Alignas`、内联等） |
| 表面扩展 | 指针色类型（`cptr` / `sptr` / `uptr` / `auto_ptr` / `gptr`）、方言内建/属性、与 Go 的导入导出声明 |
| 禁止/推迟 | 含指针的 union；用户级 tagged pointer；随意指针↔整数；非 §7.2 形状的 VLA；跨 Go 帧的 `setjmp`/`longjmp`；在 goc 里起 OS 线程再回调 Go；跨 goroutine 使用 `sptr` / `uptr` 或持有它们的 context（§8.3）；**`dsptr`（已移出语言表面，由 `uptr` 覆盖）** |

源文件建议扩展名：`.goc` / `.c`（由构建用 `goc` 驱动编译，而非系统 cc）。

---

## 2. 类型总览

### 2.1 标量（scalar）

与 C 相同的整数、浮点、枚举、`_Bool` 等。  
**严格合同：指针与标量不得混淆。** 不得把指针位型当整数运算后再当指针用（除非走 §5 内建，且结果色有明确规定）。

`JSValue` **不是** NaN-box / 整盒 scalar；必须是带独立 `pointer` 字段的显式 struct（见 §6）。禁止用 `union { ptr; double; int }` 对 `JSValue` 做位叠合。

### 2.2 指针色（核心）

| 写法 | 含义 | 典型指向 / 存储 |
|---|---|---|
| `cptr<T>` | 非栈对象指针 | C 堆 / 全局 / arena 等；**不是** Go 堆。指针*字*可在栈、寄存器或堆。直接寻址。 |
| `sptr<T>` | 当前 g 栈对象指针 | 被指物在栈上。原始绝对指针字只许在栈槽/寄存器；赋给非栈 `cptr<T>` / 已染色为 `cptr` 的 `T *` 时，目标存储改用 `uptr` 编码。直接寻址。跨 morestack：原始值须 spill 到 stackmap 跟踪的槽。 |
| `uptr<T>` | `cptr\|sptr` 编码并集 | 可存任意处（含堆字段）。**不可直接解引用**；须先 decode。 |
| `auto_ptr<T>` | 推断色；`T *` ≡ `auto_ptr<T>` | 逃逸敏感：优先 `cptr`/`sptr`；须入库则收成 `uptr`。已钉 `sptr` 后赋给非栈 `cptr<T>` 存储是窄例外：该存储自动用 `uptr` 编码，不改变其它逃逸规则。 |
| `gptr<T>` / `gptr<?>` | Go 堆指针（独立轨） | 与 `cptr`/`sptr`/`uptr`/`auto_ptr` **无隐式转换**。stackmap + 写屏障。禁止算术。 |
| `void *` | ≡ `auto_ptr<void>`（再按位置精化） | 同 `T *` 规则 |

色关键字**已表示一层指针**，不要写 `gptr<T> *` 表示「Go 指针」——那是指向 `gptr` 的 `cptr`/`auto_ptr`。需要时用 typedef。

> **无 `dsptr`：** 堆上需要挂「栈对象引用」时，一律用 `uptr` 编码；语言表面不再提供 `dsptr`。

### 2.3 聚合

- `struct` / 定长数组：允许；字段可含任意色指针，但须满足逃逸与写屏障规则。  
- **`union` 内禁止出现指针色字段**（含 `cptr`/`sptr`/`uptr`/`gptr`/`T *`）。带 tag 的值用旁路布局 `{ cptr|NULL, aux }` 或显式 struct（如 `JSValue`）。  
- 函数指针：单独一类（建议当 scalar 或显式 `fptr`）；**虚表 / 导出 ABI 禁止 `auto_ptr` / 裸 `T *`**，须钉死 `cptr` / `sptr` / `uptr` / `gptr`。

---

## 3. 指针色合同

### 3.1 `cptr<T>`

**值域：** `NULL`，或指向非栈对象：C 堆（`malloc` / 方言分配器 / QJS arena 等）、全局 / 数据段、其它非栈 arena。

**不变量：**

- `cptr` **不指向 Go 堆**（Go 堆只用 `gptr`）。  
- `cptr` **不指向当前 g 栈上的对象**（栈被指物用 `sptr`；若编码进可堆存位置则用 `uptr`）。

**操作：**

- `cptr<T>` ↔ `cptr<U>`：允许（任意互相转换，含经 `void *` / `auto_ptr` 精化路径）。  
- `cptr` ↔ 整数 / 浮点 / `gptr` / `sptr`：默认禁止。`sptr` 赋给非栈 `cptr<T>` / 已染色为 `cptr` 的 `T *` 左值时，不发生直接 `sptr`→`cptr` 转换，而是将该存储提升为 `uptr` 表示并插入编码/解码。
- 解引用、算术：允许（结果仍为 `cptr`；越界 UB，同 C）。  
- **stackmap：** 指针槽可进 map；搬栈时若值落在旧栈 `[lo,hi)` 才 `+delta`——但按合同合法 `cptr` 不应落在栈上，故通常不动。  
- **写屏障：** 不走 Go 写屏障。

**指针字存放：** 栈槽、寄存器、堆字段、全局均可。

### 3.2 `sptr<T>`

**值域：** `NULL`，或指向**所属 goroutine** 用户栈上的 `T`。当前表示是一个机器字，不携带 goroutine 指针。解引用和搬栈修正都相对这条 goroutine。换一条 goroutine 再使用，地址不再指向原来的栈对象。

**操作：**

- 解引用、算术：允许（结果仍为 `sptr`；越界 UB）。  
- **stackmap：** 进 map；搬栈时对落在旧栈的值 `+delta`。  
- **写屏障：** 无。  
- 与 `cptr` / `gptr`：无直接隐式转换。唯一例外：向非栈 `cptr<T>` / 已染色为 `cptr` 的 `T *` 存储赋值时，目标存储使用 `uptr` 编码；这不是让 `cptr` 值包含原始栈地址。

**指针字存放（硬规则）：**

> 原始绝对 `sptr` 指针*字*只许出现在**栈槽与寄存器**。  
> 赋给非栈、已染色为 `cptr` 的 `T *` 存储时，编译器将该存储的有效色改为 `uptr`，插入 `goc_uptr_from_sptr` 编码；同一存储中的 `cptr` 值也按 `uptr` 表示。goc 管理的 `T *` 读取会自动解码。堆中不保存原始栈绝对地址。  
> 跨 morestack：仍以原始 `sptr` 形式活跃的值**必须** spill 到 stackmap 跟踪的栈槽，再由运行时搬栈修正。

需要把「指向栈对象」的引用挂进 runtime / 堆 → 存储表示必须是 `uptr`（§3.3），不得存绝对 `sptr`。若目标是 `cptr<T>` / 已染色为 `cptr` 的 `T *`，赋值处自动转换；显式 `uptr<T>` 仍是可见、可审计的选择。

### 3.3 `uptr<T>`

用于「逻辑上可能是 `cptr` 或 `sptr`，且需要存进堆字段 / 长寿位置」的场景（典型：`JSStackFrame` 挂在 runtime 上）。显式 `uptr<T>` 值仍不可直接解引用；编译器为自动提升的 `cptr<T>` 存储维护相同的物理编码，并在保留 `T *` 源级访问时插入通用解码。

| | 规则 |
|---|---|
| 表示 | MSB=0：绝对 `cptr` 地址，直接当地址用；MSB=1：相对 `g.stack.hi` 的 **int64 二进制补码 offset**，`abs = stack.hi + stored` |
| 可存堆 | **允许**（Go 堆 / C 堆 / 全局字段） |
| 解引用 | **禁止直接 `*`**；须先 decode 成临时 `cptr` 或 `sptr` |
| 搬栈 | 堆上 MSB=1 的 offset **不变**；MSB=0 的绝对 `cptr` 通常不在栈上故不动 |
| 寿命 | **不延长**栈对象寿命；帧返回前必须 unlink / 清空 |
| 解码 | 仅用 **所属 goroutine** 的 `stack.hi`。当前表示不携带 `g`，实现用当前 goroutine。跨 goroutine 解码把 offset 加到错误的 `stack.hi` 上，得到错误地址。这是合同错误（§8.3）。 |

```c
// 示意内建（名称待定）
uptr<T>  goc_uptr_from_cptr(cptr<T> p);          // MSB=0
uptr<T>  goc_uptr_from_sptr(sptr<T> p);          // MSB=1，相对当前 g.stack.hi
cptr<T>  goc_uptr_as_cptr(uptr<T> u);            // 要求 MSB=0，否则错误
sptr<T>  goc_uptr_as_sptr(uptr<T> u);            // 要求 MSB=1，用 owner g 解码
```

> **重要：** `uptr` 的 MSB 协议是**独立机器字上的编码**，不得塞进 NaN-box / 标量标签字。`JSValue.pointer` 等字段用普通色指针（通常 `cptr`），由 stackmap 按字段跟踪。

### 3.4 `auto_ptr<T>` 与 `T *`

`T *` 是 `auto_ptr<T>` 的别名。编译器按逃逸 / 来源做**色精化**（在赋值/入库之前一次定色，**不是**先当 `sptr` 再逃逸升格）：

1. 能证明只指向非栈对象 → `cptr<T>`  
2. 能证明只指向当前栈对象，且指针字**永不**入库 → `sptr<T>`  
3. 两色都可能、或必须进堆字段 → **一开始就**收成 `uptr<T>`；若 `sptr` 后来赋给已染色为 `cptr` 的非栈 `T *` 存储，仅该存储自动提升为 `uptr`，不要求改写该 `T *` 声明。  
4. **实例敏感：** 若某 struct 类型在别处有堆用法、字段类型写的是 `auto_ptr`，但**本函数内该实例只活在栈上**，仍可收成 `cptr`/`sptr`，**不要**盲目整类型升成 `uptr`。  
5. 若分析曾把某值收成 `sptr`，随后赋给非栈 `cptr<T>` / 已染色为 `cptr` 的 `T *` 左值 → 对该存储自动执行 `uptr` 编码；其它逃逸（尤其返回值、`gptr` 目标、无法证明/改写的外部存储）仍 **报错**。不做类型范围的盲目升格。

形参上的 `T *` 允许接收已精化的同族色（见 §4）；**不接收 `gptr`**。

### 3.5 `gptr<T>` / `gptr<?>`

**值域：** `NULL` 或合法 Go 堆（及 Go 认可的指针槽）地址。

**操作：**

- **不得**与 `cptr` / `sptr` / `uptr` / `auto_ptr` / 整数做隐式或随意强制转换。  
- 只能经 **严格 API / 绑定生成物 / Go 导入导出** 产生与消费。  
- **stackmap：** 进 map。  
- 写入可能位于 Go 堆的槽：编译器插入 **写屏障**。  
- `gptr` 算术：禁止。

`gptr<?>`：Go 类型未知或句柄式擦除；仍是 `gptr` 合同（扫描、屏障），不是 `cptr`。

### 3.6 取地址与逃逸

```c
T x;
auto_ptr<T> p = &x;   // 精化为 sptr<T>（栈被指物）
```

- `&局部` / 方言栈缓冲 → `sptr`（经 `auto_ptr` 精化）。  
- `&堆对象` / 分配器返回 → `cptr`（非栈）或 `gptr`（若来自 Go）。  
- `&全局` → `cptr`（数据段）。

**逃逸与存储规则：**

> 栈源**绝对地址**不得原样存入堆字段、全局或其它非栈位置，也不得作为返回值逃出。  
> **窄例外：** 若赋值目标是非栈 `cptr<T>` / 已染色为 `cptr` 的 `T *` 存储，编译器自动把该存储的有效色提升为 `uptr`，对 `sptr` 插入编码、对读取插入解码；QuickJS 等 C 源码可保留原有 `T *` 声明与赋值写法。  
> 这不延长栈对象寿命；引用使用结束前，栈对象仍须存活。返回 `sptr`、存入 `gptr`、或逃入编译器无法改写的外部存储，仍是编译错误。其它场景仍可显式使用 `goc_uptr_from_sptr`。

---

## 4. `T *` / `auto_ptr` 与 ABI

### 4.1 含义随位置变化

| 位置 | `T *` / `auto_ptr<T>` |
|---|---|
| **函数形参**（goc TU 内） | 色模板 / 推断：可接收 `cptr` 或 `sptr`（推荐同色联锁）；必要时收成 `uptr`；**不接收 `gptr`** |
| 返回值 / 局部 / 结构体字段 / 全局 | 按逃逸精化；堆字段中的栈引用以 `uptr` 表示。赋给非栈 `cptr<T>` 字段或全局 `T *` 时自动编码；返回 `sptr` 仍报错。 |

```c
void foo(int *out);           // 形参 auto_ptr：可吃 &local（sptr）或堆缓冲（cptr）
int *p;                       // 局部：推断
struct S { int *q; };         // 若 S 实例可能进堆：q 不得收绝对 sptr；用 uptr 或保证非栈
```

### 4.2 形参模板体规则（推荐）

- 同色联锁：一函数内多个 `T *`/`U *` 形参默认同一精化色，避免 `2^n` 爆炸。  
- 写穿 `*p`、读、再传入其它 `U *` 形参：同色实例均允许。  
- **仅非栈源（`cptr`）或已 `uptr` 编码的实例允许：** 把指针字自身存进堆字段 / 全局 / 返回。  
- 栈源 `sptr` 写入非栈 `cptr<T>`/`T *` 字段 → 赋值处自动编码为 `uptr`，其源级 `T *` 读取自动解码；不要求逐站点改写成 `uptr`。  
- `JSContext *` 等「永不为栈源」的接收者可由实现钉死为单一 `cptr`，不参与模板。

### 4.3 函数指针、导出、虚表

```c
// 禁止：auto_ptr / 裸 T* 进 ABI
typedef void (*cb)(int *);

// 要求钉死色
typedef void (*cb)(cptr<int>);
// 或 sptr / uptr / gptr，按真实合同选一
```

Go 导出入口同理：禁止 `auto_ptr` / 裸 `T *`；钉 `cptr`、`sptr`、`uptr` 或 `gptr`。

---

## 5. 转换总表

| 从 \ 到 | scalar | `cptr` | `sptr` | `uptr` | `gptr` |
|---|---|---|---|---|---|
| scalar | C 规则 | 禁止（除内建） | 禁止 | 禁止（除内建） | 禁止 |
| `cptr` | 禁止 | 允许（任意 `T`） | 禁止 | `goc_uptr_from_cptr` | 禁止 |
| `sptr` | 禁止 | 仅赋给非栈 `cptr<T>`/`T *` 存储时，存储自动提升为 `uptr`（非直接转换） | 允许 | `goc_uptr_from_sptr` | 禁止 |
| `uptr` | 禁止直接当标量玩 MSB | `as_cptr`（MSB=0） | `as_sptr`（MSB=1，owner g） | 允许 | 禁止 |
| `gptr` | 禁止 | 禁止 | 禁止 | 禁止 | 仅 API / `gptr<?>` 擦除 |
| `auto_ptr` | — | 精化后 | 精化后 | 精化后 | 禁止 |

内建示例（名称待定）：

```c
gptr<T>     goc_gptr_from_handle(...);
cptr<T>     goc_cptr_from_bits(...);       // 高度受限
uptr<T>     goc_uptr_from_sptr(sptr<T>);
uptr<T>     goc_uptr_from_cptr(cptr<T>);
sptr<T>     goc_uptr_as_sptr(uptr<T>);     // 临时，禁把 abs 再存堆
cptr<T>     goc_uptr_as_cptr(uptr<T>);
```

---

## 6. `JSValue` 形态（语法层）

**不再**使用 NaN-box 整盒 scalar，也**禁止** `union { ptr; double; int }` 位叠合。

推荐形状（示意）：

```c
typedef struct JSObject JSObject;

typedef struct JSValue {
    int tagged_value;              /* tag 判别；也可用 int32/int64，ABI 钉死即可 */
    /* payload：真实头文件须写具体色；void* 仅作说明 */
    cptr<JSObject> pointer;        /* 推荐：对象载荷用 cptr / 可空 cptr；立即数忽略 pointer */
} JSValue;
```

要点：

- `pointer` 是**普通色指针字段**（典型 `cptr<JSObject>` 或 API 层 `auto_ptr` 再钉死）；活在栈上时由 **stackmap 按字段**跟踪。  
- **禁止**把 `uptr` 的 MSB 协议嵌进 NaN-box / 同一标量字。  
- 立即数：只看 `tagged_value`；`pointer` 可忽略或置空。  
- 若将来对象改走 Go 堆，载荷应改为 `gptr`，与 `cptr` 轨仍无隐式转换——那是引擎策略变更，不是把 tag 塞进指针字。

```c
JSValue js_from_obj(cptr<JSObject> p, int tag) {
    JSValue v;
    v.tagged_value = tag;
    v.pointer = p;
    return v;
}

cptr<JSObject> js_get_obj(JSValue v) {
    /* 按 tag 判别后再用 v.pointer */
    return v.pointer;
}
```

---

## 7. 栈与分配相关语法/API

### 7.1 方言栈 API（替代 C builtin）

禁止依赖 `__builtin_frame_address` 或创建时记下的绝对栈顶。栈界用下面两个函数，读的是当前 `g`。`goc_stack_check` 仍是占位，不代替 morestack。

```c
uintptr_t goc_stack_hi(void);
uintptr_t goc_stack_lo(void);
bool      goc_stack_check(size_t need);   // 占位，不代替 morestack
```

### 7.2 `alloca`

源码仍写 `alloca(n)`。着色前，编译器把动态 `alloca` 换成 `goc_dynalloc` / `goc_dynrelease`。结果是 `cptr`，不在 goroutine 栈上，也不是 `sptr`。Go 的 pcsp 描述不了帧中间改 SP，所以没有变长栈帧。

寿命与 C `alloca` 相同：活到函数返回。同一函数里多次分配都留到每个 `return` 前一次释放。不支持中途用 `stacksave` / `stackrestore` 回收。

接受的形状就是 clang 给 `alloca(n)` 的 IR：

- 元素类型 `i8`
- 对齐不超过 16
- 尺寸是不超过 64 位的整数

分配按 16 字节对齐。不满足则编译失败，不静默改写：

- `stacksave` / `stackrestore`
- 异常、`invoke`，以及其它非局部退出
- 降不到上述形状的 VLA（例如元素不是 `i8` 的 `int a[n]`）

降级 API 声明在 `include/goc.h`。用户写 `alloca`；编译器插入调用。手写调用同样得到 `cptr`，不会变成 `sptr`。手写时由调用方持有 `scope`，编译器只给 `alloca` 插释放。

```c
goc_cptr goc_dynalloc(size_t bytes, void **scope);
void     goc_dynrelease(void **scope);
```

`scope` 是水位。函数入口置 `NULL`。这次执行里第一次分配记下水位；池地址永非 `NULL`，所以后面的 `alloca` 不改它。返回时水位以上全部丢掉，再把 `*scope` 清掉。失败不返回 `NULL`，直接 `goc_uptr_fatal`：`scope` 为空、尺寸溢出、池耗尽。pool 路径上，释放时水位不在池内也是致命错误。

两套实现，签名和颜色相同：

| 构建 | 行为 |
|---|---|
| 定义 `GOC_DYNALLOC_POOL` | 进程全局 bump pool。默认 16 MiB，`-DGOC_DYNALLOC_POOL_SIZE=` 可改。只许一个 goroutine 调用；两个同时调用会踩同一个游标。不清零，与 C `alloca` 一致。 |
| 未定义 | 每次 `malloc` / `free`。freestanding 路径清零。指针仍是 `cptr`。 |

`GOC_INLINE_DYNALLOC=1` 只把 pool 路径的 bump 内联进调用点，不改变上面的合同。

引擎自己的栈预算不是本条。`alloca` 不再消耗 goroutine 栈之后，用绝对 SP 减去 `alloca_size` 去比一个创建时记下的栈顶，是那个引擎的事。编译器不改那些函数。

### 7.3 分配器

C 堆：`malloc`/`free` 或 `goc_alloc` 族 → `cptr`。  
`alloca` 的降级（§7.2）也是 `cptr`，但是函数作用域，不是一般的 `malloc`。  
Go 堆：仅能通过 Go/绑定 API → `gptr`。

---

## 8. 与 Go 的互操作（语法）

### 8.1 非对称

| 方向 | 规则 |
|---|---|
| **C/goc → Go 指针** | 必须使用 **`gptr`**，经严格 API；大力约束 |
| **Go → C 内存** | 可较随意（Go 侧持有 `cptr` 或等价句柄，不要求把 C 指针升成 `gptr`） |

### 8.2 导入 / 导出（示意）

```c
__attribute__((goc_goimport("pkg", "Log")))
void host_Log(goc_go_string msg);

__attribute__((goc_goexport("Eval")))
int qjs_eval(cptr<JSContext> ctx, goc_go_string src);
```

Go 结构平行头由绑定生成器生成；手写 Go 布局属违规。  
`string` / `slice` 头：ptr 字为 `gptr`，len/cap 为 scalar（拆开标色）。

### 8.3 同一 goroutine

`sptr` 和 `uptr`（MSB=1）都相对所属 goroutine 的 `stack.hi`。当前表示是一个机器字：`sptr` 是这条栈上的绝对地址，`uptr` 的 MSB=1 是相对这条 `stack.hi` 的 offset。两者都不携带 goroutine 指针，运行时用的是当前 `g`。

因此这是语法合同，不是引擎偏好：

- 任何 goc 调用，只要参数、返回值，或被调用对象里活着 `sptr` / `uptr`，就必须落在创建这些指针的同一条 goroutine 上。
- 带 context 的 API 同样受这条约束。QuickJS 的 `JSContext` / `JSRuntime`，以及任何把栈帧链、父指针、回溯栈编进对象的 context，都算。把 `JSContext` 交给另一条 goroutine 再调用，offset 会加上那条 goroutine 的 `stack.hi`，指针解到错误地址。
- 编译器当前不插入跨 goroutine 检查。违反仍是合同错误，不是未定义的实现细节。
- 下一版不改变本条的默认表示。带所属 goroutine 指针的双 64 位形式由开关打开，默认关。见 [todo.md](todo.md)。

---

## 9. 属性与限定符（草案）

```c
__goc(nosplit)     // 小帧、不调会分裂的函数
__goc(safepoint)   // 提示可插入 safepoint（可选；非语言必做）
__goc(nowb)        // 明确无需写屏障的存储（仅限证明安全时）
__goc(goimport)
__goc(goexport)
```

写屏障：对 `gptr` 的堆存储默认自动插入，无需手写。

---

## 10. 声明与定义示例

```c
#include <goc.h>

typedef struct JSObject JSObject;
typedef struct JSContext JSContext;
typedef struct JSRuntime JSRuntime;
typedef struct JSStackFrame JSStackFrame;

typedef struct JSValue {
    int tagged_value;
    cptr<JSObject> pointer;
} JSValue;

cptr<JSObject> js_new_object(cptr<JSContext> ctx);

/* out-param：T* = auto_ptr，可吃栈缓冲或堆缓冲 */
void js_to_int32(cptr<JSContext> ctx, int32_t *pres, JSValue v) {
    int32_t n = /* 按 v.tagged_value 解释 ... */;
    *pres = n;
}

/* 合法：栈帧以 uptr 编码挂进 runtime（可在 C 堆字段） */
void example_frame(cptr<JSRuntime> rt) {
    JSStackFrame sf_s;
    sptr<JSStackFrame> abs = &sf_s;                 // 栈绝对：仅栈/寄存器
    uptr<JSStackFrame> sf = goc_uptr_from_sptr(abs);
    rt->current_stack_frame = sf;                   // OK：uptr 进堆字段
    // ... 使用期间用 goc_uptr_as_sptr(sf) 临时解引用 ...
    rt->current_stack_frame = /* unlink / 空 uptr */;
}

/* 非法：sptr / 栈绝对地址进堆 —— 编译错误（不会自动升成 uptr） */
void bad(cptr<JSRuntime> rt) {
    JSStackFrame sf_s;
    sptr<JSStackFrame> p = &sf_s;
    rt->current_stack_frame_abs = p;                // ERROR：sptr 逃逸，无自动升格
    rt->current_stack_frame_abs = &sf_s;            // ERROR：同上
}

/* gptr：独轨；写入 Go 堆槽自动写屏障 */
void use_go(gptr<GoObj> o, gptr<GoObj> *slot_on_go_heap) {
    *slot_on_go_heap = o;   // 编译器插入写屏障；o 不可与 cptr/sptr/uptr 混转
}
```

---

## 11. 与「完整 C」的差异清单（速查）

1. union 禁止含指针色。  
2. 指针分色：`cptr` / `sptr` / `uptr` / `auto_ptr`（`T *`）/ `gptr`；**无 `dsptr`**。  
3. 禁止指针↔整数等野转换（除内建）。  
4. 栈源绝对地址禁止入库；长寿栈引用须**显式** `uptr`（或 `auto_ptr` 一开始收成 `uptr`）；**`sptr` 逃逸 = 编译错误，无自动升格**。  
5. `sptr` 指针字禁止进堆字段；跨 morestack 须 spill 到 stackmap 槽。  
6. `gptr` 独轨 + stackmap + 写屏障；与其它色无隐式转换。  
7. 栈探测用方言 API，不用 C builtin。  
8. `alloca(n)` 降为函数作用域的 `cptr`（§7.2），不是栈上变长帧，也不是 `sptr`。  
9. 函数指针 / 导出 / 虚表 ABI 必须钉死色（禁止 `auto_ptr` / 裸 `T *`）。  
10. `JSValue` 为显式 struct（tag + 色指针字段），**不是** NaN-box scalar；禁止 ptr/double/int union 叠字。
11. `sptr` / `uptr` 绑定所属 goroutine（§8.3）。持有它们的 context 只能在那一条 goroutine 上调用，否则 offset 解到错误地址。
