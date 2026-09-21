# P5 格式：pointer maps + 二进制 goobj

**Go：** go1.24.4 linux/amd64 · **stackguard0 = 16**  
**基线：** [../p2-goobj-min/FORMAT.md](../p2-goobj-min/FORMAT.md)

## 1. stackmap 字节

```text
offset 0  uint32 n
offset 4  uint32 nbit
offset 8  n × ceil(nbit/8) 字节 bitmap
```

| 图 | hex | 含义 |
|----|-----|------|
| LocalsPointerMaps | `02000000030000000404` | n=2 nbit=3；bit2 → SP+16 gptr（**MIR liveness**） |
| ArgsPointerMaps | `02000000010000000101` | n=2 nbit=1；bit0 → 首参 `*int`（**IR 指针形参**） |

符号：`gclocals·gocHoldArgs` / `gclocals·gocHoldLive`（nm 显示为 `gclocals.`）。

FUNCDATA：`$0` Args，`$1` Locals。

### 1.1 真源

| 图 | 真源 | 调试覆盖 |
|----|------|----------|
| Locals | `GocEmitPointerMaps` 对 MIR 的 forward dataflow（gptr FI 在 CALL safepoint 的 live 集） | 仅当 `goc-live-gptr-debug-override=1` 时读 `goc-live-gptr-sp-offs` |
| Args | IR `Function` 指针形参 → word bits | 同上 override 下可读 `goc-arg-ptr-*` |

## 2. goobj 写入路径（P5b）

| 层 | 实现 |
|----|------|
| 逻辑 | `goobj/binwriter` 读 pass maps/recipe，构造 `obj.Prog`（含 **StoreGptrWB**） |
| 序列化 | **`obj.WriteObjFile`**（经 GOROOT overlay 的 `cmd/internal/obj`） |
| 非主路径 | ~~`.s` + `go tool asm`~~（已弃用为主序列化；stubs 无 goc TEXT） |

产物：`build/goobj/goc_funcs.o`（`go object …` 头 + `\x00go120ld`）。

## 3. MachineFunctionPass 产物

| 文件 | 内容 |
|------|------|
| `build/pass-out/goc.mir` | **真** X86 MI（栈检查 + WB；非 INLINEASM） |
| `build/pass-out/stackcheck.recipe.txt` | `mi_path=real_x86_opcodes` + `mi_path=real_x86_opcodes_wb` |
| `build/pass-out/args_map.bin` / `locals_map.bin` | 上表字节 |
| `build/pass-out/maps.txt` | `locals_source mir_liveness` + algorithm/limits |
| `build/pass/libGocMachinePasses.so` / `goc-pass-driver` | pass 目标 |


## P6 maps 真源

| 图 | 真源 | 说明 |
|----|------|------|
| Locals | `liveintervals`（safepoint spill 槽的 Go SP 偏移） | 默认；`goc-live-gptr-sp-offs` 仅 debug override |
| Args | `mir_ir_args`（IR 指针形参） | |

### hold_live
`locals_hex 02000000030000000404` — nbit=3，bit2 → SP+16

### hold_two（≥2 活 gptr）
`locals_hex 02000000040000000c0c` — nbit=4，bits 2+3 → SP+16 与 SP+24。  
**禁止** FI 创建序×8（会得到 `0x03`）。

### goobj 路径（P6）
`goobj/binwriter` → vendored `goobj/enc/cmd/objlib` → `WriteObjFile`；**无** GOROOT overlay。
