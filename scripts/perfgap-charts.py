#!/usr/bin/env python3
"""Draw docs/perf-gap/charts/*.{png,svg} from docs/perf-gap/data/*.json.

All chart text is English (the doc prose is Chinese). Needs matplotlib.
Charts whose input file is missing are skipped.
Usage: perfgap-charts.py [DATA_DIR] [OUT_DIR]
"""
import json, math, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs/perf-gap/data")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs/perf-gap/charts")
os.makedirs(OUT, exist_ok=True)
plt.rcParams.update({
    "font.family": ["DejaVu Sans"], "axes.unicode_minus": False, "svg.fonttype": "none",
    "font.size": 10, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.alpha": 0.3, "axes.axisbelow": True,
})
DATE = "2026-09-27"
LEG = dict(frameon=True, facecolor="white", edgecolor="none", framealpha=1)
BUILDS = ["gcc-O2", "clang-O2", "clang-O3", "clang-O3-gocflags", "goc"]
BCOL = {"gcc-O2": "#f59e0b", "clang-O2": "#93c5fd", "clang-O3": "#3b82f6",
        "clang-O3-gocflags": "#6366f1", "goc": "#047857"}
LAYERS = ["compiler (clang-O2/gcc-O2)", "opt level (clang-O3/clang-O2)",
          "goc flags (gocflags/clang-O3)", "goc runtime (goc/gocflags)"]
LSHORT = ["compiler\nclang-O2 / gcc-O2", "opt level\nclang-O3 / clang-O2",
          "goc flags\ngocflags / clang-O3", "goc runtime\ngoc / gocflags"]
LCOL = ["#f59e0b", "#3b82f6", "#6366f1", "#047857"]
SUITES = ["V8", "SunSpider", "microbench", "microcall"]
FAM = {"ng": "quickjs-ng 0.17.0", "bellard": "Bellard QuickJS 2026-06-04"}

def save(fig, name):
    fig.savefig(os.path.join(OUT, name + ".png"), dpi=150, bbox_inches="tight")
    fig.savefig(os.path.join(OUT, name + ".svg"), bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)

def title(ax, t, sub):
    ax.set_title(t, fontsize=13, fontweight="bold", loc="left", pad=24 + 12 * sub.count("\n"))
    ax.annotate(sub, xy=(0, 1), xycoords="axes fraction", xytext=(0, 8), textcoords="offset points",
                fontsize=9, color="#475569", va="bottom", ha="left")

def load(n):
    p = os.path.join(DD, n)
    return json.load(open(p)) if os.path.exists(p) else None

A = load("all.json")

# 1. build matrix: suite time relative to the gcc -O2 build of the same source
if A:
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.6), sharey=True)
    for ax, fam in zip(axs, ["ng", "bellard"]):
        L = A["layers"][fam]["suite"]
        w = 0.16
        for i, b in enumerate(BUILDS):
            vals = [L[s]["time"][b] / L[s]["time"]["gcc-O2"] for s in SUITES]
            xs = [j + (i - 2) * w for j in range(len(SUITES))]
            bars = ax.bar(xs, vals, w, color=BCOL[b], label=b)
            for bb, v in zip(bars, vals):
                ax.annotate("%.2f" % v, (bb.get_x() + w / 2, v), xytext=(0, 2), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6.5, rotation=90)
        ax.axhline(1, color="#475569", lw=0.8)
        ax.set_xticks(range(len(SUITES)))
        ax.set_xticklabels(["V8\n(1/score)", "SunSpider\n(geomean ms)", "microbench\n(geomean ns)", "microcall\n(1/score)"])
        ax.set_title(FAM[fam], fontsize=11, loc="left")
        ax.set_ylim(0, 1.7)
    axs[0].set_ylabel("time relative to gcc -O2 (lower is better)")
    h, l = axs[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=5, fontsize=9, bbox_to_anchor=(0.5, -0.1), **LEG)
    fig.suptitle("Same source, five builds: where does the time go?", x=0.06, ha="left", fontsize=13, fontweight="bold")
    fig.text(0.06, 0.905, "pinned CPU, median of rounds (V8 5, SunSpider 3, micro 3, microcall 5); natives -DNDEBUG; %s" % DATE,
             fontsize=9, color="#475569")
    fig.subplots_adjust(top=0.82)
    save(fig, "perf-gap-matrix")

