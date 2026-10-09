#!/usr/bin/env bash
# P29 golden: clang-compiled C → Go-ABIInternal thunks → goobj → Go binary → run.
# Covers scalar register and stack ABI cases, mixed floating-point arguments,
# and two- and three-result mixed-class aggregate returns. Each Go→C thunk
# frame is that signature's stack arguments rounded up to 16.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
export GOC_MORESTACK=1 GOC_NO_NOSPLIT=1 GOC_SPTR_MAPS=1
OUT="$ROOT/build/p29-goabi"
mkdir -p "$OUT"

# shellcheck source=goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_resolve_clang >/dev/null
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"

echo "=== P29-goabi: C → goobj (Go ABIInternal entry thunks) ==="
env -u GOC_DEFAULT_PTR_COLOR "$ROOT/cmd/goc" build \
  "$ROOT/tests/goabi/goabi.c" -o "$OUT/goabi.o" --all --goabi | tee "$OUT/build.log"

go tool nm "$OUT/goabi.o" | tee "$OUT/nm.txt"
rg -q ' T main\.goabi_add2$' "$OUT/nm.txt"
rg -q ' T main\.goabi_add2\.impl$' "$OUT/nm.txt"
rg -q ' T main\.goabi_addl$' "$OUT/nm.txt"

echo "=== P29-goabi: Go caller + toolexec pack ==="
( cd "$ROOT/tests/goabi" && \
  CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/goabi.o" \
  go build -a -toolexec "$ROOT/backend/tools/toolexec_pack_goobj.sh" \
    -o "$OUT/goabi_test" . ) 2>&1 | tee "$OUT/go-build.log"

"$OUT/goabi_test" | tee "$OUT/run.log"
rg -q '^PASS p29-goabi' "$OUT/run.log"

echo ""
echo "PASS p29-goabi (Go ABIInternal scalar and aggregate thunks)"
