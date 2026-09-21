> **Public export note:** paths in this guide refer to the `goc` repository
> (`include/goc.h`, `cmd/goc`, `docs/`). The language contract is unchanged.

# goc 语言语法指导（v0.2）

**状态：** 已批准合同（2026-09-21：用户 + review 一致通过）  
**定位：** goc = 面向 Go 运行时的 C 方言：在 goroutine 用户栈上跑，遵守 Go 的 stackmap / morestack /（对 `gptr` 的）写屏障，同时保留接近 C 的代码形态。  
**非目标：** 完整 ISO C；任意 libc 语义；通用「加速所有 cgo」。

本文只描述**语法与类型合同**。实现分期、QJS 移植工程项见 [roadmap.md](roadmap.md)。  
相对 v0.1：**删除 `dsptr` 语言表面**；引入显式 `sptr` / `uptr` / `auto_ptr`；`JSValue` 改为显式 struct（不再 NaN-box 整盒 scalar）。

---

## 1. 语言轮廓

| 项 | 约定 |
|---|---|
| 基线 | C11 子集（控制流、表达式、struct、固定数组、`_Alignas`、内联等） |
| 表面扩展 | 指针色类型（`cptr` / `sptr` / `uptr` / `auto_ptr` / `gptr`）、方言内建/属性、与 Go 的导入导出声明 |
| 禁止/推迟 | 含指针的 union；用户级 tagged pointer；随意指针↔整数；VLA/`alloca`（见 §7）；跨 Go 帧的 `setjmp`/`longjmp`；在 goc 里起 OS 线程再回调 Go；**`dsptr`（已移出语言表面，由 `uptr` 覆盖）** |

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
| `sptr<T>` | 当前 g 栈对象指针 | 被指物在栈上。指针*字*只许在栈槽/寄存器；**禁止进堆字段**。直接寻址。跨 morestack：寄存器须 spill 到 stackmap 跟踪的槽。 |
| `uptr<T>` | `cptr\|sptr` 编码并集 | 可存任意处（含堆字段）。**不可直接解引用**；须先 decode。 |
| `auto_ptr<T>` | 推断色；`T *` ≡ `auto_ptr<T>` | 逃逸敏感：优先 `cptr`/`sptr`；须入库则一开始收成 `uptr`。已钉 `sptr` 后逃逸 → 报错，无自动升格。 |
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
- `cptr` ↔ 整数 / 浮点 / `gptr` / `sptr`：默认禁止（`sptr`→`cptr` 仅当证明非栈源或经错误路径）。  
- 解引用、算术：允许（结果仍为 `cptr`；越界 UB，同 C）。  
- **stackmap：** 指针槽可进 map；搬栈时若值落在旧栈 `[lo,hi)` 才 `+delta`——但按合同合法 `cptr` 不应落在栈上，故通常不动。  
- **写屏障：** 不走 Go 写屏障。

**指针字存放：** 栈槽、寄存器、堆字段、全局均可。

### 3.2 `sptr<T>`

**值域：** `NULL`，或指向**当前 owner g** 用户栈上的 `T`。

**操作：**

- 解引用、算术：允许（结果仍为 `sptr`；越界 UB）。  
- **stackmap：** 进 map；搬栈时对落在旧栈的值 `+delta`。  
- **写屏障：** 无。  
- 与 `cptr` / `gptr`：无隐式转换。

**指针字存放（硬规则）：**

> `sptr` 的指针*字*只许出现在**栈槽与寄存器**。  
> **禁止**写入堆字段、全局、或其它可被堆持有的位置。  
> 跨 morestack：寄存器中的 `sptr` **必须** spill 到 stackmap 跟踪的栈槽，再由运行时搬栈修正。

需要把「指向栈对象」的引用挂进 runtime / 堆 → 编码为 `uptr`（§3.3），不得存绝对 `sptr`。

### 3.3 `uptr<T>`

用于「逻辑上可能是 `cptr` 或 `sptr`，且需要存进堆字段 / 长寿位置」的场景（典型：`JSStackFrame` 挂在 runtime 上）。

| | 规则 |
|---|---|
| 表示 | MSB=0：绝对 `cptr` 地址，直接当地址用；MSB=1：相对 `g.stack.hi` 的 **int64 二进制补码 offset**，`abs = stack.hi + stored` |
| 可存堆 | **允许**（Go 堆 / C 堆 / 全局字段） |
| 解引用 | **禁止直接 `*`**；须先 decode 成临时 `cptr` 或 `sptr` |
| 搬栈 | 堆上 MSB=1 的 offset **不变**；MSB=0 的绝对 `cptr` 通常不在栈上故不动 |
| 寿命 | **不延长**栈对象寿命；帧返回前必须 unlink / 清空 |
| 解码 | 仅用 **owner g** 的 `stack.hi`；跨 g 解码 = **错误** |

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
3. 两色都可能、或必须进堆字段 → **一开始就**收成 `uptr<T>`  
4. **实例敏感：** 若某 struct 类型在别处有堆用法、字段类型写的是 `auto_ptr`，但**本函数内该实例只活在栈上**，仍可收成 `cptr`/`sptr`，**不要**盲目整类型升成 `uptr`。  
5. 若分析曾把某值收成 `sptr`，随后又发现逃逸 → **报错**（程序员应改成显式 `uptr` / 调整 `auto_ptr` 用法），**禁止**自动升格。

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

**逃逸硬规则：**