# 2. layer decomposition per suite (log-ratio stacked bars)
if A:
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.8), sharey=True)
    for ax, fam in zip(axs, ["ng", "bellard"]):
        L = A["layers"][fam]["suite"]
        for j, s in enumerate(SUITES):
            pos = neg = 0.0
            for k, n in enumerate(LAYERS):
                v = 100 * math.log(L[s]["layers"][n])  # percent (log points)
                base = pos if v >= 0 else neg
                ax.bar(j, v, 0.55, bottom=base, color=LCOL[k], label=LSHORT[k].replace("\n", ": ") if j == 0 else None,
                       edgecolor="white", lw=0.5)
                if abs(v) >= 2.5:
                    ax.text(j, base + v / 2, "%+.0f" % v, ha="center", va="center", fontsize=7.5, color="white")
                if v >= 0:
                    pos += v
                else:
                    neg += v
            tot = 100 * math.log(L[s]["goc/gcc"])
            ax.plot([j - 0.33, j + 0.33], [tot, tot], color="black", lw=2)
            ax.annotate("net %+.0f" % tot, (j + 0.3, tot), xytext=(3, 0), textcoords="offset points", fontsize=8, va="center")
        ax.axhline(0, color="#475569", lw=0.8)
        ax.set_xticks(range(len(SUITES)))
        ax.set_xticklabels(SUITES)
        ax.set_title(FAM[fam], fontsize=11, loc="left")
    axs[0].set_ylabel("contribution, log points (100*ln ratio; + = slower)")
    axs[1].legend(loc="lower right", fontsize=7.5, **LEG)
    fig.suptitle("Layered decomposition: goc vs gcc -O2 of the same source", x=0.06, ha="left", fontsize=13, fontweight="bold")
    fig.text(0.06, 0.905, "each layer = time ratio of adjacent builds; layers add up (in log points) to the black net bar; %s" % DATE,
             fontsize=9, color="#475569")
    fig.subplots_adjust(top=0.82)
    save(fig, "perf-gap-layers")

# 3. per-subtest heatmaps (V8 + SunSpider), both flavors
def heat(prefix, name, ttl, h):
    fams = ["ng", "bellard"]
    items = [k for k in A["layers"]["ng"]["item"] if k.startswith(prefix) and k != "v8/Score"]
    fig, axs = plt.subplots(1, 2, figsize=(13, h), sharey=True)
    cols = LAYERS + ["goc / clang-O3"]
    for ax, fam in zip(axs, fams):
        I = A["layers"][fam]["item"]
        M = np.array([I[k]["layers"] + [I[k]["time"][4] / I[k]["time"][2]] for k in items])
        im = ax.imshow(np.log(M), cmap="RdBu_r", vmin=-0.5, vmax=0.5, aspect="auto")
        for r in range(M.shape[0]):
            for c in range(M.shape[1]):
                ax.text(c, r, "%.2f" % M[r, c], ha="center", va="center", fontsize=7,
                        color="white" if abs(math.log(M[r, c])) > 0.3 else "black")
        ax.set_xticks(range(len(cols)))
        ax.set_xticklabels(["compiler", "opt level", "goc flags", "goc runtime", "goc/clang-O3"], rotation=20, fontsize=8)
        ax.set_yticks(range(len(items)))
        ax.set_yticklabels([k.split("/", 1)[1] for k in items], fontsize=8)
        ax.set_title(FAM[fam], fontsize=11, loc="left")
        ax.grid(False)
    fig.subplots_adjust(top=1 - 0.9 / h, right=0.88, wspace=0.08)
    cax = fig.add_axes([0.9, 0.2, 0.012, 0.55])
    cb = fig.colorbar(im, cax=cax)
    cb.set_label("time ratio (log color scale; red = slower)")
    cb.set_ticks([math.log(x) for x in (0.67, 0.8, 1, 1.25, 1.5)])
    cb.set_ticklabels(["0.67", "0.8", "1", "1.25", "1.5"])
    fig.suptitle(ttl, x=0.06, ha="left", fontsize=13, fontweight="bold")
    fig.text(0.06, 1 - 0.5 / h, "per-item time ratios of adjacent builds (>1 = slower); last column: goc vs clang -O3 -DNDEBUG; %s" % DATE,
             fontsize=9, color="#475569")
    save(fig, name)

if A:
    heat("v8/", "perf-gap-v8-layers", "V8 sub-tests: layer ratios", 4.6)
    heat("ss/", "perf-gap-ss-layers", "SunSpider: layer ratios", 9.5)

