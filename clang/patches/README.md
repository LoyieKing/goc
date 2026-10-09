# Clang in-tree patches

`scripts/apply-patches.sh` applies these to an LLVM 19.1.7 tree. `scripts/build-clang.sh` does that before configuring. A patch that does not apply stops the build.

| Patch | Target |
|-------|--------|
| `0001-Attr.td-goc-colors.patch` | `Attr.td` — `GocCPtr/SPtr/UPtr/AutoPtr/GPtr` |
| `../sema/SemaGocColors.cpp` | Copied to `lib/Sema/SemaGocColors.cpp` |
| `0003-mirror-goc-colors-before-codegen.patch` | `ParseAST.cpp` mirrors colors before CodeGen |
| `0004-wire-sema-goc-colors.patch` | `CMakeLists.txt`, `Sema.h`, end-of-TU call in `Sema.cpp` |

`0001-SemaGocColors-real.patch` and `0002-SemaGocColors-stub.patch` are earlier snapshots. The build copies `clang/sema/SemaGocColors.cpp` instead of applying them.

The out-of-tree plugin `../plugin/GocClangPlugin.cpp` is only for `goc test --p27`.
