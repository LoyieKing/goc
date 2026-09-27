#!/usr/bin/env python3
"""Summarize scripts/perfgap-callgrind.sh output into docs/perf-gap/data/callgrind.json.

For every <engine>.<suite>.cg: the callgrind totals (Ir, Dr, Dw, I1mr, D1mr,
D1mw, ILmr, DLmr, DLmw, Bc, Bcm, Bi, Bim; simulated caches and branch
predictor) and the top self-cost functions (names normalized so the goc
symbol main.quickjs_c.JS_CallInternal and native JS_CallInternal compare).

Usage: perfgap-cg-summarize.py [CG_DIR] [OUT_JSON]
"""
import glob, json, os, re, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CG = sys.argv[1] if len(sys.argv) > 1 else "/workspace/perf-study/cg-gap"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs/perf-gap/data/callgrind.json")
TOP = 40

def norm(fn):
    fn = re.sub(r"^main\.", "", fn)
    fn = re.sub(r"^[a-z_]+_c\.", "", fn)       # goc file-local prefix (quickjs_c.)
    fn = re.sub(r"\.(impl|isra|constprop|part|cold)(\.\d+)*$", "", fn)
    fn = re.sub(r"\.(isra|constprop|part|cold)\.\d+", "", fn)
    fn = re.sub(r"\.impl$", "", fn)
    fn = re.sub(r"'\d+$", "", fn)                # callgrind recursion suffix
    return fn

res = {}
for f in sorted(glob.glob(os.path.join(CG, "*.cg"))):
    e, s = os.path.basename(f)[:-3].rsplit(".", 1)
    ev = tot = None
    for l in open(f):
        if l.startswith("events:"):
            ev = l.split()[1:]
        elif l.startswith("totals:"):
            tot = [int(x) for x in l.split()[1:]]
    if not ev or not tot:
        continue
    d = dict(zip(ev, tot))
    agg = {}
    try:
        a = subprocess.run(["callgrind_annotate", "--threshold=99.9", "--inclusive=no", "--auto=no", f],
                           capture_output=True, text=True, timeout=600).stdout
        body = a.split("file:function", 1)[-1]
        for l in body.splitlines():
            m = re.match(r"^\s*([\d,]+) \(", l)
            if not m or "PROGRAM TOTALS" in l:
                continue
            tok = l.split(" [")[0].split()[-1]
            name = tok.split(":", 1)[1] if ":" in tok else tok
            k = norm(name)
            agg[k] = agg.get(k, 0) + int(m.group(1).replace(",", ""))
    except Exception as ex:  # keep totals even if annotate fails
        agg = {"annotate failed: %s" % ex: 0}
    top = sorted(agg.items(), key=lambda kv: -kv[1])[:TOP]
    res.setdefault(e, {})[s] = {"totals": d, "top": top}
os.makedirs(os.path.dirname(OUT), exist_ok=True)
json.dump(res, open(OUT, "w"), indent=0)
print("wrote", OUT, len(res), "engines")
