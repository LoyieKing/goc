#!/usr/bin/env bash
# Apply goc Clang patches to an LLVM 19.1.7 source tree.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
LLVM_SRC="${1:-${LLVM_SRC:-}}"

if [[ -z "$LLVM_SRC" ]]; then
  cat <<'HINT'
Usage: ./scripts/apply-patches.sh /path/to/llvm-project-19.1.7

Or set LLVM_SRC. Expected layout:
  $LLVM_SRC/clang/include/clang/Basic/Attr.td
  $LLVM_SRC/clang/lib/Sema/...

Patches applied (in order):
  1. clang/patches/0001-Attr.td-goc-colors.patch
  2. clang/patches/0001-SemaGocColors-real.patch  (P28 real Sema)
  3. Copy clang/sema/SemaGocColors.cpp over clang/lib/Sema/SemaGocColors.cpp
  4. clang/patches/0003-mirror-goc-colors-before-codegen.patch

See clang/README.md for cmake/ninja build instructions.
HINT
  exit 2
fi

LLVM_SRC="$(cd "$LLVM_SRC" && pwd)"
CLANG_ROOT="$LLVM_SRC/clang"
if [[ ! -f "$CLANG_ROOT/include/clang/Basic/Attr.td" ]]; then
  echo "error: Attr.td not found under $CLANG_ROOT" >&2
  exit 1
fi

ATTR_PATCH="$ROOT/clang/patches/0001-Attr.td-goc-colors.patch"
SEMA_PATCH="$ROOT/clang/patches/0001-SemaGocColors-real.patch"

echo "=== Applying Attr.td goc colors ==="
# Patches may be written against clang/ relative or repo-relative paths.
# Try several -p levels.
apply_flex() {
  local patch="$1" dir="$2"
  local p
  for p in 1 2 3 0; do
    if (cd "$dir" && patch -p"$p" --dry-run -i "$patch" >/dev/null 2>&1); then
      (cd "$dir" && patch -p"$p" -i "$patch")
      return 0
    fi
  done
  # Fallback: apply from llvm-project root
  for p in 1 2 3 0; do
    if (cd "$LLVM_SRC" && patch -p"$p" --dry-run -i "$patch" >/dev/null 2>&1); then
      (cd "$LLVM_SRC" && patch -p"$p" -i "$patch")
      return 0
    fi
  done
  return 1
}

if ! apply_flex "$ATTR_PATCH" "$CLANG_ROOT"; then
  echo "warning: Attr.td patch failed to apply cleanly — inspect clang/patches/" >&2
  echo "         You may need to merge Attr.td.goc-excerpt.txt manually." >&2
fi

echo "=== Applying SemaGocColors real patch ==="
if ! apply_flex "$SEMA_PATCH" "$CLANG_ROOT"; then
  echo "warning: Sema patch failed — installing SemaGocColors.cpp manually" >&2
fi

echo "=== Installing clang/sema/SemaGocColors.cpp ==="
cp -a "$ROOT/clang/sema/SemaGocColors.cpp" "$CLANG_ROOT/lib/Sema/SemaGocColors.cpp"

echo "=== Applying early goc color mirror ==="
MIRROR_PATCH="$ROOT/clang/patches/0003-mirror-goc-colors-before-codegen.patch"
if ! apply_flex "$MIRROR_PATCH" "$CLANG_ROOT"; then
  echo "warning: early-mirror patch failed — ParseAST.cpp may already mirror" >&2
fi

echo "Patches applied (or best-effort). Next:"
echo "  cmake/ninja build — see clang/README.md"
echo "  The driver finds third_party/llvm-19.1.7-clang-build/bin/clang."
echo "  Elsewhere: ./cmd/goc go --clang \$BUILD/bin/clang"
