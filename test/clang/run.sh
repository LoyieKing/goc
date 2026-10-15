#!/usr/bin/env bash
# Clang Sema tests for the pointer colors, in both forms:
#   - the patched clang (Attr.td + SemaGocColors), the one the product uses;
#   - clang/plugin/GocClangPlugin.cpp loaded into a stock clang-19.
# Each case must compile; examples/sptr_escape_bad.c is the error case.
#   test/clang/run.sh [--plugin-only|--intree-only]
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/test/clang"
INC="$ROOT/include"
CASES=("$ROOT"/test/clang/[0-9]*.c)
BAD="$ROOT/examples/sptr_escape_bad.c"
mkdir -p "$OUT"
mode="${1:-all}"

# compile CLANG FLAGS... -- SRC NAME EXPECT_FAIL
compile() {
  local clang="$1" tag="$2" src="$3" expect_fail="$4" base
  shift 4
  base="$tag-$(basename "$src" .c)"
  set +e
  "$clang" "$@" -I "$INC" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
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

if [[ "$mode" != --plugin-only ]]; then
  # shellcheck source=../../scripts/goc-product-lib.sh
  ( source "$ROOT/scripts/goc-product-lib.sh"
    CLANG="$(goc_resolve_clang)"
    goc_prepend_lib "$(goc_clang_libdir "$CLANG")"
    for c in "${CASES[@]}"; do compile "$CLANG" intree "$c" 0 -DGOC_USE_INTREE_ATTRS; done
    compile "$CLANG" intree "$BAD" 1 -DGOC_USE_INTREE_ATTRS )
fi

if [[ "$mode" != --intree-only ]]; then
# Host clang-19 links the distro libLLVM and initializes every target,
  # including AMDGPU. `goc test` prepends this tree's libLLVM.so.19.1, which
  # is built for X86 and AArch64 only, and the host compiler then fails to
  # start. Drop that directory for the plugin build and the clang-19 load.
  if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    _ld_kept=()
    IFS=':' read -ra _ld_parts <<<"$LD_LIBRARY_PATH"
    for _ld_dir in "${_ld_parts[@]}"; do
      [[ -n "$_ld_dir" ]] || continue
      if [[ "$_ld_dir" == "$ROOT/"* && -e "$_ld_dir/libLLVM.so.19.1" ]]; then
        continue
      fi
      _ld_kept+=("$_ld_dir")
    done
    if [[ ${#_ld_kept[@]} -eq 0 ]]; then
      unset LD_LIBRARY_PATH
    else
      LD_LIBRARY_PATH="$(IFS=:; printf '%s' "${_ld_kept[*]}")"
      export LD_LIBRARY_PATH
    fi
    unset _ld_kept _ld_parts _ld_dir
  fi
  PLUGIN="$ROOT/build/libGocClang.so"
  make -C "$ROOT/clang/plugin" all OBJDIR="$ROOT/build" TARGET="$PLUGIN"
  CLANG="${CLANG_FALLBACK:-${CLANG:-clang-19}}"
  for c in "${CASES[@]}"; do compile "$CLANG" plugin "$c" 0 -fplugin="$PLUGIN" -DGOC_USE_PLUGIN_ATTRS; done
  compile "$CLANG" plugin "$BAD" 1 -fplugin="$PLUGIN" -DGOC_USE_PLUGIN_ATTRS
fi
echo "PASS clang"
