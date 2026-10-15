#!/usr/bin/env bash
# gocobj-diff smoke test on objects the e2e runners already built: an object
# equals itself, and two different objects are reported as different.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
A="$ROOT/build/test/realbody/realbody.goobj"
B="$ROOT/build/test/goabi/goabi.goobj"
for f in "$A" "$B"; do
  [[ -f "$f" ]] || { echo "gocobj-diff test: missing $f (run test/e2e/realbody and goabi first)" >&2; exit 1; }
done
cd "$ROOT"
go run ./tools/gocobj-diff "$A" "$A"
if go run ./tools/gocobj-diff "$A" "$B" >/dev/null; then
  echo "FAIL gocobj-diff: realbody and goabi objects compared equal" >&2
  exit 1
fi
echo "PASS gocobj-diff"
