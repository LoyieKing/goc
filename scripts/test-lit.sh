#!/usr/bin/env bash
# FileCheck tests of the goc passes and goc-llc (test/lit). Needs lit
# (pip install lit) and LLVM 19's FileCheck/not/opt (llvm-19-tools / llvm-19).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export LLVM_BIN="${LLVM_BIN:-$(llvm-config-19 --bindir 2>/dev/null || echo /usr/lib/llvm-19/bin)}"
LIT="${LIT:-$(command -v lit || command -v llvm-lit || true)}"
[[ -n "$LIT" ]] || LIT="$(command -v "$LLVM_BIN/llvm-lit" || true)"
if [[ -z "$LIT" ]]; then
  if [[ -n "${GOC_REQUIRE_LIT:-}" ]]; then echo "FAIL test-lit: no lit" >&2; exit 1; fi
  echo "SKIP test-lit: no lit (pip install lit)"; exit 0
fi
"$LIT" -sv "$ROOT/test/lit"