# ---------------------------------------------------------------- counters
V8S = ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes"]
CG = load("callgrind.json")
if CG:
    def agg(e):
        t = {}
        for s in V8S:
            for k, v in CG[e][s]["totals"].items():
                t[k] = t.get(k, 0) + v
        return t
    MET = [("Ir", lambda a: a["Ir"]), ("D1 misses", lambda a: a["D1mr"] + a["D1mw"]),
           ("LL misses", lambda a: a["ILmr"] + a["DLmr"] + a["DLmw"]),
           ("cond. branch\nmispredicts", lambda a: a["Bcm"]), ("indirect branch\nmispredicts", lambda a: a["Bim"])]
    FAMS = {"ng": ("ng-clang-O3", ["ng-gcc-O2", "ng-clang-O2", "ng-clang-O3-gocflags", "goc-ng", "ng-clang-O3-taildup", "goc-ng-taildup"]),
            "bellard": ("bellard-clang-O3", ["bellard-gcc-O2-NDEBUG", "bellard-clang-O2-NDEBUG", "bellard-clang-O3-gocflags",
                                             "bellard-gocpipe-sm", "goc-bellard", "bellard-clang-O3-taildup", "goc-bellard-taildup"])}
    ECOL = ["#f59e0b", "#93c5fd", "#6366f1", "#a78bfa", "#047857", "#fca5a5", "#b91c1c"]
    fig, axs = plt.subplots(1, 2, figsize=(14, 4.8), sharey=True)
    for ax, fam in zip(axs, ["ng", "bellard"]):
        ref, engs = FAMS[fam]
        engs = [e for e in engs if e in CG]
        R = agg(ref)
        w = 0.8 / len(engs)
        for i, e in enumerate(engs):
            a = agg(e)
            vals = [f(a) / f(R) for _, f in MET]
            xs = [j + (i - (len(engs) - 1) / 2) * w for j in range(len(MET))]
            bars = ax.bar(xs, vals, w, color=ECOL[i % len(ECOL)], label=e)
            for bb, v in zip(bars, vals):
                ax.annotate("%.2f" % v, (bb.get_x() + w / 2, v), xytext=(0, 2), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6, rotation=90)
        ax.axhline(1, color="#475569", lw=0.8)
        ax.set_xticks(range(len(MET)))
        ax.set_xticklabels([m for m, _ in MET], fontsize=8.5)
        ax.set_ylim(0, 1.75)
        ax.set_title(FAM[fam] + "  (1.0 = %s)" % ref, fontsize=10, loc="left")
        ax.legend(fontsize=7, loc="upper left", ncol=2, **LEG)
    axs[0].set_ylabel("count relative to clang -O3 (lower is better)")
    fig.suptitle("Simulated counters (callgrind): V8 suites, fixed work", x=0.06, ha="left", fontsize=13, fontweight="bold")
    fig.text(0.06, 0.905, "no hardware PMU in this VM: Ir = instructions, cache and branch numbers from callgrind's simple models "
             "(indirect predictor = last target); %s" % DATE, fontsize=9, color="#475569")
    fig.subplots_adjust(top=0.8)
    save(fig, "perf-gap-counters")

