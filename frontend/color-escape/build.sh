#!/usr/bin/env bash
# P17 one-command build + golden color/escape tests
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DOCS="$(cd "$ROOT/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
cd "$ROOT"

echo "=== P17: build goc-color-escape ==="
make -C pass

PASS_BIN="$ROOT/build/goc-color-escape"
CLANG="${CLANG:-clang-19}"
INCDIR="$ROOT/include"
OUT="$ROOT/build/test-out"
mkdir -p "$OUT" "$DOCS/bin"

# Compat alias → unified bin/goc (P20); do not overwrite bin/goc itself
if [[ -x "$DOCS/bin/goc" ]]; then
  cat > "$DOCS/bin/goc-fe" << 'DRIVER'
#!/usr/bin/env bash
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
exec "$SELF/goc" fe "$@"
DRIVER
  chmod +x "$DOCS/bin/goc-fe"
fi

echo "=== P17: run golden tests ==="
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
  run_one "$ROOT/tests/$name" "$expect" "${needle:-}"
done < "$ROOT/tests/EXPECTATIONS"

echo "=== P17: automatic uptr storage round-trip ==="
UPTR_SRC="$ROOT/tests/09_auto_uptr_storage_roundtrip.c"
UPTR_LL="$OUT/09_auto_uptr_storage_roundtrip.ll"
UPTR_COLOR_LL="$OUT/09_auto_uptr_storage_roundtrip.color.ll"
UPTR_BIN="$OUT/09_auto_uptr_storage_roundtrip"
"$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
  -I "$INCDIR" -I "$REPO/runtime" -o "$UPTR_LL" "$UPTR_SRC"
"$PASS_BIN" "$UPTR_LL" -o "$UPTR_COLOR_LL" >"$OUT/09_auto_uptr_storage_roundtrip.log" 2>&1
"$CLANG" -O0 -I "$INCDIR" -I "$REPO/runtime" \
  "$UPTR_COLOR_LL" "$REPO/runtime/uptr/goc_uptr_runtime.c" -o "$UPTR_BIN"
"$UPTR_BIN"
echo "PASS automatic uptr storage round-trip after stack.hi movement"
REPORT_LINES+=("PASS automatic uptr storage round-trip after stack.hi movement")
PASS=$((PASS + 1))

MIXED_SRC="$ROOT/tests/10_auto_uptr_mixed_destination.c"
MIXED_LL="$OUT/10_auto_uptr_mixed_destination.ll"
MIXED_COLOR_LL="$OUT/10_auto_uptr_mixed_destination.color.ll"
MIXED_BIN="$OUT/10_auto_uptr_mixed_destination"
"$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
  -I "$INCDIR" -I "$REPO/runtime" -o "$MIXED_LL" "$MIXED_SRC"
"$PASS_BIN" "$MIXED_LL" -o "$MIXED_COLOR_LL" >"$OUT/10_auto_uptr_mixed_destination.log" 2>&1
"$CLANG" -O0 -I "$INCDIR" -I "$REPO/runtime" \
  "$MIXED_COLOR_LL" "$REPO/runtime/uptr/goc_uptr_runtime.c" -o "$MIXED_BIN"
"$MIXED_BIN"
"$MIXED_BIN" local
echo "PASS mixed stack/global uptr storage after stack.hi movement"
REPORT_LINES+=("PASS mixed stack/global uptr storage after stack.hi movement")
PASS=$((PASS + 1))

echo "=== P17 summary: $PASS passed, $FAIL failed ==="
{
  echo "P17 golden results ($(date '+%Y-%m-%d %H:%M %Z'))"
  printf '%s\n' "${REPORT_LINES[@]}"
} | tee "$OUT/PASS_LINES.txt"

if [[ $FAIL -ne 0 ]]; then
  exit 1
fi
echo "OK: bin/goc-fe ready; build/goc-color-escape ready"
