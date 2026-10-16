#!/usr/bin/env bash
# One C file with 600 functions. go1.25+ sizes the DWARF5 text-address
# index relocations (R_DWTXTADDR_U1..U4) by the object's function count
# (obj.Link.DwTextCount); a converter that leaves it 0 overflows U1 at link
# time (docs/go-version-diffs.md).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/test/manyfuncs"
rm -rf "$OUT" && mkdir -p "$OUT/src"
N=600
{
  echo '#include "goc.h"'
  for i in $(seq 0 $((N - 1))); do
    echo "__attribute__((noinline)) long mf_$i(long x) { return x + $i; }"
  done
  echo 'long manyfuncs_sum(long x) {'
  echo '  long s = 0;'
  for i in $(seq 0 $((N - 1))); do echo "  s += mf_$i(x);"; done
  echo '  return s;'
  echo '}'
} >"$OUT/src/manyfuncs.c"
cat >"$OUT/src/main.go" <<'GO'
package main

import (
	"fmt"
	"os"
)

func manyfuncs_sum(x int64) int64

func main() {
	if got, want := manyfuncs_sum(1), int64(600+599*600/2); got != want {
		fmt.Printf("FAIL manyfuncs: got %d want %d\n", got, want)
		os.Exit(1)
	}
	fmt.Println("PASS manyfuncs")
}
GO
printf 'module goc.local/manyfuncs\n\ngo 1.23\n' >"$OUT/src/go.mod"
cp "$ROOT/test/e2e/retptr/stubs_amd64.s" "$OUT/src/"
# shellcheck source=../../../scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"
GOC_OPT_LEVEL=1 "$ROOT/cmd/goc" build "$OUT/src/manyfuncs.c" -o "$OUT/manyfuncs.goobj" \
  >"$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log" >&2; exit 1; }
( cd "$OUT/src" && CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/manyfuncs.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" -o "$OUT/manyfuncs" . ) \
  >"$OUT/go-build.log" 2>&1 || { tail -20 "$OUT/go-build.log" >&2; exit 1; }
"$OUT/manyfuncs"
