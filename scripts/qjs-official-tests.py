#!/usr/bin/env python3
"""Bellard QuickJS tests/ run one function per process (docs/benchmark.md).

Files: test_language.js test_closure.js test_loop.js test_bigint.js
test_builtin.js from bellard/quickjs 2026-06-04 (pass --tests DIR). For every
top-level `name();` call line in a file, the file is emitted with its
non-call top-level code, the target function and every function it
references (transitively), `name();` is appended, and it runs in a fresh
process. PASS is
exit status 0. `std` / `os` are not provided, so functions that need them fail
on every engine.

Usage: qjs-official-tests.py --tests DIR --engine NAME=CMD [--engine ...] [--out F.json]
A later engine is compared function-by-function against the first.
"""
import argparse, concurrent.futures as cf, json, os, re, shlex, subprocess, sys, tempfile

FILES = ["test_language.js", "test_closure.js", "test_loop.js", "test_bigint.js", "test_builtin.js"]
CALL = re.compile(r"^(test\w*)\(\);\s*$")

FUNC = re.compile(r"^(?:async\s+)?function\*?\s+(\w+)\s*\(")

def split_top(lines):
    """Top-level chunks: a `function name(` line up to the next column-0
    function or call line is ('fn', name, text); other lines are ('top', ...)."""
    chunks, i = [], 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m:
            j = i + 1
            while j < len(lines) and not FUNC.match(lines[j]) and not CALL.match(lines[j]):
                j += 1
            chunks.append(("fn", m.group(1), "\n".join(lines[i:j])))
            i = j
        else:
            chunks.append(("top", None, lines[i]))
            i += 1
    return chunks

def cases(d):
    out = []
    for f in FILES:
        chunks = split_top(open(os.path.join(d, f), encoding="utf-8").read().splitlines())
        calls = [CALL.match(t).group(1) for k, _, t in chunks if k == "top" and CALL.match(t)]
        fns = {n: t for k, n, t in chunks if k == "fn"}
        for name in calls:
            # The target plus every function it (transitively) references, and
            # all non-call top-level code. Other entry points are left out.
            text = "\n".join(t for k, _, t in chunks if k == "top" and not CALL.match(t))
            keep, todo = set(), [name]
            while todo:
                n = todo.pop()
                if n in keep:
                    continue
                keep.add(n)
                for m in fns:
                    if m not in keep and re.search(r"\b%s\b" % re.escape(m), fns[n]):
                        todo.append(m)
            for m in fns:  # helpers referenced from top-level code
                if m not in calls and m not in keep and re.search(r"\b%s\b" % re.escape(m), text):
                    keep.add(m)
            body = [t for k, n, t in chunks
                    if (k == "top" and not CALL.match(t)) or (k == "fn" and n in keep)]
            out.append((f, name, "\n".join(body) + "\n" + name + "();\n"))
    return out

def run(cmd, js, timeout):
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as t:
        t.write(js)
        p = t.name
    try:
        r = subprocess.run(cmd + [p], capture_output=True, text=True, timeout=timeout, errors="replace")
        if r.returncode == 0:
            return "pass"
        msg = (r.stderr.strip() or r.stdout.strip()).replace(p, "<t>").splitlines()
        return "FAIL " + (msg[0][:120] if msg else "rc=%d" % r.returncode)
    except subprocess.TimeoutExpired:
        return "FAIL timeout"
    finally:
        os.unlink(p)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tests", required=True)
    ap.add_argument("--engine", action="append", required=True)
    ap.add_argument("--out")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=float, default=60)
    a = ap.parse_args()
    cs = cases(a.tests)
    res = {}
    for spec in a.engine:
        name, cmd = spec.split("=", 1)
        cmd = shlex.split(cmd)
        with cf.ThreadPoolExecutor(a.j) as ex:
            futs = {(f, fn): ex.submit(run, cmd, js, a.timeout) for f, fn, js in cs}
            res[name] = {"%s:%s" % k: v.result() for k, v in futs.items()}
        r = res[name]
        print("%s: %d/%d functions pass" % (name, sum(v == "pass" for v in r.values()), len(r)))
        for k in sorted(r):
            if r[k] != "pass":
                print("  %s %s" % (k, r[k]))
    names = [s.split("=", 1)[0] for s in a.engine]
    rc = 0
    for n in names[1:]:
        diff = [k for k in res[names[0]] if (res[names[0]][k] == "pass") != (res[n][k] == "pass")]
        print("%s vs %s: %d functions differ" % (n, names[0], len(diff)))
        for k in diff:
            print("  DIFF %s: %s | %s" % (k, res[names[0]][k], res[n][k]))
        rc |= bool(diff)
    if a.out:
        json.dump(res, open(a.out, "w"), indent=1)
    sys.exit(rc)

if __name__ == "__main__":
    main()
