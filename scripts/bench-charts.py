#!/usr/bin/env python3
"""Draw docs/benchmark/charts/*.{svg,png} from docs/benchmark/data/all.json.

Every chart is written twice with the same name: .png (embedded in
docs/benchmark.md) and .svg (embedded in docs/benchmark/report.html).
All chart text is English; the doc prose stays Chinese. Every performance
chart shows all four engines (log axes where Goja would dwarf the others).
Needs matplotlib.

Usage: bench-charts.py [ALL_JSON] [OUT_DIR]
"""
import json, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs/benchmark/data/all.json")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs/benchmark/charts")
os.makedirs(OUT, exist_ok=True)
D = json.load(open(ALL))

plt.rcParams.update({
    "font.family": ["DejaVu Sans"],
    "axes.unicode_minus": False, "svg.fonttype": "none", "font.size": 10,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.alpha": 0.3, "axes.axisbelow": True,
})
ENG = ["goc", "ng", "bellard", "goja"]
LABEL = {"goc": "goc", "ng": "native ng", "bellard": "Bellard", "goja": "Goja"}
COLOR = {"goc": "#2563eb", "ng": "#f59e0b", "bellard": "#10b981", "goja": "#94a3b8"}
DATE = "2026-09-27"
LEG = dict(frameon=True, facecolor="white", edgecolor="none", framealpha=1)

def save(fig, name):
    fig.savefig(os.path.join(OUT, name + ".png"), dpi=150, bbox_inches="tight")
    fig.savefig(os.path.join(OUT, name + ".svg"), bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)

def title(ax, t, sub):
    ax.set_title(t, fontsize=13, fontweight="bold", loc="left", pad=24)
    ax.annotate(sub, xy=(0, 1), xycoords="axes fraction", xytext=(0, 8), textcoords="offset points",
                fontsize=9, color="#475569", va="bottom", ha="left")

def plain_log(axis):
    """Log axis with plain-number tick labels (1, 2, 5, 10, ...)."""
    axis.set_major_locator(LogLocator(base=10, subs=(1, 2, 5)))
    axis.set_major_formatter(FuncFormatter(lambda v, _: "%g" % v))
    axis.set_minor_formatter(NullFormatter())

def grouped(ax, cats, series, engines, fmt, width=0.8, rot=0, fs=7.5, log=False):
    n = len(engines)
    w = width / n
    for i, e in enumerate(engines):
        xs = [j + (i - (n - 1) / 2) * w for j in range(len(cats))]
        vals = [series[e][j] for j in range(len(cats))]
        bars = ax.bar(xs, [v or 0 for v in vals], w, label=LABEL[e], color=COLOR[e])
        for b, v in zip(bars, vals):
            if v:
                ax.annotate(fmt(v), (b.get_x() + b.get_width() / 2, b.get_height()),
                            xytext=(0, 2), textcoords="offset points", ha="center", va="bottom",
                            fontsize=fs, rotation=rot)
    ax.set_xticks(range(len(cats)))
    ax.set_xticklabels(cats)
    if log:
        ax.set_yscale("log")

def hbars(ax, tests, engines, get, fmt, log=False, fs=6.5):
    n = len(engines); h = 0.8 / n; fails = []
    for i, e in enumerate(engines):
        ys = [j + (i - (n - 1) / 2) * h for j in range(len(tests))]
        vals = [get(e, t) for t in tests]
        bars = ax.barh(ys, [v or 0 for v in vals], h, label=LABEL[e], color=COLOR[e])
        for b, v in zip(bars, vals):
            if v:
                ax.annotate(fmt(v), (b.get_width(), b.get_y() + h / 2), xytext=(2, 0), textcoords="offset points",
                            va="center", fontsize=fs)
            else:
                fails.append(b.get_y() + h / 2)
    ax.set_yticks(range(len(tests))); ax.set_yticklabels(tests)
    ax.set_ylim(len(tests) - 0.5, -0.5)  # first test on top, no empty band
    if log:
        ax.set_xscale("log")
    ax.grid(axis="y", visible=False)
    return fails

def mark_fails(ax, fails, fs=6.5):
    x0 = ax.get_xlim()[0]
    for y in fails:
        ax.annotate("failed", (x0, y), xytext=(2, 0), textcoords="offset points", va="center", fontsize=fs, color="#dc2626")

# 1. overview: speed relative to native ng
v8m = D["v8"]["median"]; ss = D["sunspider"]; mb = D["micro"]; mc = D["microcall"]
cats = ["V8-v7 score", "SunSpider geomean\n(%d tests)" % len(ss["common4"]), "microbench geomean\n(%d tests)" % len(mb["common"])]
rel = {e: [v8m[e]["Score"] / v8m["ng"]["Score"], ss["geomean4"]["ng"] / ss["geomean4"][e],
           mb["geomean"]["ng"] / mb["geomean"][e]] for e in ENG}
