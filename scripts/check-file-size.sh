#!/usr/bin/env bash
# Fail when a goc source file grows past MAX lines (default 800). Split a big
# pass by phase behind an internal header (see llvm/lib/Transforms/StackMap)
# rather than raising the limit.
#
# Vendored code (third_party/, goobj/enc/: the Go toolchain's objlib) and test
# inputs (test/, testdata/) are not checked. The files listed in
# scripts/file-size-allow.txt predate the limit; each may not grow past the
# size recorded there, and an entry should go once its file is split.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"
MAX="${GOC_MAX_FILE_LINES:-800}"
ALLOW=scripts/file-size-allow.txt

declare -A cap
while read -r path n _; do
  [[ -z "$path" || "$path" == \#* ]] && continue
  cap[$path]=$n
done < "$ALLOW"

fail=0
while IFS= read -r f; do
  [[ -f "$f" ]] || continue
  n=$(wc -l < "$f")
  limit="${cap[$f]:-$MAX}"
  if (( n > limit )); then
    if [[ -n "${cap[$f]:-}" ]]; then
      echo "FAIL $f: $n lines, allowed $limit (grandfathered in $ALLOW; split it, do not grow it)" >&2
    else
      echo "FAIL $f: $n lines > $MAX; split it" >&2
    fi
    fail=1
  fi
done < <(git ls-files -- cmd llvm goobj tools scripts gocobj \
  | grep -Ev '^goobj/enc/|/testdata/' \
  | grep -E '\.(c|cc|cpp|h|go|py|sh|s|S|td|def)$|^cmd/goc$')

for path in "${!cap[@]}"; do
  [[ -f "$path" ]] || { echo "FAIL $ALLOW lists missing $path" >&2; fail=1; continue; }
  n=$(wc -l < "$path")
  if (( n <= MAX )); then
    echo "FAIL $path is $n lines now; remove it from $ALLOW" >&2
    fail=1
  fi
done

(( fail == 0 )) && echo "PASS file sizes (max $MAX lines)"
exit $fail
