# Frame layout

Stack check is real X86 MI: `MOV64rm FS:-8` loads `g`, `CMP64rm` compares
`SP` with `stackguard0` at offset 16, then `JCC_1 BE`, `CALL morestack_noctxt`,
and `JMP` back to the check.

`GocExpandStoreGptr` emits the write-barrier MI. Locals and args bitmaps come
from `GocEmitPointerMaps` and are packed as `gclocals·*` FUNCDATA.

## hold_two ($32 locals)

| SP offset | bit | contents |
|-----------|----:|----------|
| 16 | 2 | gptr0 spill |
| 24 | 3 | gptr1 spill |

Bitmap byte `0x0c`. Offsets come from the spill pass, not from frame-index rank times 8.

## hold_regonly ($24 locals)

| SP offset | bit | contents |
|-----------|----:|----------|
| 16 | 2 | the only gptr spill (register-only across a call) |

Bitmap byte `0x04`. An integer decoy in a GR64 is not a pointer slot.
