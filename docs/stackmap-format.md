# Pointer maps and goobj

amd64. `stackguard0` is at offset 16 in `g`.

## stackmap bytes

```text
offset 0  uint32 n
offset 4  uint32 nbit
offset 8  n * ceil(nbit/8) bytes of bitmap
```

FUNCDATA `$0` is ArgsPointerMaps. FUNCDATA `$1` is LocalsPointerMaps.
Symbols use the `gclocals·` prefix so the linker accepts them.

Locals bits come from the LLVM stack map records requested by
`GocStackMap.so` (live gptr/sptr slots at each call), parsed by elfpack's
`parseLLVMStackMaps`. Args bits come from IR pointer parameters.

A bit `i` means the word at `SP+i*8`. Do not derive the offset from frame-index
creation order.

## goobj

`goobj/elfpack` writes the object the Go linker accepts. The encoder lives in
`goobj/enc/` and does not overlay `GOROOT`.
