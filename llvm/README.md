# llvm/

goc's own LLVM code: one CMake project (`CMakeLists.txt`) built against
LLVM 19.1.7 by `scripts/build-passes.sh` into `build/llvm/`. The layout
follows LLVM's.

| Path | What it is |
|------|------------|
| `include/Goc/` | Headers shared by the passes, and `AbiProfiles.def` (Go runtime ABI constants per Go version) |
| `lib/Transforms/ColorEscape/` | IR pass `goc-color-escape`: pointer-color inference, escape checks, implicit uptr encode/decode, dynamic alloca lowering |
| `lib/Transforms/StackMap/` | IR passes `goc-stackmap`, `goc-reanchor`, `goc-inline-gate`, `goc-name-blocks`, `goc-pin-i64`: stack-map roots and frame-address rematerialization |
| `lib/Plugin/` | `libGocPasses.so`, the new-PM plugin that registers the IR passes for `opt -load-pass-plugin` |
| `lib/CodeGen/` | Target-independent Go stack check (which functions need one, small/big/huge sequences, labels for the tables) |
| `lib/Target/X86/`, `lib/Target/AArch64/` | Per-architecture hooks: the stack-check instructions, and the arm64 frame layout and post-call repair |
| `tools/goc-llc/` | `goc-llc`: llc plus the goc machine passes and the `.goc.*` table writer. LLVM 19's llc cannot load MachineFunction passes from a plugin, hence one codegen tool |

The pipeline that strings these together is in `../ARCHITECTURE.md`;
FileCheck tests are in `../test/lit`.
