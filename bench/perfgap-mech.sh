#!/usr/bin/env bash
# Build bench/perfgap/mech.c with goc (same switches as ports/quickjs/build.sh) and
# natively (clang -O3, clang -O3 + goc codegen flags, gcc -O2), then run all
# four pinned to one core. OUT defaults to build/perfgap-mech.
# goc build --llc-extra builds a goc variant.
set -euo pipefail
# shellcheck source=../test/lib.sh
source "$(cd "$(dirname "$0")/.." && pwd)/test/lib.sh"
OUT="${OUT:-$ROOT/build/perfgap-mech}"
CPU="${CPU:-3}"
ROUNDS="${ROUNDS:-5}"
mkdir -p "$OUT"
GOCB=("$GOC" build -O3 --default-ptr-color cptr --fast-stack-alloca)
SRC="$ROOT/bench/perfgap"
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
  # one goc flag at a time on top of clang -O3 (docs/perf-gap.md, section 6)
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
