#!/usr/bin/env python3
"""Summarize scripts/perfgap-bench.sh raw output (docs/perf-gap/data/raw) into
docs/perf-gap/data/all.json and docs/perf-gap/data/tables.md.

Same rule as scripts/bench-summarize.py: per engine and item, the median across
rounds; geometric means only over items every engine finished.

Everything is also expressed as a *time* so that ratios read the same way in
all suites: V8 and microcall scores are inverted (time ~ 1/score), SunSpider
is ms, microbench ns/op. A ratio > 1 means "slower".

Layers (per flavor ng / bellard), each a time ratio of adjacent builds:
  compiler      clang-O2 / gcc-O2           (both -DNDEBUG)
  opt level     clang-O3 / clang-O2
  goc flags     clang-O3-gocflags / clang-O3 (the codegen flags goc passes)
  goc runtime   goc / clang-O3-gocflags     (everything else goc does)
The product of the four is goc / gcc-O2. "source" compares ng and Bellard
built the same way.

Usage: perfgap-summarize.py [RAW_DIR ...] [--out OUT_DIR]
Several raw dirs are merged (later ones add engines, e.g. toggle runs).
"""
import glob, json, math, os, re, statistics, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = [a for a in sys.argv[1:]]
OUTD = os.path.join(ROOT, "docs/perf-gap/data")
if "--out" in args:
    i = args.index("--out"); OUTD = args[i + 1]; del args[i:i + 2]
RAWS = args or [os.path.join(ROOT, "docs/perf-gap/data/raw")]
V8 = ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes"]

LAYERS = {
    "ng": [("gcc-O2", "ng-gcc-O2"), ("clang-O2", "ng-clang-O2"), ("clang-O3", "ng-clang-O3"),
           ("clang-O3-gocflags", "ng-clang-O3-gocflags"), ("goc", "goc-ng")],
    "bellard": [("gcc-O2", "bellard-gcc-O2-NDEBUG"), ("clang-O2", "bellard-clang-O2-NDEBUG"),
                ("clang-O3", "bellard-clang-O3"), ("clang-O3-gocflags", "bellard-clang-O3-gocflags"),
                ("goc", "goc-bellard")],
}
LAYER_NAMES = ["compiler (clang-O2/gcc-O2)", "opt level (clang-O3/clang-O2)",
               "goc flags (gocflags/clang-O3)", "goc runtime (goc/gocflags)"]

def med(xs):
    return statistics.median(xs) if xs else None

def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))

def rounds(raw, prefix, e):
    fs = glob.glob(os.path.join(raw, "%s-%s-r*.txt" % (prefix, e)))
    return sorted(fs, key=lambda f: int(re.search(r"-r(\d+)\.txt$", f).group(1)))

def engines_of(raw):
    es = []
    p = os.path.join(raw, "engines.txt")
    if os.path.exists(p):
        es = [l.split()[0] for l in open(p) if l.strip()]
    return es

def parse(raw, e):
    d = {}
    # V8
    rs = []
    for f in rounds(raw, "v8", e):
        t = open(f).read()
        r = {m.group(1): int(m.group(2)) for m in re.finditer(r"^RESULT (\w+) (\d+)", t, re.M)}
        m = re.search(r"^SCORE (\d+)", t, re.M)
        r["Score"] = int(m.group(1)) if m else None
        rs.append(r)
    if rs:
        d["v8_rounds"] = rs
        d["v8"] = {k: med([r[k] for r in rs if r.get(k) is not None]) for k in V8 + ["Score"]}
        d["v8_score_spread"] = [min(r["Score"] for r in rs if r["Score"]), max(r["Score"] for r in rs if r["Score"])]
    # SunSpider
    per, fails, fs = {}, set(), rounds(raw, "ss", e)
    for f in fs:
        for line in open(f):
            p = line.split()
            if len(p) >= 2 and p[1] == "FAIL":
                fails.add(p[0])
            elif len(p) == 3:
                per.setdefault(p[0], []).append(float(p[1]))
    if fs:
        d["ss"] = {t: med(v) for t, v in per.items() if t not in fails and len(v) == len(fs)}
        d["ss_fail"] = sorted(fails)
    # microbench
    per, fs = {}, rounds(raw, "micro", e)
    for f in fs:
        for line in open(f):
            m = re.match(r"^\s*([a-z_0-9A-Z]+)\s+(\d+)\s+([\d.]+)\s*$", line)
            if m:
                per.setdefault(m.group(1), []).append(float(m.group(3)))
    if fs:
        d["micro"] = {k: med(v) for k, v in per.items() if len(v) == len(fs)}
    # microcall (per-engine files)
    rs = []
    for f in rounds(raw, "microcall", e):
        for l in open(f):
            if l.startswith("MICROCALL "):
                rs.append(json.loads(l.split(" ", 1)[1]))
    if rs:
        d["microcall_scores"] = [r["score"] for r in rs]
        d["microcall"] = med([r["score"] for r in rs])
        names = [c["name"] for c in rs[0]["cases"]]
        d["microcall_case_ns"] = {n: med([next(c["ns"] for c in r["cases"] if c["name"] == n) for r in rs]) for n in names}
    return d

