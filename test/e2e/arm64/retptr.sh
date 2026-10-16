#!/usr/bin/env bash
# test/e2e/retptr built for linux/arm64 and run under qemu-aarch64: a stack
# pointer returned through a call, used after the goroutine stack was copied.
# Skipped when qemu-aarch64 is not installed (ARM64_REQUIRE_QEMU=1: fail).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
# shellcheck source=goversion.sh
source "$ROOT/test/e2e/arm64/goversion.sh"
arm64_skip_old_go "arm64 retptr"
export GOC_ROOT="$ROOT"
cd "$ROOT"
QEMU="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
if [[ -z "$QEMU" ]]; then
  [[ -z "${ARM64_REQUIRE_QEMU:-}" ]] || { echo "FAIL arm64 retptr: no qemu-aarch64" >&2; exit 1; }
  echo "SKIP arm64 retptr: no qemu-aarch64"; exit 0
fi
OUT="$ROOT/build/test/retptr-arm64"
mkdir -p "$OUT"
STUB="$ROOT/test/e2e/retptr/stubs_arm64.s"
cp "$ROOT/test/e2e/retptr/stubs_amd64.s" "$STUB"
trap 'rm -f "$STUB"' EXIT
for O in 2 3; do
  GOC_DEFAULT_PTR_COLOR=cptr GOC_ARCH=arm64 GOC_OPT_LEVEL=$O "$ROOT/cmd/goc" build \
    "$ROOT/test/e2e/retptr/retptr.c" -o "$OUT/retptr.goobj" >"$OUT/build.log" 2>&1 || {
    tail -20 "$OUT/build.log" >&2; exit 1; }
  "${GOC_HOST_GO:-go}" run ./tools/goc-objdump verify "$OUT/retptr.gocobj"
  ( cd "$ROOT/test/e2e/retptr" && CGO_ENABLED=0 GOARCH=arm64 GOFLAGS= GOC_BINOBJ="$OUT/retptr.goobj" \
    go build -a -toolexec "$ROOT/tools/goc/toolexec_recognize_goobj.sh" -o "$OUT/retptr_test" . ) \
    >"$OUT/go-build.log" 2>&1 || { tail -20 "$OUT/go-build.log" >&2; exit 1; }
  timeout 600 "$QEMU" "$OUT/retptr_test" | tee "$OUT/run.log"
  grep -q '^PASS retptr' "$OUT/run.log"
  GOMAXPROCS=1 timeout 600 "$QEMU" "$OUT/retptr_test" | grep -q '^PASS retptr'
done
echo "PASS retptr (arm64/qemu)"
