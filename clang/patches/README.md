# Clang in-tree patches (P27)

These patches document the **intended** in-tree Clang modification. The working
frontend today is the out-of-tree plugin `../plugin/GocClangPlugin.cpp` loaded
with `clang-19 -fplugin=libGocClang.so` (same attribute spellings + Sema escape).

| Patch | Target | Status |
|-------|--------|--------|
| `0001-Attr.td-goc-colors.patch` | `Attr.td` — `GocCPtr/SPtr/UPtr/AutoPtr/GPtr` | Applied under `_deps/llvm-project-19.1.7/clang/` |
| `0002-SemaGocColors-stub.patch` | `Sema/SemaGocColors.cpp` stub | Applied (placeholder until rebuild) |

## Rebuild clang with in-tree attrs (optional, long)

```bash
# from goc-docs/_deps
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF \
  -S llvm-project-19.1.7/llvm -B llvm-19.1.7-clang-build
ninja -C llvm-19.1.7-clang-build -j8 clang
# expect: tens of minutes – hours depending on machine
```

Until that binary exists, `bin/goc build` uses system `clang-19` + `libGocClang.so`.
