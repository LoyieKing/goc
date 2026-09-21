# Building patched Clang for goc

goc’s product frontend is **Clang 19.1.7** with:

1. `Attr.td` entries for `goc_cptr` / `goc_sptr` / `goc_uptr` / `goc_auto_ptr` / `goc_gptr`
2. `SemaGocColors.cpp` — lowers attrs to `AnnotateAttr` and rejects `sptr` escape

## Option A — in-tree (recommended, P28)

```bash
# Fetch LLVM 19.1.7 (see ../third_party/README.md)
export LLVM_SRC=$HOME/src/llvm-project-19.1.7
export GOC_ROOT=/path/to/goc

./scripts/apply-patches.sh "$LLVM_SRC"

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  -S "$LLVM_SRC/llvm" \
  -B "$GOC_ROOT/third_party/llvm-19.1.7-clang-build"

ninja -C "$GOC_ROOT/third_party/llvm-19.1.7-clang-build" -j$(nproc) clang

export GOC_CLANG="$GOC_ROOT/third_party/llvm-19.1.7-clang-build/bin/clang"
./cmd/goc test --p28
```

**Notes:**

- Prefer clang-19 as the host compiler (GCC may choke on Clang-only warning flags).
- Build directory stays under `third_party/` (gitignored); do not commit binaries.
- Expect ~30–60 minutes for a Release clang build depending on hardware.

## Option B — out-of-tree plugin (P27 legacy)

```bash
make -C clang/plugin all
export CLANG=clang-19
./cmd/goc test --p27
# or: ./cmd/goc cc -emit-llvm -S file.c
```

The plugin registers the same attribute spellings and Sema escape checks when
loaded with `-fplugin=libGocClang.so`.

## Layout

| Path | Contents |
|------|----------|
| `patches/0001-Attr.td-goc-colors.patch` | Attr.td delta |
| `patches/Attr.td.goc-excerpt.txt` | Human-readable Attr excerpt |
| `patches/0001-SemaGocColors-real.patch` | P28 Sema wiring |
| `patches/0002-SemaGocColors-stub.patch` | Early stub (superseded by real) |
| `sema/SemaGocColors.cpp` | Drop-in Sema source |
| `plugin/GocClangPlugin.cpp` | Out-of-tree plugin |

## Third-party notice

LLVM/Clang remain under Apache-2.0 WITH LLVM-exception. See repo root `NOTICE`.
