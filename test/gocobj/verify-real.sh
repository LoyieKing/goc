#!/usr/bin/env bash
# Run goc-objdump verify on every .gocobj the pipeline wrote under build/.
# Usage: test/gocobj/verify-real.sh [DIR]   (default: build)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${1:-$ROOT/build}"
mapfile -t files < <(find "$DIR" -name '*.gocobj' -not -path '*/test/gocobj/*' | sort)
if [[ ${#files[@]} -eq 0 ]]; then
  echo "verify-real: no .gocobj under $DIR" >&2
  exit 1
fi
( cd "$ROOT" && "${GOC_HOST_GO:-go}" build -o "$ROOT/build/goc-objdump" ./tools/goc-objdump )
"$ROOT/build/goc-objdump" verify "${files[@]}"
echo "verify-real: ${#files[@]} files ok"
