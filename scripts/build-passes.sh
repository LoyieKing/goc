#!/usr/bin/env bash
# Build goc-llc and libGocPasses.so (CMake, llvm/CMakeLists.txt)
# against the patched clang's LLVM.
# Skips a pass that is already executable unless GOC_REBUILD_PASSES=1.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
BUILD="$ROOT/third_party/llvm-19.1.7-clang-build"
SRC="${LLVM_SRC:-$ROOT/third_party/llvm-project-19.1.7}"
LLVM_CFG="${LLVM_CFG:-$BUILD/bin/llvm-config}"
# Passes are ordinary LLVM C++. The patched clang diagnoses every
# `return &local` as an sptr escape, including the returns inside LLVM
# headers, so it cannot compile these files. Use the same host compiler
# that build-clang.sh used to build libLLVM.
CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
  if command -v clang++-19 >/dev/null 2>&1; then CXX="$(command -v clang++-19)"
  elif command -v clang++ >/dev/null 2>&1; then CXX="$(command -v clang++)"
  else CXX="$(command -v g++ || true)"
  fi
fi
LLC="$ROOT/build/llvm/goc-llc"
SM="$ROOT/build/llvm/libGocPasses.so"

if [[ "${GOC_REBUILD_PASSES:-}" != 1 && -x "$LLC" && -f "$SM" ]]; then
  echo "build-passes: already built"
  exit 0
fi

[[ -x "$LLVM_CFG" ]] || { echo "build-passes: missing $LLVM_CFG (run scripts/build-clang.sh)" >&2; exit 1; }
[[ -n "$CXX" && -x "$CXX" ]] || { echo "build-passes: missing a C++ compiler" >&2; exit 1; }

X86_SRC="$SRC/llvm/lib/Target/X86"
X86_BUILD="$BUILD/lib/Target/X86"
# A slimmed CI tree copies the headers next to the build.
if [[ ! -f "$X86_SRC/X86InstrInfo.h" && -f "$BUILD/x86-src/X86InstrInfo.h" ]]; then
  X86_SRC="$BUILD/x86-src"
fi
[[ -f "$X86_SRC/X86InstrInfo.h" && -f "$X86_BUILD/X86GenInstrInfo.inc" ]] || {
  echo "build-passes: missing X86 headers ($X86_SRC / $X86_BUILD)" >&2
  exit 1
}


# The passes must be built against the LLVM that loads them (no RTTI,
# libLLVM.so.19.1 of this build). Without its CMake package, find_package
# would quietly take a system LLVM 19 (built with RTTI): the plugin then
# references typeinfo symbols opt does not export and fails to load.
LLVM_CMAKEDIR="$("$LLVM_CFG" --cmakedir)"
[[ -f "$LLVM_CMAKEDIR/LLVMConfig.cmake" ]] || {
  echo "build-passes: no LLVMConfig.cmake in $LLVM_CMAKEDIR (a slimmed clang build lacks it; see scripts/slim-clang-build.sh)" >&2
  exit 1
}

command -v cmake >/dev/null || { echo "build-passes: cmake not found" >&2; exit 1; }
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
# GOC_CCACHE=1 (CI): see build-clang.sh.
[[ "${GOC_CCACHE:-0}" == 1 ]] && GEN+=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
cmake -S "$ROOT/llvm" -B "$ROOT/build/llvm" "${GEN[@]}" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$CXX" \
  -DLLVM_DIR="$LLVM_CMAKEDIR" -DGOC_EXPECT_LLVM_DIR="$LLVM_CMAKEDIR" \
  -DGOC_X86_SRC="$X86_SRC" -DGOC_X86_BUILD="$X86_BUILD"
cmake --build "$ROOT/build/llvm"

[[ -x "$LLC" && -f "$SM" ]] || {
  echo "build-passes: outputs missing" >&2
  exit 1
}
echo "build-passes: ready"
