#!/usr/bin/env bash
# Toggle experiments for docs/perf-gap.md, timed like scripts/perfgap-bench.sh
# (pinned, interleaved rounds via scripts/bench-all.sh), each set with its own
# reference builds in the same session.
#
#   SET=toggles  tail duplication (native + goc), the shim experiment
#                (docs/perf-gap/experiments/shim-fast.patch), goc's pipeline
#                built natively (gocpipe*), -inline-threshold=250 (inl250) and
#                all three together (best); all suites
#   SET=flags    Bellard clang -O3 plus one goc codegen flag at a time; V8 + SunSpider
#
# goc variants come from /workspace/perf-study/goc-variants (see docs/perf-gap.md,
# "reproduce"); natives from scripts/perfgap-build-natives.sh.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PS="${PS:-/workspace/perf-study}"
GV="${GV:-$PS/goc-variants}"
SET="${SET:-toggles}"
OUT="${OUT:-$ROOT/docs/perf-gap/data/raw-$SET}"
mkdir -p "$OUT"
B="--stack-size 16M"; N="-C --stack-size 16384"; G="--stack-size 16384"
case "$SET" in
toggles)
  cat > "$OUT/engines.txt" <<MAP
bellard-gcc-O2-NDEBUG $PS/bellard/gcc-O2-NDEBUG/qjs $B
bellard-clang-O2-NDEBUG $PS/bellard/clang-O2-NDEBUG/qjs $B
bellard-clang-O2-taildup $PS/bellard/clang-O2-NDEBUG-taildup/qjs $B
bellard-clang-O3 $PS/bellard/clang-O3-NDEBUG/qjs $B
bellard-clang-O3-taildup $PS/bellard/clang-O3-NDEBUG-taildup/qjs $B
bellard-clang-O3-gocflags $PS/bellard/clang-O3-gocfull/qjs $B
bellard-clang-O3-gocflags-taildup $PS/bellard/clang-O3-gocfull-taildup/qjs $B
bellard-gocpipe $PS/bellard/gocpipe/qjs $B
bellard-gocpipe-sm $PS/bellard/gocpipe-sm/qjs $B
bellard-gocpipe-inl250 $PS/bellard/gocpipe-inl250/qjs $B
bellard-gocpipe-sm-inl250 $PS/bellard/gocpipe-sm-inl250/qjs $B
goc-bellard $ROOT/build/qjs-bellard/qjscli $G
goc-bellard-taildup $GV/bellard-taildup/qjscli $G
goc-bellard-fast $GV/bellard-fast/qjscli $G
goc-bellard-fast-taildup $GV/bellard-fast-taildup/qjscli $G
goc-bellard-inl250 $GV/bellard-inl250/qjscli $G
goc-bellard-best $GV/bellard-best/qjscli $G
ng-clang-O2 $PS/native/b-O2/qjs $N
ng-clang-O3 $PS/native/b-O3/qjs $N
ng-clang-O3-taildup $PS/native/b-O3-taildup/qjs $N
ng-clang-O3-gocflags $PS/native/b-gocfull/qjs $N
ng-clang-O3-gocflags-taildup $PS/native/b-gocfull-taildup/qjs $N
goc-ng $ROOT/build/qjs/qjscli $G
goc-ng-taildup $GV/ng-taildup/qjscli $G
goc-ng-fast $GV/ng-fast/qjscli $G
goc-ng-fast-taildup $GV/ng-fast-taildup/qjscli $G
goc-ng-inl250 $GV/ng-inl250/qjscli $G
goc-ng-best $GV/ng-best/qjscli $G
MAP
  SK="t262 qjs"; RV=5; RS=3; RM=3; RC=3 ;;
flags)
  {
    echo "bellard-clang-O3 $PS/bellard/clang-O3-NDEBUG/qjs $B"
    echo "bellard-clang-O3-gocflags $PS/bellard/clang-O3-gocfull/qjs $B"
    echo "bellard-gocpipe $PS/bellard/gocpipe/qjs $B"
    for f in nsa fp nosib noredzone align8 noslotshare noshrink nocfo notailmerge signedchar; do
      echo "bellard-clang-O3-f-$f $PS/bellard/clang-O3-f-$f/qjs $B"
    done
  } > "$OUT/engines.txt"
  SK="t262 qjs micro microcall"; RV=5; RS=3; RM=3; RC=3 ;;
*) echo "SET must be toggles or flags" >&2; exit 1 ;;
esac
for c in $(awk '{print $2}' "$OUT/engines.txt"); do
  [[ -x "$c" ]] || { echo "missing $c" >&2; exit 1; }
done
[[ "${DRY:-0}" == 1 ]] && { echo "wrote $OUT/engines.txt"; exit 0; }
ENGINES="$(cut -d' ' -f1 "$OUT/engines.txt" | tr '\n' ' ')" \
ENGINE_MAP="$OUT/engines.txt" MICROCALL_PER_ENGINE=1 SKIP="${SKIP:-$SK}" OUT="$OUT" \
ROUNDS_V8="${ROUNDS_V8:-$RV}" ROUNDS_SS="${ROUNDS_SS:-$RS}" ROUNDS_MICRO="${ROUNDS_MICRO:-$RM}" \
ROUNDS_MICROCALL="${ROUNDS_MICROCALL:-$RC}" \
  "$ROOT/scripts/bench-all.sh"