data, env = {}, {}
for raw in RAWS:
    for e in engines_of(raw):
        data[e] = parse(raw, e)
    for f in glob.glob(os.path.join(raw, "env*.txt")):
        env[os.path.relpath(f, ROOT)] = open(f).read().splitlines()
ENG = list(data)

def times(e):
    """item -> time-like value (lower = faster) for every suite."""
    d, t = data[e], {}
    if "v8" in d:
        for k in V8:
            if d["v8"].get(k):
                t["v8/" + k] = 1e4 / d["v8"][k]
        if d["v8"].get("Score"):
            t["v8/Score"] = 1e4 / d["v8"]["Score"]
    for k, v in d.get("ss", {}).items():
        t["ss/" + k] = v
    for k, v in d.get("micro", {}).items():
        t["micro/" + k] = v
    if d.get("microcall"):
        t["microcall/score"] = 1e6 / d["microcall"]
    for k, v in d.get("microcall_case_ns", {}).items():
        t["microcall/" + k] = v
    return t

T = {e: times(e) for e in ENG}

def common(engs, prefix):
    ks = None
    for e in engs:
        s = {k for k in T[e] if k.startswith(prefix)}
        ks = s if ks is None else ks & s
    return sorted(ks or [])

SUITE_PREFIX = {"V8": "v8/", "SunSpider": "ss/", "microbench": "micro/"}

def suite_time(e, suite, items):
    if suite == "V8":
        return T[e].get("v8/Score")
    if suite == "microcall":
        return T[e].get("microcall/score")
    if not items:
        return None
    return gmean([T[e][k] for k in items])

def v8items(ks):
    return [k for k in ks if k != "v8/Score"]

out = {"env": env, "engines": ENG, "raw": data, "suites": {}, "layers": {}, "per_item": {}}
# suite summary for all engines over items common to all of them
for suite, pre in list(SUITE_PREFIX.items()) + [("microcall", "microcall/score")]:
    engs = [e for e in ENG if any(k.startswith(pre) for k in T[e])]
    ks = common(engs, pre)
    if suite == "V8":
        ks = v8items(ks)
    if not engs:
        continue
    out["suites"][suite] = {"items": len(ks), "time": {e: suite_time(e, suite, ks) for e in engs}}

# layered decomposition
for fl, chain in LAYERS.items():
    engs = [e for _, e in chain]
    if not all(e in T for e in engs):
        continue
    L = {"chain": chain, "suite": {}, "item": {}}
    for suite, pre in list(SUITE_PREFIX.items()) + [("microcall", "microcall/score")]:
        ks = common(engs, pre)
        if suite == "V8":
            ks = v8items(ks)
        st = [suite_time(e, suite, ks) for e in engs]
        if None in st:
            continue
        L["suite"][suite] = {
            "time": dict(zip([n for n, _ in chain], st)),
            "layers": dict(zip(LAYER_NAMES, [st[i + 1] / st[i] for i in range(4)])),
            "goc/gcc": st[4] / st[0], "goc/clang-O2": st[4] / st[1], "goc/clang-O3": st[4] / st[2],
        }
    for pre in ["v8/", "ss/", "micro/", "microcall/"]:
        for k in common(engs, pre):
            v = [T[e][k] for e in engs]
            if min(v) <= 0:  # microcall control cases (empty loop) can read 0
                continue
            L["item"][k] = {"time": v, "layers": [v[i + 1] / v[i] for i in range(4)]}
    out["layers"][fl] = L

