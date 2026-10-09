#!/usr/bin/env bash
# Memory benchmark for docs/benchmark.md (## 内存占用).
#
# Part 1, peak RSS per workload: each script runs in a fresh process under
# `/usr/bin/time -v` (Maximum resident set size), pinned with taskset -c $CPU
# like the timed suites; ROUNDS rounds, engine order rotates per round.
# Workloads: an empty script, each V8-v7 suite on its own (bench-v8.js with a
# suite filter), the whole bench-v8.js, every SunSpider file, microbench.js,
# alloc.js, mapset.js.
#   -> $OUT/mem-rss-<engine>-r<round>.txt, one line per workload:
#      <workload> <max_rss_kb> <elapsed_s> <exit_status>
#
# Part 2, multi-instance scaling: N JS runtimes kept alive in one process,
# N in $NS. goc-ng: build/qjs/qjsmem (qjscli built with -tags qjsmem, see
# tests/qjscli/mem_instances.go); goc-bellard: build/qjs-bellard/qjsmem (the
# same probe, QJS_FLAVOR=bellard). native ng / Bellard: tests/qjsmem/threads.c
# linked against each engine's own build. Goja: scripts/gojamem. Not pinned
# (see the doc for why). ng also runs with MALLOC_ARENA_MAX=1 (engine
# "ng-arena1").
#   -> $OUT/mem-inst-r<round>.txt, one MEMINST JSON line per (engine, N).
#
# Both parts append and skip entries already present, so an interrupted run
# resumes where it stopped. SKIP="rss" or SKIP="inst" skips a part.
set -uo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PS=/workspace/perf-study
GOC="${GOC:-$ROOT/build/qjs/qjscli}"
GOCMEM="${GOCMEM:-$ROOT/build/qjs/qjsmem}"
GOCB="${GOCB:-$ROOT/build/qjs-bellard/qjscli}"
GOCBMEM="${GOCBMEM:-$ROOT/build/qjs-bellard/qjsmem}"
NG="${NG:-$PS/native/b-O2/qjs}"
NGDIR="${NGDIR:-$PS/native}"             # quickjs.h; library in $NGDIR/b-O2/libqjs.a
BELLARD="${BELLARD:-$PS/bellard/quickjs-2026-06-04/qjs}"
BDIR="${BDIR:-$PS/bellard/quickjs-2026-06-04}"  # quickjs.h and .obj/*.o
GOJA="${GOJA:-$PS/goja-cli/gojacli}"
V8JS="${V8JS:-$PS/js/v8.js}"
SSDIR="${SSDIR:-$PS/ss/w}"
MICROJS="${MICROJS:-$PS/bellard/quickjs-2026-06-04/tests/microbench.js}"
ALLOCJS="${ALLOCJS:-$PS/js/alloc.js}"
MAPSETJS="${MAPSETJS:-$PS/js/mapset.js}"
OUT="${OUT:-$ROOT/docs/benchmark/data/raw}"
HB="${HB:-$ROOT/build/qjsmem}"            # harness binaries and generated inputs
CPU="${CPU:-3}"
ROUNDS="${ROUNDS:-3}"
NS="${NS:-0 1 10 100 1000}"
SKIP="${SKIP:-}"
ENGINES=(goc-ng goc-bellard ng bellard goja)
mkdir -p "$OUT" "$HB"
skip() { [[ " $SKIP " == *" $1 "* ]]; }

