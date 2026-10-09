#!/usr/bin/env bash
# Tests that do not need a QuickJS checkout or a benchmark run.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"
export GOC_ROOT="$ROOT"

echo "=== goc check ==="
./cmd/goc check

echo "=== p28 ==="
./cmd/goc test --p28

echo "=== p29 ==="
./scripts/test-p29-goabi.sh

echo "=== arm64 ==="
./backend/realbody/check_arm64.sh

echo "=== p27 ==="
./cmd/goc test --p27

echo "=== ci-test: all passed ==="