# source: ng vs Bellard, same build config
SRC_PAIRS = [("gcc-O2", "ng-gcc-O2", "bellard-gcc-O2-NDEBUG"), ("clang-O2", "ng-clang-O2", "bellard-clang-O2-NDEBUG"),
             ("clang-O3", "ng-clang-O3", "bellard-clang-O3"),
             ("clang-O3-gocflags", "ng-clang-O3-gocflags", "bellard-clang-O3-gocflags"), ("goc", "goc-ng", "goc-bellard")]
out["source"] = {}
for name, a, b in SRC_PAIRS:
    if a in T and b in T:
        r = {}
        for suite, pre in list(SUITE_PREFIX.items()) + [("microcall", "microcall/score")]:
            ks = common([a, b], pre)
            if suite == "V8":
                ks = v8items(ks)
            ta, tb = suite_time(a, suite, ks), suite_time(b, suite, ks)
            if ta and tb:
                r[suite] = ta / tb
        out["source"][name] = r  # ng time / Bellard time

os.makedirs(OUTD, exist_ok=True)
json.dump(out, open(os.path.join(OUTD, "all.json"), "w"), indent=1, sort_keys=False)

# ---- markdown tables
md = []
def row(cells):
    md.append("| " + " | ".join(cells) + " |")
md.append("# perf-gap tables (generated by scripts/perfgap-summarize.py)\n")
md.append("## Suite summary (time-like; V8/microcall = 1e4/score, 1e6/score)\n")
row(["engine", "V8 score", "SunSpider gm ms", "micro gm ns", "microcall"])
row(["---"] * 5)
for e in ENG:
    d = data[e]
    ss = out["suites"].get("SunSpider", {}).get("time", {}).get(e)
    mi = out["suites"].get("microbench", {}).get("time", {}).get(e)
    row([e, "%s" % (d.get("v8", {}).get("Score") or "-"), "%.2f" % ss if ss else "-", "%.1f" % mi if mi else "-",
         "%s" % (int(d["microcall"]) if d.get("microcall") else "-")])
for fl, L in out["layers"].items():
    md.append("\n## Layers: %s (time ratios, >1 = slower)\n" % fl)
    row(["suite"] + LAYER_NAMES + ["goc/gcc", "goc/clang-O2", "goc/clang-O3"])
    row(["---"] * 8)
    for s, v in L["suite"].items():
        row([s] + ["%.3f" % v["layers"][n] for n in LAYER_NAMES] + ["%.3f" % v["goc/gcc"], "%.3f" % v["goc/clang-O2"], "%.3f" % v["goc/clang-O3"]])
    for pre, title in [("v8/", "V8 sub-scores"), ("ss/", "SunSpider"), ("micro/", "microbench"), ("microcall/", "microcall cases")]:
        items = [k for k in L["item"] if k.startswith(pre)]
        if not items:
            continue
        md.append("\n### %s %s\n" % (fl, title))
        row(["item", "gcc-O2", "compiler", "opt level", "goc flags", "goc runtime", "goc/gcc", "goc/clang-O3"])
        row(["---"] * 8)
        for k in items:
            v = L["item"][k]
            row([k[len(pre):], "%.4g" % v["time"][0]] + ["%.3f" % x for x in v["layers"]] +
                ["%.3f" % (v["time"][4] / v["time"][0]), "%.3f" % (v["time"][4] / v["time"][2])])
md.append("\n## Source (ng time / Bellard time, same build)\n")
row(["build", "V8", "SunSpider", "microbench", "microcall"])
row(["---"] * 5)
for n, r in out["source"].items():
    row([n] + ["%.3f" % r[s] if s in r else "-" for s in ["V8", "SunSpider", "microbench", "microcall"]])
open(os.path.join(OUTD, "tables.md"), "w").write("\n".join(md) + "\n")
print("wrote", os.path.join(OUTD, "all.json"), os.path.join(OUTD, "tables.md"))
