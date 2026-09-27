#!/usr/bin/env python3
"""Summarize scripts/bench-all.sh raw output into docs/benchmark/data/all.json.

Rule: per engine, per item, take the median across rounds (V8 sub-scores and
total, SunSpider ms/iter, microbench ns/op, microcall calls/ms). An item
that failed in any round has no number. Geometric means only use items every
engine finished, so all columns cover the same items (the SunSpider
"nogoja" set also covers the four QuickJS builds without Goja).

Usage: bench-summarize.py [RAW_DIR] [OUT_JSON] [--test262 DIR]
"""
import glob, json, math, os, re, statistics, sys, importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.path.join(ROOT, "docs/benchmark/data/raw")
OUT = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else os.path.join(ROOT, "docs/benchmark/data/all.json")
T262 = sys.argv[sys.argv.index("--test262") + 1] if "--test262" in sys.argv else "/workspace/perf-study/t262/test262-head"
ENG = ["goc-ng", "goc-bellard", "ng", "bellard", "goja"]
QJS = ENG[:4]  # the four QuickJS builds
# goc build -> the native build of the same engine (correctness parity, ratios)
PAIR = {"goc-ng": "ng", "goc-bellard": "bellard"}
V8 = ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes"]

# microbench groups: every test belongs to exactly one group (first match wins).
MICRO_GROUPS = [
    ("loop", lambda k: k.startswith("empty_")),
    ("prop", lambda k: k.startswith("prop_")),
    ("var", lambda k: k.startswith(("global_read", "global_write", "global_destruct", "local_destruct", "arguments_"))),
    ("call", lambda k: "func" in k and k.endswith("call")),
    ("array", lambda k: k.startswith(("array_", "typed_array_")) or k == "sort_bench"),
    ("string", lambda k: k.startswith("string_")),
    ("numconv", lambda k: k.startswith(("int_to", "float_to"))),
    ("arith", lambda k: k in ("int_arith", "float_arith", "math_min")),
    ("bigint", lambda k: k.startswith("bigint")),
    ("map", lambda k: "map_" in k),
    ("regexp", lambda k: k.startswith("regexp_")),
    ("date", lambda k: k.startswith("date_")),
]

def med(xs):
    return statistics.median(xs) if xs else None

def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))

def rounds(prefix, e):
    fs = glob.glob(os.path.join(RAW, "%s-%s-r*.txt" % (prefix, e)))
    return sorted(fs, key=lambda f: int(re.search(r"-r(\d+)\.txt$", f).group(1)))

out = {"env": {os.path.basename(f)[:-4]: open(f).read().splitlines() for f in sorted(glob.glob(os.path.join(RAW, "env*.txt")))}}

# ---- V8
v8 = {"rounds": {}, "median": {}, "score_min": {}, "score_max": {}, "wall_s_median": {}}
for e in ENG:
    rs = []
    for f in rounds("v8", e):
        t = open(f).read()
        d = {m.group(1): int(m.group(2)) for m in re.finditer(r"^RESULT (\w+) (\d+)", t, re.M)}
        m = re.search(r"^SCORE (\d+)", t, re.M)
        d["Score"] = int(m.group(1)) if m else None
        d["wall_s"] = float(re.search(r"^WALL ([\d.]+)", t, re.M).group(1))
        rs.append(d)
    v8["rounds"][e] = rs
    v8["median"][e] = {k: med([r[k] for r in rs if r.get(k) is not None]) for k in V8 + ["Score"]}
    sc = [r["Score"] for r in rs if r["Score"] is not None]
    v8["score_min"][e], v8["score_max"][e] = min(sc), max(sc)
    v8["wall_s_median"][e] = med([r["wall_s"] for r in rs])
out["v8"] = v8

# ---- SunSpider
ss = {"rounds": {}, "ms": {}, "fail": {}}
for e in ENG:
    per = {}
    fails = {}
    fs = rounds("ss", e)
    for f in fs:
        for line in open(f):
            p = line.rstrip("\n").split(" ", 2)
            if len(p) >= 2 and p[1] == "FAIL":
                fails[p[0]] = p[2] if len(p) > 2 else ""
                per.setdefault(p[0], [])
            elif len(p) == 3:
                per.setdefault(p[0], []).append(float(p[1]))
    ss["rounds"][e] = len(fs)
    ss["ms"][e] = {t: (med(v) if t not in fails and len(v) == len(fs) else None) for t, v in per.items()}
    ss["fail"][e] = fails
