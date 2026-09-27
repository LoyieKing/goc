#!/usr/bin/env python3
"""Draw docs/benchmark/charts/*.{svg,png} from docs/benchmark/data/all.json.

Every chart is written twice with the same name: .png (embedded in
docs/benchmark.md) and .svg (embedded in docs/benchmark/report.html).
Needs matplotlib and a CJK font (Noto Sans CJK SC).

Usage: bench-charts.py [ALL_JSON] [OUT_DIR]
"""
import json, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs/benchmark/data/all.json")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs/benchmark/charts")
os.makedirs(OUT, exist_ok=True)
D = json.load(open(ALL))

for f in font_manager.findSystemFonts():
    if "NotoSansCJK-Regular" in f:
        font_manager.fontManager.addfont(f)
plt.rcParams.update({
    "font.family": ["Noto Sans CJK SC", "DejaVu Sans"],
    "axes.unicode_minus": False, "svg.fonttype": "none", "font.size": 10,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.alpha": 0.3, "axes.axisbelow": True,
})
ENG = ["goc", "ng", "bellard", "goja"]
LABEL = {"goc": "goc", "ng": "native ng", "bellard": "Bellard", "goja": "Goja"}
COLOR = {"goc": "#2563eb", "ng": "#f59e0b", "bellard": "#10b981", "goja": "#94a3b8"}
DATE = "2026-09-27"

def save(fig, name):
    fig.savefig(os.path.join(OUT, name + ".png"), dpi=150, bbox_inches="tight")
    fig.savefig(os.path.join(OUT, name + ".svg"), bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)

def title(ax, t, sub):
    ax.set_title(t, fontsize=13, fontweight="bold", loc="left", pad=24)
    ax.annotate(sub, xy=(0, 1), xycoords="axes fraction", xytext=(0, 8), textcoords="offset points",
                fontsize=9, color="#475569", va="bottom", ha="left")

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

# 1. overview: speed relative to native ng
v8m = D["v8"]["median"]; ss = D["sunspider"]; mb = D["micro"]
rel = {e: [v8m[e]["Score"] / v8m["ng"]["Score"],
           ss["geomean4"]["ng"] / ss["geomean4"][e],
           mb["geomean"]["ng"] / mb["geomean"][e]] for e in ENG}
fig, ax = plt.subplots(figsize=(9, 4.8))
grouped(ax, ["V8-v7 总分", "SunSpider 几何平均（%d 项）" % len(ss["common4"]), "microbench 几何平均（%d 项）" % len(mb["common"])],
        rel, ENG, lambda v: "%.2f" % v, fs=9)
ax.axhline(1, color="#334155", lw=1, ls="--")
ax.set_ylabel("相对 native ng 的速度（越高越快，native ng = 1）")
ax.set_ylim(0, max(max(v) for v in rel.values()) * 1.15)
title(ax, "相对 native ng 的速度", "V8 用分数之比，SunSpider 和 microbench 用耗时之比的倒数。%s 实测；每个时段内四家交替运行。" % DATE)
ax.legend(ncol=4, loc="upper right", frameon=True, facecolor="white", edgecolor="none", framealpha=1)
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
ax.set_xticks([0, 1]); ax.set_xticklabels(["test262 language 抽样", "QuickJS 官方测试（按函数）"])
ax.set_ylim(0, 115); ax.set_yticks(range(0, 101, 20))
ax.set_ylabel("通过率 %（越高越好）")
title(ax, "正确性", "test262 是 test/language 分层抽样 %d 项；QuickJS 是 Bellard tests/test_*.js 的 %d 个函数。" % (t["ran"], q["total"]))
ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.1), frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "overview-correct")

# 3. V8 per sub-benchmark
subs = ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes", "Score"]
fig, ax = plt.subplots(figsize=(12, 5.2))
grouped(ax, subs[:-1] + ["总分"], {e: [v8m[e][s] for s in subs] for e in ENG}, ENG,
        lambda v: "%g" % v, rot=90, fs=7.5)
ax.set_ylabel("分数（越高越快）")
ax.set_ylim(0, max(v8m[e][s] for e in ENG for s in subs) * 1.18)
title(ax, "V8-v7 各子项分数", "bench-v8.js，每个引擎 %d 轮，取中位数（每个子项单独取）。" % len(D["v8"]["rounds"]["goc"]))
ax.legend(ncol=4, loc="upper left", frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "v8")

# 4. SunSpider goc / ng / Bellard (linear, horizontal)
tests = ss["tests"]
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
    ax.invert_yaxis()
    if log:
        ax.set_xscale("log")
    ax.grid(axis="y", visible=False)
    return fails

def mark_fails(ax, fails, fs=6.5):
    x0 = ax.get_xlim()[0]
    for y in fails:
        ax.annotate("失败", (x0, y), xytext=(2, 0), textcoords="offset points", va="center", fontsize=fs, color="#dc2626")
