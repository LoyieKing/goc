#!/usr/bin/env bash
# Stack pointers returned through calls (p = pick(&slot), p = identity(&x))
# with the product default color (cptr), used after the stack was copied.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/test/retptr"
mkdir -p "$OUT"
# shellcheck source=../../../scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"
for O in 2 3; do
  GOC_DEFAULT_PTR_COLOR=cptr GOC_OPT_LEVEL=$O "$ROOT/cmd/goc" build \
    "$ROOT/test/e2e/retptr/retptr.c" -o "$OUT/retptr.goobj" >"$OUT/build.log" 2>&1 || {
    tail -20 "$OUT/build.log" >&2; exit 1; }
  ( cd "$ROOT/test/e2e/retptr" && \
    CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/retptr.goobj" \
    go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" \
      -o "$OUT/retptr_test" . ) >"$OUT/go-build.log" 2>&1 || {
    tail -20 "$OUT/go-build.log" >&2; exit 1; }
  "$OUT/retptr_test" | tee "$OUT/run.log"
  grep -q '^PASS retptr' "$OUT/run.log"
  GOMAXPROCS=1 "$OUT/retptr_test" | grep -q '^PASS retptr'
done
echo "PASS retptr (amd64)"
