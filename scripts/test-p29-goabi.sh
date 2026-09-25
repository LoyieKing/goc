#!/usr/bin/env bash
# P29 golden: clang-compiled C → Go-ABIInternal thunks → goobj → Go binary → run.
# Covers scalar register and stack ABI cases, mixed floating-point arguments,
# and two- and three-result mixed-class aggregate returns. GOC_CRESERVE gives
# the fixed call thunk its PCSP-described frame and C stack reservation.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
# Larger than a new goroutine's stack: the first Go→C call must enter the
# morestack slow path, exercising typed argument spill/reload and pointer maps.
export GOC_CRESERVE="${GOC_CRESERVE:-32768}"
export GOC_MORESTACK=1 GOC_NO_NOSPLIT=1 GOC_SPTR_MAPS=1
OUT="$ROOT/build/p29-goabi"
mkdir -p "$OUT"

CLANG="${GOC_CLANG:-}"
if [[ -z "$CLANG" ]]; then
  for c in "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang" \
           "$ROOT/third_party/llvm-clang-build/bin/clang"; do
    [[ -x "$c" ]] && CLANG="$c" && break
  done
fi
if [[ -z "$CLANG" || ! -x "$CLANG" ]]; then
  echo "FAIL: set GOC_CLANG to a patched clang-19 (in-tree Sema required)" >&2
  exit 1
fi
export GOC_CLANG="$CLANG"

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
