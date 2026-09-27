#!/usr/bin/env python3
"""Decompose goc's extra instructions vs the gocflags native build, per suite,
from docs/perf-gap/data/callgrind-classes.json (perfgap-cg-classify.py).

Output: docs/perf-gap/data/ir-decomposition.json and a markdown table on stdout.
Each row: total Ir ratio goc/native, then the Ir delta split into
  goc-only instruction classes inside engine code (stackcheck, guard,
  fixup+morestack, tls, got), deltas of pushpop / framemem / rest+call in
  engine code (codegen differences), and function-group deltas
  (memstr, alloc, math, time, goruntime+alloca, other).
All deltas in % of the native total Ir.
"""
import json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = json.load(open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs/perf-gap/data/callgrind-classes.json')))
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'docs/perf-gap/data/ir-decomposition.json')
PAIRS = [('goc-bellard', 'bellard-clang-O3-gocflags'), ('goc-ng', 'ng-clang-O3-gocflags'),
         ('bellard-clang-O3-gocflags', 'bellard-clang-O3'), ('ng-clang-O3-gocflags', 'ng-clang-O3'),
         # layer split through goc's pipeline built natively
         ('bellard-gocpipe', 'bellard-clang-O3-gocflags'), ('bellard-gocpipe-sm', 'bellard-gocpipe'),
         ('goc-bellard', 'bellard-gocpipe-sm'),
         # the inliner-threshold fix
         ('bellard-gocpipe-inl250', 'bellard-gocpipe'), ('bellard-gocpipe-sm-inl250', 'bellard-gocpipe-sm'),
         ('goc-bellard-inl250', 'goc-bellard'), ('goc-ng-inl250', 'goc-ng'),
         ('goc-bellard-inl250', 'bellard-clang-O3-gocflags'),
         ('goc-bellard-taildup', 'goc-bellard')]
SUITES = ['Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes', 'Parse']
COLS = ['stackcheck', 'guard', 'fixup+morestack', 'tls', 'got', 'pushpop', 'framemem', 'rest+call',
        'memstr', 'alloc', 'math', 'time', 'goruntime', 'other']
def comp(r):
    c = r['classes']; g = r['groups']
    return {'stackcheck': c.get('stackcheck', 0), 'guard': c.get('guard', 0),
            'fixup+morestack': c.get('fixup', 0) + c.get('morestack', 0), 'tls': c.get('tls', 0),
            'got': c.get('got', 0), 'pushpop': c.get('pushpop', 0), 'framemem': c.get('framemem', 0),
            'rest+call': c.get('rest', 0) + c.get('call', 0),
            'memstr': g.get('memstr', 0), 'alloc': g.get('alloc', 0), 'math': g.get('math', 0),
            'time': g.get('time', 0), 'goruntime': g.get('goruntime', 0) + g.get('alloca', 0),
            'other': g.get('other', 0)}
res = {}
for a, b in PAIRS:
    if a not in D or b not in D: continue
    print(f"\n### {a} vs {b} (Ir delta, % of {b} total)\n")
    print('| suite | Ir ratio | ' + ' | '.join(COLS) + ' | ops ratio | Ir/op (a) | Ir/op (b) |')
    print('|' + '---|' * (len(COLS) + 5))
    agg_a = {k: 0 for k in COLS}; agg_b = {k: 0 for k in COLS}; ta = tb_ = 0
    for s in SUITES:
        if s not in D[a] or s not in D[b]: continue
        if s != 'Parse':
            for k in COLS:
                agg_a[k] += comp(D[a][s])[k]; agg_b[k] += comp(D[b][s])[k]
            ta += D[a][s]['total']; tb_ += D[b][s]['total']
        ra, rb = D[a][s], D[b][s]
        ca, cb = comp(ra), comp(rb)
        tot = rb['total']
        row = {k: (ca[k] - cb[k]) / tot * 100 for k in COLS}
        row['ratio'] = ra['total'] / tot
        row['ops_a'] = ra.get('dispatch_ijmp', 0); row['ops_b'] = rb.get('dispatch_ijmp', 0)
        row['ci_a'] = ra.get('callinternal_ir', 0); row['ci_b'] = rb.get('callinternal_ir', 0)
        res.setdefault(f'{a}|{b}', {})[s] = row
        ipa = row['ci_a'] / row['ops_a'] if row['ops_a'] else 0
        ipb = row['ci_b'] / row['ops_b'] if row['ops_b'] else 0
        opr = row['ops_a'] / row['ops_b'] if row['ops_b'] else 0
        print(f"| {s} | {row['ratio']:.3f} | " + ' | '.join(f"{row[k]:+.2f}" for k in COLS) +
              f" | {opr:.2f} | {ipa:.1f} | {ipb:.1f} |")
    if tb_:
        row = {k: (agg_a[k] - agg_b[k]) / tb_ * 100 for k in COLS}; row['ratio'] = ta / tb_
        res[f'{a}|{b}']['V8sum'] = row
        print(f"| V8 sum | {row['ratio']:.3f} | " + ' | '.join(f"{row[k]:+.2f}" for k in COLS) + " | | | |")
json.dump(res, open(OUT, 'w'), indent=1)
print('\nwrote', OUT)
