#!/usr/bin/env bash
# Microcall score: geometric mean of calls/ms on the call-shaped cases.
# Higher is faster. Controls (arith, propget) are printed but not scored.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
JS="$ROOT/tests/bench/microcall.js"
GOC="${GOC_QJS:-$ROOT/build/qjs/qjscli}"
NATIVE="${NATIVE_QJS:-/tmp/goc-bench-v8/quickjs-native-build/qjs}"
STACK=(--stack-size 16384)

run_one() {
  local name="$1" bin="$2"
  if [[ ! -x "$bin" ]]; then
    echo "FAIL: missing $name binary $bin" >&2
    exit 1
  fi
  echo "=== $name ==="
  "$bin" "${STACK[@]}" "$JS" | tee "/tmp/microcall-$name.txt"
}

run_one goc "$GOC"
echo
run_one native "$NATIVE"
python3 - /tmp/microcall-goc.txt /tmp/microcall-native.txt << 'PY'
import json, sys
def load(path):
    for line in open(path):
        if line.startswith("MICROCALL "):
            return json.loads(line.split(" ", 1)[1])
    raise SystemExit("no MICROCALL line in " + path)
goc, nat = load(sys.argv[1]), load(sys.argv[2])
gm = {c["name"]: c for c in goc["cases"]}
print()
print(f"{'case':<10} {'goc ms':>8} {'native ms':>10} {'goc ns':>8} {'native ns':>10} {'ratio':>7}")
for c in nat["cases"]:
    g = gm[c["name"]]
    ratio = g["ms"] / c["ms"] if c["ms"] else 0
    print(f"{c['name']:<10} {g['ms']:8} {c['ms']:10} {g['ns']:8.0f} {c['ns']:10.0f} {ratio:7.2f}")
print(f"{'score':<10} {goc['score']:8} {nat['score']:10} {'':8} {'':10} {nat['score']/goc['score']:7.2f}")
print("score is calls/ms, geometric mean of the call cases; ratio > 1 means goc is slower")
PY
