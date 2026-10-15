#!/usr/bin/env bash
# One C file through the whole pipeline (tools/goc/realbody.sh): patched
# clang, the IR passes, goc-llc, gocobj2goobj. Checks that the .gocobj
# verifies and that the body in the Go object is the compiled source (its
# magic constant), not a placeholder.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
cd "$ROOT"
OUT="$ROOT/build/test/realbody"
mkdir -p "$OUT"
# shellcheck source=../../../scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"

"$ROOT/tools/goc/realbody.sh" "$ROOT/test/e2e/realbody/realbody.c" \
  "$OUT/realbody.goobj" | tee "$OUT/realbody.log"
test -f "$OUT/realbody.goobj"
test -f "$OUT/realbody.gocobj"
go run ./tools/goc-objdump verify "$OUT/realbody.gocobj"
grep -q 'magic 0x28C0DE42' "$OUT/realbody.log"
echo "PASS realbody"
