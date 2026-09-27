#!/usr/bin/env python3
"""Per-function time comparison from scripts/perfgap-perf.sh output.

self ms of a function = its self share of samples x the run's wall ms (the
FIXED line). Names are normalized so goc and native symbols line up.
Usage: perfgap-perf-compare.py PROF_DIR OUT_JSON PAIR... where PAIR is
goc_engine:native_engine (e.g. goc-bellard:bellard-clang-O3-gocflags).
"""
import glob, json, os, re, sys
PD, OUT = sys.argv[1], sys.argv[2]
PAIRS = [p.split(":") for p in sys.argv[3:]]

def norm(fn):
    fn = re.sub(r"^main\.", "", fn)
    fn = re.sub(r"^[a-z_]+_c\.", "", fn)
    fn = re.sub(r"\.(isra|constprop|part|cold)\.\d+", "", fn)
    fn = re.sub(r"\.impl$", "", fn)
    return fn

def load(e, s):
    t = os.path.join(PD, "%s.%s.txt" % (e, s)); lg = os.path.join(PD, "%s.%s.log" % (e, s))
    if not os.path.exists(t):
        return None
    m = re.search(r"ms=(\d+)", open(lg).read())
    ms = float(m.group(1))
    fns = {}
    for l in open(t):
        m = re.match(r"^\s*([\d.]+)%\s+\[.\]\s+(.*\S)", l)
        if m:
            k = norm(m.group(2))
            fns[k] = fns.get(k, 0) + float(m.group(1)) * ms / 100
    return ms, fns

res = {}
suites = sorted({os.path.basename(f).split(".")[1] for f in glob.glob(os.path.join(PD, "*.txt"))})
for g, n in PAIRS:
    for s in suites:
        a, b = load(g, s), load(n, s)
        if not a or not b:
            continue
        keys = set(a[1]) | set(b[1])
        rows = sorted(([k, a[1].get(k, 0), b[1].get(k, 0)] for k in keys), key=lambda r: -(r[1] - r[2]))
        res.setdefault("%s:%s" % (g, n), {})[s] = {"ms": [a[0], b[0]], "functions": rows[:25] + rows[-5:]}
json.dump(res, open(OUT, "w"), indent=0)
for p, d in res.items():
    print("==", p)
    for s, v in d.items():
        top = ", ".join("%s %+.0f" % (r[0], r[1] - r[2]) for r in v["functions"][:6])
        print("  %-12s goc %5.0f ms native %5.0f ms  (%+.0f)  top: %s" % (s, v["ms"][0], v["ms"][1], v["ms"][0] - v["ms"][1], top))
