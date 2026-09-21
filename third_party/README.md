# Third-party dependencies (fetch only — not vendored)

This directory is intentionally empty of large blobs. Fetch and build locally;
paths under `third_party/` are gitignored except this README.

## LLVM / Clang 19.1.7

```bash
# Source tarball (example)
curl -L -o llvmorg-19.1.7.tar.gz \
  https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.tar.gz
tar xf llvmorg-19.1.7.tar.gz
# Expect directory: llvm-project-llvmorg-19.1.7  or rename to llvm-project-19.1.7

export LLVM_SRC=$PWD/llvm-project-19.1.7   # adjust name
$GOC_ROOT/scripts/apply-patches.sh "$LLVM_SRC"
# Then cmake/ninja as in ../clang/README.md
```

Host tools also useful from distro packages: `clang-19`, `llc-19`,
`llvm-config-19`, `cmake`, `ninja-build`.

## quickjs-ng (optional)

Full QuickJS embedding via goc is **not complete**. For experiments that need
the upstream tree:

```bash
git clone https://github.com/quickjs-ng/quickjs.git third_party/quickjs-ng
# Configure with JS_NAN_BOXING=0 when following P26 research notes
```

Do **not** commit the cloned tree. Earlier research phases (P23–P26) used
stub interpreters and limited real-QJS smokes; see `docs/phases/` and
`docs/roadmap.md`.

## CMake

Any recent CMake ≥ 3.20 works. You may unpack an official binary under
`third_party/cmake-*-linux-x86_64/` (gitignored) if your OS package is too old.
