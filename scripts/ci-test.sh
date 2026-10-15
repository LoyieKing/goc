#!/usr/bin/env bash
# Tests that do not need a QuickJS checkout or a benchmark run.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"
export GOC_ROOT="$ROOT"

echo "=== file sizes ==="
./scripts/check-file-size.sh

echo "=== goc check ==="
./cmd/goc check

echo "=== p28 ==="
./cmd/goc test --p28

echo "=== p29 ==="
./scripts/test-p29-goabi.sh

echo "=== stress ==="
./scripts/test-stress.sh

echo "=== lines ==="
./scripts/test-lines.sh

echo "=== arm64 ==="
./test/arm64/check_arm64.sh
./scripts/test-stress-arm64.sh

echo "=== gocobj ==="
(cd "$(dirname "$0")/.." && go test ./gocobj/ && go run ./tools/goc-objdump verify test/gocobj/v1.0/valid.gocobj && go test ./goobj/lower/ ./tools/gocobj2goobj/ && go run ./tools/gocobj-diff build/p28-out/p28_real.goobj build/p28-out/p28_real.goobj)

echo "=== p27 ==="
./cmd/goc test --p27

echo "=== ci-test: all passed ==="
