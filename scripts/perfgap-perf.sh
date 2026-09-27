#!/usr/bin/env bash
# Sampling profiles for docs/perf-gap.md: perf record -e cpu-clock (the VM has
# no hardware PMU, so cycles/instructions events are unavailable), one engine
# at a time pinned to CPU 3, on the fixed-work V8 per suite. Writes
# OUT/<engine>.<suite>.txt (perf report --sort sym, self %).
#   ENGINE_MAP=file ENGINES="a b" SUITES="..." OUT=dir
set -uo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PS=/workspace/perf-study
MAP="${ENGINE_MAP:-$ROOT/docs/perf-gap/data/raw/engines.txt}"
OUT="${OUT:-$PS/perf-gap-prof}"
CPU="${CPU:-3}"
JS="${JS:-$PS/cg-gap/js}"
SUITES="${SUITES:-Richards DeltaBlue Crypto RayTrace EarleyBoyer RegExp Splay NavierStokes}"
mkdir -p "$OUT"
read -r -a ENGS <<< "${ENGINES:-$(awk '{print $1}' "$MAP" | tr '\n' ' ')}"
for e in "${ENGS[@]}"; do
  c=$(awk -v n="$e" '$1==n {$1=""; print substr($0,2)}' "$MAP")
  for s in $SUITES; do
    # shellcheck disable=SC2086
    taskset -c "$CPU" perf record -q -e cpu-clock -F 10000 -o "$OUT/$e.$s.data" $c "$JS/v8-$s.js" > "$OUT/$e.$s.log" 2>&1
    perf report -i "$OUT/$e.$s.data" --no-children --sort sym --stdio 2>/dev/null \
      | grep -v '^#' | grep -v '^$' | head -60 > "$OUT/$e.$s.txt"
    rm -f "$OUT/$e.$s.data"
    echo "$e $s $(grep -o 'FIXED.*' "$OUT/$e.$s.log")"
  done
done
echo PERFDONE
