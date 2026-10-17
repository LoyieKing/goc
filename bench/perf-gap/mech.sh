#!/usr/bin/env bash
# Mechanism probes for docs/research/perf-gap.md.
#
#   perf-gap/mech.sh [c]   build bench/perf-gap/mech/mech.c with goc (same switches
#                          as ports/quickjs/build.sh) and natively (clang -O3,
#                          clang -O3 + goc codegen flags, gcc -O2), then run them
#                          all pinned to one core. OUT defaults to build/perf-gap-mech.
#                          goc build --llc-extra builds a goc variant.
#   perf-gap/mech.sh js    run bench/js/mech.js (JS mechanism probes, ns/op) on
#                          several engines, pinned, ROUNDS interleaved rounds.
#                          Commands come from ENGINE_MAP (default
#                          docs/perf-gap/data/raw-toggles/engines.txt).
#                          OUT=docs/perf-gap/data/mech-js  ROUNDS=3  CPU=3  ENGINES="a b ..."
set -euo pipefail
MODE="${1:-c}"
CPU="${CPU:-3}"
if [[ "$MODE" == js ]]; then
  ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
  MAP="${ENGINE_MAP:-$ROOT/docs/perf-gap/data/raw-toggles/engines.txt}"
  OUT="${OUT:-$ROOT/docs/perf-gap/data/mech-js}"
  ROUNDS="${ROUNDS:-3}"
  ENGINES="${ENGINES:-bellard-gcc-O2-NDEBUG bellard-clang-O3 bellard-clang-O3-gocflags bellard-gocpipe-sm goc-bellard goc-bellard-fast goc-bellard-best ng-clang-O2 ng-clang-O3 ng-clang-O3-gocflags goc-ng goc-ng-fast goc-ng-best}"
  mkdir -p "$OUT"
  for ((r = 1; r <= ROUNDS; r++)); do
    for e in $ENGINES; do
      c=$(awk -v n="$e" '$1==n {$1=""; print substr($0,2)}' "$MAP")
      [[ -n "$c" ]] || { echo "no command for $e" >&2; exit 1; }
      # shellcheck disable=SC2086
      taskset -c "$CPU" $c "$ROOT/bench/js/mech.js" > "$OUT/$e-r$r.txt" 2>&1 || echo "FAIL $e r$r"
      echo "r$r $e $(wc -l < "$OUT/$e-r$r.txt") lines"
    done
  done
  echo MECHJSDONE
  exit 0
fi
[[ "$MODE" == c ]] || { echo "usage: $0 [c|js]" >&2; exit 2; }
# shellcheck source=../../test/lib.sh
source "$(cd "$(dirname "$0")/../.." && pwd)/test/lib.sh"
OUT="${OUT:-$ROOT/build/perf-gap-mech}"
ROUNDS="${ROUNDS:-5}"
mkdir -p "$OUT"
GOCB=("$GOC" build -O3 --default-ptr-color cptr --fast-stack-alloca)
SRC="$ROOT/bench/perf-gap/mech"
if [[ "${BUILD:-1}" == 1 ]]; then
  "${GOCB[@]}" "$SRC/mech.c" -o "$OUT/mech.goobj" -- -DNDEBUG
  ( cd "$SRC" && CGO_ENABLED=0 GOFLAGS= \
    go build -a -toolexec "$GOC toolexec $OUT/mech.goobj --" -o "$OUT/mech-goc" . )
  # goc with LLVM's tail duplication limits raised (computed-goto dispatch)
  mkdir -p "$OUT/td"
  "${GOCB[@]}" --llc-extra "-tail-dup-pred-size=1000 -tail-dup-succ-size=1000" \
    "$SRC/mech.c" -o "$OUT/td/mech.goobj" -- -DNDEBUG
  ( cd "$SRC" && CGO_ENABLED=0 GOFLAGS= \
    go build -a -toolexec "$GOC toolexec $OUT/td/mech.goobj --" -o "$OUT/mech-goc-taildup" . )
  GF_FULL="-fwrapv -fno-strict-aliasing -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
-fno-optimize-sibling-calls -mno-red-zone -fno-stack-protector -fno-asynchronous-unwind-tables \
-mstack-alignment=8 -mllvm -no-stack-slot-sharing -mllvm -enable-shrink-wrap=false \
-mllvm -no-x86-call-frame-opt -mllvm -enable-tail-merge=false"
  CL="${NATIVE_CLANG:-clang-19}"
  $CL -O3 -DNDEBUG -o "$OUT/mech-clang-O3" "$SRC/mech.c" "$SRC/mech_native_main.c"
  # shellcheck disable=SC2086
  # Only mech.c gets the goc flags: the driver calls printf, and glibc's
  # varargs code needs the 16-byte stack alignment -mstack-alignment=8 drops.
  $CL -O3 -DNDEBUG $GF_FULL -c -o "$OUT/mech-gf.o" "$SRC/mech.c"
  $CL -O3 -o "$OUT/mech-clang-O3-gocflags" "$OUT/mech-gf.o" "$SRC/mech_native_main.c"
  $CL -O3 -DNDEBUG -mllvm -tail-dup-pred-size=1000 -mllvm -tail-dup-succ-size=1000 \
    -o "$OUT/mech-clang-O3-taildup" "$SRC/mech.c" "$SRC/mech_native_main.c"
  gcc -O2 -DNDEBUG -o "$OUT/mech-gcc-O2" "$SRC/mech.c" "$SRC/mech_native_main.c"
  # one goc flag at a time on top of clang -O3 (docs/research/perf-gap.md, section 6)
  for f in "fp:-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer" \
           "nosib:-fno-optimize-sibling-calls" "notailmerge:-mllvm -enable-tail-merge=false" \
           "align8:-mstack-alignment=8" "noslotshare:-mllvm -no-stack-slot-sharing"; do
    # shellcheck disable=SC2086
    $CL -O3 -DNDEBUG ${f#*:} -c -o "$OUT/mech-f.o" "$SRC/mech.c"
    $CL -O3 -o "$OUT/mech-clang-O3-f-${f%%:*}" "$OUT/mech-f.o" "$SRC/mech_native_main.c"
  done
fi
for b in "$OUT"/mech-*; do
  [[ -x "$b" && "$b" != *.o ]] || continue
  echo "== $(basename "$b")"
  taskset -c "$CPU" "$b" "$ROUNDS"
done
