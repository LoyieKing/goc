# Clang in-tree patches

`scripts/apply-patches.sh` applies these to an LLVM 19.1.7 tree. `scripts/build-clang.sh` does that before configuring. A patch that does not apply stops the build.

| Patch | Target |
|-------|--------|
| `0001-Attr.td-goc-colors.patch` | `Attr.td` — `GocCPtr/SPtr/UPtr/AutoPtr/GPtr` |
| `../sema/SemaGocColors.cpp` | Copied to `lib/Sema/SemaGocColors.cpp` |
| `0003-mirror-goc-colors-before-codegen.patch` | `ParseAST.cpp` mirrors colors before CodeGen |
| `0004-wire-sema-goc-colors.patch` | `CMakeLists.txt`, `Sema.h`, end-of-TU call in `Sema.cpp` |

What the Sema part does:

| Change | File |
|--------|------|
| Sema checks | `lib/Sema/SemaGocColors.cpp`: mirrors the Goc* attributes to `AnnotateAttr` (what the IR passes read) and reports sptr escapes |
| Mirror before CodeGen | `lib/Parse/ParseAST.cpp` calls `MirrorGocColorAttrs` before `HandleTopLevelDecl`; the end-of-TU call in `Sema.cpp` only diagnoses, it is too late for IR |

The out-of-tree plugin `../plugin/GocClangPlugin.cpp` is only for `goc test --plugin`.
