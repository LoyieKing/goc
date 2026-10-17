#!/usr/bin/env bash
# Tests that need neither a QuickJS checkout nor a benchmark run. CI runs
# exactly this; each step is a runner under test/ (see test/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=../test/lib.sh
source test/lib.sh

step() { echo "=== $1 ==="; shift; "$@"; }

step "file sizes"        ./scripts/check-file-size.sh
step "goc check"         "$GOC" check
step "lit"               ./test/lit/run.sh   # Sema, passes, goc-llc
step "e2e"               ./test/e2e/run.sh
step "go unit tests"     "${GOC_HOST_GO:-go}" test ./internal/... ./cmd/...
step "gocobj goldens"    "$GOC" tool objdump verify test/gocobj/v1.0/valid.gocobj

echo "=== ci-test: all passed ==="