# ---------------------------------------------------------------- Ir decomposition
DEC = load("ir-decomposition.json")
if DEC:
    CATS = [("stackcheck", "morestack prologue check", "#b91c1c"), ("guard", "frame-move guard", "#f97316"),
            ("tls", "uptr / g (FS:-8) code", "#eab308"), ("got", "GOT loads", "#a3a3a3"),
            ("framemem", "extra frame loads/stores", "#6366f1"), ("pushpop", "push/pop", "#93c5fd"),
            ("rest+call", "other codegen", "#0ea5e9"), ("memstr", "memcpy/memset/memcmp (shim)", "#047857"),
            ("alloc", "malloc/free (shim)", "#10b981"), ("time", "clock/localtime (shim)", "#84cc16"),
            ("math", "libm bridge", "#14b8a6"), ("goruntime", "Go runtime / cgo", "#78716c"), ("other", "other", "#d6d3d1")]
    SU = V8S + ["Parse", "V8sum"]
    pairs = [("goc-bellard|bellard-clang-O3-gocflags", "goc-bellard vs clang -O3 + goc flags"),
             ("goc-ng|ng-clang-O3-gocflags", "goc-ng vs clang -O3 + goc flags")]
    fig, axs = plt.subplots(1, 2, figsize=(14, 5.6), sharey=True)
    for ax, (pk, ttl) in zip(axs, pairs):
        P = DEC[pk]
        for j, s in enumerate(SU):
            if s not in P: continue
            pos = neg = 0.0
            for k, lab, col in CATS:
                v = P[s][k]
                base = pos if v >= 0 else neg
                ax.bar(j, v, 0.6, bottom=base, color=col, label=lab if j == 0 else None, edgecolor="white", lw=0.4)
                if v >= 0: pos += v
                else: neg += v
            net = (P[s]["ratio"] - 1) * 100
            ax.plot([j - 0.35, j + 0.35], [net, net], color="black", lw=2)
            ax.annotate("%+.0f%%" % net, (j, max(pos, net)), xytext=(0, 3), textcoords="offset points", ha="center", fontsize=8)
        ax.axhline(0, color="#475569", lw=0.8)
        ax.set_xticks(range(len(SU)))
        ax.set_xticklabels([s if s != "V8sum" else "V8 total" for s in SU], rotation=30, fontsize=8.5)
        ax.set_title(ttl, fontsize=10.5, loc="left")
    axs[0].set_ylabel("extra instructions, % of the native build's Ir")
    h, l = axs[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=5, fontsize=8, bbox_to_anchor=(0.5, -0.13), **LEG)
    fig.suptitle("Where goc's extra instructions go (callgrind Ir, per instruction class)", x=0.06, ha="left",
                 fontsize=13, fontweight="bold")
    fig.text(0.06, 0.905, "every executed instruction mapped back through objdump and classified; black bar = net Ir change; "
             "Parse = the V8 file with no suite run; %s" % DATE, fontsize=9, color="#475569")
    fig.subplots_adjust(top=0.8, bottom=0.2)
    save(fig, "perf-gap-ir-decomp")

    # layer split through goc's pipeline built natively (Bellard, V8 total)
    steps = [("bellard-gocpipe|bellard-clang-O3-gocflags", "goc's IR pipeline\n(opt text pipeline + llc)"),
             ("bellard-gocpipe-sm|bellard-gocpipe", "stack maps +\nframe-move guards"),
             ("goc-bellard|bellard-gocpipe-sm", "goc runtime\n(morestack, uptr,\nshim, Go link)")]
    if all(k in DEC for k, _ in steps):
        fig, ax = plt.subplots(figsize=(10, 4.6))
        cum = 0.0
        for j, (k, lab) in enumerate(steps):
            P = DEC[k]["V8sum"]
            base = cum
            for c, clab, col in CATS:
                v = P[c]
                ax.bar(j, v, 0.55, bottom=base if v >= 0 else base + v, color=col,
                       label=clab if j == 0 else None, edgecolor="white", lw=0.4)
                if v >= 0: base += v
            step = (P["ratio"] - 1) * 100
            ax.annotate("%+.1f%%" % step, (j, cum + max(step, 0)), xytext=(0, 4), textcoords="offset points",
                        ha="center", fontsize=9, fontweight="bold")
            cum += step
        ax.set_xticks(range(len(steps)))
        ax.set_xticklabels([l for _, l in steps], fontsize=9)
        ax.axhline(0, color="#475569", lw=0.8)
        ax.set_ylabel("Ir added by the step, % (V8 suites total)")
        ax.legend(fontsize=7, loc="upper left", ncol=2, **LEG)
        title(ax, "Bellard: from clang -O3 + goc flags to goc, one step at a time",
              "each step is a real build; bars = extra Ir by instruction class (callgrind); %s" % DATE)
        save(fig, "perf-gap-steps")

# ---------------------------------------------------------------- C mechanism microbenchmarks
import re as _re
def mech_c():
    runs = {}
    for fn in ("mech-c.txt", "mech-c-2.txt", "mech-c-3.txt"):
        p = os.path.join(DD, fn)
        if not os.path.exists(p): continue
        cur = None
        for l in open(p):
            m = _re.match(r"== mech-(\S+)", l)
            if m: cur = m.group(1); continue
            m = _re.match(r"(\w+) ns/op=([\d.]+)", l)
            if m and cur: runs.setdefault(cur, {}).setdefault(m.group(1), []).append(float(m.group(2)))
    return {b: {t: min(v) for t, v in d.items()} for b, d in runs.items()}