tests = sorted(ss["ms"]["goc-ng"])
common = [t for t in tests if all(ss["ms"][e].get(t) for e in ENG)]
common_q = [t for t in tests if all(ss["ms"][e].get(t) for e in QJS)]
ss["tests"] = tests
ss["common_all"], ss["common_nogoja"] = common, common_q
ss["geomean_all"] = {e: gmean([ss["ms"][e][t] for t in common]) for e in ENG}
ss["geomean_nogoja"] = {e: gmean([ss["ms"][e][t] for t in common_q]) for e in QJS}
# per-round geomean (same item set) to show the spread
ss["geomean_all_by_round"] = {}
for e in ENG:
    g = []
    for f in rounds("ss", e):
        d = {}
        for line in open(f):
            p = line.split()
            if len(p) == 3 and p[1] != "FAIL":
                d[p[0]] = float(p[1])
        if all(t in d for t in common):
            g.append(gmean([d[t] for t in common]))
    ss["geomean_all_by_round"][e] = g
out["sunspider"] = ss

# ---- microbench
mb = {"rounds": {}, "ns": {}, "total_median": {}, "missing": {}}
for e in ENG:
    per, tot = {}, []
    fs = rounds("micro", e)
    for f in fs:
        for line in open(f):
            m = re.match(r"^\s*([a-z_0-9A-Z]+)\s+(\d+)\s+([\d.]+)\s*$", line)
            if m:
                per.setdefault(m.group(1), []).append(float(m.group(3)))
            m = re.match(r"^\s*total\s+([\d.]+)", line)
            if m:
                tot.append(float(m.group(1)))
    mb["rounds"][e] = len(fs)
    mb["ns"][e] = {k: med(v) for k, v in per.items() if len(v) == len(fs)}
    mb["total_median"][e] = med(tot)
alltests = sorted(set().union(*[mb["ns"][e] for e in ENG]))
for e in ENG:
    mb["missing"][e] = [k for k in alltests if k not in mb["ns"][e]]
mcommon = [k for k in alltests if all(k in mb["ns"][e] for e in ENG)]
mb["tests"], mb["common"] = alltests, mcommon
mb["geomean"] = {e: gmean([mb["ns"][e][k] for k in mcommon]) for e in ENG}
groups = {}
for k in alltests:
    g = next(n for n, f in MICRO_GROUPS if f(k))
    groups.setdefault(g, []).append(k)
mb["groups"] = {n: groups[n] for n, _ in MICRO_GROUPS if n in groups}
mb["group_geomean"] = {n: {e: gmean([mb["ns"][e][k] for k in ks if k in mcommon]) for e in ENG}
                       for n, ks in mb["groups"].items() if any(k in mcommon for k in ks)}
out["micro"] = mb

# ---- microcall (goc-ng and native ng always; the others when the round ran them)
MC_NAME = {"goc": "goc-ng", "goc-bellard": "goc-bellard", "native": "ng", "bellard": "bellard", "goja": "goja"}
mc = {"rounds": []}
for f in sorted(glob.glob(os.path.join(RAW, "microcall-r*.txt")), key=lambda f: int(re.search(r"r(\d+)", os.path.basename(f)).group(1))):
    cur, rd = None, {}
    for l in open(f):
        m = re.match(r"^=== ([\w-]+) ===", l)
        if m:
            cur = MC_NAME[m.group(1)]
        elif l.startswith("MICROCALL ") and cur:
            rd[cur] = json.loads(l.split(" ", 1)[1])
    mc["rounds"].append(rd)
if mc["rounds"]:
    mce = [e for e in ENG if all(e in r for r in mc["rounds"])]
    mc["engines"] = mce
    mc["score_median"] = {e: med([r[e]["score"] for r in mc["rounds"]]) for e in mce}
    mc["score_min"] = {e: min(r[e]["score"] for r in mc["rounds"]) for e in mce}
    mc["score_max"] = {e: max(r[e]["score"] for r in mc["rounds"]) for e in mce}
    names = [c["name"] for c in mc["rounds"][0]["ng"]["cases"]]
    mc["case_ms_median"] = {n: {e: med([next(c["ms"] for c in r[e]["cases"] if c["name"] == n) for r in mc["rounds"]])
                                for e in mce} for n in names}
out["microcall"] = mc

# ---- test262 sample
spec = importlib.util.spec_from_file_location("t262", os.path.join(ROOT, "scripts/test262-sample.py"))
t262m = importlib.util.module_from_spec(spec); spec.loader.exec_module(t262m)
tr = json.load(open(os.path.join(RAW, "test262-results.json")))
dirs = {}
for rel in tr["tests"]:
    d = rel.split("/")[2]
    dirs.setdefault(d, []).append(rel)
