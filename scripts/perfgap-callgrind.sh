#!/usr/bin/env bash
# Deterministic counters for docs/perf-gap.md. The VM has no hardware PMU
# (perf stat reports <not supported> for instructions/cycles), so callgrind is
# the counter source: Ir (instructions retired), simulated D1/LL cache misses
# and simulated branch mispredicts (Bcm conditional, Bim indirect).
#
#   ENGINE_MAP=file (NAME COMMAND... lines, as scripts/perfgap-bench.sh writes)
#   ENGINES="a b c"   default: every engine in ENGINE_MAP
#   SUITES="Richards ..."  V8 suites of the fixed-work V8 (perf-study js/v8fixed-q.js)
#   SIM=1             cache + branch simulation (default 1; 0 = Ir only, ~3x faster)
#   JOBS=6 CPUS=0-2,4-7 OUT=dir
set -uo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PS=/workspace/perf-study
MAP="${ENGINE_MAP:-$ROOT/docs/perf-gap/data/raw/engines.txt}"
OUT="${OUT:-$PS/cg-gap}"
JOBS="${JOBS:-6}"
CPUS="${CPUS:-0-2,4-7}"
SIM="${SIM:-1}"
# Go's async preemption (SIGURG from sysmon) trips a callgrind assertion
# (threads.c: sigNum == current_state.sig) on goc binaries; turn it off.
# Natives ignore GODEBUG.
export GODEBUG="${GODEBUG:-asyncpreemptoff=1}"
V8F="${V8F:-$PS/js/v8fixed-q.js}"
# v8fixed-q.js = scale 0.25 + the first 11532 lines of the V8-v7 bundle that
# scripts/bench-all.sh uses ($PS/js/v8.js, everything before its run harness)
# + tests/perfgap/v8fixed-harness.js (fixed repetitions per suite).
if [[ ! -f "$V8F" ]]; then
  V8F="$OUT/js/v8fixed-q.js"; mkdir -p "$OUT/js"
  { echo "globalThis.__SCALE=0.25;"; head -n 11532 "$PS/js/v8.js"
    cat "$ROOT/tests/perfgap/v8fixed-harness.js"; } > "$V8F"
fi
SUITES="${SUITES:-Richards DeltaBlue Crypto RayTrace EarleyBoyer RegExp Splay NavierStokes}"
mkdir -p "$OUT/js"
for s in $SUITES; do
  [[ -f "$OUT/js/v8-$s.js" ]] || { echo "globalThis.__ONLY=\"$s\";"; cat "$V8F"; } > "$OUT/js/v8-$s.js"
done
read -r -a ENGS <<< "${ENGINES:-$(awk '{print $1}' "$MAP" | tr '\n' ' ')}"
SIMARGS=()
[[ "$SIM" == 1 ]] && SIMARGS=(--cache-sim=yes --branch-sim=yes)
jobs_list() {
  for e in "${ENGS[@]}"; do
    c=$(awk -v n="$e" '$1==n {$1=""; print substr($0,2)}' "$MAP")
    [[ -n "$c" ]] || { echo "no command for $e" >&2; continue; }
    for s in $SUITES; do
      echo "$e|$s|$c"
    done
  done
}
run1() {
  IFS='|' read -r e s c <<< "$1"
  f="$OUT/$e.$s.cg"
  [[ -s "$f" && "${FORCE:-0}" != 1 ]] && return
  # shellcheck disable=SC2086
  taskset -c "$CPUS" valgrind --tool=callgrind $SIMARGS_STR --callgrind-out-file="$f.tmp" \
    $c "$OUT/js/v8-$s.js" > "$OUT/$e.$s.log" 2>&1 && mv "$f.tmp" "$f"
  echo "$e $s $(grep -o 'FIXED.*' "$OUT/$e.$s.log" | head -1) $(grep -m1 -o 'Collected : [0-9]*' "$OUT/$e.$s.log")"
}
SIMARGS_STR="${SIMARGS[*]:-}"
export -f run1; export OUT CPUS SIMARGS_STR
jobs_list | xargs -P "$JOBS" -d '\n' -I{} bash -c 'run1 "$1"' _ {}
echo CGDONE
