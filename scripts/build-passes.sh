#!/usr/bin/env bash
# Build goc-color-escape, goc-llc and GocStackMap.so against the patched clang.
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

COLOR="$ROOT/frontend/color-escape/build/goc-color-escape"
LLC="$ROOT/backend/build/pass-out/goc-llc"
SM="$ROOT/backend/build/pass-out/GocStackMap.so"

if [[ "${GOC_REBUILD_PASSES:-}" != 1 && -x "$COLOR" && -x "$LLC" && -f "$SM" ]]; then
  echo "build-passes: already built"
  exit 0
fi

libdir="$("$LLVM_CFG" --libdir)"
if [[ -e "$libdir/libLLVM-19.so" || -e "$libdir/libLLVM-19.a" ]]; then
  LIBS="-lLLVM-19"
elif [[ -e "$libdir/libLLVM.so" || -e "$libdir/libLLVM.so.19.1" ]]; then
  LIBS="-lLLVM"
else
  echo "build-passes: no libLLVM in $libdir" >&2
  exit 1
fi

make -C "$ROOT/frontend/color-escape/pass" all \
  CXX="$CXX" LLVM_CFG="$LLVM_CFG" LIBS="$LIBS"
make -C "$ROOT/backend/pass" \
  CXX="$CXX" LLVM_CFG="$LLVM_CFG" LIBS="$LIBS" \
  LLVM_X86_SRC="$X86_SRC" LLVM_X86_BUILD="$X86_BUILD" \
  "$SM" "$LLC"

[[ -x "$COLOR" && -x "$LLC" && -f "$SM" ]] || {
  echo "build-passes: outputs missing" >&2
  exit 1
}
echo "build-passes: ready"