meta = {}
base = os.path.join(T262, "test/language")
if os.path.isdir(base):
    for d in sorted(dirs):
        pos = neg = 0
        for dp, _, fns in os.walk(os.path.join(base, d)):
            for fn in fns:
                if not fn.endswith(".js"):
                    continue
                src = open(os.path.join(dp, fn), encoding="utf-8", errors="replace").read()
                y = t262m.frontmatter(src)
                if t262m.SKIP_FLAGS & set(t262m.listfield(y, "flags")) or t262m.SKIP_FEATURES & set(t262m.listfield(y, "features")) or "$262.agent" in src:
                    continue
                if re.search(r"^negative:", y, re.M):
                    neg += 1
                else:
                    pos += 1
        meta[d] = {"eligible_pos": pos, "eligible_neg": neg}
t262 = {"ran": len(tr["tests"]), "pass": {}, "dirs": {}, "fail": {}}
for e in ENG:
    r = tr["results"][e]
    t262["pass"][e] = sum(v == "PASS" for v in r.values())
    t262["fail"][e] = {k: v for k, v in sorted(r.items()) if v != "PASS"}
for d, rels in sorted(dirs.items()):
    t262["dirs"][d] = dict(meta.get(d, {}), ran=len(rels),
                           **{e: sum(tr["results"][e][x] == "PASS" for x in rels) for e in ENG})
out["test262"] = t262

# ---- QuickJS official tests
qr = json.load(open(os.path.join(RAW, "qjs-tests-results.json")))
q = {"total": len(qr["goc-ng"]), "pass": {e: sum(v == "pass" for v in qr[e].values()) for e in ENG}, "files": {}, "fn": qr}
for k in qr["goc-ng"]:
    f = k.split(":")[0]
    q["files"].setdefault(f, {"n": 0, **{e: 0 for e in ENG}})
    q["files"][f]["n"] += 1
    for e in ENG:
        q["files"][f][e] += qr[e][k] == "pass"
out["qjs_tests"] = q

# ---- correctness parity: each goc build against the native build of its engine
par = {}
for g, n in PAIR.items():
    rt, rq = tr["results"], qr
    par[g] = {"native": n,
              "test262_diff": sorted(k for k in rt[n] if (rt[n][k] == "PASS") != (rt[g][k] == "PASS")),
              "qjs_diff": sorted(k for k in rq[n] if (rq[n][k] == "pass") != (rq[g][k] == "pass"))}
out["parity"] = par

# ---- memory (scripts/bench-mem.sh)
def lsq(xs, ys):
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)

mem = {"rss_kb": {}, "rss_rounds": {}, "rss_fail": {}}
for e in ENG:
    per, fails = {}, {}
    fs = rounds("mem-rss", e)
    for f in fs:
        for line in open(f):
            p = line.split()
            if len(p) != 4:
                continue
            if p[3] != "0":
                fails[p[0]] = "exit %s" % p[3]
            per.setdefault(p[0], []).append(int(p[1]))
    mem["rss_rounds"][e] = len(fs)
    mem["rss_kb"][e] = {w: med(v) for w, v in per.items() if w not in fails and len(v) == len(fs)}
    mem["rss_fail"][e] = fails
    # SunSpider as a whole: largest peak over the files, per round, then median
    ssmax = []
    for f in fs:
        vals = [int(l.split()[1]) for l in open(f) if l.startswith("ss-") and l.split()[3] == "0"]
        if vals:
            ssmax.append(max(vals))
    if ssmax:
        mem["rss_kb"][e]["ss-max"] = med(ssmax)
if any(mem["rss_rounds"].values()):
    mem["workloads"] = [w for w in mem["rss_kb"]["ng"]]
inst = {}
for f in sorted(glob.glob(os.path.join(RAW, "mem-inst-r*.txt"))):
    for l in open(f):
        if l.startswith("MEMINST "):
            j = json.loads(l.split(" ", 1)[1])
            if "error" not in j["data"]:
                inst.setdefault(j["tag"], {}).setdefault(j["n"], []).append(j["data"])
