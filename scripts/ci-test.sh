#!/usr/bin/env bash
# Tests that need neither a QuickJS checkout nor a benchmark run. CI runs
# exactly this; each step is a runner under test/ (see test/README.md).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"
export GOC_ROOT="$ROOT"

step() { echo "=== $1 ==="; shift; "$@"; }

step "file sizes"        ./scripts/check-file-size.sh
step "goc check"         ./cmd/goc check
step "clang"             ./test/clang/run.sh
step "color-escape"      ./test/color-escape/run.sh
step "lit"               ./test/lit/run.sh
step "realbody"          ./test/e2e/realbody/run.sh
step "reproducible"      ./test/e2e/repro/run.sh
step "goabi"             ./test/e2e/goabi/run.sh
step "go_extern"         ./test/e2e/go_extern/run.sh
step "stress"            ./test/e2e/stress/run.sh
step "retptr"            ./test/e2e/retptr/run.sh
step "many functions"    ./test/e2e/manyfuncs/run.sh
step "lines"             ./test/e2e/lines/run.sh
step "arm64"             ./test/e2e/arm64/run.sh
step "arm64 stress"      ./test/e2e/arm64/stress.sh
step "arm64 retptr"      ./test/e2e/arm64/retptr.sh
step "go unit tests"     "${GOC_HOST_GO:-go}" test ./gocobj/ ./gometa/ ./goobj/... ./goobjw/ ./tools/...
step "gocobj goldens"    "${GOC_HOST_GO:-go}" run ./tools/goc-objdump verify test/gocobj/v1.0/valid.gocobj
step "gocobj-diff"       ./test/gocobj/diff.sh

echo "=== ci-test: all passed ==="
