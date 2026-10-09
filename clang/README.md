# Building patched Clang for goc

goc’s product frontend is **Clang 19.1.7** with:

1. `Attr.td` entries for `goc_cptr` / `goc_sptr` / `goc_uptr` / `goc_auto_ptr` / `goc_gptr`
2. `SemaGocColors.cpp` — lowers attrs to `AnnotateAttr` and rejects `sptr` escape

## Option A — in-tree (recommended)

The supported build is one script. It downloads LLVM 19.1.7, applies the patches, and writes `third_party/llvm-19.1.7-clang-build`. The driver finds `bin/clang` there. There is no path to pass.

```bash
./scripts/build-clang.sh
./scripts/build-passes.sh
./cmd/goc test --p28
```

The whole release archive (goc plus this Clang) is `scripts/pack-release.sh`. See [docs/build-from-source.md](../docs/build-from-source.md).

**Notes:**

- Prefer clang-19 as the host compiler (GCC may choke on Clang-only warning flags).
- Build directory stays under `third_party/` (gitignored); do not commit binaries.
- Expect ~30–60 minutes for a Release clang build depending on hardware.

## Option B — out-of-tree plugin

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
| `patches/0003-mirror-goc-colors-before-codegen.patch` | Mirror colors in ParseAST before CodeGen |
| `patches/0004-wire-sema-goc-colors.patch` | CMakeLists.txt, Sema.h, and the end-of-TU call |
| `sema/SemaGocColors.cpp` | Drop-in Sema source |
| `plugin/GocClangPlugin.cpp` | Out-of-tree plugin (`goc test --p27`) |

## Third-party notice

LLVM/Clang remain under Apache-2.0 WITH LLVM-exception. See repo root `NOTICE`.