if inst:
    mi = {}
    for tag, byn in inst.items():
        d = {"n": sorted(byn), "rounds": {str(n): len(v) for n, v in byn.items()}}
        d["live_rss_kb"] = {str(n): med([x["live"]["VmRSSKB"] for x in v]) for n, v in sorted(byn.items())}
        after = "after_gc" if "after_gc" in byn[min(byn)][0] else "after_trim"
        d["after_kind"] = after
        d["after_rss_kb"] = {str(n): med([x[after]["VmRSSKB"] for x in v]) for n, v in sorted(byn.items())}
        if "HeapSys" in byn[min(byn)][0]["live"]:
            for k in ("HeapSys", "HeapInuse", "StackSys", "StackInuse", "Sys"):
                d["live_" + k] = {str(n): med([x["live"][k] for x in v]) for n, v in sorted(byn.items())}
                d["after_" + k] = {str(n): med([x[after][k] for x in v]) for n, v in sorted(byn.items())}
        ns = [n for n in sorted(byn) if n >= 1]
        L = d["live_rss_kb"]
        if 0 in byn:
            d["per_instance_kb"] = {str(n): (L[str(n)] - L["0"]) / n for n in ns}
        if 1 in byn and 1000 in byn:
            d["slope_1_1000_kb"] = (L["1000"] - L["1"]) / 999
            d["slope_1_1000_after_kb"] = (d["after_rss_kb"]["1000"] - d["after_rss_kb"]["1"]) / 999
        if len(ns) >= 2:
            d["slope_lsq_kb"] = lsq(ns, [L[str(n)] for n in ns])
        if "live_StackInuse" in d and 0 in byn:
            d["stack_per_goroutine_b"] = {str(n): (d["live_StackInuse"][str(n)] - d["live_StackInuse"]["0"]) / n for n in ns}
            d["stack_per_goroutine_after_gc_b"] = {str(n): (d["after_StackInuse"][str(n)] - d["after_StackInuse"]["0"]) / n for n in ns}
        mi[tag] = d
    mem["instances"] = mi
out["memory"] = mem


# ---- reference run: Bellard built with clang-19 -O2 (separate short session, raw/bellard-clang/)
REF = os.path.join(RAW, "bellard-clang")
if os.path.isdir(REF):
    ref = {"engines": [e for e in ("bellard-clang", "bellard", "goc-bellard")
                       if glob.glob(os.path.join(REF, "v8-%s-r*.txt" % e))],
           "env": open(os.path.join(REF, "env.txt")).read().splitlines()}
    def rrounds(prefix, e):
        fs = glob.glob(os.path.join(REF, "%s-%s-r*.txt" % (prefix, e)))
        return sorted(fs, key=lambda f: int(re.search(r"-r(\d+)\.txt$", f).group(1)))
    ref["v8_median"], ref["v8_rounds"], ref["ss_geomean"], ref["micro_geomean"] = {}, {}, {}, {}
    for e in ref["engines"]:
        rs = []
        for f in rrounds("v8", e):
            t = open(f).read()
            d = {m.group(1): int(m.group(2)) for m in re.finditer(r"^RESULT (\w+) (\d+)", t, re.M)}
            d["Score"] = int(re.search(r"^SCORE (\d+)", t, re.M).group(1))
            rs.append(d)
        ref["v8_rounds"][e] = [r["Score"] for r in rs]
        ref["v8_median"][e] = {k: med([r[k] for r in rs if k in r]) for k in V8 + ["Score"]}
        per = {}
        fs = rrounds("ss", e)
        for f in fs:
            for line in open(f):
                p_ = line.split()
                if len(p_) == 3 and p_[1] != "FAIL":
                    per.setdefault(p_[0], []).append(float(p_[1]))
        ref["ss_geomean"][e] = gmean([med(per[t]) for t in common])
        per = {}
        fs = rrounds("micro", e)
        for f in fs:
            for line in open(f):
                m = re.match(r"^\s*([a-z_0-9A-Z]+)\s+(\d+)\s+([\d.]+)\s*$", line)
                if m:
                    per.setdefault(m.group(1), []).append(float(m.group(3)))
        ref["micro_geomean"][e] = gmean([med(per[k]) for k in mcommon])
    out["bellard_clang_ref"] = ref

json.dump(out, open(OUT, "w"), indent=1, ensure_ascii=False)
print("V8 median score", {e: v8["median"][e]["Score"] for e in ENG})
print("SunSpider geomean (%d items)" % len(common), {e: round(ss["geomean_all"][e], 2) for e in ENG})
print("microbench geomean (%d items)" % len(mcommon), {e: round(mb["geomean"][e], 1) for e in ENG})
print("microcall", mc.get("score_median"))
print("memory idle", {e: mem["rss_kb"][e].get("empty") for e in ENG})
print("test262", t262["pass"], "official", q["pass"])
print("parity", {g: (len(v["test262_diff"]), len(v["qjs_diff"])) for g, v in par.items()})
if "bellard_clang_ref" in out:
    r = out["bellard_clang_ref"]
    print("clang ref", {e: (r["v8_median"][e]["Score"], round(r["ss_geomean"][e], 2), round(r["micro_geomean"][e], 1)) for e in r["engines"]})