get = lambda e, t: ss["ms"][e].get(t)
fig, ax = plt.subplots(figsize=(10, 13))
fl = hbars(ax, tests, ENG[:3], get, lambda v: "%.1f" % v)
ax.set_xlabel("ms/次（越低越快）")
ax.set_xlim(0, max(get(e, t) or 0 for e in ENG[:3] for t in tests) * 1.12)
mark_fails(ax, fl)
title(ax, "SunSpider 1.0.2：goc / native ng / Bellard", "每项取 %d 轮的中位数；每轮是 5 批中最快一批的平均单次耗时。" % ss["rounds"]["goc"])
ax.legend(ncol=3, loc="lower right", frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "sunspider")

fig, ax = plt.subplots(figsize=(10, 15))
fl = hbars(ax, tests, ENG, get, lambda v: "%.1f" % v, log=True, fs=6)
ax.set_xlabel("ms/次（越低越快，对数轴）")
lo = min(get(e, t) for e in ENG for t in tests if get(e, t)); hi = max(get(e, t) or 0 for e in ENG for t in tests)
ax.set_xlim(lo / 1.5, hi * 2.5)
mark_fails(ax, fl, 6)
title(ax, "SunSpider 1.0.2：四引擎，对数轴", "Goja 比另外三家慢数倍，所以横轴取对数。")
ax.legend(ncol=4, loc="lower right", frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "sunspider-log")

# 5. microbench groups
G = mb["group_geomean"]; gn = list(G)
fig, ax = plt.subplots(figsize=(12, 5.2))
grouped(ax, gn, {e: [G[g][e] for g in gn] for e in ENG}, ENG, lambda v: "%.0f" % v if v >= 100 else "%.1f" % v,
        rot=90, fs=7, log=True)
ax.set_ylabel("ns/op 组内几何平均（越低越快，对数轴）")
ax.set_ylim(top=max(G[g][e] for g in gn for e in ENG) * 6)
title(ax, "microbench 分组", "Bellard tests/microbench.js，每项取 %d 轮中位数，组内求几何平均。分组规则见 scripts/bench-summarize.py。" % mb["rounds"]["goc"])
ax.legend(ncol=4, loc="upper left", frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "micro-groups")

# 6. microbench goc / ng per test
ratios = sorted(((mb["ns"]["goc"][k] / mb["ns"]["ng"][k], k) for k in mb["tests"]
                 if k in mb["ns"]["goc"] and k in mb["ns"]["ng"]), reverse=True)
fig, ax = plt.subplots(figsize=(9, 16))
cols = ["#dc2626" if r >= 1.3 else "#2563eb" if r >= 1.0 else "#16a34a" for r, _ in ratios]
bars = ax.barh(range(len(ratios)), [r for r, _ in ratios], color=cols)
for b, (r, _) in zip(bars, ratios):
    ax.annotate("%.2f" % r, (r, b.get_y() + b.get_height() / 2), xytext=(2, 0), textcoords="offset points", va="center", fontsize=7)
ax.set_yticks(range(len(ratios))); ax.set_yticklabels([k for _, k in ratios], fontsize=7.5)
ax.invert_yaxis(); ax.grid(axis="y", visible=False)
ax.axvline(1, color="#334155", ls="--", lw=1, zorder=0)
ax.set_xlim(0, max(r for r, _ in ratios) * 1.12)
ax.set_xlabel("goc 耗时 / native ng 耗时（越低越好，1 = 一样快）")
title(ax, "microbench：goc 相对 native ng", "红 ≥ 1.30，蓝 1.00–1.30，绿 < 1。")
save(fig, "micro-ratio")

# 7. test262 per directory (only directories with a failure)
dirs = [d for d, v in t["dirs"].items() if any(v[e] < v["ran"] for e in ENG)]
fig, ax = plt.subplots(figsize=(10, 7))
mark_fails(ax, hbars(ax, dirs, ENG, lambda e, d: 100 * t["dirs"][d][e] / t["dirs"][d]["ran"], lambda v: "%.1f%%" % v, fs=6.5))
ax.set_xlim(0, 112); ax.set_xlabel("通过率 %（越高越好）")
title(ax, "test262 language 抽样：有失败的目录", "满通过的目录见正文的表。")
ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.07), frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "test262")

# 8. QuickJS official tests per file
files = list(q["files"])
fig, ax = plt.subplots(figsize=(10, 4.8))
grouped(ax, ["%s（%d）" % (f.replace(".js", ""), q["files"][f]["n"]) for f in files],
        {e: [q["files"][f][e] for f in files] for e in ENG}, ENG, lambda v: "%d" % v, fs=8)
ax.set_ylabel("通过的函数数（越高越好）")
ax.set_ylim(0, max(q["files"][f]["n"] for f in files) * 1.3)
title(ax, "QuickJS 官方测试：各文件通过的函数数", "Bellard 2026-06-04 tests/test_*.js，每个函数单独进程。括号里是函数总数。")
ax.legend(ncol=4, loc="upper right", frameon=True, facecolor="white", edgecolor="none", framealpha=1)
save(fig, "qjs-tests")
