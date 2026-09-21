# P5 帧布局（复用 P4 合同）

详见 [../p4-unified-lower/FRAME_LAYOUT.md](../p4-unified-lower/FRAME_LAYOUT.md)。

P5b 差异：

- 栈检查由 `GocInsertStackCheck` 插入 **真 X86 MI**（`MOV64rm FS:-8` → g，`CMP64rm` vs `stackguard0@16`，`JCC_1 BE`，`CALL64pcrel32 morestack_noctxt`，`JMP_1` 重入）。见 [docs/MI_AND_GOOBJ.md](./docs/MI_AND_GOOBJ.md)。  
- 写屏障由 `GocExpandStoreGptr` 发真 MI；**StoreGptrWB 体在 binwriter Prog**（非 stubs）。  
- Locals / Args bitmap 由 `GocEmitPointerMaps` **MIR liveness** 写出；binwriter 打成 `gclocals·*` FUNCDATA。


## P6 hold_two（$32 本地区）

| SP 偏移 | bit | 内容 |
|---------|-----|------|
| 16 | 2 | gptr0 spill |
| 24 | 3 | gptr1 spill |

bitmap 字节 `0x0c`。由 spill pass 按 Go 布局分配，**非** FI 序号×8。

## P6.1 hold_regonly（$24 本地区，S3）

| SP 偏移 | bit | 内容 |
|---------|-----|------|
| 16 | 2 | 唯一 gptr spill（寄存器-only 活跨 CALL） |

bitmap 字节 `0x04`。integer decoy GR64 **不**占位。
