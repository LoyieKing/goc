#!/usr/bin/env python3
"""Deterministic test262 `test/language` sample used by docs/benchmark.md.

Reconstructed from docs/benchmark/data/test262-meta.json: with tc39/test262
7ab7fafa0003f73fc85c1b95d88094d33f7eb8bd every per-directory eligible count
matches, and the evenly spaced pick below contains every failing file listed
in docs/benchmark/data/test262.json for all four engines.

Eligibility: skip test/language/{import,export,module-code}; skip flags
module/async/raw/CanBlockIsFalse/CanBlockIsTrue; skip features Atomics,
SharedArrayBuffer, caller; skip $262.agent users. Per directory take at most
80 positive and 40 negative tests at round(i*(n-1)/(k-1)).

Each test runs in a fresh process: assert.js + sta.js + its includes, then the
test source through a global indirect eval (so global var declarations are
configurable; that is why global-code/decl-* fail on every engine).

Usage:
  test262-sample.py --test262 DIR --engine NAME=CMD [--engine ...] [--out F.json] [-j N]
CMD is split on spaces; the test file path is appended. A second engine is
compared file-by-file against the first.
"""
import argparse, concurrent.futures as cf, json, os, re, shlex, subprocess, sys, tempfile

SKIP_DIRS = {"import", "export", "module-code"}
SKIP_FLAGS = {"module", "async", "raw", "CanBlockIsFalse", "CanBlockIsTrue"}
SKIP_FEATURES = {"Atomics", "SharedArrayBuffer", "caller"}

def frontmatter(src):
    m = re.search(r"/\*---(.*?)---\*/", src, re.S)
    return m.group(1) if m else ""

def listfield(y, name):
    m = re.search(r"^\s*" + name + r":\s*\[(.*?)\]", y, re.M | re.S)
    if m:
        return [x.strip() for x in m.group(1).split(",") if x.strip()]
    m = re.search(r"^" + name + r":\s*\n((?:\s+-\s*.*\n?)+)", y, re.M)
    if m:
        return [x.strip()[1:].strip() for x in m.group(1).splitlines() if x.strip().startswith("-")]
    return []

def negative_type(y):
    m = re.search(r"^negative:\s*\n(?:\s+\w+:.*\n)*?\s+type:\s*(\w+)", y, re.M)
    return m.group(1) if m else None

def pick(lst, k):
    n = len(lst)
    if n <= k:
        return list(lst)
    return [lst[round(i * (n - 1) / (k - 1))] for i in range(k)]

def sample(root):
    base = os.path.join(root, "test/language")
    out = []
    for d in sorted(os.listdir(base)):
        if d in SKIP_DIRS or not os.path.isdir(os.path.join(base, d)):
            continue
        pos, neg = [], []
        for dp, _, fns in os.walk(os.path.join(base, d)):
            for f in fns:
                if not f.endswith(".js"):
                    continue
                p = os.path.join(dp, f)
                src = open(p, encoding="utf-8", errors="replace").read()
                y = frontmatter(src)
                if SKIP_FLAGS & set(listfield(y, "flags")):
                    continue
                if SKIP_FEATURES & set(listfield(y, "features")):
                    continue
                if "$262.agent" in src:
                    continue
                rel = os.path.relpath(p, root)
                (neg if re.search(r"^negative:", y, re.M) else pos).append(rel)
        out += [(d, t) for t in pick(sorted(pos), 80) + pick(sorted(neg), 40)]
    return out

PRELUDE = r"""
var $262 = {
  global: globalThis,
  createRealm: function () { throw new Error("$262.createRealm"); },
  detachArrayBuffer: function () { throw new Error("$262.detachArrayBuffer"); },
  evalScript: function (s) { return (0, eval)(s); },
  gc: function () {},
};
"""

def build(root, rel):
    src = open(os.path.join(root, rel), encoding="utf-8").read()
    y = frontmatter(src)
    flags = listfield(y, "flags")
    neg = negative_type(y)
    harness = ""
    for inc in ["assert.js", "sta.js"] + listfield(y, "includes"):
        harness += open(os.path.join(root, "harness", inc), encoding="utf-8").read() + "\n"
    body = ('"use strict";\n' if "onlyStrict" in flags else "") + src
    js = harness + PRELUDE + "var __t262$src = %s;\nvar __t262$neg = %s;\n" % (json.dumps(body), json.dumps(neg))
    js += r"""
var __t262$err = null;
try { (0, eval)(__t262$src); } catch (e) { __t262$err = e; }
if (__t262$neg) {
  var __t262$n = __t262$err && (__t262$err.name || (__t262$err.constructor && __t262$err.constructor.name));
  if (__t262$err && __t262$n === __t262$neg) print("PASS");
  else if (__t262$err) print("FAIL expected " + __t262$neg + " got " + __t262$n + ": " + String(__t262$err));
  else print("FAIL expected " + __t262$neg);
} else if (__t262$err) {
  print("FAIL " + (__t262$err && __t262$err.name) + ": " + String(__t262$err));
} else print("PASS");
"""
    return js

def run_one(cmd, js, timeout):
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as f:
        f.write(js)
        path = f.name
    try:
        p = subprocess.run(cmd + [path], capture_output=True, text=True, timeout=timeout, errors="replace")
        lines = [l for l in p.stdout.splitlines() if l.startswith(("PASS", "FAIL"))]
        if lines:
            return lines[-1][:200]
        return "CRASH rc=%d %s" % (p.returncode, (p.stderr or p.stdout).strip().replace(path, "<t>")[:160])
    except subprocess.TimeoutExpired:
        return "CRASH timeout"
    finally:
        os.unlink(path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--test262", required=True)
    ap.add_argument("--engine", action="append", required=True)
    ap.add_argument("--out")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=float, default=30)
    a = ap.parse_args()
    tests = sample(a.test262)
    jsmap = {rel: build(a.test262, rel) for _, rel in tests}
    results = {}
    for spec in a.engine:
        name, cmd = spec.split("=", 1)
        cmd = shlex.split(cmd)
        with cf.ThreadPoolExecutor(a.j) as ex:
            futs = {rel: ex.submit(run_one, cmd, jsmap[rel], a.timeout) for _, rel in tests}
            results[name] = {rel: f.result() for rel, f in futs.items()}
        r = results[name]
        npass = sum(v == "PASS" for v in r.values())
        print("%s: %d/%d pass" % (name, npass, len(tests)))
        for rel in sorted(r):
            if r[rel] != "PASS":
                print("  %s %s" % (rel, r[rel]))
    names = [s.split("=", 1)[0] for s in a.engine]
    rc = 0
    if len(names) > 1:
        ref = results[names[0]]
        for n in names[1:]:
            diff = [rel for rel in ref if (ref[rel] == "PASS") != (results[n][rel] == "PASS")]
            print("%s vs %s: %d files differ in pass/fail" % (n, names[0], len(diff)))
            for rel in diff:
                print("  DIFF %s: %s=%s | %s=%s" % (rel, names[0], ref[rel], n, results[n][rel]))
            rc |= bool(diff)
    if a.out:
        json.dump({"tests": [rel for _, rel in tests], "results": results}, open(a.out, "w"), indent=1)
    sys.exit(rc)

if __name__ == "__main__":
    main()