if all(e in mc.get("score_median", {}) for e in ENG):
    cats.append("microcall score\n(calls/ms)")
    for e in ENG:
        rel[e].append(mc["score_median"][e] / mc["score_median"]["ng"])
fig, ax = plt.subplots(figsize=(10, 4.8))
grouped(ax, cats, rel, ENG, lambda v: "%.2f" % v, fs=9)
ax.axhline(1, color="#334155", lw=1, ls="--")
ax.set_ylabel("speed relative to native ng (native ng = 1, higher is better)")
ax.set_ylim(0, max(max(v) for v in rel.values()) * 1.18)
title(ax, "Speed relative to native ng", "V8 and microcall: ratio of scores. SunSpider and microbench: inverse ratio of times. Measured %s; engines interleaved." % DATE)
ax.legend(ncol=4, loc="upper right", **LEG)
save(fig, "overview-speed")

# 2. overview: correctness
t = D["test262"]; q = D["qjs_tests"]
fig, ax = plt.subplots(figsize=(10, 4.8))
pct = {e: [100 * t["pass"][e] / t["ran"], 100 * q["pass"][e] / q["total"]] for e in ENG}
cnt = {e: ["%d/%d" % (t["pass"][e], t["ran"]), "%d/%d" % (q["pass"][e], q["total"])] for e in ENG}
n = len(ENG); w = 0.8 / n
for i, e in enumerate(ENG):
    xs = [j + (i - (n - 1) / 2) * w for j in range(2)]
    bars = ax.bar(xs, pct[e], w, label=LABEL[e], color=COLOR[e])
    for b, v, c in zip(bars, pct[e], cnt[e]):
        ax.annotate("%.1f%%\n%s" % (v, c), (b.get_x() + w / 2, v), xytext=(0, 2), textcoords="offset points",
                    ha="center", va="bottom", fontsize=8)
ax.set_xticks([0, 1]); ax.set_xticklabels(["test262 language (sample)", "QuickJS official tests (per function)"])
ax.set_ylim(0, 115); ax.set_yticks(range(0, 101, 20))
ax.set_ylabel("pass rate, % (higher is better)")
title(ax, "Correctness", "test262: stratified sample of %d tests from test/language. QuickJS: %d functions from Bellard's tests/test_*.js." % (t["ran"], q["total"]))
ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.1), **LEG)
save(fig, "overview-correct")

# 3. V8 per sub-benchmark
subs = ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes", "Score"]
fig, ax = plt.subplots(figsize=(12, 5.2))
grouped(ax, subs[:-1] + ["Total"], {e: [v8m[e][s] for s in subs] for e in ENG}, ENG,
        lambda v: "%g" % v, rot=90, fs=7.5)
ax.set_ylabel("score (higher is better)")
ax.set_ylim(0, max(v8m[e][s] for e in ENG for s in subs) * 1.18)
title(ax, "V8-v7 sub-benchmark scores", "bench-v8.js, %d rounds per engine, median taken per item." % len(D["v8"]["rounds"]["goc"]))
ax.legend(ncol=4, loc="upper left", **LEG)
save(fig, "v8")

# 4. SunSpider: time relative to native ng, per test, all engines (log axis)
tests = ss["tests"]
get = lambda e, t: ss["ms"][e].get(t)
ratio = lambda e, t: (get(e, t) / get("ng", t)) if get(e, t) and get("ng", t) else None
fig, ax = plt.subplots(figsize=(10, 15))
cmp = ["goc", "bellard", "goja"]
fl = hbars(ax, tests, cmp, ratio, lambda v: "%.2f" % v, log=True, fs=6)
ax.axvline(1, color="#334155", ls="--", lw=1, zorder=0)
lo = min(ratio(e, t) for e in cmp for t in tests if ratio(e, t)); hi = max(ratio(e, t) or 0 for e in cmp for t in tests)
ax.set_xlim(lo / 1.6, hi * 2.2)
plain_log(ax.xaxis)
ax.set_xlabel("time / native ng time (log axis; 1 = as fast as native ng, lower is better)")
mark_fails(ax, fl, 6)
title(ax, "SunSpider 1.0.2: time relative to native ng", "ms per iteration divided by native ng's, median of %d rounds. Dashed line = native ng." % ss["rounds"]["goc"])
ax.legend(ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.035), **LEG)
save(fig, "sunspider")

