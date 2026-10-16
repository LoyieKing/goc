#!/usr/bin/env bash
# Reproducible builds: the same C file built twice through the whole
# pipeline (tools/goc/realbody.sh) gives byte-identical .gocobj and .goobj,
# and gocobj2goobj gives the same Go object every time it converts one
# .gocobj. repro.c makes many GOT slots from many text sections; the old
# converter ordered them by map iteration and failed this within a few runs.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
cd "$ROOT"
OUT="$ROOT/build/test/repro"
rm -rf "$OUT"; mkdir -p "$OUT/a" "$OUT/b"
# shellcheck source=../../../scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"
SRC="$ROOT/test/e2e/repro/repro.c"

for d in a b; do
  GOC_OPT_LEVEL=2 "$ROOT/tools/goc/realbody.sh" "$SRC" "$OUT/$d/repro.goobj" >"$OUT/$d/build.log" 2>&1 \
    || { cat "$OUT/$d/build.log" >&2; exit 1; }
done
for f in repro.gocobj repro.goobj; do
  cmp "$OUT/a/$f" "$OUT/b/$f" || { echo "FAIL repro: $f differs between two builds" >&2; exit 1; }
done

# shellcheck source=../../../tools/goc/converter.sh
source "$ROOT/tools/goc/converter.sh"
CONV="$(goc_converter "$ROOT")"
for i in $(seq 10); do
  "$CONV" -in "$OUT/a/repro.gocobj" -o "$OUT/conv$i.goobj" -symabis "$OUT/conv$i.symabis"
  cmp "$OUT/conv1.goobj" "$OUT/conv$i.goobj" && cmp "$OUT/conv1.symabis" "$OUT/conv$i.symabis" \
    || { echo "FAIL repro: gocobj2goobj output differs on run $i" >&2; exit 1; }
done
echo "PASS repro"