MC = mech_c()
if MC:
    order = [b for b in ["gcc-O2", "clang-O3", "clang-O3-taildup", "clang-O3-f-fp", "clang-O3-f-nosib",
                         "clang-O3-f-notailmerge", "clang-O3-f-align8", "clang-O3-f-noslotshare",
                         "clang-O3-gocflags", "goc", "goc-taildup"] if b in MC]
    tests = ["leafcalls", "fib", "localaddr", "interp", "chase", "indirect"]
    col = {"gcc-O2": "#f59e0b", "clang-O3": "#3b82f6", "clang-O3-taildup": "#93c5fd", "clang-O3-gocflags": "#6366f1",
           "clang-O3-f-fp": "#e9d5ff", "clang-O3-f-nosib": "#d8b4fe", "clang-O3-f-notailmerge": "#c084fc",
           "clang-O3-f-align8": "#a855f7", "clang-O3-f-noslotshare": "#7e22ce",
           "goc": "#047857", "goc-taildup": "#6ee7b7"}
    fig, ax = plt.subplots(figsize=(15, 5))
    w = 0.8 / len(order)
    for i, b in enumerate(order):
        vals = [MC[b].get(t, 0) for t in tests]
        xs = [j + (i - (len(order) - 1) / 2) * w for j in range(len(tests))]
        bars = ax.bar(xs, vals, w, color=col[b], label=b)
        for bb, v in zip(bars, vals):
            ax.annotate("%.2f" % v, (bb.get_x() + w / 2, v), xytext=(0, 2), textcoords="offset points",
                        ha="center", va="bottom", fontsize=6.5, rotation=90)
    ax.set_xticks(range(len(tests)))
    ax.set_xticklabels(["leaf call", "fib (recursion)", "&local across call", "computed-goto\ninterpreter",
                        "pointer chase", "indirect call"])
    ax.set_ylabel("ns per operation (lower is better)")
    ax.legend(fontsize=7.5, ncol=4, loc="upper left", **LEG)
    title(ax, "C mechanism microbenchmarks (tests/perfgap/mech.c)",
          "same C file built several ways (f-X = clang -O3 plus only goc flag X), pinned CPU, best of the recorded runs; %s" % DATE)
    save(fig, "perf-gap-mech-c")

# ---------------------------------------------------------------- JS mechanism probes
def mech_js():
    d = os.path.join(DD, "mech-js")
    if not os.path.isdir(d): return None
    res = {}
    for f in os.listdir(d):
        m = _re.match(r"(.+)-r\d+\.txt$", f)
        if not m: continue
        for l in open(os.path.join(d, f)):
            mm = _re.match(r"(\w+) ([\d.]+) ns/op", l)
            if mm: res.setdefault(m.group(1), {}).setdefault(mm.group(1), []).append(float(mm.group(2)))
    import statistics as st
    return {e: {k: st.median(v) for k, v in d2.items()} for e, d2 in res.items()}
MJ = mech_js()
if MJ and "bellard-clang-O3" in MJ:
    ref = MJ["bellard-clang-O3"]
    probes = [k for k in ref if k != "empty_loop"]
    engs = [e for e in ["bellard-gcc-O2-NDEBUG", "bellard-clang-O3-gocflags", "bellard-gocpipe-sm", "goc-bellard",
                        "goc-bellard-fast", "goc-bellard-best"] if e in MJ]
    col = ["#f59e0b", "#6366f1", "#a78bfa", "#047857", "#6ee7b7", "#0f766e"]
    fig, ax = plt.subplots(figsize=(8.5, 11))
    h = 0.8 / len(engs)
    for i, e in enumerate(engs):
        vals = [MJ[e][p] / ref[p] for p in probes]
        ys = [j + (i - (len(engs) - 1) / 2) * h for j in range(len(probes))]
        ax.barh(ys, vals, h, color=col[i], label=e)
    ax.axvline(1, color="#475569", lw=0.8)
    ax.set_yticks(range(len(probes)))
    ax.set_yticklabels(probes, fontsize=8)
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.set_xticks([0.5, 0.75, 1, 1.5, 2, 3, 5, 8])
    ax.set_xticklabels(["0.5", "0.75", "1", "1.5", "2", "3", "5", "8"])
    ax.set_xlabel("time per op relative to Bellard clang -O3 (log scale, lower is better)")
    ax.legend(fontsize=8, loc="lower right", **LEG)
    title(ax, "JS mechanism probes (tests/bench/perfgap-mech.js)",
          "one runtime service per loop; median of 3 pinned rounds;\ngoc-bellard-fast = shim experiment, best = fast + tail-dup + inline-threshold 250; %s" % DATE)
    save(fig, "perf-gap-mech-js")

