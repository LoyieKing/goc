#!/usr/bin/env python3
"""Print the markdown tables of docs/benchmark.md from data/all.json.

Usage: bench-tables.py [ALL_JSON] > /tmp/tables.md
Each table is preceded by a line `<!-- table: NAME -->` so it can be pasted
into the doc section by section.
"""
import json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = json.load(open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs/benchmark/data/all.json")))
ENG = ["goc", "ng", "bellard", "goja"]
HDR = "| goc | native ng | Bellard | Goja |"
NAME = {"goc": "goc", "ng": "native ng", "bellard": "Bellard", "goja": "Goja"}

def sec(name):
    print("\n<!-- table: %s -->" % name)

def f(v, p=2):
    return "—" if v is None else ("%.*f" % (p, v))

v8 = D["v8"]; ss = D["sunspider"]; mb = D["micro"]; t = D["test262"]; q = D["qjs_tests"]; mc = D["microcall"]
m = v8["median"]

sec("overview")
print("| 套件 " + HDR + " goc/ng | 怎么读 |")
print("|---|---:|---:|---:|---:|---:|---|")
print("| V8-v7 总分（%d 轮中位数） | %s | goc/ng 分数比 %.3f | 越高越快 |" % (
    len(v8["rounds"]["goc"]), " | ".join("%d" % m[e]["Score"] for e in ENG), m["goc"]["Score"] / m["ng"]["Score"]) )
print("| SunSpider 几何平均 ms（%d 项） | %s | %.3f | 越低越快 |" % (
    len(ss["common4"]), " | ".join(f(ss["geomean4"][e]) for e in ENG), ss["geomean4"]["goc"] / ss["geomean4"]["ng"]))
print("| microbench 几何平均 ns（%d 项） | %s | %.3f | 越低越快 |" % (
    len(mb["common"]), " | ".join(f(mb["geomean"][e], 1) for e in ENG), mb["geomean"]["goc"] / mb["geomean"]["ng"]))
print("| test262 通过 | %s | | 抽样，不是全量 |" % " | ".join("%d/%d" % (t["pass"][e], t["ran"]) for e in ENG))
print("| QuickJS 官方测试 | %s | | 按函数计 |" % " | ".join("%d/%d" % (q["pass"][e], q["total"]) for e in ENG))

sec("v8")
print("| 子项 " + HDR + " goc/ng |")
print("|---|---:|---:|---:|---:|---:|")
for s in ["Richards", "DeltaBlue", "Crypto", "RayTrace", "EarleyBoyer", "RegExp", "Splay", "NavierStokes"]:
    print("| %s | %s | %.2f |" % (s, " | ".join("%g" % m[e][s] for e in ENG), m["goc"][s] / m["ng"][s]))
print("| **总分** | %s | **%.3f** |" % (" | ".join("**%d**" % m[e]["Score"] for e in ENG), m["goc"]["Score"] / m["ng"]["Score"]))

sec("v8-rounds")
print("| 引擎 | 各轮总分 | 中位数 | 最小–最大 | 单轮墙钟中位数 |")
print("|---|---|---:|---:|---:|")
for e in ENG:
    print("| %s | %s | %d | %d–%d | %.1f s |" % ({"goc": "goc", "ng": "native ng", "bellard": "Bellard", "goja": "Goja"}[e],
          " / ".join(str(r["Score"]) for r in v8["rounds"][e]), m[e]["Score"], v8["score_min"][e], v8["score_max"][e], v8["wall_s_median"][e]))

sec("microcall")
if mc.get("rounds"):
    me = mc["engines"]
    print("| 用例 | " + " | ".join(NAME[e] + " ms" for e in me) + " | goc/ng |")
    print("|---|" + "---:|" * len(me) + "---:|")
    for n, v in mc["case_ms_median"].items():
        print("| %s | %s | %.2f |" % (n, " | ".join("%g" % v[e] for e in me), v["goc"] / v["ng"] if v["ng"] else 0))
    sm = mc["score_median"]
    print("| **score（calls/ms，越高越快）** | %s | **%.2f** |" % (
        " | ".join("**%g**（%g–%g）" % (sm[e], mc["score_min"][e], mc["score_max"][e]) for e in me), sm["goc"] / sm["ng"]))

sec("sunspider")
print("| 测试 " + HDR + " goc/ng |")
print("|---|---:|---:|---:|---:|---:|")
for x in ss["tests"]:
    cells = []
    for e in ENG:
        v = ss["ms"][e].get(x)
        cells.append(f(v) if v is not None else "失败")
    g, n = ss["ms"]["goc"].get(x), ss["ms"]["ng"].get(x)
    print("| %s | %s | %s |" % (x, " | ".join(cells), f(g / n) if g and n else ""))
print("| **几何平均 %d 项（四家都通过）** | %s | **%.2f** |" % (len(ss["common4"]), " | ".join("**%s**" % f(ss["geomean4"][e]) for e in ENG), ss["geomean4"]["goc"] / ss["geomean4"]["ng"]))
print("| **几何平均 %d 项（不含 Goja）** | %s | — | **%.2f** |" % (len(ss["common3"]), " | ".join("**%s**" % f(ss["geomean3"][e]) for e in ENG[:3]), ss["geomean3"]["goc"] / ss["geomean3"]["ng"]))

sec("ss-rounds")
print("| 引擎 | 各轮几何平均（%d 项） |" % len(ss["common4"]))
print("|---|---|")
for e in ENG:
    print("| %s | %s |" % (e, " / ".join("%.2f" % g for g in ss["geomean4_by_round"][e])))

sec("micro-summary")
print("|  " + HDR)
print("|---|---:|---:|---:|---:|")
print("| TIME 总和（中位数） | %s |" % " | ".join("%.0f" % mb["total_median"][e] if mb["total_median"][e] else "—" for e in ENG))
print("| 几何平均 %d 项 | %s |" % (len(mb["common"]), " | ".join(f(mb["geomean"][e], 1) for e in ENG)))

sec("micro-groups")
print("| 组 | 项数 " + HDR + " goc/ng |")
print("|---|---:|---:|---:|---:|---:|---:|")
for g, v in mb["group_geomean"].items():
    print("| %s | %d | %s | %.2f |" % (g, len([k for k in mb["groups"][g] if k in mb["common"]]), " | ".join(f(v[e], 1) for e in ENG), v["goc"] / v["ng"]))

sec("micro-slowest")
r = sorted(((mb["ns"]["goc"][k] / mb["ns"]["ng"][k], k) for k in mb["tests"] if all(k in mb["ns"][e] for e in ENG)), reverse=True)
print("| 测试 | goc/ng | Bellard/ng | Goja/ng |"); print("|---|---:|---:|---:|")
for x, k in r[:8]:
    print("| %s | %.2f | %.2f | %.2f |" % (k, x, mb["ns"]["bellard"][k] / mb["ns"]["ng"][k], mb["ns"]["goja"][k] / mb["ns"]["ng"][k]))
print("\nfastest:", ", ".join("%s %.2f" % (k, x) for x, k in r[-6:]))

sec("micro-all")
print("| 测试 | 组 " + HDR + " goc/ng |")
print("|---|---|---:|---:|---:|---:|---:|")
grp = {k: g for g, ks in mb["groups"].items() for k in ks}
for k in mb["tests"]:
    vals = [mb["ns"][e].get(k) for e in ENG]
    print("| %s | %s | %s | %s |" % (k, grp[k], " | ".join(f(v) for v in vals), f(vals[0] / vals[1]) if vals[0] and vals[1] else ""))
print("\nmissing:", mb["missing"])

sec("t262-dirs")
print("| 目录 | 合格正例 | 合格反例 | 实跑 " + HDR)
print("|---|---:|---:|---:|---:|---:|---:|---:|")
for d, v in t["dirs"].items():
    print("| %s | %s | %s | %d | %s |" % (d, v.get("eligible_pos", "?"), v.get("eligible_neg", "?"), v["ran"], " | ".join("%d/%d" % (v[e], v["ran"]) for e in ENG)))
tp = sum(v.get("eligible_pos", 0) for v in t["dirs"].values()); tn = sum(v.get("eligible_neg", 0) for v in t["dirs"].values())
print("\npool: %d pos + %d neg" % (tp, tn))

sec("t262-fail")
print("| 引擎 | 路径 | 结果 |"); print("|---|---|---|")
for e in ENG:
    for k, v in t["fail"][e].items():
        print("| %s | %s | %s |" % (e, k.replace("test/language/", ""), v.replace("|", "\\|")[:140]))

sec("qjs-files")
print("| 文件 | n " + HDR); print("|---|---:|---:|---:|---:|---:|")
for fn, v in q["files"].items():
    print("| %s | %d | %s |" % (fn, v["n"], " | ".join(str(v[e]) for e in ENG)))

sec("qjs-fn")
print("| 文件 | 函数 " + HDR); print("|---|---|---|---|---|---|")
for k in q["fn"]["goc"]:
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
    print("| 负载 " + HDR + " goc/ng | Bellard/ng | Goja/ng |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for k, lab in rows:
        v = [R[e].get(k) for e in ENG]
        rat = lambda a: "%.2f" % (a / v[1]) if a and v[1] else "—"
        print("| %s | %s | %s | %s | %s |" % (lab, " | ".join(mb_(x) for x in v), rat(v[0]), rat(v[2]), rat(v[3])))
    for e in ENG:
        if M["rss_fail"][e]:
            print("\nfail %s: %s" % (e, M["rss_fail"][e]))
    sec("mem-ss")
    print("| 测试 " + HDR); print("|---|---:|---:|---:|---:|")
    for k in sorted(x for x in R["ng"] if x.startswith("ss-") and x != "ss-max"):
        print("| %s | %s |" % (k[3:], " | ".join("失败" if k in M["rss_fail"][e] else mb_(R[e].get(k)) for e in ENG)))
MI = M.get("instances")
if MI:
    TL = {"goc": "goc（goroutine）", "ng": "native ng（pthread）", "ng-arena1": "native ng，`MALLOC_ARENA_MAX=1`",
          "bellard": "Bellard（pthread）", "goja": "Goja（goroutine）"}
    tags = [x for x in ["goc", "ng", "ng-arena1", "bellard", "goja"] if x in MI]
    sec("mem-inst")
    ns = MI["goc"]["n"]
    print("| 引擎 | " + " | ".join("N=%d" % n for n in ns) + " | 每个 runtime（N=1→1000 斜率） | 最小二乘斜率 |")
    print("|---|" + "---:|" * (len(ns) + 2))
    for tg in tags:
        d = MI[tg]
        print("| %s | %s | **%.0f KiB** | %.0f KiB |" % (TL[tg], " | ".join("%.1f" % (d["live_rss_kb"][str(n)] / 1024) for n in ns),
              d["slope_1_1000_kb"], d["slope_lsq_kb"]))
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
    for tg in ("goc", "goja"):
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
