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

CLANG="${GOC_CLANG:-}"
if [[ -z "$CLANG" ]]; then
  for c in \
    "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang" \
    "$ROOT/third_party/llvm-clang-build/bin/clang"; do
    [[ -x "$c" ]] && CLANG="$c" && break
  done
fi
if [[ -z "$CLANG" || ! -x "$CLANG" ]]; then
  echo "FAIL: set GOC_CLANG to a patched clang-19 (in-tree Sema required for --p28)" >&2
  echo "See clang/README.md and scripts/apply-patches.sh" >&2
  exit 1
fi
LLC="${LLC:-llc-19}"

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
pass "PASS P28-driver-clang (GOC_CLANG set for goc build/cc)"

{
  echo "P28 results ($(date '+%Y-%m-%d %H:%M %Z'))"
  cat "$PASS_FILE"
  echo "PASS p28-clang-intree (in-tree Sema + real-body goobj)"
} | tee "$OUT/SUMMARY.txt"

echo ""
echo "PASS p28-clang-intree"
echo "Clang: $CLANG"
echo "Artifact: $OUT/p28_real.o"
