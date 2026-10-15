#!/usr/bin/env bash
# color-escape.sh IN.ll -o OUT.ll [opt flags...]
# Runs the goc-color-escape pass from libGocPasses.so under opt. With
# GOC_COLOR_REPORT=1 it also runs print<goc-color> first.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PLUGIN="${GOC_STACKMAP:-$REPO/build/llvm/libGocPasses.so}"
OPT="${OPT:-$(command -v opt-19 || command -v opt || echo /usr/lib/llvm-19/bin/opt)}"
in="$1"; shift
[[ "${1:-}" == -o ]] || { echo "usage: $0 IN.ll -o OUT.ll [flags]" >&2; exit 2; }
out="$2"; shift 2
passes=goc-color-escape
[[ "${GOC_COLOR_REPORT:-}" == 1 ]] && passes="print<goc-color>,goc-color-escape"
exec "$OPT" -load-pass-plugin="$PLUGIN" "-passes=$passes" "$@" -S "$in" -o "$out"