fig, ax = plt.subplots(figsize=(10, 15))
fl = hbars(ax, tests, ENG, get, lambda v: "%.1f" % v, log=True, fs=6)
ax.set_xlabel("ms per iteration (log axis, lower is better)")
lo = min(get(e, t) for e in ENG for t in tests if get(e, t)); hi = max(get(e, t) or 0 for e in ENG for t in tests)
ax.set_xlim(lo / 1.5, hi * 2.5)
plain_log(ax.xaxis)
mark_fails(ax, fl, 6)
title(ax, "SunSpider 1.0.2: all four engines, absolute time", "Median of %d rounds; each round is the fastest of 5 batches. Log axis because Goja is several times slower." % ss["rounds"]["goc"])
ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.035), **LEG)
save(fig, "sunspider-log")

# 5. microbench groups
G = mb["group_geomean"]; gn = list(G)
fig, ax = plt.subplots(figsize=(12, 5.2))
grouped(ax, gn, {e: [G[g][e] for g in gn] for e in ENG}, ENG, lambda v: "%.0f" % v if v >= 100 else "%.1f" % v,
        rot=90, fs=7, log=True)
plain_log(ax.yaxis)
ax.set_ylabel("ns/op, geometric mean within group\n(log axis, lower is better)")
ax.set_ylim(min(G[g][e] for g in gn for e in ENG) * 0.6, max(G[g][e] for g in gn for e in ENG) * 6)
title(ax, "microbench by group", "Bellard tests/microbench.js, median of %d rounds per test, geometric mean per group (groups: scripts/bench-summarize.py)." % mb["rounds"]["goc"])
ax.legend(ncol=4, loc="upper left", **LEG)
save(fig, "micro-groups")

# 6. microbench: time relative to native ng per test, goc / Bellard / Goja
ns = mb["ns"]
keys = [k for k in mb["tests"] if all(k in ns[e] for e in ENG)]
keys.sort(key=lambda k: ns["goc"][k] / ns["ng"][k], reverse=True)
mr = lambda e, k: ns[e][k] / ns["ng"][k]
fig, ax = plt.subplots(figsize=(10, 26))
hbars(ax, keys, cmp, mr, lambda v: "%.2f" % v, log=True, fs=5.5)
ax.axvline(1, color="#334155", ls="--", lw=1, zorder=0)
lo = min(mr(e, k) for e in cmp for k in keys); hi = max(mr(e, k) for e in cmp for k in keys)
ax.set_xlim(lo / 1.5, hi * 2.2)
plain_log(ax.xaxis)
ax.tick_params(axis="y", labelsize=7.5)
ax.set_xlabel("time / native ng time (log axis; 1 = as fast as native ng, lower is better)")
title(ax, "microbench: time relative to native ng", "%d tests every engine finished, sorted by goc / native ng (slowest first). Dashed line = native ng." % len(keys))
ax.legend(ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.02), **LEG)
save(fig, "micro-ratio")

# 7. microcall per case (only when all four engines ran)
if all(e in mc.get("score_median", {}) for e in ENG):
    cm = mc["case_ms_median"]; cases = list(cm)
    fig, ax = plt.subplots(figsize=(12, 5.2))
    grouped(ax, cases, {e: [cm[c][e] for c in cases] for e in ENG}, ENG, lambda v: "%g" % v, rot=90, fs=7, log=True)
    plain_log(ax.yaxis)
    ax.set_ylim(min(cm[c][e] for c in cases for e in ENG) * 0.6, max(cm[c][e] for c in cases for e in ENG) * 4)
    ax.set_ylabel("ms per case (log axis, lower is better)")
    sm = mc["score_median"]
    title(ax, "microcall: time per case", "scripts/microcall-bench.sh, median of %d rounds. arith and propget are controls. Score (calls/ms, higher is better): %s." % (
        len(mc["rounds"]), ", ".join("%s %g" % (LABEL[e], sm[e]) for e in ENG)))
    ax.legend(ncol=4, loc="upper left", **LEG)
    save(fig, "microcall")

# 8. test262 per directory (only directories with a failure)
dirs = [d for d, v in t["dirs"].items() if any(v[e] < v["ran"] for e in ENG)]
fig, ax = plt.subplots(figsize=(10, 7))
mark_fails(ax, hbars(ax, dirs, ENG, lambda e, d: 100 * t["dirs"][d][e] / t["dirs"][d]["ran"], lambda v: "%.1f%%" % v, fs=6.5))
ax.set_xlim(0, 112); ax.set_xlabel("pass rate, % (higher is better)")
title(ax, "test262 language sample: directories with a failure", "Directories where every engine passes everything are only in the doc's table.")
ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.07), **LEG)
save(fig, "test262")

# 9. QuickJS official tests per file
files = list(q["files"])
fig, ax = plt.subplots(figsize=(10, 4.8))
grouped(ax, ["%s (%d)" % (f.replace(".js", ""), q["files"][f]["n"]) for f in files],
        {e: [q["files"][f][e] for f in files] for e in ENG}, ENG, lambda v: "%d" % v, fs=8)
