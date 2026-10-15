#!/usr/bin/env bash
# Stack-growth and GC stress: C recursion in all three stack-check tiers on
# fresh goroutines while GCs scan and shrink stacks (test/stress).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/stress"
mkdir -p "$OUT"

# shellcheck source=goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"

env -u GOC_DEFAULT_PTR_COLOR GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-2}" "$ROOT/cmd/goc" build \
  "$ROOT/test/stress/stress.c" -o "$OUT/stress.goobj" >"$OUT/build.log" 2>&1 || {
  tail -20 "$OUT/build.log" >&2
  exit 1
}
go run ./tools/goc-objdump verify "$OUT/stress.gocobj"
( cd "$ROOT/test/stress" && \
  CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/stress.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" \
    -o "$OUT/stress_test" . ) >"$OUT/go-build.log" 2>&1 || {
  tail -20 "$OUT/go-build.log" >&2
  exit 1
}
"$OUT/stress_test" | tee "$OUT/run.log"
grep -q '^PASS stress' "$OUT/run.log"
# Once more on one P: GC, growth and preemption then interleave differently.
GOMAXPROCS=1 "$OUT/stress_test" | grep -q '^PASS stress'
echo "PASS test-stress"
