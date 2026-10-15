#!/usr/bin/env bash
# test/stress built for linux/arm64 and run under qemu-aarch64: C recursion
# in the three stack-check tiers while GCs grow and shrink the stacks.
# Skipped when qemu-aarch64 is not installed.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
cd "$ROOT"
QEMU="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
[[ -n "$QEMU" ]] || { echo "SKIP test-stress-arm64: no qemu-aarch64"; exit 0; }
OUT="$ROOT/build/stress-arm64"
mkdir -p "$OUT"
env -u GOC_DEFAULT_PTR_COLOR GOC_ARCH=arm64 GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-2}" "$ROOT/cmd/goc" build \
  "$ROOT/test/stress/stress.c" -o "$OUT/stress.goobj" >"$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log" >&2; exit 1; }
go run ./tools/goc-objdump verify "$OUT/stress.gocobj"
STUB="$ROOT/test/stress/stubs_arm64.s"
cp "$ROOT/test/stress/stubs_amd64.s" "$STUB"
trap 'rm -f "$STUB"' EXIT
( cd "$ROOT/test/stress" && CGO_ENABLED=0 GOARCH=arm64 GOFLAGS= GOC_BINOBJ="$OUT/stress.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" -o "$OUT/stress_test" . ) \
  >"$OUT/go-build.log" 2>&1 || { tail -20 "$OUT/go-build.log" >&2; exit 1; }
timeout 600 "$QEMU" "$OUT/stress_test" | tee "$OUT/run.log"
grep -q '^PASS stress' "$OUT/run.log"
GOMAXPROCS=1 timeout 600 "$QEMU" "$OUT/stress_test" | grep -q '^PASS stress'
echo "PASS test-stress-arm64"
