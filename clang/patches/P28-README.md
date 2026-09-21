# P28 in-tree Clang patches

Applied under `_deps/llvm-project-19.1.7/clang/`:

| Change | File |
|--------|------|
| Real Sema | `lib/Sema/SemaGocColors.cpp` (mirror Goc*→AnnotateAttr + sptr escape) |
| Wire TU end | `lib/Sema/Sema.cpp` calls `DiagnoseGocColorEscapes()` |
| Declare | `include/clang/Sema/Sema.h` |
| CMake | `lib/Sema/CMakeLists.txt` adds `SemaGocColors.cpp` |
| Attrs | `include/clang/Basic/Attr.td` Goc* (from P27; SimpleHandler) |

Rebuild: see `../PLAN.md` / `../build-clang.log`.
