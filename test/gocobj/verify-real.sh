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
( cd "$ROOT" && go build -o "$ROOT/build/goc-objdump" ./tools/goc-objdump )
"$ROOT/build/goc-objdump" verify "${files[@]}"
# The meta entries the converter now builds from the tables must agree with
# goc-lower's text-derived meta.json.
( cd "$ROOT" && go build -o "$ROOT/build/gocobj2goobj" ./tools/gocobj2goobj )
for f in "${files[@]}"; do
  m="${f%.gocobj}.meta.json"
  [[ -f "$m" ]] || continue
  abi=
  grep -q '"abi": "ABI0"' "$m" && abi=ABI0
  "$ROOT/build/gocobj2goobj" -check-meta -in "$f" -meta "$m" -c-abi "$abi" -o /dev/null >/dev/null || {
    "$ROOT/build/gocobj2goobj" -check-meta -in "$f" -meta "$m" -c-abi "$abi" -o /dev/null | head -20 >&2
    exit 1
  }
done
echo "verify-real: ${#files[@]} files ok (verify + check-meta)"
