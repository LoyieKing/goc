#!/usr/bin/env bash
# Curated P28 goldens: in-tree Sema + real-body goobj
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/p28-out"
INC="$ROOT/include"
mkdir -p "$OUT"
PASS_FILE="$OUT/PASS_LINES.txt"
: > "$PASS_FILE"
pass() { echo "$*" | tee -a "$PASS_FILE"; }

# shellcheck source=goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
CLANG="$(goc_resolve_clang)"
goc_prepend_lib "$(goc_clang_libdir "$CLANG")"

echo "=== P28: using Clang: $CLANG ==="
"$CLANG" --version | head -2

compile_ok() {
  local src="$1" base="$2"
  "$CLANG" -DGOC_USE_INTREE_ATTRS -I "$INC" \
    -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
    -o "$OUT/${base}.raw.ll" "$src" 2>"$OUT/${base}.clang.log"
}

echo "=== P28: OK out-param / stack ==="
compile_ok "$ROOT/tests/sema/01_ok_outparam_stack.c" 01_ok_outparam_stack
pass "PASS P28-sema-ok (in-tree clang, no -fplugin: 01_ok_outparam_stack)"

echo "=== P28: implicit uptr storage for cptr T* ==="
compile_ok "$ROOT/tests/sema/02_ok_sptr_to_cptr_uptr.c" 02_ok_sptr_to_cptr_uptr
pass "PASS P28-sema-implicit-uptr (in-tree Sema accepts cptr T* destination)"

echo "=== P28: real-body goobj ==="
"$ROOT/backend/realbody/goc_p28_realbody.sh" \
  "$ROOT/tests/realbody/03_ok_realbody_goobj.c" \
  "$OUT/p28_real.o" \
  p28_real_body \
  main.P28RealBody | tee "$OUT/realbody.log"
test -f "$OUT/p28_real.o"
test -f "$OUT/p28_real.meta.json"
rg -q 'clang-real-isel' "$OUT/p28_real.meta.json"
rg -q 'magic 0x28C0DE42|P28-proof' "$OUT/realbody.log"
pass "PASS P28-realbody-goobj (Clang .c → real ISel body → goobj; not seedMIR)"
pass "PASS P28-driver-clang (patched clang next to goc)"

{
  echo "P28 results ($(date '+%Y-%m-%d %H:%M %Z'))"
  cat "$PASS_FILE"
  echo "PASS p28-clang-intree (in-tree Sema + real-body goobj)"
} | tee "$OUT/SUMMARY.txt"

echo ""
echo "PASS p28-clang-intree"
echo "Clang: $CLANG"
echo "Artifact: $OUT/p28_real.o"
