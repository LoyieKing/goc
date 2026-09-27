#!/usr/bin/env bash
# Build tests/perfgap/mech.c with goc (same env as scripts/qjs-build.sh) and
# natively (clang -O3, clang -O3 + goc codegen flags, gcc -O2), then run all
# four pinned to one core. OUT defaults to build/perfgap-mech.
# GOC_LLC_EXTRA (see backend/realbody/goc_p28_realbody.sh) builds a goc variant.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="${OUT:-$ROOT/build/perfgap-mech}"
CPU="${CPU:-3}"
ROUNDS="${ROUNDS:-5}"
mkdir -p "$OUT"
export GOC_CLANG="${GOC_CLANG:-$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang}"
export GOC_OPT_LEVEL=3 GOC_DEFAULT_PTR_COLOR=cptr GOC_NO_NOSPLIT=1 GOC_MORESTACK=1
export GOC_SPTR_MAPS=1 GOC_INLINE_DYNALLOC=1 GOC_CRESERVE=8192
SRC="$ROOT/tests/perfgap"
if [[ "${BUILD:-1}" == 1 ]]; then
  env -u GOC_PKG "$ROOT/cmd/goc" build "$SRC/mech.c" -o "$OUT/mech.o" --all --goabi -DNDEBUG
  ( cd "$SRC" && CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/mech.o" \
    go build -a -toolexec "$ROOT/backend/tools/toolexec_pack_goobj.sh" -o "$OUT/mech-goc" . )
  # goc with LLVM's tail duplication limits raised (computed-goto dispatch)
  mkdir -p "$OUT/td"
  env -u GOC_PKG GOC_LLC_EXTRA="-tail-dup-pred-size=1000 -tail-dup-succ-size=1000" \
    "$ROOT/cmd/goc" build "$SRC/mech.c" -o "$OUT/td/mech.o" --all --goabi -DNDEBUG
  ( cd "$SRC" && CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/td/mech.o" \
    go build -a -toolexec "$ROOT/backend/tools/toolexec_pack_goobj.sh" -o "$OUT/mech-goc-taildup" . )
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
