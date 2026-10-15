#!/usr/bin/env python3
"""Wrap WebKit SunSpider 1.0.2 tests for docs/benchmark.md.

Each test body goes into `function __t(){...}`; `document.write` and
`getElementById` are stubbed; `console.log` falls back to `print`. The
footer calls __t() until 150 ms have passed (warm-up, fixing n), then runs
5 batches of n calls and prints the best batch as ms per call:
  "<name> <ms/iter> <n>"
Timing is Date.now(), so ms resolution over a >=150 ms batch.

Usage: sunspider-wrap.py SRC_DIR OUT_DIR   (SRC_DIR holds <test>.js files)
"""
import os, sys

HEAD = ('if (typeof console === "undefined") globalThis.console = { log: print };\n'
        'var document={write:function(){},getElementById:function(){return {getContext:function(){return {}}}}};\n'
        'function __t(){\n')
FOOT = ('var t0=Date.now(),n=0; do { __t(); n++; } while (Date.now()-t0 < 150);\n'
        'var best=1e9; for (var r=0;r<5;r++){ t0=Date.now(); for (var i=0;i<n;i++) __t(); var d=(Date.now()-t0)/n; if(d<best)best=d; }\n'
        'console.log("%s "+best.toFixed(4)+" "+n);\n')

def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for f in sorted(os.listdir(src)):
        if not f.endswith(".js"):
            continue
        name = f[:-3]
        body = open(os.path.join(src, f), encoding="utf-8").read()
        with open(os.path.join(out, f), "w", encoding="utf-8") as o:
            o.write(HEAD + body + "\n}\n" + FOOT % name)

if __name__ == "__main__":
    main()
