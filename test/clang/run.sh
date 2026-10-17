#!/usr/bin/env bash
# Clang Sema tests for the pointer colors (the patched clang: Attr.td +
# SemaGocColors). Each case must compile; examples/sptr_escape_bad.c is the
# error case.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/test/clang"
INC="$ROOT/include"
CASES=("$ROOT"/test/clang/[0-9]*.c)
BAD="$ROOT/examples/sptr_escape_bad.c"
mkdir -p "$OUT"

# compile CLANG SRC EXPECT_FAIL
compile() {
  local clang="$1" src="$2" expect_fail="$3" base
  base="$(basename "$src" .c)"
  set +e
  "$clang" -I "$INC" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
    -o "$OUT/$base.ll" "$src" 2>"$OUT/$base.log"
  local rc=$?
  set -e
  if [[ "$expect_fail" == 1 ]]; then
    [[ $rc -ne 0 ]] && grep -q 'goc: sptr escape' "$OUT/$base.log" || {
      echo "FAIL $base: expected the sptr escape diagnostic" >&2; cat "$OUT/$base.log" >&2; exit 1; }
  else
    [[ $rc -eq 0 ]] || { echo "FAIL $base" >&2; cat "$OUT/$base.log" >&2; exit 1; }
  fi
  echo "PASS $base"
}

# shellcheck source=../../scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
CLANG="$(goc_resolve_clang)"
goc_prepend_lib "$(goc_clang_libdir "$CLANG")"
for c in "${CASES[@]}"; do compile "$CLANG" "$c" 0; done
compile "$CLANG" "$BAD" 1
echo "PASS clang"
