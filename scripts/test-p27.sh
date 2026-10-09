#!/usr/bin/env bash
# Curated P27 goldens: out-of-tree Clang plugin Sema
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
# Host clang-19 links the distro libLLVM and initializes every target,
# including AMDGPU. `goc test` prepends this tree's libLLVM.so.19.1, which
# is built for X86 and AArch64 only, and the host compiler then fails to
# start. Drop that directory for the plugin build and the clang-19 load.
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
  _p27_kept=()
  IFS=':' read -ra _p27_parts <<<"$LD_LIBRARY_PATH"
  for _p27_dir in "${_p27_parts[@]}"; do
    [[ -n "$_p27_dir" ]] || continue
    if [[ "$_p27_dir" == "$ROOT/"* && -e "$_p27_dir/libLLVM.so.19.1" ]]; then
      continue
    fi
    _p27_kept+=("$_p27_dir")
  done
  if [[ ${#_p27_kept[@]} -eq 0 ]]; then
    unset LD_LIBRARY_PATH
  else
    LD_LIBRARY_PATH="$(IFS=:; printf '%s' "${_p27_kept[*]}")"
    export LD_LIBRARY_PATH
  fi
  unset _p27_kept _p27_parts _p27_dir
fi
OUT="$ROOT/build/p27-out"
INC="$ROOT/include"
CLANG="${CLANG_FALLBACK:-${CLANG:-clang-19}}"
PLUGIN="$ROOT/build/libGocClang.so"
mkdir -p "$OUT" "$ROOT/build"
PASS_FILE="$OUT/PASS_LINES.txt"
: > "$PASS_FILE"
pass() { echo "$*" | tee -a "$PASS_FILE"; }

echo "=== P27: build Clang plugin ==="
make -C "$ROOT/clang/plugin" all OBJDIR="$ROOT/build" TARGET="$PLUGIN"
test -f "$PLUGIN"
pass "PASS P27-plugin (libGocClang.so built)"

compile_c() {
  local src="$1" base="$2" expect_fail="${3:-0}"
  set +e
  "$CLANG" -fplugin="$PLUGIN" -DGOC_USE_PLUGIN_ATTRS -I "$INC" \
    -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
    -o "$OUT/${base}.raw.ll" "$src" 2>"$OUT/${base}.clang.log"
  local rc=$?
  set -e
  if [[ "$expect_fail" == "1" ]]; then
    [[ $rc -ne 0 ]] || { echo "FAIL: expected Sema error"; exit 1; }
    rg -q 'goc: sptr escape' "$OUT/${base}.clang.log" || {
      echo "FAIL: missing Sema diagnostic"; cat "$OUT/${base}.clang.log"; exit 1
    }
    return 0
  fi
  [[ $rc -eq 0 ]] || { echo "FAIL: compile $src"; cat "$OUT/${base}.clang.log"; exit 1; }
}

echo "=== P27: OK / ERROR goldens ==="
compile_c "$ROOT/tests/sema/01_ok_outparam_stack.c" 01_ok_outparam_stack 0
pass "PASS P27-ok-outparam"
compile_c "$ROOT/tests/sema/02_ok_sptr_to_cptr_uptr.c" 02_ok_sptr_to_cptr_uptr 0
pass "PASS P27-implicit-uptr-store"

{
  echo "P27 results ($(date '+%Y-%m-%d %H:%M %Z'))"
  cat "$PASS_FILE"
  echo "PASS p27-clang-frontend (plugin Sema goldens)"
} | tee "$OUT/SUMMARY.txt"

echo ""
echo "PASS p27-clang-frontend"
echo "Plugin: $PLUGIN"
