#!/usr/bin/env bash
# Same-session five-engine benchmark run for docs/benchmark.md.
#
# Timed suites are pinned to one core (taskset -c $CPU) and interleaved: each
# round runs every engine once, and the engine order rotates between rounds.
# Correctness suites (test262 sample, QuickJS official tests) run afterwards,
# in parallel, unpinned. Raw output goes to $OUT; scripts/bench-summarize.py
# turns it into all.json and scripts/bench-charts.py draws the charts.
#
# Inputs (override with env):
#   GOC      build/qjs/qjscli          (goc-ng: scripts/qjs-cli-build.sh, O3 + NDEBUG)
#   GOCB     build/qjs-bellard/qjscli  (goc-bellard: QJS_FLAVOR=bellard scripts/qjs-cli-build.sh)
#   NG       native QuickJS-ng 0.17.0 qjs (CMake Release, clang-19 -O2 -DNDEBUG)
#   BELLARD  Bellard QuickJS 2026-06-04 qjs (upstream Makefile, gcc -O2)
#   BELLARD_CLANG  same Bellard sources, upstream Makefile with CONFIG_CLANG=y CC=clang-19 (-O2), reference only
#   GOJA     gojacli from scripts/gojacli (Goja cfe4039)
#   V8JS     bench-v8 combined.js (QuickJS tests/bench-v8) with a console prelude
#   SSDIR    SunSpider 1.0.2 wrapped by scripts/sunspider-wrap.py
#   MICROJS  Bellard tests/microbench.js
#   T262     tc39/test262 checkout at 7ab7fafa0003f73fc85c1b95d88094d33f7eb8bd
#   QJSTESTS Bellard 2026-06-04 tests/ directory
#   ROUNDS_V8 / ROUNDS_SS / ROUNDS_MICRO / ROUNDS_MICROCALL  (default 5/3/3/5)
#   SKIP     space-separated subset of: v8 ss micro microcall t262 qjs
#   ENGINE_MAP  optional file of "NAME COMMAND..." lines; a NAME listed there overrides
#            the built-in command table (used by scripts/perfgap-bench.sh for extra builds)
#   MICROCALL_PER_ENGINE=1  run tests/bench/microcall.js once per engine per round
#            (microcall-<engine>-r<round>.txt) instead of scripts/microcall-bench.sh
set -uo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PS=/workspace/perf-study
GOC="${GOC:-$ROOT/build/qjs/qjscli}"
GOCB="${GOCB:-$ROOT/build/qjs-bellard/qjscli}"
NG="${NG:-$PS/native/b-O2/qjs}"
BELLARD="${BELLARD:-$PS/bellard/quickjs-2026-06-04/qjs}"
GOJA="${GOJA:-$PS/goja-cli/gojacli}"
V8JS="${V8JS:-$PS/js/v8.js}"
SSDIR="${SSDIR:-$PS/ss/w}"
MICROJS="${MICROJS:-$PS/bellard/quickjs-2026-06-04/tests/microbench.js}"
T262="${T262:-$PS/t262/test262-head}"
QJSTESTS="${QJSTESTS:-$PS/bellard/quickjs-2026-06-04/tests}"
OUT="${OUT:-$ROOT/docs/benchmark/data/raw}"
CPU="${CPU:-3}"
ROUNDS_V8="${ROUNDS_V8:-5}" ROUNDS_SS="${ROUNDS_SS:-3}"
ROUNDS_MICRO="${ROUNDS_MICRO:-3}" ROUNDS_MICROCALL="${ROUNDS_MICROCALL:-5}"
SKIP="${SKIP:-}"
BELLARD_CLANG="${BELLARD_CLANG:-$PS/bellard/clang-O2/qjs}"
# ENGINES may be overridden, e.g. the clang reference run:
#   ENGINES="bellard-clang bellard" SKIP="microcall t262 qjs" OUT=docs/benchmark/data/raw/bellard-clang scripts/bench-all.sh
read -r -a ENGINES <<< "${ENGINES:-goc-ng goc-bellard ng bellard goja}"
mkdir -p "$OUT"

