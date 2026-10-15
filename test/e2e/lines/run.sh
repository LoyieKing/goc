#!/usr/bin/env bash
# C file and line numbers in the Go object: runtime.FuncForPC and
# runtime.CallersFrames on pcs inside C, with an inlined frame (test/e2e/lines).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/test/lines"
mkdir -p "$OUT"

# shellcheck source=goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"

env -u GOC_DEFAULT_PTR_COLOR GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-2}" "$ROOT/cmd/goc" build \
  "$ROOT/test/e2e/lines/lines.c" -o "$OUT/lines.goobj" >"$OUT/build.log" 2>&1 || {
  tail -20 "$OUT/build.log" >&2
  exit 1
}
go run ./tools/goc-objdump verify "$OUT/lines.gocobj"
( cd "$ROOT/test/e2e/lines" && \
  CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/lines.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" \
    -o "$OUT/lines_test" . ) >"$OUT/go-build.log" 2>&1 || {
  tail -20 "$OUT/go-build.log" >&2
  exit 1
}
"$OUT/lines_test" | tee "$OUT/run.log"
grep -q '^PASS lines' "$OUT/run.log"
