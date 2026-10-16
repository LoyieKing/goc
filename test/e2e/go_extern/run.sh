#!/usr/bin/env bash
# go_extern: C → Go stubs (__go_extern) → ordinary C calls → stack growth in Go.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/test/go_extern"
mkdir -p "$OUT"

# shellcheck source=goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"
goc_resolve_clang >/dev/null
goc_prepend_lib "$(goc_clang_libdir "$(goc_resolve_clang)")"

echo "=== go_extern: C → goobj (C→Go stubs) ==="
env -u GOC_DEFAULT_PTR_COLOR "$ROOT/cmd/goc" build \
  "$ROOT/test/e2e/go_extern/go_extern.c" -o "$OUT/go_extern.goobj" | tee "$OUT/build.log"
rg -q 'go_extern: 3 stubs' "$OUT/build.log"

go tool nm "$OUT/go_extern.goobj" | tee "$OUT/nm.txt"
rg -q ' T main\.go_extern_probe$' "$OUT/nm.txt"
rg -q ' T main\.go_extern_add$' "$OUT/nm.txt" || rg -q ' T main\.go_extern_add\b' "$OUT/nm.txt" || true

echo "=== go_extern: link and run ==="
( cd "$ROOT/test/e2e/go_extern" && \
  CGO_ENABLED=0 GOFLAGS= GOC_BINOBJ="$OUT/go_extern.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" \
    -o "$OUT/go_extern_test" . ) 2>&1 | tee "$OUT/go-build.log"

"$OUT/go_extern_test" | tee "$OUT/run.log"
rg -q '^PASS go_extern' "$OUT/run.log"

echo ""
echo "PASS go_extern (C→Go stubs including stack growth)"