cmd() {  # cmd ENGINE [micro] -> command prefix (file appended by caller)
  if [[ -n "${ENGINE_MAP:-}" ]]; then
    local line
    line=$(awk -v n="$1" '$1 == n { $1 = ""; sub(/^ /, ""); print; exit }' "$ENGINE_MAP")
    if [[ -n "$line" ]]; then echo "$line"; return; fi
  fi
  case "$1" in
    goc-ng)  echo "$GOC --stack-size 16384" ;;
    goc-bellard) echo "$GOCB --stack-size 16384" ;;
    ng)      echo "$NG -C --stack-size 16384" ;;
    bellard) echo "$BELLARD --stack-size 16M" ;;
    bellard-clang) echo "$BELLARD_CLANG --stack-size 16M" ;;
    goja)    if [[ "${2:-}" == micro ]]; then echo "$GOJA --micro"; else echo "$GOJA"; fi ;;
  esac
}
order() {  # order ROUND -> engines rotated by ROUND
  local r=$1 n=${#ENGINES[@]} i
  for ((i = 0; i < n; i++)); do echo "${ENGINES[$(( (i + r) % n ))]}"; done
}
skip() { [[ " $SKIP " == *" $1 "* ]]; }
pin() { taskset -c "$CPU" "$@"; }

{
  echo "date: $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo "host: $(uname -srm), $(nproc) vCPU, pinned to CPU $CPU"
  echo "goc commit: $(git -C "$ROOT" rev-parse HEAD)"
  for e in "${ENGINES[@]}"; do echo "$e: $(cmd "$e")  sha256=$(sha256sum "$(cmd "$e" | cut -d' ' -f1)" | cut -c1-16)"; done
  echo "rounds: v8=$ROUNDS_V8 ss=$ROUNDS_SS micro=$ROUNDS_MICRO microcall=$ROUNDS_MICROCALL"
} > "$OUT/env.txt"
cat "$OUT/env.txt"

if ! skip v8; then
  for ((r = 1; r <= ROUNDS_V8; r++)); do
    for e in $(order $r); do
      s=$(date +%s.%N)
      pin $(cmd $e) "$V8JS" > "$OUT/v8-$e-r$r.txt" 2>&1
      awk -v a="$s" -v b="$(date +%s.%N)" 'BEGIN{printf "WALL %.2f\n", b-a}' >> "$OUT/v8-$e-r$r.txt"
      echo "v8 r$r $e $(grep SCORE "$OUT/v8-$e-r$r.txt")"
    done
  done
fi

if ! skip ss; then
  for ((r = 1; r <= ROUNDS_SS; r++)); do
    for e in $(order $r); do
      o="$OUT/ss-$e-r$r.txt"; : > "$o"
      for f in "$SSDIR"/*.js; do
        t=$(basename "$f" .js)
        res=$(cd "$SSDIR" && pin $(cmd $e) "$f" 2>&1 | tail -1)
        [[ "$res" == "$t "* ]] || res="$t FAIL ${res:0:160}"
        echo "$res" >> "$o"
      done
      echo "ss r$r $e done ($(grep -c FAIL "$o") fail)"
    done
  done
fi

if ! skip micro; then
  # qjscli (goc-ng / goc-bellard) has no console global; give every engine the same fallback
  # that the V8 and SunSpider files carry.
  MB="$(mktemp /tmp/microbench-XXXX.js)"
  { echo 'if (typeof console === "undefined") globalThis.console = { log: print };'; cat "$MICROJS"; } > "$MB"
  for ((r = 1; r <= ROUNDS_MICRO; r++)); do
    for e in $(order $r); do
      pin $(cmd $e micro) "$MB" > "$OUT/micro-$e-r$r.txt" 2>&1
      echo "micro r$r $e $(grep -i total "$OUT/micro-$e-r$r.txt" | tail -1)"
    done
  done
fi

if ! skip microcall && [[ "${MICROCALL_PER_ENGINE:-0}" == 1 ]]; then
  for ((r = 1; r <= ROUNDS_MICROCALL; r++)); do
    for e in $(order $r); do
      pin $(cmd $e) "$ROOT/tests/bench/microcall.js" > "$OUT/microcall-$e-r$r.txt" 2>&1
      echo "microcall r$r $e $(grep -o '"score":[0-9.]*' "$OUT/microcall-$e-r$r.txt")"
    done
  done
elif ! skip microcall; then
  for ((r = 1; r <= ROUNDS_MICROCALL; r++)); do
    GOC_QJS="$GOC" GOC_BELLARD_QJS="$GOCB" NATIVE_QJS="$NG" BELLARD_QJS="$BELLARD" GOJA_CLI="$GOJA" pin "$ROOT/scripts/microcall-bench.sh" > "$OUT/microcall-r$r.txt" 2>&1
    echo "microcall r$r $(grep '^score' "$OUT/microcall-r$r.txt")"
  done
fi

if ! skip t262; then
  python3 "$ROOT/scripts/test262-sample.py" --test262 "$T262" \
    --engine "goc-ng=$(cmd goc-ng)" --engine "goc-bellard=$(cmd goc-bellard)" \
    --engine "ng=$(cmd ng)" --engine "bellard=$(cmd bellard)" --engine "goja=$(cmd goja)" \
    --out "$OUT/test262-results.json" > "$OUT/test262-summary.txt" 2>&1
  grep -E 'pass$|differ' "$OUT/test262-summary.txt"
fi

if ! skip qjs; then
  python3 "$ROOT/scripts/qjs-official-tests.py" --tests "$QJSTESTS" \
    --engine "goc-ng=$(cmd goc-ng)" --engine "goc-bellard=$(cmd goc-bellard)" \
    --engine "ng=$(cmd ng)" --engine "bellard=$(cmd bellard)" --engine "goja=$(cmd goja)" \
    --out "$OUT/qjs-tests-results.json" > "$OUT/qjs-tests-summary.txt" 2>&1
  grep -E 'pass|differ' "$OUT/qjs-tests-summary.txt" | grep -v '^  '
fi
