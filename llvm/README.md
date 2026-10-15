# llvm/

goc's own LLVM code: one CMake project (`CMakeLists.txt`) built against
LLVM 19.1.7 by `scripts/build-passes.sh` into `build/llvm/`. The layout
follows LLVM's. Two products: `libGocPasses.so` (every IR pass, loaded with
`opt -load-pass-plugin`) and `goc-llc` (llc plus goc's machine passes).

| Path | What it is |
|------|------------|
| `include/Goc/` | Public entry points: `ColorEscape.h`, `StackMap.h`, `MachinePasses.h`, `StackCheck.h`, `AArch64Frame.h`, `SlotMD.h`, and `AbiProfiles.def` (Go runtime ABI constants per Go version) |
| `include/Goc/Internal/` | State shared by the files of one pass family (`ColorEscape.h`, `StackMap.h`); not an API |
| `lib/Analysis/GocColor/` | Pointer-color seeding and propagation (`ColorAnalysis.cpp`) and the `print<goc-color>` table (`ColorPrinter.cpp`) |
| `lib/Analysis/StackMap/` | Read-only safepoint, liveness and stack-address queries used by the stack-map passes |
| `lib/Transforms/ColorEscape/` | IR pass `goc-color-escape`: escape checks, implicit uptr encode/decode, dynamic alloca lowering, metadata |
| `lib/Transforms/StackMap/` | IR passes `goc-stackmap`, `goc-reanchor`, `goc-inline-gate`, `goc-name-blocks`, `goc-pin-i64`: stack-map roots and frame-address rematerialization |
| `lib/Plugin/` | `libGocPasses.so`: registers the passes above, plus `print<goc-color>` |
| `lib/CodeGen/` | Target-independent machine passes: the Go stack check and `GocStackmapPlacement` |
| `lib/Target/X86/` | x86 stack-check hook and `GocX86FrameAddrFix` (re-derive frame addresses after calls; lattice in `GocX86FrameAddrLattice.cpp`) |
| `lib/Target/AArch64/` | arm64 stack-check hook, frame layout (LR slot) and post-call repair |
| `lib/Object/` | `GocObjTables`: collects and writes the `.goc.*` tables of a `.gocobj` |
| `tools/goc-llc/` | The `goc-llc` driver only. LLVM 19's llc cannot load MachineFunction passes from a plugin, hence one codegen tool |

Examples:

```bash
opt -load-pass-plugin=build/llvm/libGocPasses.so -passes=goc-color-escape -S in.ll -o out.ll
opt -load-pass-plugin=build/llvm/libGocPasses.so -passes='print<goc-color>' -disable-output in.ll  # needs -g IR
```

The pipeline that strings these together is in `../ARCHITECTURE.md`;
FileCheck tests are in `../test/lit`.