# ---------------------------------------------------------------- toggles / flags
def suite_times(sub):
    p = os.path.join(DD, sub, "all.json")
    if not os.path.exists(p): return None
    return json.load(open(p))["suites"]
TG = suite_times("toggles")
if TG:
    groups = [("Bellard", "bellard-clang-O3", ["bellard-gcc-O2-NDEBUG", "bellard-clang-O3-taildup", "bellard-clang-O3-gocflags",
                                               "bellard-clang-O3-gocflags-taildup", "bellard-gocpipe", "bellard-gocpipe-inl250",
                                               "bellard-gocpipe-sm", "goc-bellard", "goc-bellard-inl250", "goc-bellard-taildup",
                                               "goc-bellard-fast", "goc-bellard-best"]),
              ("quickjs-ng", "ng-clang-O3", ["ng-clang-O2", "ng-clang-O3-taildup", "ng-clang-O3-gocflags",
                                             "ng-clang-O3-gocflags-taildup", "goc-ng", "goc-ng-inl250", "goc-ng-taildup",
                                             "goc-ng-fast", "goc-ng-best"])]
    fig, axs = plt.subplots(1, 2, figsize=(15, 6.5))
    scol = {"V8": "#3b82f6", "SunSpider": "#f59e0b", "microbench": "#10b981", "microcall": "#a855f7"}
    for ax, (fam, ref, engs) in zip(axs, groups):
        engs = [e for e in engs if e in TG["V8"]["time"]]
        h = 0.2
        for i, s in enumerate(SUITES):
            vals = [TG[s]["time"][e] / TG[s]["time"][ref] for e in engs]
            ys = [j + (i - 1.5) * h for j in range(len(engs))]
            ax.barh(ys, vals, h, color=scol[s], label=s)
            for y, v in zip(ys, vals):
                ax.text(v, y, " %.2f" % v, va="center", fontsize=6)
        ax.axvline(1, color="#475569", lw=0.8)
        ax.set_yticks(range(len(engs)))
        ax.set_yticklabels(engs, fontsize=8)
        ax.invert_yaxis()
        ax.set_xlim(0.5, 1.45)
        ax.set_xlabel("time relative to %s (lower is better)" % ref)
        ax.set_title(fam, fontsize=11, loc="left")
    axs[0].legend(fontsize=8, loc="lower right", **LEG)
    fig.suptitle("Mechanism toggles: tail-dup dispatch, shim fixes, inliner threshold, goc pipeline built natively",
                 x=0.04, ha="left", fontsize=13, fontweight="bold")
    fig.text(0.04, 0.93, "same pinned session, interleaved rounds (V8 5, SunSpider 3, micro 3, microcall 3); %s" % DATE,
             fontsize=9, color="#475569")
    fig.subplots_adjust(top=0.86, wspace=0.35)
    save(fig, "perf-gap-toggles")
FL = suite_times("flags")
if FL:
    ref = "bellard-clang-O3"
    engs = [e for e in FL["V8"]["time"] if e != ref]
    engs.sort(key=lambda e: FL["V8"]["time"][e])
    fig, ax = plt.subplots(figsize=(10, 5.2))
    h = 0.38
    for i, s in enumerate(["V8", "SunSpider"]):
        vals = [FL[s]["time"][e] / FL[s]["time"][ref] for e in engs]
        ys = [j + (i - 0.5) * h for j in range(len(engs))]
        ax.barh(ys, vals, h, color=["#3b82f6", "#f59e0b"][i], label=s)
        for y, v in zip(ys, vals):
            ax.text(v, y, " %.3f" % v, va="center", fontsize=7)
    ax.axvline(1, color="#475569", lw=0.8)
    ax.set_yticks(range(len(engs)))
    ax.set_yticklabels([e.replace("bellard-clang-O3-", "") for e in engs], fontsize=8.5)
    ax.invert_yaxis()
    ax.set_xlim(0.8, 1.15)
    ax.set_xlabel("time relative to Bellard clang -O3 -DNDEBUG (lower is better)")
    ax.legend(fontsize=8, loc="lower right", **LEG)
    title(ax, "goc's codegen flags one at a time (Bellard, clang -O3)",
          "f-X = clang -O3 plus only flag X; gocflags = all of them; gocpipe = goc's opt+llc pipeline; %s" % DATE)
    save(fig, "perf-gap-flags")