> 栈源**绝对地址**（已钉成 `sptr` 的值，或尚未编码的栈绝对字）**不得**存入堆字段、全局、或作为返回值逃出。  
> **`sptr` 一旦逃逸 → 一律编译错误。禁止「逃逸时自动升格为 `uptr`」。**  
> 需要把栈引用挂进堆 / 长寿位置时：  
> - 在类型/写法上就用 **`auto_ptr`**（由染色一开始收成 `uptr`），或  
> - **显式**调用 `goc_uptr_from_sptr`（等）写成 `uptr` 再入库。  
> 已经写成 `sptr` 的值，编译器**不会**在逃逸点偷偷改色。

---

## 4. `T *` / `auto_ptr` 与 ABI

### 4.1 含义随位置变化

| 位置 | `T *` / `auto_ptr<T>` |
|---|---|
| **函数形参**（goc TU 内） | 色模板 / 推断：可接收 `cptr` 或 `sptr`（推荐同色联锁）；必要时收成 `uptr`；**不接收 `gptr`** |
| 返回值 / 局部 / 结构体字段 / 全局 | 按逃逸精化；字段若可能被堆持有且含栈引用 → 须一开始就是 `uptr`/`auto_ptr→uptr`；**`sptr` 逃逸 = 报错**（无自动升格） |

```c
void foo(int *out);           // 形参 auto_ptr：可吃 &local（sptr）或堆缓冲（cptr）
int *p;                       // 局部：推断
struct S { int *q; };         // 若 S 实例可能进堆：q 不得收绝对 sptr；用 uptr 或保证非栈
```

### 4.2 形参模板体规则（推荐）

- 同色联锁：一函数内多个 `T *`/`U *` 形参默认同一精化色，避免 `2^n` 爆炸。  
- 写穿 `*p`、读、再传入其它 `U *` 形参：同色实例均允许。  
- **仅非栈源（`cptr`）或已 `uptr` 编码的实例允许：** 把指针字自身存进堆字段 / 全局 / 返回。  
- 栈源 `sptr` 若直接入库 → **编译错误**（或强制改写为 `uptr` 编码，见 §3.6）。  
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
| `sptr` | 禁止 | 禁止 | 允许 | `goc_uptr_from_sptr` | 禁止 |
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

禁止依赖 `__builtin_frame_address` / 手写栈极限假设。使用 goc 提供的 API（名称待定），例如：

```c
uintptr_t goc_stack_hi(void);
uintptr_t goc_stack_lo(void);
bool      goc_stack_check(size_t need);   // 与 morestack 合同一致
```

### 7.2 `alloca` / VLA

- **当前：** 不作为一等优化；移植代码暂用 `malloc` + `free`（或方言堆分配）替代。  
- **未来：** 可恢复受限栈分配（结果为 `sptr`，走 §3.6 逃逸规则）。

### 7.3 分配器

C 堆：`malloc`/`free` 或 `goc_alloc` 族 → `cptr`。  
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

### 8.3 Owner goroutine

「一 JS runtime 一条 owner g」是 **引擎实现纪律**，不是 goc 语法条款；goc 不为此增加关键字。方言只保证：`sptr` 与 `uptr`（MSB=1）的解码相对 **owner / 当前 g** 的 `stack.hi`；跨 g = 错误。

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
8. `alloca` 非一等公民（现阶段降为堆分配）。  
9. 函数指针 / 导出 / 虚表 ABI 必须钉死色（禁止 `auto_ptr` / 裸 `T *`）。  
10. `JSValue` 为显式 struct（tag + 色指针字段），**不是** NaN-box scalar；禁止 ptr/double/int union 叠字。

---

## 12. 未决 / 未来项（不阻塞 v0.2 合同）

| 项 | 状态 |
|---|---|
| `uptr` 编码：MSB=1 时 offset 相对 `stack.hi`（已钉）vs 其它 base | **已钉 `stack.hi`**；实现按此 |
| ~~逃逸时自动 `uptr` 提升~~ | **已否决**：`sptr` 逃逸一律编译错误；入库用显式 `uptr` / `auto_ptr` 预收 |
| 形参模板：真单态 vs 借用检查一份码 | 推荐默认同色联锁单态 |
| `alloca` 一等支持 | 未来优化；结果为 `sptr` |
| 编译器强制 safepoint | 可选，非必做 |
| 属性确切拼写 / `goc.h` 最终 API 名 | 实现期定 |
| `JSValue.tagged_value` 宽度（int / int32 / int64）与立即数布局 | ABI 钉死即可；不回退 NaN-box |
| `JSValue.pointer` 默认钉 `cptr` 还是可空 / 按 tag 变体 | 推荐对象载荷 `cptr<JSObject>`；实现可再收紧 |

---

## 13. 文档修订

| 版本 | 日期 | 摘要 |
|---|---|---|
| 0.1 | 2026-09-20 | 首版：色、`T *`、`dsptr`、逃逸、Go 非对称互操作、栈 API、JSValue scalar（NaN-box） |
| 0.2 | 2026-09-21 | **批准合同：** 删除 `dsptr`；显式 `cptr`/`sptr`/`uptr`/`auto_ptr`/`gptr`；`uptr` MSB 编码（hi + int64 offset）；ABI 禁 auto_ptr；`JSValue` 改显式 struct |
| 0.2.1 | 2026-09-21 | **`sptr` 逃逸 = 编译错误**；禁止逃逸自动升格 `uptr`；入库须显式 `uptr` 或 `auto_ptr` 预收成 `uptr` |

**相关：** `docs/roadmap.md`（产品与分期；其中旧「三色 / NaN-box」叙述以本指南 v0.2 为准）、`glossary.md`（术语；指针色条目已对齐）、`qjs-addr-of-local-analysis.md`（QJS `&local` 实证，历史分析）。