cmd() {  # same command lines as scripts/bench-all.sh
  case "$1" in
    goc-ng)  echo "$GOC --stack-size 16384 --script" ;;
    goc-bellard) echo "$GOCB --stack-size 16384 --script" ;;
    ng)      echo "$NG -C --stack-size 16384" ;;
    bellard) echo "$BELLARD --stack-size 16M" ;;
    goja)    if [[ "${2:-}" == micro ]]; then echo "$GOJA --micro"; else echo "$GOJA"; fi ;;
  esac
}
order() { local r=$1 n=${#ENGINES[@]} i; for ((i = 0; i < n; i++)); do echo "${ENGINES[$(( (i + r) % n ))]}"; done; }

# ---- inputs
: > "$HB/empty.js"
V8SUITES=(Richards DeltaBlue Crypto RayTrace EarleyBoyer RegExp Splay NavierStokes)
for s in "${V8SUITES[@]}"; do
  # Same file, but only suite $s runs (the other suites are still parsed).
  # Keep a leading "use strict" as the first statement. Putting __ONLY
  # above it would turn the directive into a no-op string.
  awk -v s="$s" 'NR == 1 && $0 == "\"use strict\";" { print; print "globalThis.__ONLY = \"" s "\";"; next }
    NR == 1 { print "globalThis.__ONLY = \"" s "\";" }
    /^try \{$/ && !done { print "BenchmarkSuite.suites = BenchmarkSuite.suites.filter(function (x) { return x.name === __ONLY; });"; done = 1 }
    { print }' "$V8JS" > "$HB/v8-$s.js"
done
MB="$HB/microbench.js"
{ echo 'if (typeof console === "undefined") globalThis.console = { log: print };'; cat "$MICROJS"; } > "$MB"

# ---- harnesses
if [[ ! -x "$GOCMEM" || "${REBUILD:-0}" == 1 ]]; then
  QJSCLI_TAGS=qjsmem QJSCLI_OUT="$GOCMEM" "$ROOT/scripts/qjs-cli-build.sh" > "$HB/qjsmem-build.log" 2>&1 \
    || { echo "qjsmem build failed, see $HB/qjsmem-build.log"; exit 1; }
fi
if [[ ! -x "$GOCBMEM" || "${REBUILD:-0}" == 1 ]]; then
  QJS_FLAVOR=bellard QJSCLI_TAGS=qjsmem QJSCLI_OUT="$GOCBMEM" "$ROOT/scripts/qjs-cli-build.sh" \
    > "$HB/qjsmem-bellard-build.log" 2>&1 \
    || { echo "goc-bellard qjsmem build failed, see $HB/qjsmem-bellard-build.log"; exit 1; }
fi
clang-19 -O2 -D_GNU_SOURCE -DQJSMEM_ENGINE='"ng"' -I"$NGDIR" "$ROOT/tests/qjsmem/threads.c" \
  "$NGDIR/b-O2/libqjs.a" -lm -lpthread -o "$HB/threads-ng" || exit 1
gcc -O2 -D_GNU_SOURCE -DQJSMEM_ENGINE='"bellard"' -I"$BDIR" "$ROOT/tests/qjsmem/threads.c" \
  "$BDIR"/.obj/{quickjs,libregexp,libunicode,cutils,dtoa}.o -lm -lpthread -o "$HB/threads-bellard" || exit 1
( cd "$ROOT/scripts/gojamem" && go build -o "$HB/gojamem" . ) || exit 1
INST="$ROOT/tests/qjscli/instance.js"

{
  echo "date: $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo "host: $(uname -srm), $(nproc) vCPU; RSS runs pinned to CPU $CPU, instance runs not pinned"
  echo "goc commit: $(git -C "$ROOT" rev-parse HEAD)"
  echo "go: $(go version)"
  echo "GOGC=${GOGC:-<unset, default 100>} GOMEMLIMIT=${GOMEMLIMIT:-<unset>}"
  for e in "${ENGINES[@]}"; do echo "$e: $(cmd "$e")  sha256=$(sha256sum "$(cmd "$e" | cut -d' ' -f1)" | cut -c1-16)"; done
  for b in "$GOCMEM" "$GOCBMEM" "$HB/threads-ng" "$HB/threads-bellard" "$HB/gojamem"; do
    echo "harness: ${b#$ROOT/}  sha256=$(sha256sum "$b" | cut -c1-16)"
  done
  echo "rounds: $ROUNDS; N: $NS; C harness thread stack: ${QJSMEM_STACK_KB:-1024} KiB"
} > "$OUT/env-mem.txt"
cat "$OUT/env-mem.txt"

measure() {  # measure FILE WORKLOAD CWD CMD... -> appends one line unless present
  local f="$1" w="$2" dir="$3"; shift 3
  grep -q "^$w " "$f" 2>/dev/null && return
  local tv; tv=$(mktemp)
  ( cd "$dir" && /usr/bin/time -v -o "$tv" taskset -c "$CPU" "$@" > /dev/null 2>&1 )
  local kb el st
  kb=$(awk -F': ' '/Maximum resident set size/ {print $2}' "$tv")
  el=$(awk -F': ' '/Elapsed \(wall clock\)/ {print $2}' "$tv" | awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; print s }')
  st=$(awk -F': ' '/Exit status/ {print $2}' "$tv")
  rm -f "$tv"
  echo "$w $kb $el $st" >> "$f"
}

if ! skip rss; then
  for ((r = 1; r <= ROUNDS; r++)); do
    for e in $(order $r); do
      f="$OUT/mem-rss-$e-r$r.txt"; touch "$f"
      c=$(cmd $e)
      measure "$f" empty "$HB" $c "$HB/empty.js"
      for s in "${V8SUITES[@]}"; do measure "$f" "v8-$s" "$HB" $c "$HB/v8-$s.js"; done
      measure "$f" v8-all "$HB" $c "$V8JS"
      for js in "$SSDIR"/*.js; do measure "$f" "ss-$(basename "$js" .js)" "$SSDIR" $c "$js"; done
      measure "$f" micro "$HB" $(cmd $e micro) "$MB"
      # alloc.js and mapset.js are not in the repo. Skip a missing file
      # instead of recording a failed open as a workload.
      if [[ -f "$ALLOCJS" ]]; then measure "$f" alloc "$HB" $c "$ALLOCJS"; fi
      if [[ -f "$MAPSETJS" ]]; then measure "$f" mapset "$HB" $c "$MAPSETJS"; fi
      echo "rss r$r $e: $(awk '$1=="empty"||$1=="v8-all"||$1=="micro"{printf "%s=%s ", $1, $2}' "$f")"
    done
  done
fi

if ! skip inst; then
  for ((r = 1; r <= ROUNDS; r++)); do
    f="$OUT/mem-inst-r$r.txt"; touch "$f"
    for n in $NS; do
      for e in goc-ng goc-bellard ng ng-arena1 bellard goja; do
        grep -q "^MEMINST {\"tag\":\"$e\",\"n\":$n," "$f" && continue
        case $e in
          goc-ng)    line=$("$GOCMEM" --mem-instances "$n" 2>/dev/null) ;;
          goc-bellard) line=$("$GOCBMEM" --mem-instances "$n" 2>/dev/null) ;;
          ng)        line=$("$HB/threads-ng" "$n" "$INST" 2>/dev/null) ;;
          ng-arena1) line=$(MALLOC_ARENA_MAX=1 "$HB/threads-ng" "$n" "$INST" 2>/dev/null) ;;
          bellard)   line=$("$HB/threads-bellard" "$n" "$INST" 2>/dev/null) ;;
          goja)      line=$("$HB/gojamem" "$n" "$INST" 2>/dev/null) ;;
        esac
        line=$(grep '^MEMINST ' <<< "$line")
        [[ -n "$line" ]] || line='MEMINST {"error":"no output"}'
        echo "MEMINST {\"tag\":\"$e\",\"n\":$n,\"data\":${line#MEMINST }}" >> "$f"
        echo "inst r$r $e N=$n $(grep -o '"live":{"VmRSSKB":[0-9]*' <<< "$line")"
      done
    done
  done
fi
