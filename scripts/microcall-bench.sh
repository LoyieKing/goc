#!/usr/bin/env bash
# Microcall score: geometric mean of calls/ms on the call-shaped cases.
# Higher is faster. Controls (arith, propget) are printed but not scored.
# goc (goc-ng) and native ng always run; goc-bellard, Bellard QuickJS and Goja
# run too when GOC_BELLARD_QJS / BELLARD_QJS / GOJA_CLI point at their binaries
# (scripts/bench-all.sh sets all three).
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
JS="$ROOT/tests/bench/microcall.js"
GOC="${GOC_QJS:-$ROOT/build/qjs/qjscli}"
NATIVE="${NATIVE_QJS:-/tmp/goc-bench-v8/quickjs-native-build/qjs}"
BELLARD="${BELLARD_QJS:-}"
GOCB="${GOC_BELLARD_QJS:-}"
GOJA="${GOJA_CLI:-}"

run_one() {  # run_one NAME BIN [ARGS...]
  local name="$1" bin="$2"; shift 2
  if [[ ! -x "$bin" ]]; then
    echo "FAIL: missing $name binary $bin" >&2
    exit 1
  fi
  echo "=== $name ==="
  "$bin" "$@" "$JS" | tee "/tmp/microcall-$name.txt"
  echo
}

names=(goc native)
run_one goc "$GOC" --stack-size 16384
run_one native "$NATIVE" --stack-size 16384
if [[ -n "$GOCB" ]]; then run_one goc-bellard "$GOCB" --stack-size 16384; names+=(goc-bellard); fi
if [[ -n "$BELLARD" ]]; then run_one bellard "$BELLARD" --stack-size 16M; names+=(bellard); fi
if [[ -n "$GOJA" ]]; then run_one goja "$GOJA"; names+=(goja); fi
python3 - "${names[@]}" << 'PY'
import json, sys
def load(name):
    for line in open("/tmp/microcall-%s.txt" % name):
        if line.startswith("MICROCALL "):
            return json.loads(line.split(" ", 1)[1])
    raise SystemExit("no MICROCALL line for " + name)
names = sys.argv[1:]
res = {n: load(n) for n in names}
cm = {n: {c["name"]: c for c in res[n]["cases"]} for n in names}
print("%-10s" % "case" + "".join("%11s" % (n + " ms") for n in names) + "%10s" % "goc/nat")
for c in res["native"]["cases"]:
    k = c["name"]
    g = cm["goc"][k]["ms"]
    print("%-10s" % k + "".join("%11s" % cm[n][k]["ms"] for n in names) + "%10.2f" % (g / c["ms"] if c["ms"] else 0))
print("%-10s" % "score" + "".join("%11s" % res[n]["score"] for n in names) +
      "%10.2f" % (res["native"]["score"] / res["goc"]["score"]))
print("score is calls/ms, geometric mean of the call cases; goc/nat > 1 means goc is slower")
PY
