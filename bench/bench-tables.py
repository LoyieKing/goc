#!/usr/bin/env python3
"""Print the markdown tables of docs/benchmark.md from data/all.json.

Usage: bench-tables.py [ALL_JSON] > /tmp/tables.md
Each table is preceded by a line `<!-- table: NAME -->` so it can be pasted
into the doc section by section.
"""
import json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = json.load(open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs/benchmark/data/all.json")))
ENG = ["goc-ng", "goc-bellard", "ng", "bellard", "goja"]
HDR = "| goc-ng | goc-bellard | native ng | native Bellard | Goja |"
NAME = {"goc-ng": "goc-ng", "goc-bellard": "goc-bellard", "ng": "native ng", "bellard": "native Bellard", "goja": "Goja"}
RH = " goc-ng/ng | goc-bellard/Bellard |"  # ratio columns
RA = "---:|---:|"
NUM = "---:|" * len(ENG)

def rat(a, b, p=2):
    return "—" if not a or not b else "%.*f" % (p, a / b)

def rats(d, p=2):
    """goc-ng/ng and goc-bellard/Bellard cells from a {engine: value} dict."""
    return "%s | %s" % (rat(d.get("goc-ng"), d.get("ng"), p), rat(d.get("goc-bellard"), d.get("bellard"), p))

def sec(name):
    print("\n<!-- table: %s -->" % name)

def f(v, p=2):
    return "—" if v is None else ("%.*f" % (p, v))

v8 = D["v8"]; ss = D["sunspider"]; mb = D["micro"]; t = D["test262"]; q = D["qjs_tests"]; mc = D["microcall"]
m = v8["median"]
ssg, ssc = ss["geomean_all"], ss["common_all"]

sec("overview")
print("| 套件 " + HDR + RH + " 怎么读 |")
print("|---|" + NUM + RA + "---|")
print("| V8-v7 总分（%d 轮中位数） | %s | %s | 分数比，越高越快 |" % (
    len(v8["rounds"]["goc-ng"]), " | ".join("%d" % m[e]["Score"] for e in ENG), rats({e: m[e]["Score"] for e in ENG}, 3)))
print("| SunSpider 几何平均 ms（%d 项） | %s | %s | 时间比，越低越快 |" % (
    len(ssc), " | ".join(f(ssg[e]) for e in ENG), rats(ssg, 3)))
print("| microbench 几何平均 ns（%d 项） | %s | %s | 时间比，越低越快 |" % (
    len(mb["common"]), " | ".join(f(mb["geomean"][e], 1) for e in ENG), rats(mb["geomean"], 3)))
if mc.get("score_median"):
    sm = mc["score_median"]
    print("| microcall score（calls/ms） | %s | %s | 分数比，越高越快 |" % (
        " | ".join("%g" % sm[e] if e in sm else "—" for e in ENG), rats(sm, 3)))
print("| test262 通过 | %s | | | 抽样，不是全量 |" % " | ".join("%d/%d" % (t["pass"][e], t["ran"]) for e in ENG))
print("| QuickJS 官方测试 | %s | | | 按函数计 |" % " | ".join("%d/%d" % (q["pass"][e], q["total"]) for e in ENG))

sec("parity")
P = D.get("parity", {})
print("| goc 构建 | 对照的原生构建 | test262 通过（goc / 原生） | test262 结果不同的文件 | 官方测试通过（goc / 原生） | 官方测试结果不同的函数 |")
print("|---|---|---:|---:|---:|---:|")
for g, v in P.items():
    n = v["native"]
    print("| %s | %s | %d / %d | %d | %d / %d | %d |" % (NAME[g], NAME[n], t["pass"][g], t["pass"][n], len(v["test262_diff"]),
          q["pass"][g], q["pass"][n], len(v["qjs_diff"])))

sec("v8")
print("| 子项 " + HDR + RH)
print("|---|" + NUM + RA)
for s in ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes"]:
    print("| %s | %s | %s |" % (s, " | ".join("%g" % m[e][s] for e in ENG), rats({e: m[e][s] for e in ENG})))
print("| **总分** | %s | **%s** |" % (" | ".join("**%d**" % m[e]["Score"] for e in ENG),
      rats({e: m[e]["Score"] for e in ENG}, 3).replace(" | ", "** | **")))

sec("v8-rounds")
print("| 引擎 | 各轮总分 | 中位数 | 最小–最大 | 单轮墙钟中位数 |")
print("|---|---|---:|---:|---:|")
for e in ENG:
    print("| %s | %s | %d | %d–%d | %.1f s |" % (NAME[e],
          " / ".join(str(r["Score"]) for r in v8["rounds"][e]), m[e]["Score"], v8["score_min"][e], v8["score_max"][e], v8["wall_s_median"][e]))

sec("microcall")
if mc.get("rounds"):
    me = mc["engines"]
    print("| 用例 | " + " | ".join(NAME[e] + " ms" for e in me) + " |" + RH)
    print("|---|" + "---:|" * len(me) + RA)
    for n, v in mc["case_ms_median"].items():
        print("| %s | %s | %s |" % (n, " | ".join("%g" % v[e] for e in me), rats(v)))
    sm = mc["score_median"]
    print("| **score（calls/ms，越高越快）** | %s | **%s** |" % (
        " | ".join("**%g**（%g–%g）" % (sm[e], mc["score_min"][e], mc["score_max"][e]) for e in me),
        rats(sm).replace(" | ", "** | **")))
    print("\n（用例行是时间比，越低越快；score 行是分数比，越高越快。）")

sec("sunspider")
print("| 测试 " + HDR + RH)
print("|---|" + NUM + RA)
for x in ss["tests"]:
    cells = []
    for e in ENG:
        v = ss["ms"][e].get(x)
        cells.append(f(v) if v is not None else "失败")
    print("| %s | %s | %s |" % (x, " | ".join(cells), rats({e: ss["ms"][e].get(x) for e in ENG})))
print("| **几何平均 %d 项（五家都通过）** | %s | **%s** |" % (len(ssc), " | ".join("**%s**" % f(ssg[e]) for e in ENG),
      rats(ssg).replace(" | ", "** | **")))
gq = ss["geomean_nogoja"]
print("| **几何平均 %d 项（四个 QuickJS 构建都通过，不含 Goja）** | %s | — | **%s** |" % (len(ss["common_nogoja"]),
      " | ".join("**%s**" % f(gq[e]) for e in ENG[:4]), rats(gq).replace(" | ", "** | **")))

sec("ss-rounds")
print("| 引擎 | 各轮几何平均（%d 项） |" % len(ssc))
print("|---|---|")
for e in ENG:
    print("| %s | %s |" % (NAME[e], " / ".join("%.2f" % g for g in ss["geomean_all_by_round"][e])))

sec("micro-summary")
print("|  " + HDR + RH)
print("|---|" + NUM + RA)
print("| TIME 总和（中位数） | %s | %s |" % (" | ".join("%.0f" % mb["total_median"][e] if mb["total_median"][e] else "—" for e in ENG),
      rats(mb["total_median"])))
print("| 几何平均 %d 项 | %s | %s |" % (len(mb["common"]), " | ".join(f(mb["geomean"][e], 1) for e in ENG), rats(mb["geomean"])))

sec("micro-groups")
print("| 组 | 项数 " + HDR + RH)
print("|---|---:|" + NUM + RA)
for g, v in mb["group_geomean"].items():
    print("| %s | %d | %s | %s |" % (g, len([k for k in mb["groups"][g] if k in mb["common"]]), " | ".join(f(v[e], 1) for e in ENG), rats(v)))

sec("micro-slowest")
ns = mb["ns"]
full = [k for k in mb["tests"] if all(k in ns[e] for e in ENG)]
for g, n in (("goc-ng", "ng"), ("goc-bellard", "bellard")):
    r = sorted(((ns[g][k] / ns[n][k], k) for k in full), reverse=True)
    print("\n%s/%s slowest:" % (g, n))
    print("| 测试 | goc-ng/ng | goc-bellard/Bellard | Bellard/ng | Goja/ng |"); print("|---|---:|---:|---:|---:|")
    for x, k in r[:8]:
        print("| %s | %.2f | %.2f | %.2f | %.2f |" % (k, ns["goc-ng"][k] / ns["ng"][k], ns["goc-bellard"][k] / ns["bellard"][k],
              ns["bellard"][k] / ns["ng"][k], ns["goja"][k] / ns["ng"][k]))
    print("\nfastest:", ", ".join("%s %.2f" % (k, x) for x, k in r[-6:]))

sec("micro-all")
print("| 测试 | 组 " + HDR + RH)
print("|---|---|" + NUM + RA)
grp = {k: g for g, ks in mb["groups"].items() for k in ks}
for k in mb["tests"]:
    vals = {e: ns[e].get(k) for e in ENG}
    print("| %s | %s | %s | %s |" % (k, grp[k], " | ".join(f(vals[e]) for e in ENG), rats(vals)))
print("\nmissing:", mb["missing"])

sec("t262-dirs")
print("| 目录 | 合格正例 | 合格反例 | 实跑 " + HDR)
print("|---|---:|---:|---:|" + NUM)
for d, v in t["dirs"].items():
    print("| %s | %s | %s | %d | %s |" % (d, v.get("eligible_pos", "?"), v.get("eligible_neg", "?"), v["ran"], " | ".join("%d/%d" % (v[e], v["ran"]) for e in ENG)))
tp = sum(v.get("eligible_pos", 0) for v in t["dirs"].values()); tn = sum(v.get("eligible_neg", 0) for v in t["dirs"].values())
print("\npool: %d pos + %d neg" % (tp, tn))

sec("t262-fail")
print("| 引擎 | 路径 | 结果 |"); print("|---|---|---|")
for e in ENG:
    for k, v in t["fail"][e].items():
        print("| %s | %s | %s |" % (NAME[e], k.replace("test/language/", ""), v.replace("|", "\\|")[:140]))

sec("qjs-files")
print("| 文件 | n " + HDR); print("|---|---:|" + NUM)
for fn, v in q["files"].items():
    print("| %s | %d | %s |" % (fn, v["n"], " | ".join(str(v[e]) for e in ENG)))

sec("qjs-fn")
print("| 文件 | 函数 " + HDR); print("|---|---|" + "---|" * len(ENG))
for k in q["fn"]["goc-ng"]:
    fn, name = k.split(":")
    cells = []
    for e in ENG:
        v = q["fn"][e][k]
        cells.append("pass" if v == "pass" else v.replace("|", "\\|").replace("\n", " ")[:80])
    print("| %s | %s | %s |" % (fn.replace(".js", ""), name, " | ".join(cells)))

# ---- memory
M = D.get("memory", {})
if M.get("rss_kb") and M["rss_kb"].get("ng"):
    R = M["rss_kb"]
    mb_ = lambda kb: "—" if kb is None else "%.1f" % (kb / 1024)
    rows = [("empty", "空脚本（空载）")] + [("v8-" + x, "V8 " + x) for x in
            ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes"]] + [
            ("v8-all", "V8-v7 整套"), ("ss-max", "SunSpider（26 个文件中的最大值）"), ("micro", "microbench"),
            ("alloc", "alloc.js"), ("mapset", "mapset.js")]
    sec("mem-peak")
    print("| 负载 " + HDR + RH + " Bellard/ng | Goja/ng |")
    print("|---|" + NUM + RA + "---:|---:|")
    for k, lab in rows:
        v = {e: R[e].get(k) for e in ENG}
        print("| %s | %s | %s | %s | %s |" % (lab, " | ".join(mb_(v[e]) for e in ENG), rats(v), rat(v["bellard"], v["ng"]), rat(v["goja"], v["ng"])))
    for e in ENG:
        if M["rss_fail"][e]:
            print("\nfail %s: %s" % (e, M["rss_fail"][e]))
    sec("mem-ss")
    print("| 测试 " + HDR); print("|---|" + NUM)
    for k in sorted(x for x in R["ng"] if x.startswith("ss-") and x != "ss-max"):
        print("| %s | %s |" % (k[3:], " | ".join("失败" if k in M["rss_fail"][e] else mb_(R[e].get(k)) for e in ENG)))
MI = M.get("instances")
if MI:
    TL = {"goc-ng": "goc-ng（goroutine）", "goc-bellard": "goc-bellard（goroutine）", "ng": "native ng（pthread）",
          "ng-arena1": "native ng，`MALLOC_ARENA_MAX=1`", "bellard": "native Bellard（pthread）", "goja": "Goja（goroutine）"}
    tags = [x for x in ["goc-ng", "goc-bellard", "ng", "ng-arena1", "bellard", "goja"] if x in MI]
    sec("mem-inst")
    ns = MI["goc-ng"]["n"]
    print("| 引擎 | " + " | ".join("N=%d" % n for n in ns) + " | 每个 runtime（N=1→1000 斜率） | 最小二乘斜率 |")
    print("|---|" + "---:|" * (len(ns) + 2))
    for tg in tags:
        d = MI[tg]
        print("| %s | %s | **%.0f KiB** | %.0f KiB |" % (TL[tg], " | ".join("%.1f" % (d["live_rss_kb"][str(n)] / 1024) for n in ns),
              d["slope_1_1000_kb"], d["slope_lsq_kb"]))
    sl = {tg: MI[tg]["slope_1_1000_kb"] for tg in tags}
    print("\n斜率比：goc-ng/ng %s，goc-bellard/Bellard %s" % (rat(sl.get("goc-ng"), sl.get("ng")), rat(sl.get("goc-bellard"), sl.get("bellard"))))
    sec("mem-inst-per")
    print("| 引擎 | " + " | ".join("N=%d" % n for n in ns if n) + " |")
    print("|---|" + "---:|" * len([n for n in ns if n]))
    for tg in tags:
        d = MI[tg]
        print("| %s | %s |" % (TL[tg], " | ".join("%.0f" % d["per_instance_kb"][str(n)] for n in ns if n)))
    sec("mem-inst-after")
    print("| 引擎 | 回收方式 | " + " | ".join("N=%d" % n for n in ns) + " | 斜率 |")
    print("|---|---|" + "---:|" * (len(ns) + 1))
    for tg in tags:
        d = MI[tg]
        how = "`runtime.GC()` + `debug.FreeOSMemory()`" if d["after_kind"] == "after_gc" else "`malloc_trim(0)`"
        print("| %s | %s | %s | %.0f KiB |" % (TL[tg], how, " | ".join("%.1f" % (d["after_rss_kb"][str(n)] / 1024) for n in ns), d["slope_1_1000_after_kb"]))
    for tg in ("goc-ng", "goc-bellard", "goja"):
        if tg not in MI:
            continue
        d = MI[tg]
        sec("mem-go-" + tg)
        print("| N | RSS MiB | HeapSys MiB | HeapInuse MiB | StackSys MiB | StackInuse MiB | 平均每个 goroutine 栈 KiB | GC 后每个 goroutine 栈 KiB |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|")
        for n in ns:
            k = str(n)
            sp = d["stack_per_goroutine_b"].get(k); sa = d["stack_per_goroutine_after_gc_b"].get(k)
            print("| %d | %.1f | %.1f | %.1f | %.2f | %.2f | %s | %s |" % (n, d["live_rss_kb"][k] / 1024, d["live_HeapSys"][k] / 2**20,
                  d["live_HeapInuse"][k] / 2**20, d["live_StackSys"][k] / 2**20, d["live_StackInuse"][k] / 2**20,
                  "%.1f" % (sp / 1024) if sp is not None else "—", "%.1f" % (sa / 1024) if sa is not None else "—"))

if "bellard_clang_ref" in D:
    r = D["bellard_clang_ref"]
    sec("bellard-clang")
    has_g = "goc-bellard" in r["engines"]
    print("| 指标 | native Bellard（gcc -O2） | native Bellard（clang-19 -O2） | clang/gcc |" + (" goc-bellard | goc-bellard / clang 版 |" if has_g else "") + " 怎么读 |")
    print("|---|---:|---:|---:|" + ("---:|---:|" if has_g else "") + "---|")
    rows = [("V8-v7 总分（%d 轮中位数）" % len(r["v8_rounds"]["bellard-clang"]), {e: r["v8_median"][e]["Score"] for e in r["engines"]}, "%d", "分数比，越高越快"),
            ("SunSpider 几何平均 ms（%d 项）" % len(D["sunspider"]["common_all"]), r["ss_geomean"], "%.2f", "时间比，越低越快"),
            ("microbench 几何平均 ns（%d 项）" % len(D["micro"]["common"]), r["micro_geomean"], "%.1f", "时间比，越低越快")]
    for name, v, fm, how in rows:
        line = "| %s | %s | %s | %.3f |" % (name, fm % v["bellard"], fm % v["bellard-clang"], v["bellard-clang"] / v["bellard"])
        if has_g:
            line += " %s | %.3f |" % (fm % v["goc-bellard"], v["goc-bellard"] / v["bellard-clang"])
        print(line + " " + how + " |")
    print("\nv8 rounds:", r["v8_rounds"])