ax.set_ylabel("functions passed (higher is better)")
ax.set_ylim(0, max(q["files"][f]["n"] for f in files) * 1.3)
title(ax, "QuickJS official tests: functions passed per file", "Bellard 2026-06-04 tests/test_*.js, one process per function. Total functions in parentheses.")
ax.legend(ncol=4, loc="upper right", **LEG)
save(fig, "qjs-tests")

# 10. memory: idle and peak RSS per workload
M = D.get("memory", {})
if M.get("rss_kb") and M["rss_kb"].get("ng"):
    R = M["rss_kb"]
    W = [("empty", "empty\nscript (idle)")] + [("v8-" + s, s) for s in subs[:-1]] + [
        ("v8-all", "V8-v7\nwhole"), ("ss-max", "SunSpider\n(max file)"), ("micro", "micro-\nbench"),
        ("alloc", "alloc.js"), ("mapset", "mapset.js")]
    W = [(k, l) for k, l in W if any(k in R[e] for e in ENG)]
    fig, ax = plt.subplots(figsize=(14, 5.6))
    grouped(ax, [l for _, l in W], {e: [R[e].get(k, 0) / 1024 for k, _ in W] for e in ENG}, ENG,
            lambda v: "%.0f" % v if v >= 10 else "%.1f" % v, rot=90, fs=6.5, log=True)
    plain_log(ax.yaxis)
    ax.set_ylim(1, max(R[e].get(k, 0) for e in ENG for k, _ in W) / 1024 * 5)
    ax.set_ylabel("peak RSS, MiB (log axis, lower is better)")
    ax.tick_params(axis="x", labelsize=8)
    title(ax, "Peak memory (max RSS) per workload", "One fresh process per workload, /usr/bin/time -v, median of %d rounds. V8 suites run one at a time. SunSpider = largest file." % M["rss_rounds"]["goc"])
    ax.legend(ncol=4, loc="upper left", **LEG)
    save(fig, "mem-peak")

# 11. memory: multi-instance scaling
MI = M.get("instances")
if MI:
    tags = [x for x in ["goc", "ng", "ng-arena1", "bellard", "goja"] if x in MI]
    TL = {"goc": "goc (goroutines)", "ng": "native ng (pthreads)", "ng-arena1": "native ng, MALLOC_ARENA_MAX=1",
          "bellard": "Bellard (pthreads)", "goja": "Goja (goroutines)"}
    TC = dict(COLOR, **{"ng-arena1": "#b45309"})
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(14, 5.4), gridspec_kw={"width_ratios": [1.1, 1]})
    for tg in tags:
        d = MI[tg]; xs = [n for n in d["n"] if n >= 1]
        a1.plot(xs, [d["live_rss_kb"][str(n)] / 1024 for n in xs], marker="o", color=TC[tg], label=TL[tg],
                ls="--" if tg == "ng-arena1" else "-")
    a1.set_xscale("log"); a1.set_yscale("log")
    plain_log(a1.xaxis); plain_log(a1.yaxis)
    a1.set_xlabel("live JS runtimes in one process (N, log axis)")
    a1.set_ylabel("process RSS, MiB (log axis, lower is better)")
    title(a1, "Total RSS vs number of runtimes", "Each runtime evaluated a small script and stays alive. Median of 3 runs.")
    a1.legend(loc="upper left", fontsize=8.5, **LEG)
    ns_ = [n for n in (1, 10, 100, 1000) if all(str(n) in MI[tg].get("per_instance_kb", {}) for tg in tags)]
    k = len(tags); w = 0.8 / k
    for i, tg in enumerate(tags):
        xs = [j + (i - (k - 1) / 2) * w for j in range(len(ns_))]
        vals = [MI[tg]["per_instance_kb"][str(n)] for n in ns_]
        bars = a2.bar(xs, vals, w, color=TC[tg], label=TL[tg], hatch="//" if tg == "ng-arena1" else None, edgecolor="white")
        for b, v in zip(bars, vals):
            a2.annotate("%.0f" % v, (b.get_x() + w / 2, v), xytext=(0, 2), textcoords="offset points", ha="center",
                        va="bottom", fontsize=6.5, rotation=90)
    a2.set_xticks(range(len(ns_))); a2.set_xticklabels(["N=%d" % n for n in ns_])
    a2.set_yscale("log"); plain_log(a2.yaxis)
    a2.set_ylabel("(RSS(N) - RSS(0)) / N, KiB per runtime\n(log axis, lower is better)")
    a2.set_ylim(min(MI[tg]["per_instance_kb"][str(n)] for tg in tags for n in ns_) / 2,
                max(MI[tg]["per_instance_kb"][str(n)] for tg in tags for n in ns_) * 2.5)
    title(a2, "Memory per runtime", "RSS(0) = same harness with no runtime. At N=1000 this is close to the marginal cost.")
    save(fig, "mem-scaling")
