#!/usr/bin/env python3
"""Ten-engine V8-v7 comparison (docs/benchmark/README.md, "多引擎 V8-v7 对比").

Reads RAW/<engine>-r<round>.txt (bench-v8.js output plus a WALL line), takes
the median per item over the rounds, writes RAW/../v8multi.json, prints the
Markdown tables, and draws OUT/v8multi-total.svg and OUT/v8multi-subs.svg.
Chart text is English. Needs matplotlib.

Usage: report/v8multi.py RAW_DIR [OUT_DIR]
"""
import collections, glob, json, os, re, statistics as st, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.plot import plt, LEG, saver, title  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RAW = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs/benchmark/charts")
DATE = "2026-10-21"
ENG = ["bellard", "liustack-c-gcc", "bellard-clang", "liustack-c-clang", "goc-bellard",
       "ng", "goc-ng", "liustack-go", "modernc-go", "goja"]
LABEL = {"bellard": "native Bellard (gcc)", "liustack-c-gcc": "liustack C (gcc)",
         "bellard-clang": "native Bellard (clang)", "liustack-c-clang": "liustack C (clang)",
         "goc-bellard": "goc-bellard", "ng": "native ng (clang)", "goc-ng": "goc-ng",
         "liustack-go": "liustack/quickjs (ccgo, Go)", "modernc-go": "modernc.org/quickjs (ccgo, Go)",
         "goja": "Goja"}
COLOR = {"bellard": "#6ee7b7", "liustack-c-gcc": "#a7f3d0", "bellard-clang": "#34d399",
         "liustack-c-clang": "#99f6e4", "goc-bellard": "#047857", "ng": "#93c5fd", "goc-ng": "#1d4ed8",
         "liustack-go": "#ea580c", "modernc-go": "#fdba74", "goja": "#94a3b8"}
SHORT = {"bellard": "Bellard gcc", "liustack-c-gcc": "liustack C gcc", "bellard-clang": "Bellard clang",
         "liustack-c-clang": "liustack C clang", "goc-bellard": "goc-bellard", "ng": "native ng", "goc-ng": "goc-ng",
         "liustack-go": "liustack Go", "modernc-go": "modernc Go", "goja": "Goja"}
SUBS = ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes", "Score"]
BASE = "bellard"

R = collections.defaultdict(lambda: collections.defaultdict(list))
wall = collections.defaultdict(list)
for f in sorted(glob.glob(os.path.join(RAW, "*-r*.txt"))):
    e = re.sub(r"-r\d+\.txt$", "", os.path.basename(f))
    for l in open(f):
        m = re.match(r"(RESULT (\S+)|SCORE) (\d+)", l)
        if m:
            R[e][m.group(2) or "Score"].append(int(m.group(3)))
        m = re.match(r"WALL ([\d.]+)", l)
        if m:
            wall[e].append(float(m.group(1)))
ENG = [e for e in ENG if R[e]["Score"]]
med = {e: {s: st.median(R[e][s]) for s in SUBS} for e in ENG}
json.dump({"date": DATE, "rounds": {e: R[e]["Score"] for e in ENG}, "median": med,
           "all": {e: dict(R[e]) for e in ENG}, "wall_median": {e: st.median(wall[e]) for e in ENG if wall[e]}},
          open(os.path.join(os.path.dirname(os.path.abspath(RAW)), "v8multi.json"), "w"), indent=1)

f0 = lambda v: "%.0f" % v
print("| 子项 | " + " | ".join(SHORT[e] for e in ENG) + " |")
print("|---|" + "---:|" * len(ENG))
for s in SUBS:
    nm = "**总分**" if s == "Score" else s
    cell = (lambda v: "**%s**" % f0(v)) if s == "Score" else f0
    print("| %s | %s |" % (nm, " | ".join(cell(med[e][s]) for e in ENG)))
print("\n相对 native Bellard（gcc）的比值：\n")
print("| 子项 | " + " | ".join(SHORT[e] for e in ENG) + " |")
print("|---|" + "---:|" * len(ENG))
for s in SUBS:
    nm = "**总分**" if s == "Score" else s
    print("| %s | %s |" % (nm, " | ".join("%.3f" % (med[e][s] / med[BASE][s]) for e in ENG)))
print("\n| 引擎 | 各轮总分 | 中位数 | 最小–最大 | 单轮墙钟中位数 |\n|---|---|---:|---:|---:|")
for e in ENG:
    sc = R[e]["Score"]
    print("| %s | %s | %.0f | %d–%d | %.1f s |" % (LABEL[e], " / ".join(map(str, sc)), st.median(sc), min(sc), max(sc), st.median(wall[e])))

save = saver(OUT)
n = len(R[BASE]["Score"])
# total score, horizontal bars
fig, ax = plt.subplots(figsize=(10, 5.2))
ys = range(len(ENG))
vals = [med[e]["Score"] for e in ENG]
bars = ax.barh(list(ys), vals, 0.7, color=[COLOR[e] for e in ENG])
for b, e, v in zip(bars, ENG, vals):
    ax.annotate("%.0f  (%.2fx)" % (v, v / med[BASE]["Score"]), (v, b.get_y() + b.get_height() / 2),
                xytext=(3, 0), textcoords="offset points", va="center", fontsize=8.5)
ax.set_yticks(list(ys)); ax.set_yticklabels([LABEL[e] for e in ENG]); ax.set_ylim(len(ENG) - 0.5, -0.5)
ax.set_xlim(0, max(vals) * 1.25); ax.grid(axis="y", visible=False); ax.set_xlabel("V8-v7 score (higher is faster)")
title(ax, "V8-v7 total score, ten engines (%s)" % DATE,
      "Median of %d interleaved rounds per engine; ratio in parentheses is vs native Bellard (gcc -O2)." % n)
save(fig, "v8multi-total")
# per-subtest ratio vs native Bellard gcc, log scale
fig, ax = plt.subplots(figsize=(13, 5.5))
k = len(ENG); w = 0.86 / k
for i, e in enumerate(ENG):
    xs = [j + (i - (k - 1) / 2) * w for j in range(len(SUBS))]
    ax.bar(xs, [med[e][s] / med[BASE][s] for s in SUBS], w, label=LABEL[e], color=COLOR[e])
ax.set_xticks(range(len(SUBS))); ax.set_xticklabels(SUBS[:-1] + ["Total"])
ax.set_yscale("log"); ax.set_ylim(0.05, 1.15); ax.axhline(1, color="#334155", lw=0.8)
from matplotlib.ticker import FixedLocator, FuncFormatter, NullFormatter
ax.yaxis.set_major_locator(FixedLocator([0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 1, 1.2]))
ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: "%g" % v)); ax.yaxis.set_minor_formatter(NullFormatter())
ax.set_ylabel("score / native Bellard (gcc), log scale")
ax.legend(ncol=5, fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.08), **LEG)
title(ax, "V8-v7 per sub-benchmark, relative to native Bellard (gcc -O2)",
      "Median of %d interleaved rounds per engine, %s. Above 1 is faster than native Bellard." % (n, DATE))
save(fig, "v8multi-subs")
