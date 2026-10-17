# clang/

goc's frontend is Clang 19.1.7 with a few patches. They add the pointer-color attributes and a Sema step that checks them and passes them on to the IR.

| Path | What it changes |
|---|---|
| `patches/0001-Attr.td-goc-colors.patch` | `Attr.td`: the `GocCPtr`, `GocSPtr`, `GocUPtr`, `GocAutoPtr`, `GocGPtr` attributes (`include/goc.h` spells them `cptr(T)` and so on) |
| `sema/SemaGocColors.cpp` | Copied to `lib/Sema/`: mirrors the Goc* attributes to `AnnotateAttr` (what the IR passes read) and reports sptr escapes, such as returning the address of a local |
| `patches/0003-mirror-goc-colors-before-codegen.patch` | `ParseAST.cpp` mirrors the colors before `HandleTopLevelDecl`. CodeGen is incremental, so the end-of-TU call is too late for IR and only diagnoses |
| `patches/0004-wire-sema-goc-colors.patch` | `CMakeLists.txt`, `Sema.h`, and the end-of-TU call in `Sema.cpp` |

`scripts/build-clang.sh` downloads LLVM 19.1.7, applies these in this order (a patch that does not apply stops the build) and builds into `third_party/llvm-19.1.7-clang-build`, where goc finds it. See [docs/build-from-source.md](../docs/build-from-source.md). Use clang-19 as the host compiler if you can; GCC may reject Clang-only warning flags.

What the colors mean is [docs/language.md](../docs/language.md); how they reach the IR is [docs/design/frontend-ir.md](../docs/design/frontend-ir.md) §1. Sema tests are in [test/lit/Sema](../test/lit/Sema).

LLVM/Clang is Apache-2.0 WITH LLVM-exception; see [NOTICE](../NOTICE).
