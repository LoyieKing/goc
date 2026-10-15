#!/usr/bin/env bash
# test/e2e/stress built for linux/arm64 and run under qemu-aarch64: C recursion
# in the three stack-check tiers while GCs grow and shrink the stacks.
# Skipped when qemu-aarch64 is not installed (ARM64_REQUIRE_QEMU=1: fail).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export GOC_ROOT="$ROOT"
cd "$ROOT"
QEMU="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
if [[ -z "$QEMU" ]]; then
  [[ -z "${ARM64_REQUIRE_QEMU:-}" ]] || { echo "FAIL arm64 stress: no qemu-aarch64" >&2; exit 1; }
  echo "SKIP arm64 stress: no qemu-aarch64"; exit 0
fi
OUT="$ROOT/build/test/stress-arm64"
mkdir -p "$OUT"
env -u GOC_DEFAULT_PTR_COLOR GOC_ARCH=arm64 GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-2}" "$ROOT/cmd/goc" build \
  "$ROOT/test/e2e/stress/stress.c" -o "$OUT/stress.goobj" >"$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log" >&2; exit 1; }
go run ./tools/goc-objdump verify "$OUT/stress.gocobj"
STUB="$ROOT/test/e2e/stress/stubs_arm64.s"
cp "$ROOT/test/e2e/stress/stubs_amd64.s" "$STUB"
trap 'rm -f "$STUB"' EXIT
( cd "$ROOT/test/e2e/stress" && CGO_ENABLED=0 GOARCH=arm64 GOFLAGS= GOC_BINOBJ="$OUT/stress.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" -o "$OUT/stress_test" . ) \
  >"$OUT/go-build.log" 2>&1 || { tail -20 "$OUT/go-build.log" >&2; exit 1; }
timeout 600 "$QEMU" "$OUT/stress_test" | tee "$OUT/run.log"
grep -q '^PASS stress' "$OUT/run.log"
GOMAXPROCS=1 timeout 600 "$QEMU" "$OUT/stress_test" | grep -q '^PASS stress'
# C->C calls with stack arguments, across a stack copy.
SA="$ROOT/test/e2e/arm64/stackargs"
env -u GOC_DEFAULT_PTR_COLOR GOC_ARCH=arm64 GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-2}" "$ROOT/cmd/goc" build \
  "$SA/sa.c" -o "$OUT/sa.goobj" >"$OUT/sa-build.log" 2>&1 || { tail -20 "$OUT/sa-build.log" >&2; exit 1; }
( cd "$SA" && CGO_ENABLED=0 GOARCH=arm64 GOFLAGS= GOC_BINOBJ="$OUT/sa.goobj" \
  go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" -o "$OUT/sa_test" . ) \
  >"$OUT/sa-go-build.log" 2>&1 || { tail -20 "$OUT/sa-go-build.log" >&2; exit 1; }
got="$(timeout 60 "$QEMU" "$OUT/sa_test" 2>&1)"
[[ "$got" == "$(printf '801\n536')" ]] || { echo "FAIL stackargs: $got" >&2; exit 1; }
echo "PASS arm64 stack arguments"
echo "PASS arm64 stress"
