#!/usr/bin/env bash
# Run tests/bench/perfgap-mech.js (JS mechanism probes, ns/op) on several
# engines, pinned, ROUNDS interleaved rounds. Commands come from ENGINE_MAP
# (default docs/perf-gap/data/raw-toggles/engines.txt).
#   OUT=docs/perf-gap/data/mech-js  ROUNDS=3  CPU=3  ENGINES="a b ..."
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
MAP="${ENGINE_MAP:-$ROOT/docs/perf-gap/data/raw-toggles/engines.txt}"
OUT="${OUT:-$ROOT/docs/perf-gap/data/mech-js}"
ROUNDS="${ROUNDS:-3}"; CPU="${CPU:-3}"
ENGINES="${ENGINES:-bellard-gcc-O2-NDEBUG bellard-clang-O3 bellard-clang-O3-gocflags bellard-gocpipe-sm goc-bellard goc-bellard-fast goc-bellard-best ng-clang-O2 ng-clang-O3 ng-clang-O3-gocflags goc-ng goc-ng-fast goc-ng-best}"
mkdir -p "$OUT"
for ((r = 1; r <= ROUNDS; r++)); do
  for e in $ENGINES; do
    c=$(awk -v n="$e" '$1==n {$1=""; print substr($0,2)}' "$MAP")
    [[ -n "$c" ]] || { echo "no command for $e" >&2; exit 1; }
    # shellcheck disable=SC2086
    taskset -c "$CPU" $c "$ROOT/tests/bench/perfgap-mech.js" > "$OUT/$e-r$r.txt" 2>&1 || echo "FAIL $e r$r"
    echo "r$r $e $(wc -l < "$OUT/$e-r$r.txt") lines"
  done
done
echo MECHJSDONE
