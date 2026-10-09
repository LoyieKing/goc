#!/usr/bin/env bash
# Build the patched Clang 19.1.7 that ships inside a goc release.
# Writes third_party/llvm-19.1.7-clang-build. Skips the build when bin/clang,
# opt, llc, llvm-mc and llvm-objdump are already there, unless GOC_REBUILD_CLANG=1.
#
# Host tools: clang-19 or clang++-19 (gcc if those are absent), cmake, ninja, curl.
# Targets: X86 and AArch64, shared libLLVM, clang, opt, llc, llvm-mc,
# llvm-objdump, llvm-config, and ld.lld.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
BUILD="$ROOT/third_party/llvm-19.1.7-clang-build"
SRC="${LLVM_SRC:-$ROOT/third_party/llvm-project-19.1.7}"
JOBS="${JOBS:-$(nproc)}"

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "build-clang: missing $1" >&2; exit 1; }
}

if [[ "${GOC_REBUILD_CLANG:-}" != 1 \
    && -x "$BUILD/bin/clang" && -x "$BUILD/bin/opt" && -x "$BUILD/bin/llc" \
    && -x "$BUILD/bin/llvm-mc" && -x "$BUILD/bin/llvm-objdump" \
    && -e "$BUILD/lib/libLLVM.so.19.1" ]]; then
  echo "build-clang: already built ($BUILD/bin/clang)"
  exit 0
fi

need cmake
need ninja
need curl
need tar

CC="${CC:-}"
CXX="${CXX:-}"
if [[ -z "$CC" ]]; then
  if command -v clang-19 >/dev/null 2>&1; then CC="$(command -v clang-19)"
  elif command -v clang >/dev/null 2>&1; then CC="$(command -v clang)"
  else CC="$(command -v gcc)"
  fi
fi
if [[ -z "$CXX" ]]; then
  if command -v clang++-19 >/dev/null 2>&1; then CXX="$(command -v clang++-19)"
  elif command -v clang++ >/dev/null 2>&1; then CXX="$(command -v clang++)"
  else CXX="$(command -v g++)"
  fi
fi
echo "build-clang: host CC=$CC CXX=$CXX"

if [[ ! -f "$SRC/llvm/CMakeLists.txt" ]]; then
  mkdir -p "$ROOT/third_party"
  tarball="$ROOT/third_party/llvmorg-19.1.7.tar.gz"
  if [[ ! -f "$tarball" ]]; then
    curl -L --fail -o "$tarball" \
      https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.tar.gz
  fi
  tar -C "$ROOT/third_party" -xf "$tarball"
  extracted="$ROOT/third_party/llvm-project-llvmorg-19.1.7"
  if [[ -d "$extracted" && ! -d "$SRC" ]]; then
    mv "$extracted" "$SRC"
  fi
  [[ -f "$SRC/llvm/CMakeLists.txt" ]] || {
    echo "build-clang: LLVM source missing after extract ($SRC)" >&2
    exit 1
  }
fi

if [[ ! -f "$SRC/.goc-patches-applied" ]]; then
  "$ROOT/scripts/apply-patches.sh" "$SRC"
  touch "$SRC/.goc-patches-applied"
fi

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=ON \
  -DLLVM_PARALLEL_LINK_JOBS="${LLVM_PARALLEL_LINK_JOBS:-1}" \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  -S "$SRC/llvm" \
  -B "$BUILD"

ninja -C "$BUILD" -j"$JOBS" clang opt llc llvm-mc llvm-objdump llvm-config lld

[[ -x "$BUILD/bin/clang" && -x "$BUILD/bin/opt" && -e "$BUILD/lib/libLLVM.so.19.1" ]] || {
  echo "build-clang: build finished without bin/clang or libLLVM.so.19.1" >&2
  exit 1
}
# lld's driver is bin/lld. The name realbody looks up is ld.lld.
if [[ -x "$BUILD/bin/lld" && ! -e "$BUILD/bin/ld.lld" ]]; then
  ln -sfn lld "$BUILD/bin/ld.lld"
fi
echo "build-clang: ready $BUILD/bin/clang"
