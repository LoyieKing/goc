#!/usr/bin/env bash
# goc-color-escape golden tests (build libGocPasses.so, then run every case in EXPECTATIONS)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
cd "$ROOT"

echo "=== color-escape: build libGocPasses.so ==="
"$REPO/scripts/build-passes.sh"

PASS_BIN="$ROOT/color-escape.sh"
# The patched clang (goc.h's color attributes are its native attributes).
CLANG="$(bash -c 'ROOT="$0"; source "$ROOT/scripts/goc-product-lib.sh"; goc_resolve_clang' "$REPO")"
INCDIR="$REPO/include"
OUT="$REPO/build/test/color-escape"
mkdir -p "$OUT"

echo "=== color-escape: run golden tests ==="
PASS=0
FAIL=0
REPORT_LINES=()

run_one() {
  local src="$1" expect="$2" needle="${3:-}"
  local base
  base="$(basename "$src" .c)"
  local ll="$OUT/${base}.ll"
  local colored="$OUT/${base}.color.ll"
  local log="$OUT/${base}.log"
  local rc=0

  if ! "$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
      -I "$INCDIR" -o "$ll" "$src" 2>"$OUT/${base}.clang.err"; then
    if [[ "$expect" == "fail" ]] && grep -Eiq "${needle:-sptr escape|gptr cannot|error}" "$OUT/${base}.clang.err"; then
      echo "PASS $base (Sema diagnostic OK)"
      REPORT_LINES+=("PASS $base (expected Sema error: matched)")
      PASS=$((PASS + 1))
    else
      echo "FAIL $base (clang frontend)"
      REPORT_LINES+=("FAIL $base — clang failed")
      FAIL=$((FAIL + 1))
      cat "$OUT/${base}.clang.err" || true
    fi
    return
  fi

  set +e
  "$PASS_BIN" "$ll" -o "$colored" >"$log" 2>&1
  rc=$?
  set -e

  if [[ "$expect" == "pass" ]]; then
    if [[ $rc -eq 0 ]] && grep -q "summary: 0 error" "$log"; then
      echo "PASS $base"
      REPORT_LINES+=("PASS $base")
      PASS=$((PASS + 1))
    else
      echo "FAIL $base (expected pass, rc=$rc)"
      REPORT_LINES+=("FAIL $base — expected pass rc=$rc")
      FAIL=$((FAIL + 1))
      tail -40 "$log" || true
    fi
  else
    if [[ $rc -ne 0 ]] && grep -Eiq "${needle:-sptr escape|gptr cannot|error}" "$log"; then
      echo "PASS $base (diagnostic OK)"
      REPORT_LINES+=("PASS $base (expected error: matched)")
      PASS=$((PASS + 1))
    else
      echo "FAIL $base (expected error containing '${needle}')"
      REPORT_LINES+=("FAIL $base — expected diagnostic")
      FAIL=$((FAIL + 1))
      tail -40 "$log" || true
    fi
  fi
}

while read -r name expect needle; do
  [[ -z "${name:-}" || "$name" =~ ^# ]] && continue
  run_one "$ROOT/$name" "$expect" "${needle:-}"
done < "$ROOT/EXPECTATIONS"

# Host executables of the compiler's uptr helper bodies: color-escape, then
# goc-inline-gate (the same pass goc build runs), then a native link. No
# runtime library: fake_g.h stands in for g, so goc_stack_lo/hi move.
OPT="${OPT:-$(command -v opt-19 || command -v opt || echo /usr/lib/llvm-19/bin/opt)}"
PLUGIN="${GOC_STACKMAP:-$REPO/build/llvm/libGocPasses.so}"
build_host_uptr() {
  local base="$1"
  "$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
    -I "$INCDIR" -I "$ROOT" -o "$OUT/$base.ll" "$ROOT/$base.c"
  "$PASS_BIN" "$OUT/$base.ll" -o "$OUT/$base.color.ll" >"$OUT/$base.log" 2>&1
  "$OPT" -load-pass-plugin="$PLUGIN" -passes=goc-inline-gate \
    -S "$OUT/$base.color.ll" -o "$OUT/$base.gate.ll"
  # Only the static inline goc_uptr_is_*_encoded may stay defined; nothing
  # may be left as an external declaration.
  if grep -Eq '^declare .*@goc_(uptr_|stack_|dyn)' "$OUT/$base.gate.ll" &&
     grep -Eq 'call .*@goc_(uptr_(from|as|decode|require)|stack_|dyn)' "$OUT/$base.gate.ll"; then
    fail_host "$base: a uptr helper call survived goc-inline-gate"
    return 1
  fi
  "$CLANG" -O0 "$OUT/$base.gate.ll" -o "$OUT/$base"
}
fail_host() {
  echo "FAIL $1"
  REPORT_LINES+=("FAIL $1")
  FAIL=$((FAIL + 1))
}
pass_host() {
  echo "PASS $1"
  REPORT_LINES+=("PASS $1")
  PASS=$((PASS + 1))
}

echo "=== color-escape: automatic uptr storage round-trip ==="
if build_host_uptr 09_auto_uptr_storage_roundtrip; then
  if "$OUT/09_auto_uptr_storage_roundtrip"; then
    pass_host "automatic uptr storage round-trip after stack.hi movement"
  else
    fail_host "automatic uptr storage round-trip (rc=$?)"
  fi
fi

if build_host_uptr 10_auto_uptr_mixed_destination; then
  if "$OUT/10_auto_uptr_mixed_destination" && "$OUT/10_auto_uptr_mixed_destination" local; then
    pass_host "mixed stack/global uptr storage after stack.hi movement"
  else
    fail_host "mixed stack/global uptr storage"
  fi
fi

echo "=== color-escape: explicit uptr tags ==="
if build_host_uptr 11_uptr_explicit_tags; then
  set +e
  "$OUT/11_uptr_explicit_tags"
  rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    pass_host "explicit uptr round trips (sptr/cptr, _hi, stack.hi movement)"
  else
    fail_host "explicit uptr round trips (rc=$rc)"
  fi
  # A wrong tag writes the message and traps (SIGILL: rc 132).
  for mode in as_sptr:"goc_uptr_as_sptr: MSB clear" \
              as_cptr:"goc_uptr_as_cptr: MSB set" \
              from_cptr:"goc_uptr_from_cptr: address has MSB set" \
              from_sptr:"goc_uptr_from_sptr: offset cleared MSB" \
              require_cptr:"sptr cannot be returned as cptr"; do
    arg="${mode%%:*}" want="goc FATAL uptr: ${mode#*:}"
    set +e
    "$OUT/11_uptr_explicit_tags" "$arg" 2>"$OUT/11_$arg.err"
    rc=$?
    set -e
    if [[ $rc -eq 132 ]] && grep -qF "$want" "$OUT/11_$arg.err"; then
      pass_host "wrong tag ($arg) is fatal: $want"
    else
      fail_host "wrong tag ($arg): rc=$rc, stderr: $(cat "$OUT/11_$arg.err")"
    fi
  done
fi

echo "=== color-escape summary: $PASS passed, $FAIL failed ==="
{
  echo "color-escape golden results ($(date '+%Y-%m-%d %H:%M %Z'))"
  printf '%s\n' "${REPORT_LINES[@]}"
} | tee "$OUT/PASS_LINES.txt"

if [[ $FAIL -ne 0 ]]; then
  exit 1
fi

echo "=== color-escape: color report ==="
bash "$ROOT/check_color_report.sh"

echo "OK: color-escape goldens passed"
