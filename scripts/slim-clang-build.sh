#!/usr/bin/env bash
# Drop the bulky parts of an LLVM build tree after the passes are built.
# Keeps the binaries, libLLVM, the clang resource dir, generated X86 .inc
# files, and a copy of the X86 headers the pass Makefile needs.
# GOC_KEEP_CLANG_BUILD=1 leaves the tree alone.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD="$ROOT/third_party/llvm-19.1.7-clang-build"
SRC="${LLVM_SRC:-$ROOT/third_party/llvm-project-19.1.7}"
[[ "${GOC_KEEP_CLANG_BUILD:-}" == 1 ]] && exit 0
[[ -d "$BUILD" ]] || exit 0

if [[ -d "$SRC/llvm/lib/Target/X86" && ! -f "$BUILD/x86-src/X86InstrInfo.h" ]]; then
  mkdir -p "$BUILD/x86-src"
  cp -a "$SRC/llvm/lib/Target/X86/." "$BUILD/x86-src/"
fi

# Static archives and object files dominate the tree. The shared library stays.
find "$BUILD" -type f \( -name '*.o' -o -name '*.a' \) -delete
rm -rf "$BUILD/tools" "$BUILD/utils" "$BUILD/unittests" "$BUILD/test" \
  "$BUILD/examples" "$BUILD/projects" "$BUILD/runtimes" || true
echo "slim-clang-build: $(du -sh "$BUILD" | awk '{print $1}') $BUILD"
