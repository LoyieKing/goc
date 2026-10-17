#!/usr/bin/env bash
# Fail when a goc source file grows past MAX lines (default 800). There are
# no exceptions: split a big file by phase (see llvm/lib/Transforms/StackMap
# and its Goc/Internal header, a Go package split into several files, or a
# C file split into #included .inc parts as in ports/quickjs/host)
# rather than raising the limit.
#
# Vendored code (third_party/) and test
# inputs (test/, testdata/) are not checked.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
MAX="${GOC_MAX_FILE_LINES:-800}"

fail=0
while IFS= read -r f; do
  [[ -f "$f" ]] || continue
  n=$(wc -l < "$f")
  if (( n > MAX )); then
    echo "FAIL $f: $n lines > $MAX; split it" >&2
    fail=1
  fi
done < <(git ls-files -- cmd internal llvm scripts ports \
  | grep -Ev '/testdata/' \
  | grep -E '\.(c|cc|cpp|h|inc|go|py|sh|s|S|td|def)$')

(( fail == 0 )) && echo "PASS file sizes (max $MAX lines)"
exit $fail
